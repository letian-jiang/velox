/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//! Rust type parameters are SQL variables. The export adapter instantiates the
//! author's function with borrowed Generic values; the original Rust function
//! remains generic and can also be called with concrete SDK value types.
use super::*;
use std::collections::{BTreeMap, BTreeSet};
use syn::fold::Fold;
use syn::{GenericParam, Lifetime, TypeParamBound, WherePredicate};

struct Variable {
    lifetime: Lifetime,
    constraint: &'static str,
}

fn sql_bound(bound: &TypeParamBound) -> syn::Result<Option<(Lifetime, &'static str)>> {
    let TypeParamBound::Trait(bound) = bound else {
        return Ok(None);
    };
    let segment = bound.path.segments.last().unwrap();
    let constraint = match segment.ident.to_string().as_str() {
        "SqlValue" => "any",
        "SqlKnown" => "known",
        "SqlComparable" => "comparable",
        "SqlOrderable" => "orderable",
        _ => return Ok(None),
    };
    let PathArguments::AngleBracketed(args) = &segment.arguments else {
        return Err(syn::Error::new(
            bound.span(),
            "SQL trait requires an explicit input lifetime, e.g. SqlValue<'a>",
        ));
    };
    if bound.lifetimes.is_some() || !matches!(bound.modifier, syn::TraitBoundModifier::None) {
        return Err(syn::Error::new(
            bound.span(),
            "SQL trait must use a lifetime declared on the function",
        ));
    }
    match (args.args.first(), args.args.len()) {
        (Some(GenericArgument::Lifetime(lifetime)), 1) => Ok(Some((lifetime.clone(), constraint))),
        _ => Err(syn::Error::new(
            bound.span(),
            "SQL trait takes exactly one input lifetime",
        )),
    }
}

fn variables(function: &ItemFn) -> syn::Result<BTreeMap<String, Variable>> {
    let mut variables = BTreeMap::new();
    for parameter in &function.sig.generics.params {
        let GenericParam::Type(parameter) = parameter else {
            if matches!(parameter, GenericParam::Const(_)) {
                return Err(syn::Error::new(
                    parameter.span(),
                    "const generic SQL exports are not supported",
                ));
            }
            continue;
        };
        let name = parameter.ident.to_string();
        let mut bounds: Vec<_> = parameter.bounds.iter().collect();
        if let Some(clause) = &function.sig.generics.where_clause {
            for predicate in &clause.predicates {
                if let WherePredicate::Type(predicate) = predicate {
                    if matches!(&predicate.bounded_ty, Type::Path(path) if path.qself.is_none() && path.path.is_ident(&name))
                    {
                        if predicate.lifetimes.is_some() {
                            return Err(syn::Error::new(
                                predicate.span(),
                                "SQL type bounds cannot be higher ranked",
                            ));
                        }
                        bounds.extend(predicate.bounds.iter());
                    }
                }
            }
        }
        let mut variable: Option<Variable> = None;
        let mut known = false;
        let mut comparable = false;
        for bound in bounds {
            if let Some((lifetime, constraint)) = sql_bound(bound)? {
                known |= constraint == "known";
                comparable |= matches!(constraint, "comparable" | "orderable");
                if !function
                    .sig
                    .generics
                    .lifetimes()
                    .any(|p| p.lifetime == lifetime)
                {
                    return Err(syn::Error::new(
                        lifetime.span(),
                        "SQL input lifetime must be declared on the function",
                    ));
                }
                let rank = |c| match c {
                    "any" => 0,
                    "known" => 1,
                    "comparable" => 2,
                    _ => 3,
                };
                match &mut variable {
                    Some(variable) => {
                        if variable.lifetime != lifetime {
                            return Err(syn::Error::new(
                                bound.span(),
                                "one SQL variable cannot borrow from different input lifetimes",
                            ));
                        }
                        if rank(constraint) > rank(variable.constraint) {
                            variable.constraint = constraint;
                        }
                    }
                    None => {
                        variable = Some(Variable {
                            lifetime,
                            constraint,
                        })
                    }
                }
            }
        }
        let mut variable = variable.ok_or_else(|| syn::Error::new(parameter.span(), "SQL type parameter requires SqlValue<'a>, SqlKnown<'a>, SqlComparable<'a> or SqlOrderable<'a>"))?;
        if known && comparable {
            variable.constraint = if variable.constraint == "orderable" {
                "known&orderable"
            } else {
                "known&comparable"
            };
        }
        variables.insert(name, variable);
    }
    Ok(variables)
}

