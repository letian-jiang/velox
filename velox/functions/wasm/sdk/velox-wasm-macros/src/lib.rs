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

use heck::ToSnakeCase;
use proc_macro::TokenStream;
use quote::{format_ident, quote};
use serde_json::{json, Value};
use syn::{
    parse_macro_input, spanned::Spanned, FnArg, GenericArgument, ImplItem, ItemFn, ItemImpl,
    LitByteStr, LitStr, PathArguments, ReturnType, Type,
};

fn sql_type(ty: &ParsedType) -> Value {
    let name = match ty.scalar {
        ScalarType::Bool => "boolean",
        ScalarType::I8 => "tinyint",
        ScalarType::I16 => "smallint",
        ScalarType::I32 => "integer",
        ScalarType::I64 => "bigint",
        ScalarType::F32 => "real",
        ScalarType::F64 => "double",
        ScalarType::String => "varchar",
        ScalarType::Binary => "varbinary",
        ScalarType::Date32 => "date",
        ScalarType::Timestamp => "timestamp",
        ScalarType::Complex => "complex",
    };
    json!({"type": name, "nullable": ty.nullable})
}

fn metadata_section(name: syn::Ident, value: Value) -> proc_macro2::TokenStream {
    let mut bytes = serde_json::to_vec(&value).expect("metadata must serialize");
    bytes.push(0);
    let length = bytes.len();
    let literal = LitByteStr::new(&bytes, proc_macro2::Span::call_site());
    quote! {
        #[cfg(target_arch = "wasm32")]
        #[used]
        #[link_section = "velox.udf.v1"]
        static #name: [u8; #length] = *#literal;
    }
}

struct ScalarArgs {
    export: Option<LitStr>,
    name: Option<LitStr>,
    deterministic: bool,
    default_null_behavior: Option<bool>,
}

impl syn::parse::Parse for ScalarArgs {
    fn parse(input: syn::parse::ParseStream<'_>) -> syn::Result<Self> {
        let mut export = None;
        let mut sql_name = None;
        let mut deterministic = true;
        let mut default_null_behavior = None;
        while !input.is_empty() {
            let key: syn::Ident = input.parse()?;
            input.parse::<syn::Token![=]>()?;
            match key.to_string().as_str() {
                "export" => export = Some(input.parse()?),
                "name" => sql_name = Some(input.parse()?),
                "deterministic" => deterministic = input.parse::<syn::LitBool>()?.value,
                "default_null_behavior" => {
                    default_null_behavior = Some(input.parse::<syn::LitBool>()?.value)
                }
                _ => return Err(syn::Error::new(key.span(), "unknown scalar option")),
            }
            if input.peek(syn::Token![,]) {
                input.parse::<syn::Token![,]>()?;
            } else if !input.is_empty() {
                return Err(input.error("expected `,` between scalar options"));
            }
        }
        Ok(Self {
            export,
            name: sql_name,
            deterministic,
            default_null_behavior,
        })
    }
}

#[derive(Clone)]
enum ScalarType {
    Bool,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    String,
    Binary,
    Date32,
    Timestamp,
    Complex,
}

struct ParsedType {
    scalar: ScalarType,
    nullable: bool,
    borrowed: bool,
}

fn final_ident(ty: &Type) -> Option<String> {
    match ty {
        Type::Path(path) => path
            .path
            .segments
            .last()
            .map(|segment| segment.ident.to_string()),
        Type::Reference(reference) => final_ident(&reference.elem),
        Type::Slice(slice) => final_ident(&slice.elem).map(|name| format!("[{name}]")),
        _ => None,
    }
}

fn parse_type(ty: &Type) -> syn::Result<ParsedType> {
    if let Type::Reference(reference) = ty {
        let mut parsed = parse_type(&reference.elem)?;
        match parsed.scalar {
            ScalarType::String | ScalarType::Binary => parsed.borrowed = true,
            _ => {
                return Err(syn::Error::new(
                    ty.span(),
                    "only string and binary scalar arguments may be borrowed",
                ))
            }
        }
        return Ok(parsed);
    }
    if let Type::Path(path) = ty {
        if let Some(segment) = path.path.segments.last() {
            if segment.ident == "Option" {
                if let PathArguments::AngleBracketed(arguments) = &segment.arguments {
                    if let Some(GenericArgument::Type(inner)) = arguments.args.first() {
                        let mut parsed = parse_type(inner)?;
                        parsed.nullable = true;
                        return Ok(parsed);
                    }
                }
                return Err(syn::Error::new(
                    ty.span(),
                    "Option must contain one scalar type",
                ));
            }
        }
    }
    let name = final_ident(ty)
        .ok_or_else(|| syn::Error::new(ty.span(), "unsupported scalar type in #[velox_scalar]"))?;
    let scalar = match name.as_str() {
        "bool" => ScalarType::Bool,
        "i8" => ScalarType::I8,
        "i16" => ScalarType::I16,
        "i32" => ScalarType::I32,
        "i64" => ScalarType::I64,
        "f32" => ScalarType::F32,
        "f64" => ScalarType::F64,
        "String" | "str" => ScalarType::String,
        "Vec" | "[u8]" => ScalarType::Binary,
        "Date32" => ScalarType::Date32,
        "Timestamp" => ScalarType::Timestamp,
        "ArrayRef" => ScalarType::Complex,
        _ => {
            return Err(syn::Error::new(
                ty.span(),
                format!("unsupported scalar type `{name}` in #[velox_scalar]"),
            ))
        }
    };
    Ok(ParsedType {
        scalar,
        nullable: false,
        borrowed: false,
    })
}

