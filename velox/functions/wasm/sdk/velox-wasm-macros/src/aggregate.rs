/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
use super::*;

pub(super) struct RowAggregateArgs {
    signature: ArrowScalarArgs,
    intermediate: LitStr,
    order_sensitive: bool,
    ignore_duplicates: bool,
    checkpoint: bool,
    lambdas: Vec<LitStr>,
}
impl syn::parse::Parse for RowAggregateArgs {
    fn parse(input: syn::parse::ParseStream<'_>) -> syn::Result<Self> {
        let mut signature = quote!();
        let mut intermediate = None;
        let mut order_sensitive = false;
        let mut ignore_duplicates = false;
        let mut checkpoint = false;
        let mut lambdas = Vec::new();
        let mut seen = std::collections::HashSet::new();
        while !input.is_empty() {
            let key: syn::Ident = input.parse()?;
            if !seen.insert(key.to_string()) {
                return Err(syn::Error::new(key.span(), "duplicate aggregate option"));
            }
            input.parse::<syn::Token![=]>()?;
            match key.to_string().as_str() {
                "intermediate" => intermediate = Some(input.parse()?),
                "order_sensitive" => order_sensitive = input.parse::<syn::LitBool>()?.value,
                "ignore_duplicates" => ignore_duplicates = input.parse::<syn::LitBool>()?.value,
                "checkpoint" => checkpoint = input.parse::<syn::LitBool>()?.value,
                "lambdas" => {
                    let content;
                    syn::bracketed!(content in input);
                    lambdas = content
                        .parse_terminated(|input| input.parse::<LitStr>(), syn::Token![,])?
                        .into_iter()
                        .collect();
                }
                "initialize" | "call_ascii" | "call_null_free" | "deterministic" => {
                    return Err(syn::Error::new(
                        key.span(),
                        "unsupported row aggregate option",
                    ));
                }
                _ => {
                    let value: syn::Expr = input.parse()?;
                    signature.extend(quote!(#key = #value,));
                }
            }
            if !input.is_empty() {
                input.parse::<syn::Token![,]>()?;
            }
        }
        // Initialization is generated for every row aggregate; its context is
        // passed to create, rather than accepting a separate user callback.
        signature.extend(quote!(initialize = "generated"));
        Ok(Self {
            signature: syn::parse2(signature)?,
            intermediate: intermediate.ok_or_else(|| input.error("missing intermediate"))?,
            order_sensitive,
            ignore_duplicates,
            checkpoint,
            lambdas,
        })
    }
}

pub(super) fn expand(
    args: RowAggregateArgs,
    implementation: ItemImpl,
) -> syn::Result<proc_macro2::TokenStream> {
    if !implementation.generics.params.is_empty()
        || implementation
            .trait_
            .as_ref()
            .and_then(|(_, path, _)| path.segments.last())
            .map(|segment| segment.ident.to_string())
            .as_deref()
            != Some("VeloxRowAggregate")
    {
        return Err(syn::Error::new(
            implementation.span(),
            "expected impl VeloxRowAggregate for a named struct without Rust type parameters",
        ));
    }
    if args.lambdas.len() > 64 {
        return Err(syn::Error::new(
            implementation.span(),
            "aggregate signatures allow at most 64 lambdas",
        ));
    }
    let self_type = &implementation.self_ty;
    let name = final_ident(self_type)
        .ok_or_else(|| syn::Error::new(self_type.span(), "expected named aggregate struct"))?;
    let prefix = args
        .signature
        .export
        .as_ref()
        .map(LitStr::value)
        .unwrap_or_else(|| name.to_snake_case());
    if prefix.is_empty()
        || !prefix
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
    {
        return Err(syn::Error::new(
            self_type.span(),
            "aggregate export prefix must be ASCII letters/digits/underscores",
        ));
    }
    let Type::Tuple(inputs) = associated_type(&implementation, "Input")? else {
        return Err(syn::Error::new(
            implementation.span(),
            "aggregate Input<'a> must be a tuple",
        ));
    };
    let fixed = args.signature.arguments.len();
    let variadic = args.signature.variadic.is_some();
    if inputs.elems.len() != fixed + usize::from(variadic) {
        return Err(syn::Error::new(
            inputs.span(),
            "Input tuple must match fixed arguments plus one variadic tail",
        ));
    }
    let mut calls = Vec::new();
    let mut setup = quote!();
    let mut nullable = Vec::new();
    for (index, ty) in inputs.elems.iter().enumerate() {
        let tail = variadic_element(ty)?;
        if variadic && index == fixed {
            let element = tail.ok_or_else(|| {
                syn::Error::new(ty.span(), "last input must be VariadicView or Variadic")
            })?;
            validate_row_input_type(element)?;
            nullable.push(nullable_row_type(element));
            if final_ident(ty).as_deref() == Some("VariadicView") {
                setup.extend(
                    quote!(let __tail = ::velox_wasm_sdk::variadic::VariadicReader::new(
                    &__batch.columns()[__start + #fixed..], __batch.num_rows())?;),
                );
                calls.push(quote!(__tail.row(__row)?));
            } else {
                calls.push(quote!(::velox_wasm_sdk::Variadic::from_row(&__batch.columns()[__start + #fixed..], __row)?));
            }
        } else {
            if tail.is_some() {
                return Err(syn::Error::new(ty.span(), "variadic input must be final"));
            }
            validate_row_input_type(ty)?;
            nullable.push(nullable_row_type(ty));
            let reader = format_ident!("__argument_reader_{}", index);
            setup.extend(quote!(let #reader = ::velox_wasm_sdk::scalar::prepare_row(
                __batch.column(__start + #index).as_ref());));
            calls.push(quote!(#reader(__row)?));
        }
    }
    let mut arguments = args.signature.arguments.clone();
    if let Some(tail) = &args.signature.variadic {
        arguments.push(tail.clone());
    }
    if args
        .signature
        .constant_arguments
        .iter()
        .any(|index| *index >= arguments.len())
    {
        return Err(syn::Error::new(
            implementation.span(),
            "constant argument index outside signature",
        ));
    }
    let declared: Vec<_> = arguments
        .iter()
        .enumerate()
        .map(|(index, ty)| {
            json!({
                "type": ty.value(), "nullable": nullable[index],
                "constant": args.signature.constant_arguments.contains(&index),
            })
        })
        .collect();
    let returns = args
        .signature
        .returns
        .as_ref()
        .ok_or_else(|| syn::Error::new(implementation.span(), "missing returns"))?;
    let default_null = if args.signature.null_behavior_explicit {
        args.signature.default_null_behavior
    } else {
        !nullable.iter().any(|value| *value)
    };
    let registry = format_ident!("__VELOX_ROW_AGGREGATE_{}", prefix.to_uppercase());
    let initialize_export = format!("{prefix}_initialize");
    let mut exports = serde_json::Map::new();
    for (key, suffix) in [
        ("create", "create"),
        ("destroy", "destroy"),
        ("update", "update"),
        ("update_single_group", "update_single_group"),
        ("serialize", "serialize"),
        ("merge", "merge"),
        ("merge_single_group", "merge_single_group"),
        ("finalize", "finalize"),
        ("to_intermediate", "to_intermediate"),
        ("compact", "compact"),
    ] {
        exports.insert(key.into(), json!(format!("{prefix}_{suffix}")));
    }
    if args.checkpoint {
        exports.insert("checkpoint".into(), json!(format!("{prefix}_checkpoint")));
        exports.insert("restore".into(), json!(format!("{prefix}_restore")));
    }
    let declaration = metadata_section(
        format_ident!("__VELOX_ROW_AGGREGATE_METADATA_{}", prefix.to_uppercase()),
        {
            let mut declaration_json = json!({
                "abi_version": 3, "kind": "aggregate", "row_api": true,
                "name": args.signature.name.as_ref().map(LitStr::value).unwrap_or_else(|| prefix.clone()),
                "entrypoints": exports, "initialize": initialize_export,
                "arguments": declared, "variable_arity": variadic,
                "type_variables": signature_variables(&args.signature.type_variables, false)?,
                "integer_variables": signature_variables(&args.signature.integer_variables, true)?,
                "intermediate": {"type": args.intermediate.value(), "nullable": true},
                "return": {"type": returns.value(), "nullable": true},
                "config_keys": args.signature.config_keys.iter().map(LitStr::value).collect::<Vec<_>>(),
                "order_sensitive": args.order_sensitive, "ignore_duplicates": args.ignore_duplicates,
                "default_null_behavior": default_null,
            });
            if args.checkpoint {
                declaration_json["state_checkpoint"] = json!("owned-v1");
            }
            if !args.lambdas.is_empty() {
                declaration_json["lambda_callback"] = json!("ipc-v1");
                declaration_json["lambdas"] =
                    json!(args.lambdas.iter().map(LitStr::value).collect::<Vec<_>>());
            }
            declaration_json
        },
    );
    let mut functions = quote!();
    let batch_update = implementation.items.iter().any(
        |item| matches!(item, syn::ImplItem::Fn(method) if method.sig.ident == "update_batch"),
    );
    let batch_merge = implementation
        .items
        .iter()
        .any(|item| matches!(item, syn::ImplItem::Fn(method) if method.sig.ident == "merge_batch"));
    let merge_method = format_ident!(
        "{}",
        if batch_merge {
            "merge_batches"
        } else {
            "merge"
        }
    );
    let update_method = format_ident!(
        "{}",
        if batch_update {
            "update_batches"
        } else {
            "update"
        }
    );
    let update_closure = if batch_update {
        quote!(|state, __rows| {
            <#self_type as ::velox_wasm_sdk::VeloxRowAggregate>::update_batch(
                state,
                __rows.iter().copied().map(|__row| -> ::std::result::Result<_, String> {Ok((#(#calls,)*))})
            ).map_err(::velox_wasm_sdk::AggregateBatchError::encode)
        })
    } else {
        quote!(|state, __row| {
            <#self_type as ::velox_wasm_sdk::VeloxRowAggregate>::update(state, (#(#calls,)*))
                .map_err(::velox_wasm_sdk::aggregate::encode_error)
        })
    };
    for suffix in [
        "initialize",
        "destroy",
        "update",
        "serialize",
        "merge",
        "finalize",
        "to_intermediate",
        "compact",
    ] {
        let function = format_ident!("__velox_row_{}_{}", prefix, suffix);
        let export = format!("{prefix}_{suffix}");
        let body = match suffix {
            "initialize" => {
                quote!(#registry.with(|adapter| adapter.borrow_mut().initialize(__batch)))
            }
            "destroy" => quote!(#registry.with(|adapter| adapter.borrow_mut().destroy(&__batch))),
            "compact" => quote!(#registry.with(|adapter| adapter.borrow_mut().compact(&__batch))),
            "update" => quote! {
                let __start = 1usize;
                if (!#variadic && __batch.num_columns() != #fixed + __start)
                    || __batch.num_columns() < #fixed + __start { return Err("update argument count mismatch".into()); }
                #setup
                #registry.with(|adapter| adapter.borrow_mut().#update_method(&__batch, None, #update_closure))
            },
            "to_intermediate" => quote! {
                let __start = 0usize;
                if (!#variadic && __batch.num_columns() != #fixed) || __batch.num_columns() < #fixed {
                    return Err("toIntermediate argument count mismatch".into());
                }
                #setup
                #registry.with(|adapter| adapter.borrow().to_intermediate(&__batch, |state, __row| {
                    <#self_type as ::velox_wasm_sdk::VeloxRowAggregate>::update(state, (#(#calls,)*))
                        .map_err(::velox_wasm_sdk::aggregate::encode_error)
                }))
            },
            "merge" => {
                quote!(#registry.with(|adapter| adapter.borrow_mut().#merge_method(&__batch, None, #default_null)))
            }
            "serialize" => {
                quote!(#registry.with(|adapter| adapter.borrow().extract(&__batch, true)))
            }
            _ => quote!(#registry.with(|adapter| adapter.borrow().extract(&__batch, false))),
        };
        functions.extend(quote! {
            #[cfg(target_arch = "wasm32")]
            #[export_name = #export]
            #[target_feature(enable = "simd128")]
            pub unsafe extern "C" fn #function(pointer: u32, length: u32) -> core::arch::wasm32::v128 {
                ::velox_wasm_sdk::runtime::execute_aggregate(pointer, length, |__batch| { #body })
            }
        });
    }
    for suffix in ["update_single_group", "merge_single_group"] {
        let function = format_ident!("__velox_row_{}_{}", prefix, suffix);
        let export = format!("{prefix}_{suffix}");
        let body = if suffix == "update_single_group" {
            quote! {
                let __start = 0usize;
                if (!#variadic && __batch.num_columns() != #fixed)
                    || __batch.num_columns() < #fixed { return Err("single update argument count mismatch".into()); }
                #setup
                #registry.with(|adapter| adapter.borrow_mut().#update_method(&__batch, Some(handle), #update_closure))
            }
        } else {
            quote!(#registry.with(|adapter| adapter.borrow_mut().#merge_method(&__batch, Some(handle), #default_null)))
        };
        functions.extend(quote! {
            #[cfg(target_arch = "wasm32")]
            #[export_name = #export]
            #[target_feature(enable = "simd128")]
            pub unsafe extern "C" fn #function(handle: u32, pointer: u32, length: u32) -> core::arch::wasm32::v128 {
                ::velox_wasm_sdk::runtime::execute_aggregate(pointer, length, |__batch| { #body })
            }
        });
    }
    if args.checkpoint {
        let checkpoint_fn = format_ident!("__velox_row_{}_checkpoint", prefix);
        let checkpoint_export = format!("{prefix}_checkpoint");
        let restore_fn = format_ident!("__velox_row_{}_restore", prefix);
        let restore_export = format!("{prefix}_restore");
        functions.extend(quote! {
            #[cfg(target_arch="wasm32")]
            #[export_name=#checkpoint_export]
            #[target_feature(enable="simd128")]
            pub unsafe extern "C" fn #checkpoint_fn(limit:u32)->core::arch::wasm32::v128 {
                ::velox_wasm_sdk::runtime::execute_aggregate_count(limit,|limit|#registry.with(|adapter|adapter.borrow().checkpoint(limit)))
            }
            #[cfg(target_arch="wasm32")]
            #[export_name=#restore_export]
            #[target_feature(enable="simd128")]
            pub unsafe extern "C" fn #restore_fn(limit:u32,pointer:u32,length:u32)->core::arch::wasm32::v128 {
                ::velox_wasm_sdk::runtime::execute_aggregate_bytes(pointer,length,|bytes|#registry.with(|adapter|adapter.borrow_mut().restore(bytes,limit as u64)))
            }
        });
    }
    let create_fn = format_ident!("__velox_row_{}_create", prefix);
    let create_export = format!("{prefix}_create");
    Ok(quote! {
        #implementation
        #declaration
        #[cfg(target_arch = "wasm32")]
        std::thread_local! {
            static #registry: std::cell::RefCell<::velox_wasm_sdk::aggregate::AggregateAdapter<#self_type>>
                = std::cell::RefCell::new(Default::default());
        }
        #[cfg(target_arch = "wasm32")]
        #[export_name = #create_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #create_fn(count: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute_aggregate_count(count, |count| #registry.with(|adapter| adapter.borrow_mut().create(count)))
        }
        #functions
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn implementation(input: Type) -> ItemImpl {
        syn::parse_quote!(impl VeloxRowAggregate for Example {
            type State = i64;
            type Input<'a> = #input;
            type Error = String;
        })
    }
    #[test]
    fn accepts_generic_typed_intermediate_and_borrowed_variadic() {
        let args: RowAggregateArgs = syn::parse2(quote!(
            arguments = ["T"],
            variadic = "T",
            type_variables = ["T:comparable"],
            returns = "T",
            intermediate = "row(T,bigint)",
            constant_arguments = [0],
            config_keys = ["timezone"]
        ))
        .unwrap();
        let expanded = expand(
            args,
            implementation(syn::parse_quote!((
                Generic<'a>,
                VariadicView<'a, Option<Generic<'a>>>
            ))),
        )
        .unwrap()
        .to_string();
        assert!(expanded.contains("VariadicReader"));
        assert!(expanded.contains("AggregateAdapter"));
        assert!(expanded.contains("encode_error"));
        assert!(expanded.contains("execute_aggregate_count"));
    }
    #[test]
    fn rejects_batch_inputs_and_invalid_variadic_position() {
        for input in [
            syn::parse_quote!((ArrayRef,)),
            syn::parse_quote!((Variadic<i64>, i64)),
        ] {
            let args = syn::parse2(quote!(
                arguments = ["bigint"],
                variadic = "bigint",
                returns = "bigint",
                intermediate = "bigint"
            ))
            .unwrap();
            assert!(expand(args, implementation(input)).is_err());
        }
        assert!(
            syn::parse2::<RowAggregateArgs>(quote!(arguments = [], returns = "bigint")).is_err()
        );
        assert!(syn::parse2::<RowAggregateArgs>(quote!(
            arguments = [],
            returns = "bigint",
            intermediate = "bigint",
            initialize = "foo"
        ))
        .is_err());
    }

    #[test]
    fn lambdas_are_declared_separately_from_row_value_arguments() {
        let args = syn::parse2(quote!(
            arguments = ["T", "S"],
            returns = "S",
            intermediate = "S",
            type_variables = ["T", "S"],
            lambdas = ["function(S,T,S)", "function(S,S,S)"]
        ))
        .unwrap();
        let expanded = expand(
            args,
            implementation(syn::parse_quote!((Generic<'a>, Generic<'a>))),
        )
        .unwrap()
        .to_string();
        assert!(expanded.contains("ipc-v1") && expanded.contains("lambdas"));
        let args = syn::parse2(quote!(
            arguments = ["bigint"],
            variadic = "bigint",
            returns = "bigint",
            intermediate = "bigint",
            lambdas = ["function(bigint,bigint)"]
        ))
        .unwrap();
        let expanded = expand(
            args,
            implementation(syn::parse_quote!((i64, VariadicView<'a, i64>))),
        )
        .unwrap()
        .to_string();
        assert!(expanded.contains("ipc-v1") && expanded.contains("VariadicReader"));
    }

    #[test]
    fn checkpoint_is_an_explicit_additive_owned_state_contract() {
        for enabled in [false, true] {
            let args = syn::parse2(quote!(
                arguments = ["bigint"], returns = "bigint", intermediate = "bigint",
                checkpoint = #enabled
            ))
            .unwrap();
            let expanded = expand(args, implementation(syn::parse_quote!((i64,))))
                .unwrap()
                .to_string();
            assert_eq!(expanded.contains("execute_aggregate_bytes"), enabled);
            assert_eq!(expanded.contains("owned-v1"), enabled);
        }
        assert!(syn::parse2::<RowAggregateArgs>(quote!(
            arguments = ["bigint"],
            returns = "bigint",
            intermediate = "bigint",
            checkpoint = true,
            checkpoint = false
        ))
        .is_err());
    }
    #[test]
    fn batch_hooks_opt_in_to_bulk_dispatch_without_changing_value_arguments() {
        let arguments = quote!(
            arguments = ["bigint"],
            returns = "bigint",
            intermediate = "bigint"
        );
        let scalar = expand(
            syn::parse2(arguments.clone()).unwrap(),
            implementation(syn::parse_quote!((i64,))),
        )
        .unwrap()
        .to_string();
        assert!(!scalar.contains("update_batches") && !scalar.contains("merge_batches"));
        let mut bulk = implementation(syn::parse_quote!((i64,)));
        bulk.items.push(syn::parse_quote!(
            fn update_batch() {}
        ));
        bulk.items.push(syn::parse_quote!(
            fn merge_batch() {}
        ));
        let bulk = expand(syn::parse2(arguments).unwrap(), bulk)
            .unwrap()
            .to_string();
        assert!(bulk.contains("update_batches") && bulk.contains("merge_batches"));
        assert!(bulk.contains("prepare_row") && bulk.contains("iter () . copied"));
    }
}