fn sql_type(ty: &Type, variables: &BTreeMap<String, Variable>) -> syn::Result<String> {
    if let Type::Path(path) = ty {
        if path.qself.is_none() && path.path.segments.len() == 1 {
            let segment = &path.path.segments[0];
            if variables.contains_key(&segment.ident.to_string())
                && matches!(segment.arguments, PathArguments::None)
            {
                return Ok(segment.ident.to_string());
            }
        }
    }
    if let Type::Path(path) = ty {
        if path.qself.is_none()
            && path.path.segments.len() == 2
            && path.path.segments[1].ident == "Owned"
        {
            let name = path.path.segments[0].ident.to_string();
            if variables.contains_key(&name) {
                return Ok(name);
            }
        }
    }
    let name = final_ident(ty).unwrap_or_default();
    let types = type_parameters(ty);
    match name.as_str() {
        "Option" | "Result" if !types.is_empty() => sql_type(types[0], variables),
        "ArrayView" | "NullFreeArrayView" | "ArrayWriter" if types.len() == 1 => {
            Ok(format!("array({})", sql_type(types[0], variables)?))
        }
        "MapView" | "NullFreeMapView" | "MapWriter" if types.len() == 2 => Ok(format!(
            "map({},{})",
            sql_type(types[0], variables)?,
            sql_type(types[1], variables)?
        )),
        "RowView" | "NullFreeRowView" | "RowWriter" if types.len() == 1 => {
            let Type::Tuple(fields) = types[0] else {
                return Err(syn::Error::new(
                    ty.span(),
                    "typed row requires a field tuple",
                ));
            };
            let fields = fields
                .elems
                .iter()
                .map(|field| sql_type(field, variables))
                .collect::<syn::Result<Vec<_>>>()?;
            Ok(format!("row({})", fields.join(",")))
        }
        _ => inferred_row_sql_type(ty),
    }
}

