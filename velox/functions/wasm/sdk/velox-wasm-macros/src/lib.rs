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
mod generics;
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
            quote!(::velox_wasm_sdk::Timestamp(#array.value(__row) as i128))
        }
        (ScalarType::Complex, _) => quote!(#array.slice(__row, 1)),
        _ => quote!(#array.value(__row)),
    }
}

fn output_value(scalar: &ScalarType) -> proc_macro2::TokenStream {
    match scalar {
        ScalarType::Date32 => quote!(value.0),
        ScalarType::Timestamp => quote!(i64::try_from(value.0)
            .map_err(|_| "legacy timestamp exceeds nanosecond range".to_owned())?),
        _ => quote!(value),
    }
}

#[proc_macro_attribute]
pub fn velox_scalar(args: TokenStream, item: TokenStream) -> TokenStream {
    let function = parse_macro_input!(item as ItemFn);
    let raw: proc_macro2::TokenStream = args.into();
    let extended = raw.clone().into_iter().any(|token| match token {
        proc_macro2::TokenTree::Ident(key) => [
            "arguments",
            "returns",
            "variadic",
            "type_variables",
            "integer_variables",
            "constant_arguments",
            "initialize",
            "config_keys",
            "call_ascii",
            "call_null_free",
        ]
        .contains(&key.to_string().as_str()),
        _ => false,
    });
    let expansion = if function.sig.generics.type_params().next().is_some() {
        generics::expand(raw, function)
    } else if extended {
        syn::parse2::<ArrowScalarArgs>(raw).and_then(|args| expand_row_scalar(args, function))
    } else {
        syn::parse2::<ScalarArgs>(raw).and_then(|args| {
            let row_args = infer_row_signature(args, &function)?;
            expand_row_scalar(row_args, function)
        })
    };
    match expansion {
        Ok(tokens) => tokens.into(),
        Err(error) => error.into_compile_error().into(),
    }
}

fn nullable_row_type(ty: &Type) -> bool {
    final_ident(ty).as_deref() == Some("Option")
}

fn variadic_element(ty: &Type) -> syn::Result<Option<&Type>> {
    let Type::Path(path) = ty else {
        return Ok(None);
    };
    let Some(segment) = path.path.segments.last() else {
        return Ok(None);
    };
    let view = segment.ident == "VariadicView";
    if segment.ident != "Variadic" && !view {
        return Ok(None);
    }
    let PathArguments::AngleBracketed(args) = &segment.arguments else {
        return Err(syn::Error::new(
            ty.span(),
            "variadic tail requires one element type",
        ));
    };
    let mut arguments = args.args.iter();
    if view && matches!(arguments.clone().next(), Some(GenericArgument::Lifetime(_))) {
        arguments.next();
    }
    match (arguments.next(), arguments.next()) {
        (Some(GenericArgument::Type(element)), None) => Ok(Some(element)),
        _ => Err(syn::Error::new(
            ty.span(),
            "use Variadic<T> or VariadicView<'a, T> with one element type",
        )),
    }
}

fn infer_row_signature(args: ScalarArgs, function: &ItemFn) -> syn::Result<ArrowScalarArgs> {
    let mut fixed = Vec::new();
    let mut variadic = None;
    for input in &function.sig.inputs {
        let FnArg::Typed(input) = input else {
            return Err(syn::Error::new(input.span(), "methods are not supported"));
        };
        let ty = variadic_element(&input.ty)?;
        let name = inferred_row_sql_type(ty.unwrap_or(&input.ty))?;
        let literal = LitStr::new(&name, input.span());
        if ty.is_some() {
            variadic = Some(literal);
        } else {
            fixed.push(literal);
        }
    }
    let ReturnType::Type(_, output) = &function.sig.output else {
        return Err(syn::Error::new(
            function.sig.span(),
            "scalar must return a value",
        ));
    };
    let returns = LitStr::new(&inferred_row_sql_type(output)?, function.sig.span());
    Ok(ArrowScalarArgs {
        arguments: fixed,
        variadic,
        returns: Some(returns),
        name: args.name,
        export: args.export,
        deterministic: args.deterministic,
        default_null_behavior: args.default_null_behavior.unwrap_or(false),
        null_behavior_explicit: args.default_null_behavior.is_some(),
        ..Default::default()
    })
}

fn type_parameters(ty: &Type) -> Vec<&Type> {
    if let Type::Path(path) = ty {
        if let PathArguments::AngleBracketed(arguments) =
            &path.path.segments.last().unwrap().arguments
        {
            return arguments
                .args
                .iter()
                .filter_map(|arg| match arg {
                    GenericArgument::Type(ty) => Some(ty),
                    _ => None,
                })
                .collect();
        }
    }
    vec![]
}
fn has_null_free_view(ty: &Type) -> bool {
    if let Type::Tuple(tuple) = ty {
        return tuple.elems.iter().any(has_null_free_view);
    }
    matches!(
        final_ident(ty).as_deref(),
        Some("NullFreeArrayView" | "NullFreeMapView" | "NullFreeRowView")
    ) || type_parameters(ty).iter().any(|ty| has_null_free_view(ty))
}
fn inferred_row_sql_type(ty: &Type) -> syn::Result<String> {
    let name = final_ident(ty).unwrap_or_default();
    let types = type_parameters(ty);
    match name.as_str() {
        "i128" => Ok("hugeint".to_owned()),
        "VarcharBytes" => Ok("varchar".to_owned()),
        "Option" | "Result" if !types.is_empty() => inferred_row_sql_type(types[0]),
        "ArrayView" | "NullFreeArrayView" | "ArrayWriter" if types.len() == 1 => {
            Ok(format!("array({})", inferred_row_sql_type(types[0])?))
        }
        "MapView" | "NullFreeMapView" | "MapWriter" if types.len() == 2 => Ok(format!(
            "map({},{})",
            inferred_row_sql_type(types[0])?,
            inferred_row_sql_type(types[1])?
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
                .enumerate()
                .map(|(i, field)| Ok(format!("\"field{i}\" {}", inferred_row_sql_type(field)?)))
                .collect::<syn::Result<Vec<_>>>()?;
            Ok(format!("row({})", fields.join(",")))
        }
        "Generic" | "Value" | "GenericWriter" | "Decimal" | "CustomValue" | "OpaqueHandle" => {
            Err(syn::Error::new(
                ty.span(),
                "declare explicit SQL arguments/returns for Generic, dynamic rows, and Decimal",
            ))
        }
        _ => {
            let parsed = parse_type(ty)?;
            Ok(sql_type(&parsed)["type"].as_str().unwrap().to_owned())
        }
    }
}
fn validate_row_input_type(ty: &Type) -> syn::Result<()> {
    if let Type::Tuple(tuple) = ty {
        for field in &tuple.elems {
            validate_row_input_type(field)?;
        }
        return Ok(());
    }
    if matches!(
        final_ident(ty).as_deref(),
        Some("Value" | "GenericWriter" | "ArrayWriter" | "MapWriter" | "RowWriter")
    ) {
        return Err(syn::Error::new(
            ty.span(),
            "input uses a borrowed view; writers and Value are output types",
        ));
    }
    for child in type_parameters(ty) {
        validate_row_input_type(child)?;
    }
    validate_row_value_type(ty)
}
fn validate_row_value_type(ty: &Type) -> syn::Result<()> {
    if let Type::Path(path) = ty {
        let segment = path.path.segments.last().unwrap();
        if segment.ident == "Option" {
            if let PathArguments::AngleBracketed(arguments) = &segment.arguments {
                if arguments.args.len() == 1 {
                    if let Some(GenericArgument::Type(inner)) = arguments.args.first() {
                        return validate_row_value_type(inner);
                    }
                }
            }
            return Err(syn::Error::new(
                ty.span(),
                "Option requires one row value type",
            ));
        }
        if [
            "Generic",
            "Decimal",
            "Value",
            "GenericWriter",
            "i128",
            "CustomValue",
            "OpaqueHandle",
        ]
        .contains(&segment.ident.to_string().as_str())
        {
            return Ok(());
        }
        if [
            "VarcharBytes",
            "ArrayView",
            "MapView",
            "RowView",
            "NullFreeArrayView",
            "NullFreeMapView",
            "NullFreeRowView",
            "ArrayWriter",
            "MapWriter",
            "RowWriter",
        ]
        .contains(&segment.ident.to_string().as_str())
        {
            for child in type_parameters(ty) {
                if let Type::Tuple(tuple) = child {
                    for field in &tuple.elems {
                        validate_row_value_type(field)?;
                    }
                } else {
                    validate_row_value_type(child)?;
                }
            }
            return Ok(());
        }
    }
    let parsed = parse_type(ty)?;
    if matches!(parsed.scalar, ScalarType::Complex) {
        return Err(syn::Error::new(
            ty.span(),
            "scalar parameters and results are single-row values; use Generic for nested SQL types",
        ));
    }
    // Row readers implement borrowed str/[u8] and owned String/Vec<u8>.
    let supported = match ty {
        Type::Reference(reference) => {
            reference.mutability.is_none()
                && matches!(
                    final_ident(&reference.elem).as_deref(),
                    Some("str" | "[u8]")
                )
        }
        Type::Path(path) if matches!(parsed.scalar, ScalarType::Binary) => {
            let segment = path.path.segments.last().unwrap();
            matches!(&segment.arguments, PathArguments::AngleBracketed(arguments)
                if arguments.args.len() == 1 && matches!(arguments.args.first(),
                    Some(GenericArgument::Type(inner)) if final_ident(inner).as_deref() == Some("u8")))
        }
        Type::Path(_) => final_ident(ty).as_deref() != Some("str"),
        _ => false,
    };
    if !supported {
        return Err(syn::Error::new(ty.span(), "unsupported row value type"));
    }
    Ok(())
}

fn validate_row_output_type(ty: &Type) -> syn::Result<bool> {
    if let Type::Path(path) = ty {
        let segment = path.path.segments.last().unwrap();
        if segment.ident == "Result" {
            if let PathArguments::AngleBracketed(arguments) = &segment.arguments {
                if arguments.args.len() == 2 {
                    if let Some(GenericArgument::Type(output)) = arguments.args.first() {
                        validate_row_value_type(output)?;
                        return Ok(true);
                    }
                }
            }
            return Err(syn::Error::new(
                ty.span(),
                "Result requires a row value and an error type",
            ));
        }
    }
    validate_row_value_type(ty)?;
    Ok(false)
}

fn expand_row_scalar(
    mut args: ArrowScalarArgs,
    function: ItemFn,
) -> syn::Result<proc_macro2::TokenStream> {
    if function.sig.asyncness.is_some()
        || function
            .sig
            .generics
            .params
            .iter()
            .any(|param| !matches!(param, syn::GenericParam::Lifetime(_)))
    {
        return Err(syn::Error::new(function.sig.span(), "SQL generics use Generic values; Rust type/const generics and async entrypoints are not supported"));
    }
    let fixed_count = args.arguments.len();
    let variadic = args.variadic.is_some();
    let null_free = function
        .sig
        .inputs
        .iter()
        .any(|input| matches!(input, FnArg::Typed(input) if has_null_free_view(&input.ty)));
    if function.sig.inputs.len() != fixed_count + usize::from(variadic) {
        return Err(syn::Error::new(
            function.sig.span(),
            "Rust parameters must match the fixed SQL arguments plus one Variadic<T> or VariadicView tail",
        ));
    }
    let mut input_nullable = Vec::new();
    let mut call_arguments = Vec::new();
    let mut callback_arguments = Vec::new();
    let mut variadic_reader = quote!();
    for (index, input) in function.sig.inputs.iter().enumerate() {
        let FnArg::Typed(input) = input else {
            return Err(syn::Error::new(input.span(), "methods are not supported"));
        };
        let tail = variadic_element(&input.ty)?;
        if variadic && index == fixed_count {
            let element = tail.ok_or_else(|| {
                syn::Error::new(
                    input.span(),
                    "the final parameter must be Variadic<T> or VariadicView",
                )
            })?;
            validate_row_input_type(element)?;
            input_nullable.push(nullable_row_type(element));
            if final_ident(&input.ty).as_deref() == Some("VariadicView") {
                variadic_reader.extend(
                    quote!(let __variadic_reader = ::velox_wasm_sdk::variadic::VariadicReader::new(
                    &__batch.columns()[#fixed_count..], __batch.num_rows())?;),
                );
                call_arguments.push(quote!(__variadic_reader.row(__row)?));
            } else {
                call_arguments.push(quote!(::velox_wasm_sdk::Variadic::from_row(&__batch.columns()[#fixed_count..], __row)?));
            }
        } else {
            if tail.is_some() {
                return Err(syn::Error::new(
                    input.span(),
                    "Variadic must be the last parameter and have a variadic SQL signature",
                ));
            }
            validate_row_input_type(&input.ty)?;
            input_nullable.push(nullable_row_type(&input.ty));
            let reader = format_ident!("__argument_reader_{}", index);
            variadic_reader.extend(quote!(let #reader = ::velox_wasm_sdk::scalar::prepare_row(
                __batch.column(#index).as_ref());));
            call_arguments.push(quote!(#reader(__row)?));
            callback_arguments.push(quote!(::velox_wasm_sdk::scalar::read_row(
                __batch.column(#index).as_ref(), __row)?));
        }
    }
    if variadic {
        callback_arguments.push(call_arguments.last().unwrap().clone());
    }
    if !args.null_behavior_explicit {
        args.default_null_behavior = !input_nullable.iter().any(|nullable| *nullable);
    }
    let ReturnType::Type(_, output) = &function.sig.output else {
        return Err(syn::Error::new(
            function.sig.span(),
            "scalar must return a value",
        ));
    };
    let fallible = validate_row_output_type(output)?;
    let function_name = &function.sig.ident;
    let export = args
        .export
        .clone()
        .unwrap_or_else(|| LitStr::new(&function_name.to_string(), function_name.span()));
    let returns = args
        .returns
        .as_ref()
        .ok_or_else(|| syn::Error::new(function.sig.span(), "missing returns"))?;
    let mut arguments = args.arguments.clone();
    if let Some(tail) = &args.variadic {
        arguments.push(tail.clone());
    }
    if args
        .constant_arguments
        .iter()
        .any(|index| *index >= arguments.len())
    {
        return Err(syn::Error::new(
            function.sig.span(),
            "constant argument index is outside the signature",
        ));
    }
    let mut metadata = json!({
        "abi_version": 1, "kind": "scalar", "row_api": true, "has_ascii": args.call_ascii.is_some(),
        "name": args.name.as_ref().map(LitStr::value).unwrap_or_else(|| function_name.to_string()),
        "entrypoint": export.value(),
        "arguments": arguments.iter().enumerate().map(|(index, ty)| json!({
            "type": ty.value(), "nullable": input_nullable[index], "constant": args.constant_arguments.contains(&index),
        })).collect::<Vec<_>>(),
        "return": {"type": returns.value(), "nullable": true},
        "type_variables": signature_variables(&args.type_variables, false)?,
        "integer_variables": signature_variables(&args.integer_variables, true)?,
        "variable_arity": variadic,
        "deterministic": args.deterministic, "default_null_behavior": args.default_null_behavior,
    });
    let initialize = scalar_initialization(
        args.initialize.as_ref(),
        &args.config_keys,
        function_name,
        &mut metadata,
    )?;
    let declaration = metadata_section(
        format_ident!(
            "__VELOX_WASM_METADATA_{}",
            function_name.to_string().to_uppercase()
        ),
        metadata,
    );
    let make_call = |path: proc_macro2::TokenStream, arguments: &Vec<proc_macro2::TokenStream>| {
        if fallible {
            quote!(#path(#(#arguments),*).map_err(|error| { use ::velox_wasm_sdk::scalar::EncodeRowError as _; (&error).encode_row_error() }))
        } else {
            quote!(Ok(#path(#(#arguments),*)))
        }
    };
    let parse_callback = |path: &LitStr| {
        syn::parse_str::<syn::Path>(&path.value())
            .map_err(|error| syn::Error::new(path.span(), error))
    };
    let mut call = make_call(quote!(#function_name), &call_arguments);
    if let Some(path) = &args.call_ascii {
        let path = parse_callback(path)?;
        let ascii_call = make_call(quote!(#path), &callback_arguments);
        call = quote!(if __batch_is_ascii { #ascii_call } else { #call });
    }
    let has_ascii_call = args.call_ascii.is_some();
    let has_null_free_call = args.call_null_free.is_some();
    if let Some(path) = &args.call_null_free {
        let path = parse_callback(path)?;
        let null_free_call = make_call(quote!(#path), &callback_arguments);
        call = quote!(if !__has_nulls { #null_free_call } else { #call });
    }
    let wrapper = format_ident!("__velox_wasm_row_adapter_{}", function_name);
    Ok(quote! {
        #function
        #declaration
        #initialize
        #[cfg(target_arch = "wasm32")]
        #[export_name = #export]
        #[allow(non_snake_case)]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #wrapper(__pointer: u32, __length: u32) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if (!#variadic && __batch.num_columns() != #fixed_count) ||
                    (#variadic && __batch.num_columns() < #fixed_count) {
                    return Err("scalar argument count does not match signature".to_owned());
                }
                #variadic_reader
                let __return_sql_type = __batch.schema_ref().metadata().get("velox.return_type")
                    .ok_or("missing bound scalar return type")?;
                let __return_type = ::velox_wasm_sdk::runtime::bound_return_type(__batch.schema_ref().metadata())?;
                let mut __output = ::velox_wasm_sdk::scalar::RowOutputBuilder::new(__return_type.clone(), __batch.num_rows());
                let __batch_may_have_nulls = (#null_free || #has_null_free_call) && __batch.columns().iter()
                    .any(|column|::velox_wasm_sdk::scalar::array_may_have_recursive_nulls(column.as_ref()));
                let __batch_is_ascii = #has_ascii_call && ::velox_wasm_sdk::scalar::batch_is_ascii(&__batch)?;
                for __row in 0..__batch.num_rows() {
                    let mut __has_nulls = false;
                    if __batch_may_have_nulls {
                        for __column in __batch.columns() {
                            __has_nulls |= ::velox_wasm_sdk::Generic::new(__column.as_ref(), __row)?.contains_nulls()?;
                        }
                        if #null_free && __has_nulls {
                            __output.append(None::<::velox_wasm_sdk::Generic<'_>>);
                            continue;
                        }
                    }
                    let __value: ::std::result::Result<_, String> = (|| { #call })();
                    match __value {
                        Ok(__value) => __output.append(__value),
                        Err(__error) => __output.append_error(__error),
                    }
                }
                let (__values, __errors) = __output.finish()?;
                ::velox_wasm_sdk::runtime::encode_batch(
                    vec![
                        ::velox_wasm_sdk::arrow_schema::Field::new("result", __return_type, true),
                        ::velox_wasm_sdk::arrow_schema::Field::new("__velox_wasm_error", ::velox_wasm_sdk::arrow_schema::DataType::Utf8, true),
                    ], vec![__values, __errors],
                )
            })
        }
    })
}

#[derive(Default)]
struct ArrowScalarArgs {
    arguments: Vec<LitStr>,
    returns: Option<LitStr>,
    name: Option<LitStr>,
    export: Option<LitStr>,
    deterministic: bool,
    default_null_behavior: bool,
    null_behavior_explicit: bool,
    type_variables: Vec<LitStr>,
    integer_variables: Vec<LitStr>,
    variadic: Option<LitStr>,
    constant_arguments: Vec<usize>,
    initialize: Option<LitStr>,
    config_keys: Vec<LitStr>,
    call_ascii: Option<LitStr>,
    call_null_free: Option<LitStr>,
}

impl syn::parse::Parse for ArrowScalarArgs {
    fn parse(input: syn::parse::ParseStream<'_>) -> syn::Result<Self> {
        let mut args = Self {
            deterministic: true,
            ..Self::default()
        };
        let mut seen = std::collections::HashSet::new();
        while !input.is_empty() {
            let key: syn::Ident = input.parse()?;
            let key_name = key.to_string();
            if !seen.insert(key_name.clone()) {
                return Err(syn::Error::new(key.span(), "duplicate Arrow scalar option"));
            }
            input.parse::<syn::Token![=]>()?;
            match key_name.as_str() {
                "arguments" | "type_variables" | "integer_variables" | "config_keys" => {
                    let content;
                    syn::bracketed!(content in input);
                    let values = content
                        .parse_terminated(|input| input.parse::<LitStr>(), syn::Token![,])?;
                    let values = values.into_iter().collect();
                    match key_name.as_str() {
                        "arguments" => args.arguments = values,
                        "type_variables" => args.type_variables = values,
                        "integer_variables" => args.integer_variables = values,
                        _ => args.config_keys = values,
                    }
                }
                "constant_arguments" => {
                    let content;
                    syn::bracketed!(content in input);
                    let values = content
                        .parse_terminated(|input| input.parse::<syn::LitInt>(), syn::Token![,])?;
                    args.constant_arguments = values
                        .iter()
                        .map(|value| value.base10_parse())
                        .collect::<syn::Result<_>>()?;
                }
                "export" => args.export = Some(input.parse()?),
                "returns" => args.returns = Some(input.parse()?),
                "name" => args.name = Some(input.parse()?),
                "variadic" => args.variadic = Some(input.parse()?),
                "initialize" => args.initialize = Some(input.parse()?),
                "call_ascii" => args.call_ascii = Some(input.parse()?),
                "call_null_free" => args.call_null_free = Some(input.parse()?),
                "deterministic" => args.deterministic = input.parse::<syn::LitBool>()?.value,
                "default_null_behavior" => {
                    args.default_null_behavior = input.parse::<syn::LitBool>()?.value
                }
                _ => return Err(syn::Error::new(key.span(), "unknown Arrow scalar option")),
            }
            if !input.is_empty() {
                input.parse::<syn::Token![,]>()?;
            }
        }
        if !seen.contains("arguments") {
            return Err(input.error("missing `arguments`"));
        }
        if args.initialize.is_none() && !args.config_keys.is_empty() {
            return Err(input.error("config_keys requires initialize"));
        }
        args.null_behavior_explicit = seen.contains("default_null_behavior");
        Ok(args)
    }
}

fn signature_variables(values: &[LitStr], integer: bool) -> syn::Result<serde_json::Value> {
    let mut variables = serde_json::Map::new();
    for value in values {
        let text = value.value();
        let separator = if integer { '=' } else { ':' };
        let (name, constraint) = text
            .split_once(separator)
            .unwrap_or((&text, if integer { "" } else { "any" }));
        let name = name.trim();
        let constraint = constraint.trim();
        syn::parse_str::<syn::Ident>(name)
            .map_err(|_| syn::Error::new(value.span(), "invalid signature variable name"))?;
        if !integer
            && ![
                "any",
                "known",
                "comparable",
                "orderable",
                "known&comparable",
                "known&orderable",
            ]
            .contains(&constraint)
        {
            return Err(syn::Error::new(
                value.span(),
                "expected any, known, comparable, orderable, known&comparable or known&orderable constraint",
            ));
        }
        if variables
            .insert(name.to_owned(), json!(constraint))
            .is_some()
        {
            return Err(syn::Error::new(
                value.span(),
                "duplicate signature variable",
            ));
        }
    }
    Ok(serde_json::Value::Object(variables))
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

fn scalar_initialization(
    initialize: Option<&LitStr>,
    config_keys: &[LitStr],
    function_name: &syn::Ident,
    metadata: &mut Value,
) -> syn::Result<proc_macro2::TokenStream> {
    let tokens = if let Some(initialize) = initialize {
        let initializer: syn::Path = syn::parse_str(&initialize.value())
            .map_err(|error| syn::Error::new(initialize.span(), error))?;
        let init_export = LitStr::new(&format!("{}_init", function_name), function_name.span());
        let init_wrapper = format_ident!("__velox_wasm_initialize_{}", function_name);
        metadata["initialize"] = json!(init_export.value());
        metadata["config_keys"] = json!(config_keys.iter().map(LitStr::value).collect::<Vec<_>>());
        quote! {
            #[cfg(target_arch = "wasm32")]
            #[export_name = #init_export]
            #[target_feature(enable = "simd128")]
            pub unsafe extern "C" fn #init_wrapper(__pointer: u32, __length: u32) -> core::arch::wasm32::v128 {
                ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                    let __context = ::velox_wasm_sdk::InitContext::new(&__batch)?;
                    #initializer(&__context).map_err(|error| error.to_string())?;
                    ::velox_wasm_sdk::runtime::encode_output(
                        ::velox_wasm_sdk::arrow_schema::Field::new("initialized", ::velox_wasm_sdk::arrow_schema::DataType::Boolean, false),
                        ::std::sync::Arc::new(::velox_wasm_sdk::arrow_array::BooleanArray::from(vec![true])),
                    )
                })
            }
        }
    } else {
        quote! {}
    };
    Ok(tokens)
}

fn expand_arrow_scalar(
    args: ArrowScalarArgs,
    function: ItemFn,
) -> syn::Result<proc_macro2::TokenStream> {
    if args.call_ascii.is_some() || args.call_null_free.is_some() {
        return Err(syn::Error::new(
            function.sig.span(),
            "call_ascii / call_null_free require the row-oriented velox_scalar adapter",
        ));
    }
    if function.sig.inputs.len() != 1
        || !function.sig.generics.params.is_empty()
        || function.sig.asyncness.is_some()
    {
        return Err(syn::Error::new(
            function.sig.span(),
            "#[velox_arrow_scalar] requires one RecordBatch argument and no generics",
        ));
    }
    let returns = args
        .returns
        .ok_or_else(|| syn::Error::new(function.sig.span(), "missing `returns`"))?;
    let function_name = &function.sig.ident;
    let export = args
        .export
        .clone()
        .unwrap_or_else(|| LitStr::new(&function_name.to_string(), function_name.span()));
    let wrapper_name = format_ident!("__velox_wasm_arrow_batch_{}", function_name);
    let sql_name = args
        .name
        .map(|value| value.value())
        .unwrap_or_else(|| function_name.to_string());
    let input_count = args.arguments.len();
    let variable_arity = args.variadic.is_some();
    let mut arguments = args.arguments.clone();
    if let Some(tail) = args.variadic {
        arguments.push(tail);
    }
    if args
        .constant_arguments
        .iter()
        .any(|index| *index >= arguments.len())
    {
        return Err(syn::Error::new(
            function.sig.span(),
            "constant argument index is outside the signature",
        ));
    }
    let mut metadata = json!({
        "abi_version": 1,
        "kind": "scalar",
        "name": sql_name,
        "entrypoint": export.value(),
        "arguments": arguments.iter().enumerate().map(|(index, value)| json!({
            "type": value.value(), "nullable": true,
            "constant": args.constant_arguments.contains(&index),
        })).collect::<Vec<_>>(),
        "return": {"type": returns.value(), "nullable": true},
        "type_variables": signature_variables(&args.type_variables, false)?,
        "integer_variables": signature_variables(&args.integer_variables, true)?,
        "variable_arity": variable_arity,
        "deterministic": args.deterministic,
        "default_null_behavior": args.default_null_behavior,
    });
    let initialization = scalar_initialization(
        args.initialize.as_ref(),
        &args.config_keys,
        function_name,
        &mut metadata,
    )?;
    let declaration = metadata_section(
        format_ident!(
            "__VELOX_WASM_METADATA_{}",
            function_name.to_string().to_uppercase()
        ),
        metadata,
    );
    Ok(quote! {
        #function
        #declaration
        #initialization

        #[cfg(target_arch = "wasm32")]
        #[export_name = #export]
        #[allow(non_snake_case)]
        #[target_feature(enable = "simd128")]
        pub unsafe extern "C" fn #wrapper_name(
            __pointer: u32,
            __length: u32,
        ) -> core::arch::wasm32::v128 {
            ::velox_wasm_sdk::runtime::execute(__pointer, __length, |__batch| {
                if (!#variable_arity && __batch.num_columns() != #input_count) ||
                    (#variable_arity && __batch.num_columns() < #input_count) {
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

mod aggregate;

#[proc_macro_attribute]
pub fn velox_row_aggregate(args: TokenStream, item: TokenStream) -> TokenStream {
    let args = parse_macro_input!(args as aggregate::RowAggregateArgs);
    let implementation = parse_macro_input!(item as ItemImpl);
    match aggregate::expand(args, implementation) {
        Ok(tokens) => tokens.into(),
        Err(error) => error.into_compile_error().into(),
    }
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

#[cfg(test)]
mod arrow_signature_tests {
    use super::*;

    fn expansion_error(options: proc_macro2::TokenStream) -> String {
        let args = syn::parse2::<ArrowScalarArgs>(options).unwrap();
        let function: ItemFn = syn::parse_quote!(
            fn udf(batch: &RecordBatch) -> Result<ArrayRef, String> {
                unreachable!()
            }
        );
        expand_arrow_scalar(args, function).unwrap_err().to_string()
    }

    #[test]
    fn rejects_invalid_constraints_and_constant_indexes() {
        assert!(expansion_error(quote!(
            arguments = ["T"],
            returns = "T",
            type_variables = ["T:invalid"]
        ))
        .contains("expected any"));
        assert!(expansion_error(quote!(
            arguments = ["T"],
            returns = "T",
            type_variables = ["T", "T"]
        ))
        .contains("duplicate signature variable"));
        assert!(expansion_error(quote!(
            arguments = ["bigint"],
            returns = "bigint",
            constant_arguments = [1]
        ))
        .contains("outside the signature"));
        assert!(expansion_error(quote!(
            arguments = [],
            variadic = "bigint",
            returns = "bigint",
            constant_arguments = [1]
        ))
        .contains("outside the signature"));
    }

    #[test]
    fn rejects_duplicate_options_and_configuration_without_initialization() {
        assert!(syn::parse2::<ArrowScalarArgs>(quote!(arguments = [], arguments = [])).is_err());
        assert!(
            syn::parse2::<ArrowScalarArgs>(quote!(arguments = [], config_keys = ["timezone"]))
                .is_err()
        );
        assert!(expansion_error(quote!(
            arguments = [],
            returns = "bigint",
            initialize = "not a path"
        ))
        .contains("unexpected token"));
    }

    #[test]
    fn accepts_generic_variadic_initialization_and_decimal_constraints() {
        let function: ItemFn = syn::parse_quote!(
            fn udf(batch: &RecordBatch) -> Result<ArrayRef, String> {
                unreachable!()
            }
        );
        for options in [
            quote!(
                arguments = ["T"],
                variadic = "T",
                returns = "T",
                type_variables = ["T:comparable"],
                constant_arguments = [0, 1],
                initialize = "module::init",
                config_keys = ["timezone"]
            ),
            quote!(
                arguments = ["decimal(p,s)"],
                returns = "decimal(r,s)",
                integer_variables = ["p", "s", "r=min(38,p+1)"]
            ),
            quote!(arguments = [], variadic = "any", returns = "bigint"),
        ] {
            assert!(expand_arrow_scalar(syn::parse2(options).unwrap(), function.clone()).is_ok());
        }
        let generic_rust_function: ItemFn = syn::parse_quote!(
            fn udf<T>(batch: &T) -> T {
                unreachable!()
            }
        );
        let args = syn::parse2(quote!(
            arguments = ["T"],
            returns = "T",
            type_variables = ["T"]
        ))
        .unwrap();
        assert!(expand_arrow_scalar(args, generic_rust_function).is_err());
    }
}

#[cfg(test)]
mod row_signature_tests {
    use super::*;

    #[test]
    fn infers_nested_types_and_detects_null_free_fields_inside_rows() {
        assert_eq!(
            inferred_row_sql_type(&syn::parse_quote!(MapView<'_, &str, ArrayView<'_, i128>>))
                .unwrap(),
            "map(varchar,array(hugeint))"
        );
        assert_eq!(
            inferred_row_sql_type(&syn::parse_quote!(
                RowWriter<(Option<i64>, Option<ArrayWriter<String>>)>
            ))
            .unwrap(),
            "row(\"field0\" bigint,\"field1\" array(varchar))"
        );
        assert!(has_null_free_view(&syn::parse_quote!(
            RowView<'_, (i64, NullFreeArrayView<'_, i64>)>
        )));
        assert!(
            validate_row_input_type(&syn::parse_quote!(ArrayView<'_, ArrayWriter<i64>>)).is_err()
        );
    }
    #[test]
    fn accepts_checked_dispatch_callbacks_and_rejects_invalid_paths() {
        let function: ItemFn = syn::parse_quote!(
            fn sum(v: Option<ArrayView<'_, i64>>) -> Result<Option<i64>, String> {
                Ok(None)
            }
        );
        let args = syn::parse2(quote!(
            arguments = ["array(bigint)"],
            returns = "bigint",
            call_null_free = "module::nullfree"
        ))
        .unwrap();
        let expanded = expand_row_scalar(args, function.clone())
            .unwrap()
            .to_string();
        assert!(expanded.contains("module :: nullfree"));
        assert!(expanded.contains("contains_nulls"));
        let args = syn::parse2(quote!(
            arguments = ["array(bigint)"],
            returns = "bigint",
            call_ascii = "not a path"
        ))
        .unwrap();
        assert!(expand_row_scalar(args, function).is_err());
    }
    #[test]
    fn ascii_dispatch_is_chosen_once_before_the_row_loop() {
        let function: ItemFn = syn::parse_quote!(
            fn upper(v: Option<&str>) -> Option<String> {
                None
            }
        );
        let args = syn::parse2(quote!(
            arguments = ["varchar"],
            returns = "varchar",
            call_ascii = "module::ascii_upper"
        ))
        .unwrap();
        let expanded = expand_row_scalar(args, function).unwrap().to_string();
        assert!(expanded.contains("module :: ascii_upper"));
        assert!(expanded.find("batch_is_ascii").unwrap() < expanded.find("for __row").unwrap());
        assert!(!expanded.contains("row_is_ascii"));
    }

    #[test]
    fn accepts_lifetime_generic_values_and_typed_variadic_inference() {
        let function: ItemFn = syn::parse_quote!(
            fn first<'a>(
                value: Option<Generic<'a>>,
                rest: Variadic<Option<Generic<'a>>>,
            ) -> Result<Option<Generic<'a>>, String> {
                Ok(value)
            }
        );
        let args = syn::parse2(quote!(
            arguments = ["T"],
            variadic = "T",
            returns = "T",
            type_variables = ["T"]
        ))
        .unwrap();
        let expanded = expand_row_scalar(args, function).unwrap().to_string();
        assert!(expanded.contains("__velox_wasm_error"));
        assert!(expanded.contains("velox.return_type"));
        let function: ItemFn = syn::parse_quote!(
            fn sum(values: Variadic<Option<i64>>) -> Result<i64, String> {
                Ok(0)
            }
        );
        let args = infer_row_signature(syn::parse2(quote!()).unwrap(), &function).unwrap();
        assert_eq!(args.variadic.as_ref().unwrap().value(), "bigint");
        assert_eq!(args.returns.as_ref().unwrap().value(), "bigint");
        let expanded = expand_row_scalar(args, function).unwrap().to_string();
        assert!(expanded.contains("from_row"));
    }

    #[test]
    fn borrowed_variadic_checks_schema_once_and_supports_inferred_and_generic_tails() {
        let function: ItemFn = syn::parse_quote!(
            fn sum(values: VariadicView<'_, Option<i64>>) -> Result<i64, String> {
                Ok(0)
            }
        );
        let args = infer_row_signature(syn::parse2(quote!()).unwrap(), &function).unwrap();
        assert_eq!(args.variadic.as_ref().unwrap().value(), "bigint");
        let expanded = expand_row_scalar(args, function).unwrap().to_string();
        assert!(
            expanded.find("VariadicReader :: new").unwrap() < expanded.find("for __row").unwrap()
        );
        assert!(expanded.contains("__variadic_reader . row"));
        assert!(!expanded.contains("Variadic :: from_row"));
        let function: ItemFn = syn::parse_quote!(
            fn first<'a>(
                value: Option<Generic<'a>>,
                rest: VariadicView<'a, Option<Generic<'a>>>,
            ) -> Option<Generic<'a>> {
                value
            }
        );
        let args = syn::parse2(quote!(
            arguments = ["T"],
            variadic = "T",
            returns = "T",
            type_variables = ["T"]
        ))
        .unwrap();
        assert!(expand_row_scalar(args, function).is_ok());
        for ty in [
            syn::parse_quote!(VariadicView<'a, i64, i32>),
            syn::parse_quote!(VariadicView<'a>),
            syn::parse_quote!(VariadicView<'a, 'b, i64>),
        ] {
            assert!(variadic_element(&ty).is_err());
        }
    }

    #[test]
    fn rejects_batch_parameters_and_batch_results() {
        let args = || {
            syn::parse2(quote!(
                arguments = ["array(bigint)"],
                returns = "array(bigint)"
            ))
            .unwrap()
        };
        for function in [
            syn::parse_quote!(
                fn udf(batch: &RecordBatch) -> Option<Generic<'_>> {
                    None
                }
            ),
            syn::parse_quote!(
                fn udf(value: Generic<'_>) -> Result<ArrayRef, String> {
                    unreachable!()
                }
            ),
        ] {
            assert!(expand_row_scalar(args(), function).is_err());
        }
        assert!(validate_row_value_type(&syn::parse_quote!(Vec<i64>)).is_err());
        assert!(validate_row_value_type(&syn::parse_quote!(&String)).is_err());
    }

    #[test]
    fn rejects_wrong_arity_misplaced_variadic_and_rust_type_generics() {
        let args = || {
            syn::parse2(quote!(
                arguments = ["T"],
                variadic = "T",
                returns = "T",
                type_variables = ["T"]
            ))
            .unwrap()
        };
        for function in [
            syn::parse_quote!(
                fn udf(value: Generic<'_>) -> Generic<'_> {
                    value
                }
            ),
            syn::parse_quote!(
                fn udf(tail: Variadic<Generic<'_>>, value: Generic<'_>) -> Generic<'_> {
                    value
                }
            ),
            syn::parse_quote!(
                fn udf<T>(value: T, tail: Variadic<T>) -> T {
                    value
                }
            ),
            syn::parse_quote!(
                async fn udf(value: Generic<'_>, tail: Variadic<Generic<'_>>) -> Generic<'_> {
                    value
                }
            ),
        ] {
            assert!(expand_row_scalar(args(), function).is_err());
        }
    }
}

/// Derive owned aggregate state footprint/compaction for structs. Shared or
/// allocator-specific fields need their own explicit AggregateState policy.
#[proc_macro_derive(AggregateState)]
pub fn aggregate_state(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as syn::DeriveInput);
    match aggregate_state_impl(input) {
        Ok(tokens) => tokens.into(),
        Err(error) => error.into_compile_error().into(),
    }
}
fn aggregate_state_impl(input: syn::DeriveInput) -> syn::Result<proc_macro2::TokenStream> {
    let syn::Data::Struct(data) = input.data else {
        return Err(syn::Error::new(input.ident.span(), "derive AggregateState supports structs; implement a custom policy for enums/shared ownership"));
    };
    let fields = data
        .fields
        .iter()
        .enumerate()
        .map(|(index, field)| {
            field
                .ident
                .clone()
                .map(syn::Member::Named)
                .unwrap_or_else(|| syn::Member::Unnamed(syn::Index::from(index)))
        })
        .collect::<Vec<_>>();
    let name = input.ident;
    let mut generics = input.generics;
    for field in &data.fields {
        let ty = &field.ty;
        generics
            .make_where_clause()
            .predicates
            .push(syn::parse_quote!(#ty: ::velox_wasm_sdk::AggregateState));
    }
    let (impl_generics, type_generics, where_clause) = generics.split_for_impl();
    Ok(quote! {
        impl #impl_generics ::velox_wasm_sdk::AggregateState for #name #type_generics #where_clause {
            fn heap_bytes(&self) -> ::std::result::Result<u64, String> {
                let mut bytes = 0u64;
                #(bytes = ::velox_wasm_sdk::state::add_bytes(bytes,
                    ::velox_wasm_sdk::AggregateState::heap_bytes(&self.#fields)?)?;)*
                Ok(bytes)
            }
            fn compact(&mut self) -> ::std::result::Result<(), String> {
                #(::velox_wasm_sdk::AggregateState::compact(&mut self.#fields)?;)*
                Ok(())
            }
        }
    })
}

/// Derive complete owned-state checkpointing, independently of SQL intermediates.
#[proc_macro_derive(AggregateCheckpoint)]
pub fn aggregate_checkpoint(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as syn::DeriveInput);
    match aggregate_checkpoint_impl(input) {
        Ok(tokens) => tokens.into(),
        Err(error) => error.into_compile_error().into(),
    }
}
fn aggregate_checkpoint_impl(input: syn::DeriveInput) -> syn::Result<proc_macro2::TokenStream> {
    let syn::Data::Struct(data) = input.data else {
        return Err(syn::Error::new(input.ident.span(),"derive AggregateCheckpoint supports owned structs; implement custom checkpointing for enums/shared ownership"));
    };
    let members = data
        .fields
        .iter()
        .enumerate()
        .map(|(index, field)| {
            field
                .ident
                .clone()
                .map(syn::Member::Named)
                .unwrap_or_else(|| syn::Member::Unnamed(syn::Index::from(index)))
        })
        .collect::<Vec<_>>();
    let mut generics = input.generics;
    let types = data
        .fields
        .iter()
        .map(|field| &field.ty)
        .collect::<Vec<_>>();
    for ty in &types {
        generics
            .make_where_clause()
            .predicates
            .push(syn::parse_quote!(#ty: ::velox_wasm_sdk::AggregateCheckpoint));
    }
    let values = types
        .iter()
        .map(|ty| quote!(<#ty as ::velox_wasm_sdk::AggregateCheckpoint>::decode_checkpoint(input)?))
        .collect::<Vec<_>>();
    let constructor = match &data.fields {
        syn::Fields::Named(_) => quote!(Self { #(#members: #values,)* }),
        syn::Fields::Unnamed(_) => quote!(Self(#(#values,)*)),
        syn::Fields::Unit => quote!(Self),
    };
    let name = input.ident;
    let (impl_generics, type_generics, where_clause) = generics.split_for_impl();
    Ok(quote! {
        impl #impl_generics ::velox_wasm_sdk::AggregateCheckpoint for #name #type_generics #where_clause {
            fn encode_checkpoint(&self,out:&mut ::velox_wasm_sdk::CheckpointWriter)->::std::result::Result<(),String> {
                out.nested(|out| {#(::velox_wasm_sdk::AggregateCheckpoint::encode_checkpoint(&self.#members,out)?;)*Ok(())})
            }
            fn decode_checkpoint(input:&mut ::velox_wasm_sdk::CheckpointReader<'_>)->::std::result::Result<Self,String> {
                input.nested(|input|Ok(#constructor))
            }
        }
    })
}