fn parse_scalar_output(ty: &Type) -> syn::Result<(ParsedType, bool)> {
    if let Type::Path(path) = ty {
        if let Some(segment) = path.path.segments.last() {
            if segment.ident == "Result" {
                let PathArguments::AngleBracketed(arguments) = &segment.arguments else {
                    return Err(syn::Error::new(
                        ty.span(),
                        "Result must contain an output and an error type",
                    ));
                };
                let types: Vec<_> = arguments
                    .args
                    .iter()
                    .filter_map(|argument| match argument {
                        GenericArgument::Type(ty) => Some(ty),
                        _ => None,
                    })
                    .collect();
                if types.len() != 2 {
                    return Err(syn::Error::new(
                        ty.span(),
                        "Result must contain an output and an error type",
                    ));
                }
                return Ok((parse_type(types[0])?, true));
            }
        }
    }
    Ok((parse_type(ty)?, false))
}

fn array_type(scalar: &ScalarType) -> proc_macro2::TokenStream {
    if matches!(scalar, ScalarType::Complex) {
        return quote!(());
    }
    let ident = format_ident!(
        "{}Array",
        match scalar {
            ScalarType::Bool => "Boolean",
            ScalarType::I8 => "Int8",
            ScalarType::I16 => "Int16",
            ScalarType::I32 => "Int32",
            ScalarType::I64 => "Int64",
            ScalarType::F32 => "Float32",
            ScalarType::F64 => "Float64",
            ScalarType::String => "String",
            ScalarType::Binary => "Binary",
            ScalarType::Date32 => "Date32",
            ScalarType::Timestamp => "TimestampNanosecond",
            ScalarType::Complex => unreachable!(),
        }
    );
    quote!(::velox_wasm_sdk::arrow_array::#ident)
}

fn builder_type(scalar: &ScalarType) -> proc_macro2::TokenStream {
    if matches!(scalar, ScalarType::Complex) {
        return quote!(());
    }
    let ident = format_ident!(
        "{}Builder",
        match scalar {
            ScalarType::Bool => "Boolean",
            ScalarType::I8 => "Int8",
            ScalarType::I16 => "Int16",
            ScalarType::I32 => "Int32",
            ScalarType::I64 => "Int64",
            ScalarType::F32 => "Float32",
            ScalarType::F64 => "Float64",
            ScalarType::String => "String",
            ScalarType::Binary => "Binary",
            ScalarType::Date32 => "Date32",
            ScalarType::Timestamp => "TimestampNanosecond",
            ScalarType::Complex => unreachable!(),
        }
    );
    quote!(::velox_wasm_sdk::arrow_array::builder::#ident)
}

fn builder_init(scalar: &ScalarType) -> proc_macro2::TokenStream {
    let builder = builder_type(scalar);
    match scalar {
        ScalarType::String | ScalarType::Binary => {
            quote!(#builder::with_capacity(__batch.num_rows(), __batch.num_rows() * 8))
        }
        _ => quote!(#builder::with_capacity(__batch.num_rows())),
    }
}

fn data_type(scalar: &ScalarType) -> proc_macro2::TokenStream {
    if matches!(scalar, ScalarType::Complex) {
        return quote!(::velox_wasm_sdk::arrow_schema::DataType::Null);
    }
    let variant = format_ident!(
        "{}",
        match scalar {
            ScalarType::Bool => "Boolean",
            ScalarType::I8 => "Int8",
            ScalarType::I16 => "Int16",
            ScalarType::I32 => "Int32",
            ScalarType::I64 => "Int64",
            ScalarType::F32 => "Float32",
            ScalarType::F64 => "Float64",
            ScalarType::String => "Utf8",
            ScalarType::Binary => "Binary",
            ScalarType::Date32 => "Date32",
            ScalarType::Timestamp => "Timestamp",
            ScalarType::Complex => unreachable!(),
        }
    );
    match scalar {
        ScalarType::Timestamp => quote!(::velox_wasm_sdk::arrow_schema::DataType::Timestamp(
            ::velox_wasm_sdk::arrow_schema::TimeUnit::Nanosecond,
            None,
        )),
        _ => quote!(::velox_wasm_sdk::arrow_schema::DataType::#variant),
    }
}

fn input_value(parsed: &ParsedType, array: &syn::Ident) -> proc_macro2::TokenStream {
    match (&parsed.scalar, parsed.borrowed) {
        (ScalarType::String, false) => quote!(#array.value(__row).to_owned()),
        (ScalarType::Binary, false) => quote!(#array.value(__row).to_vec()),
        (ScalarType::Date32, _) => {
            quote!(::velox_wasm_sdk::Date32(#array.value(__row)))
        }
        (ScalarType::Timestamp, _) => {
            quote!(::velox_wasm_sdk::Timestamp(#array.value(__row)))
        }
        (ScalarType::Complex, _) => quote!(#array.slice(__row, 1)),
        _ => quote!(#array.value(__row)),
    }
}

fn output_value(scalar: &ScalarType) -> proc_macro2::TokenStream {
    match scalar {
        ScalarType::Date32 | ScalarType::Timestamp => quote!(value.0),
        _ => quote!(value),
    }
}

#[proc_macro_attribute]
pub fn velox_scalar(args: TokenStream, item: TokenStream) -> TokenStream {
    let args = parse_macro_input!(args as ScalarArgs);
    let function = parse_macro_input!(item as ItemFn);
    match expand_scalar(args, function) {
        Ok(tokens) => tokens.into(),
        Err(error) => error.into_compile_error().into(),
    }
}

fn expand_scalar(args: ScalarArgs, function: ItemFn) -> syn::Result<proc_macro2::TokenStream> {
    if !function.sig.generics.params.is_empty() || function.sig.asyncness.is_some() {
        return Err(syn::Error::new(
            function.sig.span(),
            "#[velox_scalar] functions cannot be generic or async",
        ));
    }
    let function_name = &function.sig.ident;
    let wrapper_name = format_ident!("__velox_wasm_batch_{}", function_name);
    let export = args
        .export
        .unwrap_or_else(|| LitStr::new(&function_name.to_string(), function_name.span()));
    let deterministic = args.deterministic;
    let default_null_behavior = args.default_null_behavior;
    let sql_name = args
        .name
        .map(|name| name.value())
        .unwrap_or_else(|| function_name.to_string());

    let mut parsed_inputs = Vec::new();
    for input in &function.sig.inputs {
        let FnArg::Typed(argument) = input else {
            return Err(syn::Error::new(input.span(), "methods are not supported"));
        };
        parsed_inputs.push(parse_type(&argument.ty)?);
    }
    let (output_type, fallible) = match &function.sig.output {
        ReturnType::Type(_, ty) => parse_scalar_output(ty)?,
        ReturnType::Default => {
            return Err(syn::Error::new(
                function.sig.output.span(),
                "#[velox_scalar] functions must return a supported scalar type",
            ))
        }
    };
    if parsed_inputs
        .iter()
        .any(|input| matches!(input.scalar, ScalarType::Complex))
        || matches!(output_type.scalar, ScalarType::Complex)
    {
        return Err(syn::Error::new(
            function.sig.span(),
            "use #[velox_arrow_scalar] for complex scalar types",
        ));
    }
    if output_type.borrowed {
        return Err(syn::Error::new(
            function.sig.output.span(),
            "#[velox_scalar] string and binary results must be owned",
        ));
    }

    let arrays: Vec<_> = (0..parsed_inputs.len())
        .map(|index| format_ident!("__arg_array_{index}"))
        .collect();
    let array_bindings =
        parsed_inputs
            .iter()
            .zip(&arrays)
            .enumerate()
            .map(|(index, (parsed, array))| {
                let ty = array_type(&parsed.scalar);
                let expected = match parsed.scalar {
                    ScalarType::String => "Utf8",
                    ScalarType::Binary => "Binary",
                    _ => "primitive",
                };
                quote! {
                    let #array = __batch.column(#index).as_any().downcast_ref::<#ty>()
                        .ok_or_else(|| ::velox_wasm_sdk::runtime::type_error(#index, #expected))?;
                }
            });
    let call_arguments =
        parsed_inputs
            .iter()
            .zip(&arrays)
            .enumerate()
            .map(|(index, (parsed, array))| {
                let value = input_value(parsed, array);
                if parsed.nullable {
                    quote! {
                        if ::velox_wasm_sdk::arrow_array::Array::is_null(#array, __row) {
                            None
                        } else {
                            Some(#value)
                        }
                    }
                } else {
                    quote! {{
                        if ::velox_wasm_sdk::arrow_array::Array::is_null(#array, __row) {
                            return Err(::velox_wasm_sdk::runtime::null_error(__row, #index));
                        }
                        #value
                    }}
                }
            });
    let builder = builder_init(&output_type.scalar);
    let output_value = output_value(&output_type.scalar);
    let append = if output_type.nullable {
        quote! {
            match __value {
                Some(value) => __builder.append_value(#output_value),
                None => __builder.append_null(),
            }
        }
    } else {
        match output_type.scalar {
            ScalarType::Date32 | ScalarType::Timestamp => {
                quote!(__builder.append_value(__value.0);)
            }
            _ => quote!(__builder.append_value(__value);),
        }
    };
    let arrow_data_type = data_type(&output_type.scalar);
    let output_nullable = output_type.nullable;
    let input_count = parsed_inputs.len();
    let evaluate_row = if fallible {
        quote! {
            match #function_name(#(#call_arguments),*) {
                Ok(__value) => {
                    #append
                    __error_builder.append_null();
                }
                Err(__error) => {
                    __builder.append_null();
                    __error_builder.append_value(__error.to_string());
                }
            }
        }
    } else {
        quote! {
            let __value = #function_name(#(#call_arguments),*);
            #append
        }
    };
    let error_builder = fallible.then(|| {
        quote! {
            let mut __error_builder =
                ::velox_wasm_sdk::arrow_array::builder::StringBuilder::new();
        }
    });
    let encode_output = if fallible {
        quote! {
            let __error_field = ::velox_wasm_sdk::arrow_schema::Field::new(
                "__velox_wasm_error",
                ::velox_wasm_sdk::arrow_schema::DataType::Utf8,
                true,
            );
            ::velox_wasm_sdk::runtime::encode_batch(
                vec![__field, __error_field],
                vec![
                    std::sync::Arc::new(__builder.finish())
                        as ::velox_wasm_sdk::arrow_array::ArrayRef,
                    std::sync::Arc::new(__error_builder.finish())
                        as ::velox_wasm_sdk::arrow_array::ArrayRef,
                ],
            )
        }
    } else {
        quote! {
            ::velox_wasm_sdk::runtime::encode_output(
                __field,
                std::sync::Arc::new(__builder.finish()),
            )
        }
    };
    let result_nullable = output_nullable || fallible;
    let declaration = metadata_section(
        format_ident!(
            "__VELOX_WASM_METADATA_{}",
            function_name.to_string().to_uppercase()
        ),
        json!({
            "abi_version": 1,
            "kind": "scalar",
            "name": sql_name,
            "entrypoint": export.value(),
            "arguments": parsed_inputs.iter().map(sql_type).collect::<Vec<_>>(),
            "return": sql_type(&output_type),
            "deterministic": deterministic,
            "default_null_behavior": default_null_behavior
                .unwrap_or_else(|| !parsed_inputs.iter().any(|input| input.nullable)),
        }),
    );

    Ok(quote! {
        #function
        #declaration

        #[cfg(target_arch = "wasm32")]
        #[export_name = #export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #wrapper_name(
            __pointer: u32,
            __length: u32,
        ) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __batch.num_columns() != #input_count {
                    return Err(format!(
                        "expected {} arguments, received {}",
                        #input_count,
                        __batch.num_columns(),
                    ));
                }
                #(#array_bindings)*
                let mut __builder = #builder;
                #error_builder
                for __row in 0..__batch.num_rows() {
                    #evaluate_row
                }
                let __field = ::velox_wasm_sdk::arrow_schema::Field::new(
                    "result",
                    #arrow_data_type,
                    #result_nullable,
                );
                #encode_output
            })
        }
    })
}

struct ArrowScalarArgs {
    arguments: Vec<LitStr>,
    returns: Option<LitStr>,
    name: Option<LitStr>,
    deterministic: bool,
    default_null_behavior: bool,
}

impl syn::parse::Parse for ArrowScalarArgs {
    fn parse(input: syn::parse::ParseStream<'_>) -> syn::Result<Self> {
        let mut arguments = None;
        let mut returns = None;
        let mut name = None;
        let mut deterministic = true;
        let mut default_null_behavior = false;
        while !input.is_empty() {
            let key: syn::Ident = input.parse()?;
            input.parse::<syn::Token![=]>()?;
            match key.to_string().as_str() {
                "arguments" => {
                    let content;
                    syn::bracketed!(content in input);
                    let values = content.parse_terminated(|input| input.parse(), syn::Token![,])?;
                    arguments = Some(values.into_iter().collect());
                }
                "returns" => returns = Some(input.parse()?),
                "name" => name = Some(input.parse()?),
                "deterministic" => deterministic = input.parse::<syn::LitBool>()?.value,
                "default_null_behavior" => {
                    default_null_behavior = input.parse::<syn::LitBool>()?.value
                }
                _ => return Err(syn::Error::new(key.span(), "unknown Arrow scalar option")),
            }
            if !input.is_empty() {
                input.parse::<syn::Token![,]>()?;
            }
        }
        Ok(Self {
            arguments: arguments.ok_or_else(|| input.error("missing `arguments`"))?,
            returns,
            name,
            deterministic,
            default_null_behavior,
        })
    }
}

/// A batch-oriented escape hatch for nested Arrow types. The user function
/// receives one compact RecordBatch and returns one Arrow array of equal length.
#[proc_macro_attribute]
pub fn velox_arrow_scalar(args: TokenStream, item: TokenStream) -> TokenStream {
    let args = parse_macro_input!(args as ArrowScalarArgs);
    let function = parse_macro_input!(item as ItemFn);
    match expand_arrow_scalar(args, function) {
        Ok(tokens) => tokens.into(),
        Err(error) => error.into_compile_error().into(),
    }
}

fn expand_arrow_scalar(
    args: ArrowScalarArgs,
    function: ItemFn,
) -> syn::Result<proc_macro2::TokenStream> {
    if function.sig.inputs.len() != 1 || !function.sig.generics.params.is_empty() {
        return Err(syn::Error::new(
            function.sig.span(),
            "#[velox_arrow_scalar] requires one RecordBatch argument and no generics",
        ));
    }
    let returns = args
        .returns
        .ok_or_else(|| syn::Error::new(function.sig.span(), "missing `returns`"))?;
    let function_name = &function.sig.ident;
    let export = LitStr::new(&function_name.to_string(), function_name.span());
    let wrapper_name = format_ident!("__velox_wasm_arrow_batch_{}", function_name);
    let sql_name = args
        .name
        .map(|value| value.value())
        .unwrap_or_else(|| function_name.to_string());
    let input_count = args.arguments.len();
    let declaration = metadata_section(
        format_ident!(
            "__VELOX_WASM_METADATA_{}",
            function_name.to_string().to_uppercase()
        ),
        json!({
            "abi_version": 1,
            "kind": "scalar",
            "name": sql_name,
            "entrypoint": export.value(),
            "arguments": args.arguments.iter().map(|value| json!({
                "type": value.value(), "nullable": true
            })).collect::<Vec<_>>(),
            "return": {"type": returns.value(), "nullable": true},
            "deterministic": args.deterministic,
            "default_null_behavior": args.default_null_behavior,
        }),
    );
    Ok(quote! {
        #function
        #declaration

        #[cfg(target_arch = "wasm32")]
        #[export_name = #export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #wrapper_name(
            __pointer: u32,
            __length: u32,
        ) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __batch.num_columns() != #input_count {
                    return Err(format!(
                        "expected {} arguments, received {}",
                        #input_count,
                        __batch.num_columns(),
                    ));
                }
                let __row_count = __batch.num_rows();
                let __result = #function_name(&__batch).map_err(|error| error.to_string())?;
                if __result.len() != __row_count {
                    return Err(format!(
                        "expected {} result rows, received {}",
                        __row_count,
                        __result.len(),
                    ));
                }
                ::velox_wasm_sdk::runtime::encode_output(
                    ::velox_wasm_sdk::arrow_schema::Field::new(
                        "result", __result.data_type().clone(), true,
                    ),
                    __result,
                )
            })
        }
    })
}

struct AggregateArgs {
    prefix: Option<LitStr>,
    argument_types: Option<Vec<LitStr>>,
    return_type: Option<LitStr>,
    ignore_duplicates: bool,
    order_sensitive: bool,
    default_null_behavior: bool,
}

impl syn::parse::Parse for AggregateArgs {
    fn parse(input: syn::parse::ParseStream<'_>) -> syn::Result<Self> {
        let mut prefix = None;
        let mut argument_types = None;
        let mut return_type = None;
        let mut ignore_duplicates = false;
        let mut order_sensitive = false;
        let mut default_null_behavior = false;
        while !input.is_empty() {
            let key: syn::Ident = input.parse()?;
            input.parse::<syn::Token![=]>()?;
            match key.to_string().as_str() {
                "prefix" => {
                    if prefix.is_some() {
                        return Err(syn::Error::new(key.span(), "duplicate aggregate prefix"));
                    }
                    prefix = Some(input.parse()?);
                }
                "argument_types" => {
                    let content;
                    syn::bracketed!(content in input);
                    let values = content.parse_terminated(|input| input.parse(), syn::Token![,])?;
                    argument_types = Some(values.into_iter().collect());
                }
                "return_type" => return_type = Some(input.parse()?),
                "ignore_duplicates" => ignore_duplicates = input.parse::<syn::LitBool>()?.value,
                "order_sensitive" => order_sensitive = input.parse::<syn::LitBool>()?.value,
                "default_null_behavior" => {
                    default_null_behavior = input.parse::<syn::LitBool>()?.value
                }
                _ => return Err(syn::Error::new(key.span(), "unknown aggregate option")),
            }
            if !input.is_empty() {
                input.parse::<syn::Token![,]>()?;
            }
        }
        Ok(Self {
            prefix,
            argument_types,
            return_type,
            ignore_duplicates,
            order_sensitive,
            default_null_behavior,
        })
    }
}

#[proc_macro_attribute]
pub fn velox_aggregate(args: TokenStream, item: TokenStream) -> TokenStream {
    let args = parse_macro_input!(args as AggregateArgs);
    let implementation = parse_macro_input!(item as ItemImpl);
    match expand_aggregate(args, implementation) {
        Ok(tokens) => tokens.into(),
        Err(error) => error.into_compile_error().into(),
    }
}

fn associated_type(implementation: &ItemImpl, name: &str) -> syn::Result<Type> {
    implementation
        .items
        .iter()
        .find_map(|item| match item {
            ImplItem::Type(item) if item.ident == name => Some(item.ty.clone()),
            _ => None,
        })
        .ok_or_else(|| {
            syn::Error::new(
                implementation.span(),
                format!("#[velox_aggregate] requires associated type `{name}`"),
            )
        })
}

fn expand_aggregate(
    args: AggregateArgs,
    implementation: ItemImpl,
) -> syn::Result<proc_macro2::TokenStream> {
    if implementation.trait_.is_none() {
        return Err(syn::Error::new(
            implementation.span(),
            "#[velox_aggregate] must annotate an impl VeloxAggregate block",
        ));
    }
    let default_prefix = match implementation.self_ty.as_ref() {
        Type::Path(path) => path
            .path
            .segments
            .last()
            .map(|segment| segment.ident.to_string()),
        _ => None,
    }
    .ok_or_else(|| {
        syn::Error::new(
            implementation.self_ty.span(),
            "expected a named aggregate struct",
        )
    })?;
    let prefix = args
        .prefix
        .as_ref()
        .map(LitStr::value)
        .unwrap_or_else(|| default_prefix.to_snake_case());
    let prefix_span = args
        .prefix
        .as_ref()
        .map(LitStr::span)
        .unwrap_or_else(|| implementation.self_ty.span());
    if prefix.is_empty()
        || !prefix
            .chars()
            .all(|character| character.is_ascii_alphanumeric() || character == '_')
    {
        return Err(syn::Error::new(
            prefix_span,
            "aggregate prefix must contain only ASCII letters, digits, and underscores",
        ));
    }
    let self_type = &implementation.self_ty;
    let state_type = associated_type(&implementation, "State")?;
    let input_type = associated_type(&implementation, "Input")?;
    let output_type = parse_type(&associated_type(&implementation, "Output")?)?;
    if matches!(output_type.scalar, ScalarType::Complex) && args.return_type.is_none() {
        return Err(syn::Error::new(
            implementation.span(),
            "complex aggregate outputs require `return_type`",
        ));
    }
    if output_type.borrowed {
        return Err(syn::Error::new(
            implementation.span(),
            "aggregate string and binary results must be owned",
        ));
    }
    let Type::Tuple(input_tuple) = input_type else {
        return Err(syn::Error::new(
            implementation.span(),
            "aggregate Input<'a> must be a tuple",
        ));
    };
    let parsed_inputs: Vec<_> = input_tuple
        .elems
        .iter()
        .map(parse_type)
        .collect::<syn::Result<_>>()?;
    if parsed_inputs
        .iter()
        .any(|input| matches!(input.scalar, ScalarType::Complex))
    {
        let declarations = args.argument_types.as_ref().ok_or_else(|| {
            syn::Error::new(
                implementation.span(),
                "complex aggregate inputs require `argument_types`",
            )
        })?;
        if declarations.len() != parsed_inputs.len() {
            return Err(syn::Error::new(
                implementation.span(),
                "argument_types must match the aggregate input count",
            ));
        }
    }
    let declared_inputs: Vec<_> = parsed_inputs
        .iter()
        .enumerate()
        .map(|(index, parsed)| {
            if matches!(parsed.scalar, ScalarType::Complex) {
                json!({
                    "type": args.argument_types.as_ref().unwrap()[index].value(),
                    "nullable": parsed.nullable,
                })
            } else {
                sql_type(parsed)
            }
        })
        .collect();
    let input_count = parsed_inputs.len();
    let registry = format_ident!("__VELOX_WASM_{}_STATE_REGISTRY", prefix.to_uppercase());
    let create_fn = format_ident!("__velox_wasm_{}_create", prefix);
    let destroy_fn = format_ident!("__velox_wasm_{}_destroy", prefix);
    let update_fn = format_ident!("__velox_wasm_{}_update", prefix);
    let update_single_fn = format_ident!("__velox_wasm_{}_update_single_group", prefix);
    let serialize_fn = format_ident!("__velox_wasm_{}_serialize", prefix);
    let merge_fn = format_ident!("__velox_wasm_{}_merge", prefix);
    let merge_single_fn = format_ident!("__velox_wasm_{}_merge_single_group", prefix);
    let finalize_fn = format_ident!("__velox_wasm_{}_finalize", prefix);
    let state_handle_fn = format_ident!("__velox_wasm_{}_state_handle", prefix);
    let create_export = LitStr::new(&format!("{prefix}_create"), prefix_span);
    let destroy_export = LitStr::new(&format!("{prefix}_destroy"), prefix_span);
    let update_export = LitStr::new(&format!("{prefix}_update"), prefix_span);
    let update_single_export = LitStr::new(&format!("{prefix}_update_single_group"), prefix_span);
    let serialize_export = LitStr::new(&format!("{prefix}_serialize"), prefix_span);
    let merge_export = LitStr::new(&format!("{prefix}_merge"), prefix_span);
    let merge_single_export = LitStr::new(&format!("{prefix}_merge_single_group"), prefix_span);
    let finalize_export = LitStr::new(&format!("{prefix}_finalize"), prefix_span);

    let arrays: Vec<_> = (0..input_count)
        .map(|index| format_ident!("__arg_array_{index}"))
        .collect();
    let array_bindings =
        parsed_inputs
            .iter()
            .zip(&arrays)
            .enumerate()
            .map(|(index, (parsed, array))| {
                let column = index + 1;
                if matches!(parsed.scalar, ScalarType::Complex) {
                    return quote!(let #array = __batch.column(#column););
                }
                let ty = array_type(&parsed.scalar);
                let expected = match parsed.scalar {
                    ScalarType::String => "Utf8",
                    ScalarType::Binary => "Binary",
                    _ => "primitive",
                };
                quote! {
                    let #array = __batch.column(#column).as_any().downcast_ref::<#ty>()
                        .ok_or_else(|| ::velox_wasm_sdk::runtime::type_error(#index, #expected))?;
                }
            });
    let call_arguments =
        parsed_inputs
            .iter()
            .zip(&arrays)
            .enumerate()
            .map(|(index, (parsed, array))| {
                let value = input_value(parsed, array);
                let is_null = if matches!(parsed.scalar, ScalarType::Complex) {
                    quote!(#array.as_ref().is_null(__row))
                } else {
                    quote!(::velox_wasm_sdk::arrow_array::Array::is_null(#array, __row))
                };
                if parsed.nullable {
                    quote! {
                        if #is_null {
                            None
                        } else {
                            Some(#value)
                        }
                    }
                } else {
                    quote! {{
                        if #is_null {
                            return Err(::velox_wasm_sdk::runtime::null_error(__row, #index));
                        }
                        #value
                    }}
                }
            });
    let single_array_bindings =
        parsed_inputs
            .iter()
            .zip(&arrays)
            .enumerate()
            .map(|(index, (parsed, array))| {
                if matches!(parsed.scalar, ScalarType::Complex) {
                    return quote!(let #array = __batch.column(#index););
                }
                let ty = array_type(&parsed.scalar);
                let expected = match parsed.scalar {
                    ScalarType::String => "Utf8",
                    ScalarType::Binary => "Binary",
                    _ => "primitive",
                };
                quote! {
                    let #array = __batch.column(#index).as_any().downcast_ref::<#ty>()
                        .ok_or_else(|| ::velox_wasm_sdk::runtime::type_error(#index, #expected))?;
                }
            });
    let single_call_arguments =
        parsed_inputs
            .iter()
            .zip(&arrays)
            .enumerate()
            .map(|(index, (parsed, array))| {
                let value = input_value(parsed, array);
                let is_null = if matches!(parsed.scalar, ScalarType::Complex) {
                    quote!(#array.as_ref().is_null(__row))
                } else {
                    quote!(::velox_wasm_sdk::arrow_array::Array::is_null(#array, __row))
                };
                if parsed.nullable {
                    quote! {
                        if #is_null {
                            None
                        } else {
                            Some(#value)
                        }
                    }
                } else {
                    quote! {{
                        if #is_null {
                            return Err(::velox_wasm_sdk::runtime::null_error(__row, #index));
                        }
                        #value
                    }}
                }
            });

    let complex_output = matches!(output_type.scalar, ScalarType::Complex);
    let output_builder = if complex_output {
        let sql_type = args.return_type.as_ref().unwrap();
        quote!(::velox_wasm_sdk::runtime::ComplexOutputBuilder::new(#sql_type)?)
    } else {
        builder_init(&output_type.scalar)
    };
    let output_value = output_value(&output_type.scalar);
    let output_append = if complex_output {
        quote!(__builder.append_value(__value)?;)
    } else if output_type.nullable {
        quote! {
            match __value {
                Some(value) => __builder.append_value(#output_value),
                None => __builder.append_null(),
            }
        }
    } else {
        match output_type.scalar {
            ScalarType::Date32 | ScalarType::Timestamp => {
                quote!(__builder.append_value(__value.0);)
            }
            _ => quote!(__builder.append_value(__value);),
        }
    };
    let finish_output = if complex_output {
        quote!(__builder.finish()?)
    } else {
        quote!(std::sync::Arc::new(__builder.finish()))
    };
    let output_data_type = if complex_output {
        quote!(__output.data_type().clone())
    } else {
        data_type(&output_type.scalar)
    };
    let output_nullable = output_type.nullable;
    let declaration = metadata_section(
        format_ident!("__VELOX_WASM_METADATA_{}", prefix.to_uppercase()),
        json!({
            "abi_version": 1,
            "kind": "aggregate",
            "name": prefix.clone(),
            "entrypoints": {
                "create": create_export.value(),
                "destroy": destroy_export.value(),
                "update": update_export.value(),
                "update_single_group": update_single_export.value(),
                "serialize": serialize_export.value(),
                "merge": merge_export.value(),
                "merge_single_group": merge_single_export.value(),
                "finalize": finalize_export.value(),
            },
            "arguments": declared_inputs,
            "intermediate": {"type": "varbinary", "nullable": output_type.nullable},
            "return": if complex_output {
                json!({
                    "type": args.return_type.as_ref().unwrap().value(),
                    "nullable": output_type.nullable,
                })
            } else {
                sql_type(&output_type)
            },
            "order_sensitive": args.order_sensitive,
            "ignore_duplicates": args.ignore_duplicates,
            "default_null_behavior": args.default_null_behavior,
        }),
    );

    Ok(quote! {
        #implementation
        #declaration

        #[cfg(target_arch = "wasm32")]
        std::thread_local! {
            static #registry: std::cell::RefCell<
                ::velox_wasm_sdk::runtime::StateRegistry<#state_type>
            > = std::cell::RefCell::new(::velox_wasm_sdk::runtime::StateRegistry::new());
        }

        #[cfg(target_arch = "wasm32")]
        fn #state_handle_fn(__handles: &::velox_wasm_sdk::arrow_array::UInt32Array, __row: usize)
            -> ::velox_wasm_sdk::runtime::Result<u32> {
            if ::velox_wasm_sdk::arrow_array::Array::is_null(__handles, __row) {
                return Err(format!("state handle is null at compact row {}", __row));
            }
            let __handle = __handles.value(__row);
            if __handle == 0 {
                return Err("state handle 0 is invalid".to_owned());
            }
            Ok(__handle)
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #create_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #create_fn(__count: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute_count(__count, |__count| {
                let mut __handles = Vec::with_capacity(__count as usize);
                for _ in 0..__count {
                    let __state = match <#self_type as ::velox_wasm_sdk::VeloxAggregate>::create() {
                        Ok(state) => state,
                        Err(error) => {
                            for handle in __handles.drain(..) {
                                if let Some(state) = #registry.with(|registry| registry.borrow_mut().remove(handle)) {
                                    <#self_type as ::velox_wasm_sdk::VeloxAggregate>::destroy(state);
                                }
                            }
                            return Err(error.to_string());
                        }
                    };
                    match #registry.with(|registry| registry.borrow_mut().insert(__state)) {
                        Ok(handle) => __handles.push(handle),
                        Err((error, state)) => {
                            <#self_type as ::velox_wasm_sdk::VeloxAggregate>::destroy(state);
                            for handle in __handles.drain(..) {
                                if let Some(state) = #registry.with(|registry| registry.borrow_mut().remove(handle)) {
                                    <#self_type as ::velox_wasm_sdk::VeloxAggregate>::destroy(state);
                                }
                            }
                            return Err(error);
                        }
                    }
                }
                let mut __output = Vec::with_capacity(__handles.len() * 4);
                for handle in __handles {
                    __output.extend_from_slice(&handle.to_le_bytes());
                }
                Ok(__output)
            })
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #destroy_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #destroy_fn(__pointer: u32, __length: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __batch.num_columns() != 1 {
                    return Err(format!("destroy expected 1 column, received {}", __batch.num_columns()));
                }
                let __handles = __batch.column(0).as_any()
                    .downcast_ref::<::velox_wasm_sdk::arrow_array::UInt32Array>()
                    .ok_or_else(|| "state handle column is not Arrow UInt32".to_owned())?;
                for __row in 0..__batch.num_rows() {
                    let __handle = #state_handle_fn(__handles, __row)?;
                    if let Some(__state) = #registry.with(|registry| registry.borrow_mut().remove(__handle)) {
                        <#self_type as ::velox_wasm_sdk::VeloxAggregate>::destroy(__state);
                    }
                }
                Ok(Vec::new())
            })
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #update_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #update_fn(__pointer: u32, __length: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __batch.num_columns() != #input_count + 1 {
                    return Err(format!("update expected {} columns, received {}", #input_count + 1, __batch.num_columns()));
                }
                let __handles = __batch.column(0).as_any()
                    .downcast_ref::<::velox_wasm_sdk::arrow_array::UInt32Array>()
                    .ok_or_else(|| "state handle column is not Arrow UInt32".to_owned())?;
                #(#array_bindings)*
                for __row in 0..__batch.num_rows() {
                    let __handle = #state_handle_fn(__handles, __row)?;
                    #registry.with(|registry| {
                        let mut registry = registry.borrow_mut();
                        let state = registry.get_mut(__handle)?;
                        <#self_type as ::velox_wasm_sdk::VeloxAggregate>::update(
                            state,
                            (#(#call_arguments,)*),
                        ).map_err(|error| error.to_string())
                    })?;
                }
                Ok(Vec::new())
            })
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #update_single_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #update_single_fn(
            __handle: u32,
            __pointer: u32,
            __length: u32,
        ) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __handle == 0 {
                    return Err("state handle 0 is invalid".to_owned());
                }
                if __batch.num_columns() != #input_count {
                    return Err(format!("single-group update expected {} columns, received {}", #input_count, __batch.num_columns()));
                }
                #(#single_array_bindings)*
                #registry.with(|registry| {
                    let mut registry = registry.borrow_mut();
                    let state = registry.get_mut(__handle)?;
                    for __row in 0..__batch.num_rows() {
                        <#self_type as ::velox_wasm_sdk::VeloxAggregate>::update(
                            state,
                            (#(#single_call_arguments,)*),
                        ).map_err(|error| error.to_string())?;
                    }
                    Ok::<(), String>(())
                })?;
                Ok(Vec::new())
            })
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #serialize_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #serialize_fn(__pointer: u32, __length: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __batch.num_columns() != 1 {
                    return Err(format!("serialize expected 1 column, received {}", __batch.num_columns()));
                }
                let __handles = __batch.column(0).as_any()
                    .downcast_ref::<::velox_wasm_sdk::arrow_array::UInt32Array>()
                    .ok_or_else(|| "state handle column is not Arrow UInt32".to_owned())?;
                let mut __builder = ::velox_wasm_sdk::arrow_array::builder::BinaryBuilder::new();
                for __row in 0..__batch.num_rows() {
                    let __handle = #state_handle_fn(__handles, __row)?;
                    let __bytes = #registry.with(|registry| {
                        let registry = registry.borrow();
                        let state = registry.get(__handle)?;
                        <#self_type as ::velox_wasm_sdk::VeloxAggregate>::serialize(state)
                            .map_err(|error| error.to_string())
                    })?;
                    __builder.append_value(__bytes);
                }
                ::velox_wasm_sdk::runtime::encode_output(
                    ::velox_wasm_sdk::arrow_schema::Field::new(
                        "__velox_wasm_intermediate",
                        ::velox_wasm_sdk::arrow_schema::DataType::Binary,
                        false,
                    ),
                    std::sync::Arc::new(__builder.finish()),
                )
            })
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #merge_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #merge_fn(__pointer: u32, __length: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __batch.num_columns() != 2 {
                    return Err(format!("merge expected 2 columns, received {}", __batch.num_columns()));
                }
                let __handles = __batch.column(0).as_any()
                    .downcast_ref::<::velox_wasm_sdk::arrow_array::UInt32Array>()
                    .ok_or_else(|| "state handle column is not Arrow UInt32".to_owned())?;
                let __intermediate = __batch.column(1).as_any()
                    .downcast_ref::<::velox_wasm_sdk::arrow_array::BinaryArray>()
                    .ok_or_else(|| "intermediate column is not Arrow Binary".to_owned())?;
                for __row in 0..__batch.num_rows() {
                    if ::velox_wasm_sdk::arrow_array::Array::is_null(__intermediate, __row) {
                        continue;
                    }
                    let __handle = #state_handle_fn(__handles, __row)?;
                    #registry.with(|registry| {
                        let mut registry = registry.borrow_mut();
                        let state = registry.get_mut(__handle)?;
                        <#self_type as ::velox_wasm_sdk::VeloxAggregate>::merge(
                            state,
                            __intermediate.value(__row),
                        ).map_err(|error| error.to_string())
                    })?;
                }
                Ok(Vec::new())
            })
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #merge_single_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #merge_single_fn(
            __handle: u32,
            __pointer: u32,
            __length: u32,
        ) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __handle == 0 {
                    return Err("state handle 0 is invalid".to_owned());
                }
                if __batch.num_columns() != 1 {
                    return Err(format!("single-group merge expected 1 column, received {}", __batch.num_columns()));
                }
                let __intermediate = __batch.column(0).as_any()
                    .downcast_ref::<::velox_wasm_sdk::arrow_array::BinaryArray>()
                    .ok_or_else(|| "intermediate column is not Arrow Binary".to_owned())?;
                #registry.with(|registry| {
                    let mut registry = registry.borrow_mut();
                    let state = registry.get_mut(__handle)?;
                    for __row in 0..__batch.num_rows() {
                        if ::velox_wasm_sdk::arrow_array::Array::is_null(__intermediate, __row) {
                            continue;
                        }
                        <#self_type as ::velox_wasm_sdk::VeloxAggregate>::merge(
                            state,
                            __intermediate.value(__row),
                        ).map_err(|error| error.to_string())?;
                    }
                    Ok::<(), String>(())
                })?;
                Ok(Vec::new())
            })
        }

        #[cfg(target_arch = "wasm32")]
        #[export_name = #finalize_export]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #finalize_fn(__pointer: u32, __length: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if __batch.num_columns() != 1 {
                    return Err(format!("finalize expected 1 column, received {}", __batch.num_columns()));
                }
                let __handles = __batch.column(0).as_any()
                    .downcast_ref::<::velox_wasm_sdk::arrow_array::UInt32Array>()
                    .ok_or_else(|| "state handle column is not Arrow UInt32".to_owned())?;
                let mut __builder = #output_builder;
                for __row in 0..__batch.num_rows() {
                    let __handle = #state_handle_fn(__handles, __row)?;
                    let __value = #registry.with(|registry| {
                        let registry = registry.borrow();
                        let state = registry.get(__handle)?;
                        <#self_type as ::velox_wasm_sdk::VeloxAggregate>::finalize(state)
                            .map_err(|error| error.to_string())
                    })?;
                    #output_append
                }
                let __output: ::velox_wasm_sdk::arrow_array::ArrayRef =
                    #finish_output;
                ::velox_wasm_sdk::runtime::encode_output(
                    ::velox_wasm_sdk::arrow_schema::Field::new(
                        "result",
                        #output_data_type,
                        #output_nullable,
                    ),
                    __output,
                )
            })
        }
    })
}