struct Substitute<'a> {
    variables: &'a BTreeMap<String, Variable>,
    used: BTreeSet<String>,
    error: Option<syn::Error>,
}
impl Fold for Substitute<'_> {
    fn fold_type(&mut self, ty: Type) -> Type {
        if let Type::Path(path) = &ty {
            if let Some(segment) = path.path.segments.first() {
                let name = segment.ident.to_string();
                if let Some(variable) = self.variables.get(&name) {
                    if path.qself.is_none()
                        && path.path.segments.len() == 1
                        && matches!(segment.arguments, PathArguments::None)
                    {
                        self.used.insert(name);
                        let lifetime = &variable.lifetime;
                        return syn::parse_quote!(::velox_wasm_sdk::Generic<#lifetime>);
                    }
                    if path.qself.is_none()
                        && path.path.segments.len() == 2
                        && path.path.segments[1].ident == "Owned"
                    {
                        return syn::parse_quote!(::velox_wasm_sdk::Value<'static>);
                    }
                    self.error = Some(syn::Error::new(ty.span(), "associated SQL input/output types are not yet supported; use T, T::Owned or a nested SDK view/writer of T"));
                }
            }
        }
        syn::fold::fold_type(self, ty)
    }
}

pub(super) fn expand(
    raw: proc_macro2::TokenStream,
    function: ItemFn,
) -> syn::Result<proc_macro2::TokenStream> {
    if raw
        .clone()
        .into_iter()
        .any(|t| matches!(t, proc_macro2::TokenTree::Ident(key) if key == "instantiations"))
    {
        return expand_concrete(raw, function);
    }
    let variables = variables(&function)?;
    for token in raw.clone() {
        if let proc_macro2::TokenTree::Ident(key) = token {
            if [
                "arguments",
                "returns",
                "variadic",
                "type_variables",
                "integer_variables",
            ]
            .contains(&key.to_string().as_str())
            {
                return Err(syn::Error::new(key.span(), "Rust generic SQL signatures are inferred; put type variables and constraints in Rust trait bounds"));
            }
        }
    }
    let options = if raw.is_empty() {
        quote!(arguments = [])
    } else {
        quote!(arguments = [], #raw)
    };
    let mut args: ArrowScalarArgs = syn::parse2(options)?;
    for input in &function.sig.inputs {
        let FnArg::Typed(input) = input else {
            return Err(syn::Error::new(input.span(), "methods are not supported"));
        };
        let tail = variadic_element(&input.ty)?;
        let ty = LitStr::new(
            &sql_type(tail.unwrap_or(&input.ty), &variables)?,
            input.span(),
        );
        if tail.is_some() {
            args.variadic = Some(ty);
        } else {
            args.arguments.push(ty);
        }
    }
    let ReturnType::Type(_, output) = &function.sig.output else {
        return Err(syn::Error::new(
            function.sig.span(),
            "scalar must return a value",
        ));
    };
    args.returns = Some(LitStr::new(&sql_type(output, &variables)?, output.span()));
    args.type_variables = variables
        .iter()
        .map(|(name, variable)| {
            LitStr::new(
                &format!("{name}:{}", variable.constraint),
                function.sig.span(),
            )
        })
        .collect();
    let name = &function.sig.ident;
    if args.name.is_none() {
        args.name = Some(LitStr::new(&name.to_string(), name.span()));
    }
    if args.export.is_none() {
        args.export = Some(LitStr::new(&name.to_string(), name.span()));
    }
    let mut bridge = function.clone();
    bridge.sig.ident = format_ident!("__velox_sql_bridge_{}", name);
    bridge.vis = syn::Visibility::Inherited;
    bridge
        .attrs
        .retain(|attr| attr.path().is_ident("cfg") || attr.path().is_ident("cfg_attr"));
    bridge.sig.generics.params = bridge
        .sig
        .generics
        .params
        .into_iter()
        .filter(|p| matches!(p, GenericParam::Lifetime(_)))
        .collect();
    if let Some(clause) = &mut bridge.sig.generics.where_clause {
        clause.predicates = clause
            .predicates
            .clone()
            .into_iter()
            .filter(|p| matches!(p, WherePredicate::Lifetime(_)))
            .collect();
        if clause.predicates.is_empty() {
            bridge.sig.generics.where_clause = None;
        }
    }
    let mut substitute = Substitute {
        variables: &variables,
        used: BTreeSet::new(),
        error: None,
    };
    let mut arguments = Vec::new();
    for (index, input) in bridge.sig.inputs.iter_mut().enumerate() {
        let FnArg::Typed(input) = input else {
            unreachable!()
        };
        input.ty = Box::new(substitute.fold_type(*input.ty.clone()));
        let ident = format_ident!("__value_{}", index);
        input.pat = Box::new(syn::parse_quote!(#ident));
        arguments.push(ident);
    }
    for parameter in function.sig.generics.type_params() {
        if !substitute.used.contains(&parameter.ident.to_string()) {
            return Err(syn::Error::new(
                parameter.span(),
                "SQL type variable must occur in a value input so Velox can bind it",
            ));
        }
    }
    bridge.sig.output = substitute.fold_return_type(bridge.sig.output);
    if let Some(error) = substitute.error {
        return Err(error);
    }
    let instances: Vec<Type> = function
        .sig
        .generics
        .type_params()
        .map(|p| {
            let lifetime = &variables[&p.ident.to_string()].lifetime;
            syn::parse_quote!(::velox_wasm_sdk::Generic<#lifetime>)
        })
        .collect();
    bridge.block = Box::new(syn::parse_quote!({ #name::<#(#instances),*>(#(#arguments),*) }));
    let callback_types: Vec<Type> = instances
        .iter()
        .map(|_| syn::parse_quote!(::velox_wasm_sdk::Generic<'_>))
        .collect();
    instantiate_callbacks(&mut args, &callback_types)?;
    let adapter = expand_row_scalar(args, bridge)?;
    Ok(quote!(#function #adapter))
}

// Alternate readers infer view shapes from callback signatures. Explicit Rust
// type instances keep generic callback arguments unambiguous without forcing the
// primary reader shape onto an ASCII or null-free callback.
fn instantiate_callbacks(args: &mut ArrowScalarArgs, types: &[Type]) -> syn::Result<()> {
    for callback in [&mut args.call_ascii, &mut args.call_null_free]
        .into_iter()
        .flatten()
    {
        let mut path: syn::Path = syn::parse_str(&callback.value())?;
        let segment = path.segments.last_mut().unwrap();
        if !matches!(segment.arguments, PathArguments::None) {
            return Err(syn::Error::new(callback.span(), "Rust generic alternate callbacks are instantiated automatically; use a bare function path with matching type parameters"));
        }
        segment.arguments = PathArguments::AngleBracketed(syn::parse_quote!(::<#(#types),*>));
        *callback = LitStr::new(&quote!(#path).to_string(), callback.span());
    }
    Ok(())
}

struct ConcreteOptions {
    instances: Vec<Type>,
    options: proc_macro2::TokenStream,
}
impl syn::parse::Parse for ConcreteOptions {
    fn parse(input: syn::parse::ParseStream<'_>) -> syn::Result<Self> {
        let mut instances = None;
        let mut options = proc_macro2::TokenStream::new();
        while !input.is_empty() {
            let key: syn::Ident = input.parse()?;
            input.parse::<syn::Token![=]>()?;
            if key == "instantiations" {
                if instances.is_some() {
                    return Err(syn::Error::new(key.span(), "duplicate instantiations"));
                }
                let content;
                syn::bracketed!(content in input);
                instances = Some(
                    content
                        .parse_terminated(|input| input.parse::<Type>(), syn::Token![,])?
                        .into_iter()
                        .collect::<Vec<_>>(),
                );
            } else {
                if [
                    "arguments",
                    "returns",
                    "variadic",
                    "type_variables",
                    "integer_variables",
                ]
                .contains(&key.to_string().as_str())
                {
                    return Err(syn::Error::new(
                        key.span(),
                        "instantiated SQL signatures are inferred from concrete Rust types",
                    ));
                }
                let value: syn::Expr = input.parse()?;
                options.extend(quote!(#key = #value,));
            }
            if !input.is_empty() {
                input.parse::<syn::Token![,]>()?;
            }
        }
        let instances = instances.ok_or_else(|| input.error("missing instantiations"))?;
        if instances.is_empty() {
            return Err(input.error("instantiations must not be empty"));
        }
        Ok(Self { instances, options })
    }
}
struct ConcreteSubstitute<'a>(&'a BTreeMap<String, Type>);
impl Fold for ConcreteSubstitute<'_> {
    fn fold_type(&mut self, ty: Type) -> Type {
        if let Type::Path(path) = &ty {
            if path.qself.is_none() && path.path.segments.len() == 1 {
                let segment = &path.path.segments[0];
                if matches!(segment.arguments, PathArguments::None) {
                    if let Some(ty) = self.0.get(&segment.ident.to_string()) {
                        return ty.clone();
                    }
                }
            }
        }
        syn::fold::fold_type(self, ty)
    }
}
fn expand_concrete(
    raw: proc_macro2::TokenStream,
    function: ItemFn,
) -> syn::Result<proc_macro2::TokenStream> {
    if let Some(parameter) = function.sig.generics.const_params().next() {
        return Err(syn::Error::new(
            parameter.span(),
            "const generic SQL exports are not supported",
        ));
    }
    let options: ConcreteOptions = syn::parse2(raw)?;
    let name = &function.sig.ident;
    let parameters: Vec<_> = function
        .sig
        .generics
        .type_params()
        .map(|p| p.ident.to_string())
        .collect();
    let mut adapters = proc_macro2::TokenStream::new();
    let mut seen = BTreeSet::new();
    for (index, instance) in options.instances.iter().enumerate() {
        let types: Vec<Type> = if parameters.len() == 1 {
            vec![instance.clone()]
        } else if let Type::Tuple(tuple) = instance {
            tuple.elems.iter().cloned().collect()
        } else {
            return Err(syn::Error::new(
                instance.span(),
                "use a tuple of concrete types for multiple Rust type parameters",
            ));
        };
        if types.len() != parameters.len() {
            return Err(syn::Error::new(
                instance.span(),
                "instantiation arity must match the Rust type parameters",
            ));
        }
        let replacements = parameters
            .iter()
            .cloned()
            .zip(types.iter().cloned())
            .collect();
        let mut substitute = ConcreteSubstitute(&replacements);
        let mut bridge = function.clone();
        bridge.sig.ident = format_ident!("__velox_sql_instance_{}_{}", name, index);
        bridge.vis = syn::Visibility::Inherited;
        bridge
            .attrs
            .retain(|attr| attr.path().is_ident("cfg") || attr.path().is_ident("cfg_attr"));
        bridge.sig.generics.params = bridge
            .sig
            .generics
            .params
            .into_iter()
            .filter(|p| matches!(p, GenericParam::Lifetime(_)))
            .collect();
        if let Some(clause) = &mut bridge.sig.generics.where_clause {
            clause.predicates = clause
                .predicates
                .clone()
                .into_iter()
                .filter(|p| matches!(p, WherePredicate::Lifetime(_)))
                .collect();
            if clause.predicates.is_empty() {
                bridge.sig.generics.where_clause = None;
            }
        }
        let mut arguments = Vec::new();
        for (i, input) in bridge.sig.inputs.iter_mut().enumerate() {
            let FnArg::Typed(input) = input else {
                return Err(syn::Error::new(input.span(), "methods are not supported"));
            };
            input.ty = Box::new(substitute.fold_type(*input.ty.clone()));
            let ident = format_ident!("__value_{}", i);
            input.pat = Box::new(syn::parse_quote!(#ident));
            arguments.push(ident);
        }
        bridge.sig.output = substitute.fold_return_type(bridge.sig.output);
        bridge.block = Box::new(syn::parse_quote!({ #name::<#(#types),*>(#(#arguments),*) }));
        let inferred = infer_row_signature(syn::parse2(quote!())?, &bridge)?;
        let signature = (
            inferred
                .arguments
                .iter()
                .map(LitStr::value)
                .collect::<Vec<_>>(),
            inferred.variadic.as_ref().map(LitStr::value),
        );
        if !seen.insert(signature) {
            return Err(syn::Error::new(
                instance.span(),
                "duplicate SQL overload in instantiations",
            ));
        }
        let rest = &options.options;
        let mut args: ArrowScalarArgs = syn::parse2(quote!(arguments = [], #rest))?;
        args.arguments = inferred.arguments;
        args.variadic = inferred.variadic;
        args.returns = inferred.returns;
        if args.name.is_none() {
            args.name = Some(LitStr::new(&name.to_string(), name.span()));
        }
        let export = args
            .export
            .as_ref()
            .map(LitStr::value)
            .unwrap_or_else(|| name.to_string());
        args.export = Some(LitStr::new(
            &format!("{export}__instance_{index}"),
            name.span(),
        ));
        instantiate_callbacks(&mut args, &types)?;
        adapters.extend(expand_row_scalar(args, bridge)?);
    }
    Ok(quote!(#function #adapters))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn metadata(options: proc_macro2::TokenStream, function: ItemFn) -> Vec<Value> {
        let file: syn::File = syn::parse2(expand(options, function).unwrap()).unwrap();
        file.items
            .into_iter()
            .filter_map(|item| {
                let syn::Item::Static(item) = item else {
                    return None;
                };
                let syn::Expr::Unary(expr) = *item.expr else {
                    return None;
                };
                let syn::Expr::Lit(expr) = *expr.expr else {
                    return None;
                };
                let syn::Lit::ByteStr(bytes) = expr.lit else {
                    return None;
                };
                let bytes = bytes.value();
                Some(serde_json::from_slice(&bytes[..bytes.len() - 1]).unwrap())
            })
            .collect()
    }
    #[test]
    fn infers_variables_constraints_and_default_null_behavior() {
        let records = metadata(
            quote!(),
            syn::parse_quote!(
                pub fn identity<'a, T: SqlValue<'a>>(value: Option<T>) -> Option<T> {
                    value
                }
            ),
        );
        let m = &records[0];
        assert_eq!(m["name"], "identity");
        assert_eq!(m["entrypoint"], "identity");
        assert_eq!(m["arguments"][0]["type"], "T");
        assert_eq!(m["return"]["type"], "T");
        assert_eq!(m["type_variables"], json!({"T":"any"}));
        assert_eq!(m["default_null_behavior"], false);
        let records = metadata(
            quote!(name = "ordered", export = "compare_export"),
            syn::parse_quote!(
                fn compare<'a, T>(left: T, right: T) -> bool
                where
                    T: SqlComparable<'a> + SqlOrderable<'a>,
                {
                    false
                }
            ),
        );
        assert_eq!(records[0]["type_variables"], json!({"T":"orderable"}));
        assert_eq!(records[0]["default_null_behavior"], true);
        assert_eq!(records[0]["name"], "ordered");
        assert_eq!(records[0]["entrypoint"], "compare_export");
    }
    #[test]
    fn infers_nested_relations_and_multiple_where_parameters() {
        let records = metadata(
            quote!(),
            syn::parse_quote!(
                fn lookup<'a, K, V>(map: MapView<'a, K, V>, key: K) -> Result<Option<V>, String>
                where
                    K: SqlComparable<'a>,
                    V: SqlValue<'a>,
                {
                    unreachable!()
                }
            ),
        );
        assert_eq!(records[0]["arguments"][0]["type"], "map(K,V)");
        assert_eq!(records[0]["arguments"][1]["type"], "K");
        assert_eq!(records[0]["return"]["type"], "V");
        assert_eq!(
            records[0]["type_variables"],
            json!({"K":"comparable", "V":"any"})
        );
        let records = metadata(
            quote!(),
            syn::parse_quote!(
                fn row<'a, T: SqlValue<'a>, U: SqlValue<'a>>(
                    v: RowView<'a, (T, ArrayView<'a, U>)>,
                ) -> ArrayWriter<U> {
                    unreachable!()
                }
            ),
        );
        assert_eq!(records[0]["arguments"][0]["type"], "row(T,array(U))");
        assert_eq!(records[0]["return"]["type"], "array(U)");
    }
    #[test]
    fn infers_owned_result_type_relation() {
        let records = metadata(
            quote!(),
            syn::parse_quote!(
                fn own<'a, T: SqlValue<'a>>(v: T) -> Result<T::Owned, String> {
                    v.sql_owned()
                }
            ),
        );
        assert_eq!(records[0]["return"]["type"], "T");
        assert_eq!(records[0]["type_variables"], json!({"T":"any"}));
    }

    #[test]
    fn infers_variadic_and_preserves_initialization_options() {
        let records = metadata(
            quote!(
                constant_arguments = [0],
                initialize = "init",
                config_keys = ["timezone"]
            ),
            syn::parse_quote!(
                fn first<'a, T: SqlValue<'a>>(
                    first: Option<T>,
                    rest: VariadicView<'a, Option<T>>,
                ) -> Option<T> {
                    first
                }
            ),
        );
        assert_eq!(records[0]["arguments"].as_array().unwrap().len(), 2);
        assert_eq!(records[0]["arguments"][1]["type"], "T");
        assert_eq!(records[0]["variable_arity"], true);
        assert_eq!(records[0]["arguments"][0]["constant"], true);
        assert!(records[0].get("initialize").is_some());
    }
    #[test]
    fn preserves_known_and_comparable_intersections() {
        for (bound, expected) in [
            (quote!(SqlComparable<'a>), "known&comparable"),
            (quote!(SqlOrderable<'a>), "known&orderable"),
        ] {
            let records = metadata(
                quote!(),
                syn::parse_quote!(fn f<'a, T: SqlKnown<'a> + #bound>(v:T)->T {v}),
            );
            assert_eq!(records[0]["type_variables"]["T"], expected);
        }
    }

    #[test]
    fn rejects_unbound_or_unsupported_generic_signatures() {
        for function in [
            syn::parse_quote!(
                fn f<T>(v: T) -> T {
                    v
                }
            ),
            syn::parse_quote!(
                fn f<'a, T: SqlValue<'a>>() -> T {
                    unreachable!()
                }
            ),
            syn::parse_quote!(
                fn f<'a, T: SqlValue<'a>, const N: usize>(v: T) -> T {
                    v
                }
            ),
            syn::parse_quote!(
                async fn f<'a, T: SqlValue<'a>>(v: T) -> T {
                    v
                }
            ),
            syn::parse_quote!(
                fn f<'a, 'b, T: SqlValue<'a> + SqlValue<'b>>(v: T) -> T {
                    v
                }
            ),
            syn::parse_quote!(
                fn f<'a, T: SqlValue<'static>>(v: T) -> T {
                    v
                }
            ),
            syn::parse_quote!(
                fn f<T: for<'a> SqlValue<'a>>(v: T) -> T {
                    v
                }
            ),
            syn::parse_quote!(
                fn f<'a, T: SqlValue<'a>>(v: T) -> T::UnsupportedAssociated {
                    unreachable!()
                }
            ),
        ] {
            assert!(expand(quote!(), function).is_err());
        }
        let f: ItemFn = syn::parse_quote!(
            fn f<'a, T: SqlValue<'a>>(v: T) -> T {
                v
            }
        );
        assert!(expand(quote!(arguments = ["T"]), f).is_err());
    }
    #[test]
    fn emits_concrete_overloads_from_ordinary_rust_bounds() {
        let records = metadata(
            quote!(instantiations = [i64, f64]),
            syn::parse_quote!(
                fn add<T: std::ops::Add<Output = T>>(a: T, b: T) -> T {
                    a + b
                }
            ),
        );
        assert_eq!(records.len(), 2);
        assert_eq!(records[0]["name"], "add");
        assert_eq!(records[1]["name"], "add");
        assert_eq!(records[0]["arguments"][0]["type"], "bigint");
        assert_eq!(records[1]["arguments"][0]["type"], "double");
        assert_ne!(records[0]["entrypoint"], records[1]["entrypoint"]);
        assert_eq!(records[0]["type_variables"], json!({}));
        let records = metadata(
            quote!(instantiations = [(i64, f64), (f64, i64)]),
            syn::parse_quote!(
                fn pick<T, U>(a: T, _b: U) -> T {
                    a
                }
            ),
        );
        assert_eq!(records[0]["arguments"][1]["type"], "double");
        assert_eq!(records[1]["return"]["type"], "double");
    }
    #[test]
    fn rejects_empty_duplicate_and_wrong_arity_instantiations() {
        let f: ItemFn = syn::parse_quote!(
            fn identity<T>(v: T) -> T {
                v
            }
        );
        for args in [
            quote!(instantiations = []),
            quote!(instantiations = [i64, i64]),
            quote!(instantiations = [i64], instantiations = [f64]),
            quote!(instantiations = [i64], returns = "bigint"),
        ] {
            assert!(expand(args, f.clone()).is_err());
        }
        let f: ItemFn = syn::parse_quote!(
            fn f<T, U>(t: T, u: U) -> T {
                t
            }
        );
        assert!(expand(quote!(instantiations = [i64]), f.clone()).is_err());
        assert!(expand(quote!(instantiations = [(i64,)]), f).is_err());
    }
}
