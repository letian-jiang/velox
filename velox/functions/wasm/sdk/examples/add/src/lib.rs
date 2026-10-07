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

use std::collections::BTreeMap;
use std::io::Cursor;

use roaring::RoaringBitmap;
use velox_wasm_sdk::{velox_aggregate, velox_scalar, Date32, Timestamp, VeloxAggregate};

#[velox_scalar(arguments = ["array<bigint>"], returns = "array<bigint>")]
pub fn array_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["map<varchar,bigint>"], returns = "map<varchar,bigint>")]
pub fn map_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["row<a:bigint,b:array<varchar>>"], returns = "row<a:bigint,b:array<varchar>>")]
pub fn row_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

// MapView uses the native Simple Function's NULL-as-value key equality.
#[velox_scalar(
    arguments = ["map(K,V)", "K"], returns = "V", type_variables = ["K", "V"]
)]
pub fn map_lookup<'a>(
    map: Option<MapView<'a, Generic<'a>, Generic<'a>>>,
    key: Option<Generic<'a>>,
) -> Result<Option<Generic<'a>>, String> {
    let (Some(map), Some(key)) = (map, key) else {
        return Ok(None);
    };
    Ok(map.find(key)?.flatten())
}

#[velox_scalar(arguments = ["T"], returns = "T", type_variables = ["T"])]
pub fn owned_generic_identity(
    value: Option<Generic<'_>>,
) -> Result<velox_wasm_sdk::Value<'static>, String> {
    value
        .map(|v| v.materialize())
        .transpose()
        .map(|v| v.unwrap_or_default())
}

// Arithmetic uses ordinary Rust trait bounds and build-time monomorphization.
#[velox_scalar(instantiations = [i64, f64])]
pub fn rust_add<T: std::ops::Add<Output = T>>(left: T, right: T) -> T {
    left + right
}

// Rust generic entrypoints: the macro infers SQL variables and nesting.
use velox_wasm_sdk::{SqlComparable, SqlKnown, SqlOrderable, SqlValue};

#[velox_scalar]
pub fn rust_identity<'a, T: SqlValue<'a>>(value: Option<T>) -> Option<T> {
    value
}

#[velox_scalar]
pub fn rust_owned_identity<'a, T: SqlValue<'a>>(
    value: Option<T>,
) -> Result<Option<T::Owned>, String> {
    value.map(T::sql_owned).transpose()
}

#[velox_scalar]
pub fn rust_first<'a, T: SqlValue<'a>>(
    first: Option<T>,
    _rest: velox_wasm_sdk::VariadicView<'a, Option<T>>,
) -> Option<T> {
    first
}

#[velox_scalar]
pub fn rust_coalesce<'a, T: SqlValue<'a>>(
    first: Option<T>,
    rest: velox_wasm_sdk::VariadicView<'a, Option<T>>,
) -> Result<Option<T>, String> {
    if first.is_some() {
        return Ok(first);
    }
    for value in rest.iter() {
        let value = value?;
        if value.is_some() {
            return Ok(value);
        }
    }
    Ok(None)
}

#[velox_scalar(call_ascii = "rust_ascii_identity")]
pub fn rust_ascii_dispatch<'a, T: SqlValue<'a>>(value: Option<T>) -> Option<T> {
    value
}

pub fn rust_ascii_identity<'a, T: SqlValue<'a>>(value: Option<T>) -> Option<T> {
    value
}

#[velox_scalar(call_null_free = "rust_null_free_head")]
pub fn rust_head_dispatch<'a, T: SqlValue<'a>>(
    array: ArrayView<'a, T>,
) -> Result<Option<T>, String> {
    if array.is_empty() {
        Ok(None)
    } else {
        array.at(0)
    }
}

pub fn rust_null_free_head<'a, T: SqlValue<'a>>(
    array: NullFreeArrayView<'a, T>,
) -> Result<Option<T>, String> {
    if array.is_empty() {
        Ok(None)
    } else {
        array.get(0).map(Some)
    }
}

#[velox_scalar]
pub fn rust_array_first<'a, T: SqlValue<'a>>(
    value: Option<ArrayView<'a, T>>,
) -> Result<Option<T>, String> {
    match value {
        None => Ok(None),
        Some(value) if value.is_empty() => Err("empty array".into()),
        Some(value) => value.at(0),
    }
}

#[velox_scalar]
pub fn rust_map_lookup<'a, K, V>(
    map: Option<MapView<'a, K, V>>,
    key: Option<K>,
) -> Result<Option<V>, String>
where
    K: SqlComparable<'a>,
    V: SqlValue<'a>,
{
    match (map, key) {
        (Some(map), Some(key)) => Ok(map.find(key)?.flatten()),
        _ => Ok(None),
    }
}

#[velox_scalar]
pub fn rust_row_first<'a, T: SqlValue<'a>, U: SqlValue<'a>>(
    value: Option<RowView<'a, (T, U)>>,
) -> Result<Option<T>, String> {
    match value {
        Some(value) => value.get::<0>(),
        None => Ok(None),
    }
}

#[velox_scalar]
pub fn rust_equal<'a, T: SqlComparable<'a>>(left: T, right: T) -> Result<Option<bool>, String> {
    left.sql_equals_with(right, velox_wasm_sdk::NullHandling::NullAsIndeterminate)
}

#[velox_scalar]
pub fn rust_known_equal<'a, T: SqlKnown<'a> + SqlComparable<'a>>(
    left: T,
    right: T,
) -> Result<bool, String> {
    left.sql_equals(right)
}

#[velox_scalar]
pub fn rust_less<'a, T: SqlOrderable<'a>>(left: T, right: T) -> Result<bool, String> {
    Ok(
        left.sql_compare(right, velox_wasm_sdk::CompareFlags::default())?
            == Some(std::cmp::Ordering::Less),
    )
}

#[velox_scalar]
pub fn rust_pair<'a, T: SqlValue<'a>>(
    left: Option<T>,
    right: Option<T>,
) -> velox_wasm_sdk::ArrayWriter<T> {
    let mut output = velox_wasm_sdk::ArrayWriter::with_capacity(2);
    output.push(left);
    output.push(right);
    output
}

#[velox_scalar]
pub fn rust_known_identity<'a, T: SqlKnown<'a>>(value: Option<T>) -> Option<T> {
    value
}

// SQL generics are single-row values; the SDK generates the Arrow adapter.
#[velox_scalar(arguments = ["T"], returns = "T", type_variables = ["T"])]
pub fn generic_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["array(T)"], returns = "array(T)", type_variables = ["T"])]
pub fn generic_array_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["array(T)"], returns = "T", type_variables = ["T"])]
pub fn generic_array_first(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Result<Option<velox_wasm_sdk::Generic<'_>>, String> {
    let Some(value) = value else {
        return Ok(None);
    };
    if value.list_len()? == 0 {
        return Err("empty array".to_owned());
    }
    let first = value.element(0)?;
    Ok((!first.is_null()).then_some(first))
}

#[velox_scalar(arguments = ["T"], variadic = "T", returns = "T", type_variables = ["T"])]
pub fn generic_first<'a>(
    first: Option<velox_wasm_sdk::Generic<'a>>,
    _rest: velox_wasm_sdk::VariadicView<'a, Option<velox_wasm_sdk::Generic<'a>>>,
) -> Option<velox_wasm_sdk::Generic<'a>> {
    first
}

#[velox_scalar(arguments = ["T"], returns = "T", type_variables = ["T:orderable"])]
pub fn orderable_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["T"], returns = "T", type_variables = ["T:comparable"])]
pub fn comparable_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["T"], returns = "T", type_variables = ["T:known"])]
pub fn known_identity(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["decimal(p,s)"], returns = "decimal(p,s)", integer_variables = ["p", "s"])]
pub fn decimal_identity(value: Option<velox_wasm_sdk::Decimal>) -> Option<velox_wasm_sdk::Decimal> {
    value
}

#[velox_scalar(arguments = ["decimal(p,s)"], returns = "decimal(r,s)", integer_variables = ["p", "s", "r=min(38,p+1)"])]
pub fn decimal_widen(value: Option<velox_wasm_sdk::Decimal>) -> Option<velox_wasm_sdk::Decimal> {
    value.map(|mut value| {
        value.precision = (value.precision + 1).min(38);
        value
    })
}

#[velox_scalar(arguments = [], variadic = "any", returns = "bigint")]
pub fn variadic_count<'a>(
    values: velox_wasm_sdk::VariadicView<'a, Option<velox_wasm_sdk::Generic<'a>>>,
) -> i64 {
    values.len() as i64
}

// The typed variadic signature is inferred without explicit SQL declarations.
#[velox_scalar]
pub fn variadic_sum(values: velox_wasm_sdk::VariadicView<'_, Option<i64>>) -> Result<i64, String> {
    values.skip_nulls().try_fold(0_i64, |sum, value| {
        sum.checked_add(value?)
            .ok_or_else(|| "sum overflow".to_owned())
    })
}

// Short-circuiting does not decode unused tail values, including non-UTF8 VARCHAR.
#[velox_scalar(arguments = [], variadic = "varchar", returns = "varchar")]
pub fn variadic_first_text<'a>(
    values: velox_wasm_sdk::VariadicView<'a, Option<&'a str>>,
) -> Result<Option<String>, String> {
    values
        .skip_nulls()
        .next()
        .transpose()
        .map(|value| value.map(str::to_owned))
}

#[velox_scalar(arguments = ["T"], returns = "T", type_variables = ["T"], name = "priority_identity")]
pub fn priority_generic(
    value: Option<velox_wasm_sdk::Generic<'_>>,
) -> Option<velox_wasm_sdk::Generic<'_>> {
    value
}

#[velox_scalar(arguments = ["bigint"], returns = "bigint", name = "priority_identity", default_null_behavior = false)]
pub fn priority_concrete(_value: Option<i64>) -> i64 {
    42
}

#[velox_scalar(arguments = ["decimal(p,s)"], returns = "decimal(r,s)", integer_variables = ["p", "s", "r=min(38,p+1)"], name = "priority_identity")]
pub fn priority_decimal(value: Option<velox_wasm_sdk::Decimal>) -> Option<velox_wasm_sdk::Decimal> {
    decimal_widen(value)
}

#[derive(Default)]
struct PrefixState {
    prefix: String,
    suffix: String,
    calls: usize,
}
thread_local! { static INITIALIZED_PREFIX: std::cell::RefCell<PrefixState> = std::cell::RefCell::new(PrefixState::default()); }

pub fn initialize_prefix(context: &velox_wasm_sdk::InitContext<'_>) -> Result<(), String> {
    if context.return_sql_type() != "varchar" || context.argument_sql_type(0) != Some("varchar") {
        return Err("incorrect bound types in initialization".to_owned());
    }
    if context.config("private_token").is_some() {
        return Err("undeclared query configuration was exposed".to_owned());
    }
    if context.config("preferred_output_batch_rows") != Some("1024") {
        return Err("registered query configuration default was not exposed".to_owned());
    }
    let prefix = context
        .constant_value(0)?
        .ok_or("prefix must be a constant")?;
    let value = prefix.get::<Option<&str>>()?.unwrap_or("<null>");
    INITIALIZED_PREFIX.with(|state| {
        let mut state = state.borrow_mut();
        state.calls += 1;
        if state.calls != 1 {
            return Err("initialization retried".to_owned());
        }
        if value == "fail" {
            return Err("requested initialization failure".to_owned());
        }
        state.prefix = value.to_owned();
        state.suffix = context
            .config("wasm_prefix_suffix")
            .unwrap_or("")
            .to_owned();
        Ok(())
    })
}

#[velox_scalar(arguments = ["varchar"], variadic = "varchar", returns = "varchar",
    constant_arguments = [0], initialize = "initialize_prefix",
    config_keys = ["wasm_prefix_suffix", "preferred_output_batch_rows"])]
pub fn initialized_prefix<'a>(
    _prefix: Option<&str>,
    values: velox_wasm_sdk::VariadicView<'a, Option<&'a str>>,
) -> Result<String, String> {
    INITIALIZED_PREFIX.with(|state| {
        let state = state.borrow();
        if state.calls != 1 {
            return Err("prefix was not initialized exactly once".to_owned());
        }
        let mut result = state.prefix.clone();
        for value in values.skip_nulls() {
            result.push_str(value?);
        }
        result.push_str(&state.suffix);
        Ok(result)
    })
}

pub struct ArraySum;

#[velox_aggregate(argument_types = ["array<bigint>"])]
impl VeloxAggregate for ArraySum {
    type State = i64;
    type Input<'a> = (Option<velox_wasm_sdk::arrow_array::ArrayRef>,);
    type Output = i64;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(0)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        use velox_wasm_sdk::arrow_array::{Array, Int64Array, ListArray};
        let Some(array) = input.0 else {
            return Ok(());
        };
        let list = array
            .as_any()
            .downcast_ref::<ListArray>()
            .ok_or("expected an Arrow list")?;
        let values = list.value(0);
        let values = values
            .as_any()
            .downcast_ref::<Int64Array>()
            .ok_or("expected Arrow Int64 list values")?;
        for index in 0..values.len() {
            if !values.is_null(index) {
                *state = state
                    .checked_add(values.value(index))
                    .ok_or("sum overflow")?;
            }
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        Ok(state.to_le_bytes().to_vec())
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        let bytes: [u8; 8] = intermediate
            .try_into()
            .map_err(|_| "invalid array sum intermediate".to_owned())?;
        *state = state
            .checked_add(i64::from_le_bytes(bytes))
            .ok_or("sum overflow")?;
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok(*state)
    }
}

pub struct ArrayCollect;

#[velox_aggregate(return_type = "array<bigint>")]
impl VeloxAggregate for ArrayCollect {
    type State = Vec<i64>;
    type Input<'a> = (Option<i64>,);
    type Output = velox_wasm_sdk::arrow_array::ArrayRef;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(Vec::new())
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            state.push(value);
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut bytes = Vec::with_capacity(state.len() * 8);
        for value in state {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        Ok(bytes)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() % 8 != 0 {
            return Err("invalid array collect intermediate".to_owned());
        }
        for chunk in intermediate.chunks_exact(8) {
            state.push(i64::from_le_bytes(chunk.try_into().unwrap()));
        }
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        use velox_wasm_sdk::arrow_array::{types::Int64Type, ListArray};
        let values: Vec<Option<i64>> = state.iter().copied().map(Some).collect();
        Ok(std::sync::Arc::new(ListArray::from_iter_primitive::<
            Int64Type,
            _,
            _,
        >(vec![Some(values)])))
    }
}

#[velox_scalar]
pub fn add_i64(left: i64, right: i64) -> Result<i64, String> {
    left.checked_add(right)
        .ok_or_else(|| "integer overflow".to_owned())
}

#[velox_scalar(name = "overloaded_add")]
pub fn overloaded_add_i64(left: i64, right: i64) -> Result<i64, String> {
    left.checked_add(right)
        .ok_or_else(|| "integer overflow".to_owned())
}

#[velox_scalar(name = "overloaded_add")]
pub fn overloaded_add_f64(left: f64, right: f64) -> f64 {
    left + right
}

#[velox_scalar]
pub fn prefix(prefix: &str, value: Option<&str>) -> Option<String> {
    value.map(|value| {
        let mut result = String::with_capacity(prefix.len() + value.len());
        result.push_str(prefix);
        result.push_str(value);
        result
    })
}

#[velox_scalar(name = "not_bool")]
pub fn not(value: bool) -> bool {
    !value
}

#[velox_scalar]
pub fn widen(a: i8, b: i16, c: i32) -> i64 {
    i64::from(a) + i64::from(b) + i64::from(c)
}

#[velox_scalar]
pub fn hypotenuse(a: f32, b: f64) -> f64 {
    f64::from(a).hypot(b)
}

#[velox_scalar]
pub fn binary_length(value: &[u8]) -> i32 {
    value.len() as i32
}

#[velox_scalar]
pub fn next_date(value: Date32) -> Date32 {
    Date32(value.0 + 1)
}

#[velox_scalar]
pub fn timestamp_identity(value: Timestamp) -> Timestamp {
    value
}

#[velox_scalar]
pub fn checked_divide(dividend: i64, divisor: i64) -> Result<i64, String> {
    if divisor == 0 {
        Err("division by zero".to_owned())
    } else {
        dividend
            .checked_div(divisor)
            .ok_or_else(|| "integer overflow".to_owned())
    }
}

fn deserialize_bitmap(bytes: &[u8]) -> Result<RoaringBitmap, String> {
    let mut input = Cursor::new(bytes);
    let bitmap = RoaringBitmap::deserialize_from(&mut input)
        .map_err(|error| format!("invalid RoaringBitmap payload: {error}"))?;
    if input.position() != bytes.len() as u64 {
        return Err("RoaringBitmap payload contains trailing bytes".to_owned());
    }
    Ok(bitmap)
}

fn serialize_bitmap(bitmap: &RoaringBitmap) -> Result<Vec<u8>, String> {
    let mut output = Vec::with_capacity(bitmap.serialized_size());
    bitmap
        .serialize_into(&mut output)
        .map_err(|error| format!("cannot serialize RoaringBitmap: {error}"))?;
    Ok(output)
}

#[velox_scalar]
pub fn bitmap_union(left: &[u8], right: &[u8]) -> Result<Vec<u8>, String> {
    let mut result = deserialize_bitmap(left)?;
    result |= deserialize_bitmap(right)?;
    serialize_bitmap(&result)
}

#[velox_scalar]
pub fn bitmap_intersection(left: &[u8], right: &[u8]) -> Result<Vec<u8>, String> {
    let mut result = deserialize_bitmap(left)?;
    result &= deserialize_bitmap(right)?;
    serialize_bitmap(&result)
}

#[velox_scalar]
pub fn bitmap_difference(left: &[u8], right: &[u8]) -> Result<Vec<u8>, String> {
    let mut result = deserialize_bitmap(left)?;
    result -= deserialize_bitmap(right)?;
    serialize_bitmap(&result)
}

pub struct Average;

pub struct AverageState {
    sum: f64,
    count: u64,
}

#[velox_aggregate(prefix = "avg_f64")]
impl VeloxAggregate for Average {
    type State = AverageState;
    type Input<'a> = (Option<f64>,);
    type Output = Option<f64>;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(AverageState { sum: 0.0, count: 0 })
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            state.sum += value;
            state.count += 1;
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut output = Vec::with_capacity(16);
        output.extend_from_slice(&state.sum.to_le_bytes());
        output.extend_from_slice(&state.count.to_le_bytes());
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() != 16 {
            return Err("average intermediate must contain 16 bytes".to_owned());
        }
        state.sum += f64::from_le_bytes(intermediate[0..8].try_into().unwrap());
        state.count += u64::from_le_bytes(intermediate[8..16].try_into().unwrap());
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok((state.count != 0).then(|| state.sum / state.count as f64))
    }
}

pub struct SumI64;

#[velox_aggregate]
impl VeloxAggregate for SumI64 {
    type State = Option<i64>;
    type Input<'a> = (Option<i64>,);
    type Output = Option<i64>;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(None)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            *state = Some(
                state
                    .unwrap_or(0)
                    .checked_add(value)
                    .ok_or("sum overflow")?,
            );
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut output = Vec::with_capacity(9);
        output.push(u8::from(state.is_some()));
        if let Some(value) = state {
            output.extend_from_slice(&value.to_le_bytes());
        }
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        match intermediate {
            [0] => Ok(()),
            [1, bytes @ ..] if bytes.len() == 8 => Self::update(
                state,
                (Some(i64::from_le_bytes(bytes.try_into().unwrap())),),
            ),
            _ => Err("invalid sum intermediate".to_owned()),
        }
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok(*state)
    }
}

pub struct LongestString;

#[velox_aggregate(ignore_duplicates = true)]
impl VeloxAggregate for LongestString {
    type State = Option<String>;
    type Input<'a> = (Option<&'a str>,);
    type Output = Option<String>;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(None)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            if state
                .as_ref()
                .map_or(true, |current| value.len() > current.len())
            {
                *state = Some(value.to_owned());
            }
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut output = Vec::new();
        output.push(u8::from(state.is_some()));
        if let Some(value) = state {
            output.extend_from_slice(value.as_bytes());
        }
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        let Some((&present, bytes)) = intermediate.split_first() else {
            return Err("invalid longest-string intermediate".to_owned());
        };
        match present {
            0 if bytes.is_empty() => Ok(()),
            1 => Self::update(
                state,
                (Some(
                    std::str::from_utf8(bytes).map_err(|error| error.to_string())?,
                ),),
            ),
            _ => Err("invalid longest-string intermediate".to_owned()),
        }
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok(state.clone())
    }
}

pub struct FunnelSteps;

#[velox_aggregate(ignore_duplicates = true)]
impl VeloxAggregate for FunnelSteps {
    type State = u64;
    type Input<'a> = (Option<i64>,);
    type Output = i64;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(0)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(stage) = input.0 {
            if !(1..=63).contains(&stage) {
                return Err("funnel stage must be between 1 and 63".to_owned());
            }
            *state |= 1_u64 << (stage - 1);
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        Ok(state.to_le_bytes().to_vec())
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() != 8 {
            return Err("funnel intermediate must contain 8 bytes".to_owned());
        }
        *state |= u64::from_le_bytes(intermediate.try_into().unwrap());
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        let completed = (!state).trailing_zeros().min(63);
        Ok(i64::from(completed))
    }
}

pub struct WindowFunnel;

pub struct WindowFunnelState {
    window: Option<i64>,
    events: BTreeMap<i64, u64>,
}

#[velox_aggregate(ignore_duplicates = true)]
impl VeloxAggregate for WindowFunnel {
    type State = WindowFunnelState;
    type Input<'a> = (Option<i64>, Option<i64>, Option<i64>);
    type Output = i64;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(WindowFunnelState {
            window: None,
            events: BTreeMap::new(),
        })
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        let (Some(timestamp), Some(stage), Some(window)) = input else {
            return Ok(());
        };
        if window < 0 {
            return Err("window must be non-negative".to_owned());
        }
        if !(1..=63).contains(&stage) {
            return Err("funnel stage must be between 1 and 63".to_owned());
        }
        Self::set_window(state, window)?;
        *state.events.entry(timestamp).or_default() |= 1_u64 << (stage - 1);
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let event_count = u32::try_from(state.events.len())
            .map_err(|_| "too many window-funnel events".to_owned())?;
        let mut output = Vec::with_capacity(12 + state.events.len() * 16);
        output.extend_from_slice(&state.window.unwrap_or(-1).to_le_bytes());
        output.extend_from_slice(&event_count.to_le_bytes());
        for (&timestamp, &stage_mask) in &state.events {
            output.extend_from_slice(&timestamp.to_le_bytes());
            output.extend_from_slice(&stage_mask.to_le_bytes());
        }
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() < 12 || (intermediate.len() - 12) % 16 != 0 {
            return Err("invalid window-funnel intermediate state".to_owned());
        }
        let window = i64::from_le_bytes(intermediate[0..8].try_into().unwrap());
        let event_count = u32::from_le_bytes(intermediate[8..12].try_into().unwrap()) as usize;
        if intermediate.len() != 12 + event_count * 16 {
            return Err("invalid window-funnel intermediate state".to_owned());
        }
        if window == -1 && event_count == 0 {
            return Ok(());
        }
        if window < 0 {
            return Err("invalid window-funnel window".to_owned());
        }
        Self::set_window(state, window)?;
        for event in intermediate[12..].chunks_exact(16) {
            let timestamp = i64::from_le_bytes(event[0..8].try_into().unwrap());
            let stage_mask = u64::from_le_bytes(event[8..16].try_into().unwrap());
            *state.events.entry(timestamp).or_default() |= stage_mask;
        }
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        let Some(window) = state.window else {
            return Ok(0);
        };
        let mut starts = [None; 63];
        let mut completed = 0_usize;
        for (&timestamp, &stage_mask) in &state.events {
            for stage in 0..63 {
                if stage_mask & (1_u64 << stage) == 0 {
                    continue;
                }
                if stage == 0 {
                    starts[0] = Some(timestamp);
                    completed = completed.max(1);
                    continue;
                }
                let Some(start) = starts[stage - 1] else {
                    continue;
                };
                if timestamp
                    .checked_sub(start)
                    .is_some_and(|elapsed| elapsed <= window)
                {
                    starts[stage] = Some(starts[stage].map_or(start, |current| current.max(start)));
                    completed = completed.max(stage + 1);
                }
            }
        }
        Ok(completed as i64)
    }
}

impl WindowFunnel {
    fn set_window(state: &mut WindowFunnelState, window: i64) -> Result<(), String> {
        match state.window {
            None => state.window = Some(window),
            Some(current) if current != window => {
                return Err("window must be constant within each group".to_owned());
            }
            Some(_) => {}
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rust_generic_exports_remain_rust_generic_functions() {
        use velox_wasm_sdk::{
            arrow_array::{types::Int64Type, GenericListArray, Int64Array},
            Generic, VariadicView,
        };
        assert_eq!(rust_add(20_i64, 22_i64), 42);
        assert_eq!(rust_add(1.5_f64, 2.25_f64), 3.75);
        assert_eq!(rust_owned_identity(Some(42_i64)), Ok(Some(42)));
        assert_eq!(
            rust_owned_identity(Some("hello")),
            Ok(Some("hello".to_owned()))
        );
        assert_eq!(rust_identity::<i64>(Some(42)), Some(42));
        assert_eq!(rust_identity::<&str>(Some("hello")), Some("hello"));
        assert_eq!(
            rust_first(Some(42), VariadicView::from_values(&[])),
            Some(42)
        );
        assert_eq!(
            rust_coalesce(None, VariadicView::from_values(&[None, Some("hello")])),
            Ok(Some("hello"))
        );
        assert_eq!(rust_equal(f64::NAN, f64::NAN), Ok(Some(true)));
        assert_eq!(rust_less(1.0, f64::NAN), Ok(true));
        let list = GenericListArray::<i32>::from_iter_primitive::<Int64Type, _, _>([Some(vec![
            Some(42),
            None,
        ])]);
        let array = Generic::new(&list, 0).unwrap().as_array::<i64>().unwrap();
        assert_eq!(rust_array_first(Some(array)), Ok(Some(42)));
        let integers = Int64Array::from(vec![42]);
        let value = Generic::new(&integers, 0).unwrap();
        assert_eq!(
            rust_identity(Some(value)).unwrap().get::<i64>().unwrap(),
            42
        );
    }

    #[test]
    fn row_function_remains_directly_testable() {
        assert_eq!(add_i64(20, 22), Ok(42));
        assert!(add_i64(i64::MAX, 1).is_err());
        assert!(checked_divide(i64::MIN, -1).is_err());
        assert_eq!(checked_divide(10, 2), Ok(5));
        assert_eq!(checked_divide(84, 2), Ok(42));
        assert_eq!(checked_divide(84, 0), Err("division by zero".to_owned()));

        let left: RoaringBitmap = [1, 2, 65_537].into_iter().collect();
        let right: RoaringBitmap = [2, 3, 65_537].into_iter().collect();
        let left = serialize_bitmap(&left).unwrap();
        let right = serialize_bitmap(&right).unwrap();
        assert_eq!(
            deserialize_bitmap(&bitmap_union(&left, &right).unwrap()).unwrap(),
            [1, 2, 3, 65_537].into_iter().collect()
        );
        assert_eq!(
            deserialize_bitmap(&bitmap_intersection(&left, &right).unwrap()).unwrap(),
            [2, 65_537].into_iter().collect()
        );
        assert_eq!(
            deserialize_bitmap(&bitmap_difference(&left, &right).unwrap()).unwrap(),
            [1].into_iter().collect()
        );
    }

    #[test]
    fn generic_and_variadic_functions_are_directly_testable_per_row() {
        use velox_wasm_sdk::{
            arrow_array::{Int64Array, StringArray},
            Generic, VariadicView,
        };
        assert_eq!(
            variadic_sum(VariadicView::from_values(&[Some(20), None, Some(22)])),
            Ok(42)
        );
        assert_eq!(variadic_sum(VariadicView::from_values(&[])), Ok(0));
        assert_eq!(
            variadic_sum(VariadicView::from_values(&[Some(i64::MAX), Some(1)])),
            Err("sum overflow".into())
        );
        let integers = Int64Array::from(vec![Some(42), None]);
        let strings = StringArray::from(vec!["hello"]);
        let number = Generic::new(&integers, 0).unwrap();
        let text = Generic::new(&strings, 0).unwrap();
        assert_eq!(
            generic_identity(Some(number))
                .unwrap()
                .get::<i64>()
                .unwrap(),
            42
        );
        assert!(generic_identity(None).is_none());
        assert_eq!(
            generic_first(Some(text), VariadicView::from_values(&[Some(text)]))
                .unwrap()
                .get::<&str>()
                .unwrap(),
            "hello"
        );
        assert_eq!(
            variadic_count(VariadicView::from_values(&[Some(number), Some(text), None])),
            3
        );
        let list = velox_wasm_sdk::arrow_array::ListArray::from_iter_primitive::<
            velox_wasm_sdk::arrow_array::types::Int64Type,
            _,
            _,
        >([Some(vec![Some(42), None]), Some(vec![]), None]);
        let first = Generic::new(&list, 0).unwrap();
        assert_eq!(
            generic_array_first(Some(first))
                .unwrap()
                .unwrap()
                .get::<i64>()
                .unwrap(),
            42
        );
        assert!(first.element(1).unwrap().is_null());
        assert!(generic_array_first(Some(Generic::new(&list, 1).unwrap())).is_err());
        assert!(generic_array_first(None).unwrap().is_none());
        assert_eq!(integers.len(), 2);
    }

    #[test]
    fn aggregate_row_apis_remain_directly_testable() {
        let mut average = Average::create().unwrap();
        Average::update(&mut average, (Some(2.0),)).unwrap();
        Average::update(&mut average, (None,)).unwrap();
        Average::update(&mut average, (Some(4.0),)).unwrap();
        let serialized = Average::serialize(&average).unwrap();
        let mut merged_average = Average::create().unwrap();
        Average::merge(&mut merged_average, &serialized).unwrap();
        assert_eq!(Average::finalize(&merged_average).unwrap(), Some(3.0));

        let mut sum = SumI64::create().unwrap();
        SumI64::update(&mut sum, (Some(20),)).unwrap();
        SumI64::update(&mut sum, (Some(22),)).unwrap();
        assert_eq!(SumI64::finalize(&sum).unwrap(), Some(42));

        let mut longest = LongestString::create().unwrap();
        LongestString::update(&mut longest, (Some("short"),)).unwrap();
        LongestString::update(&mut longest, (Some("a longer value"),)).unwrap();
        assert_eq!(
            LongestString::finalize(&longest).unwrap().as_deref(),
            Some("a longer value")
        );

        let mut funnel = FunnelSteps::create().unwrap();
        FunnelSteps::update(&mut funnel, (Some(1),)).unwrap();
        FunnelSteps::update(&mut funnel, (Some(3),)).unwrap();
        assert_eq!(FunnelSteps::finalize(&funnel).unwrap(), 1);
        let serialized = FunnelSteps::serialize(&funnel).unwrap();
        let mut merged_funnel = FunnelSteps::create().unwrap();
        FunnelSteps::merge(&mut merged_funnel, &serialized).unwrap();
        FunnelSteps::update(&mut merged_funnel, (Some(2),)).unwrap();
        assert_eq!(FunnelSteps::finalize(&merged_funnel).unwrap(), 3);

        let mut window_funnel = WindowFunnel::create().unwrap();
        WindowFunnel::update(&mut window_funnel, (Some(100), Some(1), Some(30))).unwrap();
        WindowFunnel::update(&mut window_funnel, (Some(120), Some(2), Some(30))).unwrap();
        WindowFunnel::update(&mut window_funnel, (Some(140), Some(3), Some(30))).unwrap();
        assert_eq!(WindowFunnel::finalize(&window_funnel).unwrap(), 2);

        let serialized = WindowFunnel::serialize(&window_funnel).unwrap();
        let mut merged_window_funnel = WindowFunnel::create().unwrap();
        WindowFunnel::merge(&mut merged_window_funnel, &serialized).unwrap();
        WindowFunnel::update(&mut merged_window_funnel, (Some(125), Some(3), Some(30))).unwrap();
        assert_eq!(WindowFunnel::finalize(&merged_window_funnel).unwrap(), 3);
    }
}

// These functions only see one SQL row, including recursively nested children.
use velox_wasm_sdk::{
    ArrayView, ArrayWriter, CompareFlags, Generic, MapView, MapWriter, NullFreeArrayView,
    NullHandling, RowView, RowWriter, Value,
};

#[velox_scalar]
pub fn nested_array_increment(
    values: Option<ArrayView<'_, i64>>,
) -> Result<Option<ArrayWriter<i64>>, String> {
    let Some(values) = values else {
        return Ok(None);
    };
    let mut output = ArrayWriter::with_capacity(values.len());
    for value in values.iter() {
        output.push(
            value?
                .map(|v| v.checked_add(1).ok_or("array increment overflow"))
                .transpose()?,
        );
    }
    Ok(Some(output))
}

#[velox_scalar]
pub fn nested_map_copy(
    values: Option<MapView<'_, &str, ArrayView<'_, i64>>>,
) -> Result<Option<MapWriter<String, ArrayWriter<i64>>>, String> {
    values.map(|v| v.materialize()).transpose()
}

#[velox_scalar(arguments=["row(\"id\" bigint,\"values\" array(varchar))"], returns="row(\"id\" bigint,\"values\" array(varchar))")]
pub fn nested_row_copy(
    value: Option<RowView<'_, (i64, ArrayView<'_, &str>)>>,
) -> Result<Option<RowWriter<(Option<i64>, Option<ArrayWriter<String>>)>>, String> {
    let Some(value) = value else {
        return Ok(None);
    };
    Ok(Some(RowWriter::new((
        value.get::<0>()?,
        value.get::<1>()?.map(|v| v.materialize()).transpose()?,
    ))))
}

/// Named ROW signatures require the declared field names. Generic T rows
/// retain the field names of their bound input type.
#[velox_scalar(arguments=["row(\"id\" bigint,\"values\" array(varchar))"], returns="bigint")]
pub fn named_row_length(
    value: Option<RowView<'_, (i64, ArrayView<'_, velox_wasm_sdk::VarcharBytes<&[u8]>>)>>,
) -> Result<Option<i64>, String> {
    let Some(value) = value else {
        return Ok(None);
    };
    let id = value.field("id")?.get::<Option<i64>>()?;
    let values = value.field("values")?;
    if values.is_null() {
        return Ok(None);
    }
    id.map(|id| {
        id.checked_add(
            values
                .as_array::<velox_wasm_sdk::VarcharBytes<&[u8]>>()?
                .len() as i64,
        )
        .ok_or_else(|| "row length overflow".into())
    })
    .transpose()
}

#[velox_scalar(arguments=["T"], returns="array(T)", type_variables=["T"])]
pub fn generic_wrap(value: Option<Generic<'_>>) -> ArrayWriter<Generic<'_>> {
    ArrayWriter::from(vec![value])
}

#[velox_scalar(arguments=["T"], returns="bigint", type_variables=["T"], default_null_behavior=false)]
pub fn generic_hash(value: Generic<'_>) -> Result<i64, String> {
    Ok(value.hash()? as i64)
}

#[velox_scalar(arguments=["T","T"], returns="bigint", type_variables=["T"], default_null_behavior=false)]
pub fn generic_compare(left: Generic<'_>, right: Generic<'_>) -> Result<i64, String> {
    Ok(
        match left.compare(&right, CompareFlags::default())?.unwrap() {
            std::cmp::Ordering::Less => -1,
            std::cmp::Ordering::Equal => 0,
            std::cmp::Ordering::Greater => 1,
        },
    )
}

#[velox_scalar(arguments=["T","T"], returns="boolean", type_variables=["T:comparable"], default_null_behavior=false)]
pub fn generic_equal(left: Generic<'_>, right: Generic<'_>) -> Result<bool, String> {
    left.equals(&right)
}

#[velox_scalar(arguments=["T","T"], returns="boolean", type_variables=["T:comparable"], default_null_behavior=false)]
pub fn generic_equal_sql(left: Generic<'_>, right: Generic<'_>) -> Result<Option<bool>, String> {
    Ok(left
        .compare(
            &right,
            CompareFlags::equality(NullHandling::NullAsIndeterminate),
        )?
        .map(|v| v == std::cmp::Ordering::Equal))
}

// Like a native function with only callNullFree: recursively NULL inputs produce
// NULL without calling user code. get() returns T rather than Option<T> here.
#[velox_scalar]
pub fn nullfree_array_sum(values: NullFreeArrayView<'_, i64>) -> Result<i64, String> {
    values.iter().try_fold(0_i64, |sum, value| {
        sum.checked_add(value?)
            .ok_or_else(|| "sum overflow".to_owned())
    })
}

#[velox_scalar(arguments=[], returns="row(\"items\" array(map(varchar,array(bigint))))")]
pub fn make_nested() -> Value<'static> {
    Value::Row(vec![Value::Array(vec![
        Value::Map(vec![(
            Value::Varchar("key".into()),
            Value::Array(vec![Value::BigInt(42), Value::Null]),
        )]),
        Value::Null,
    ])])
}

#[velox_scalar]
pub fn hugeint_identity(value: Option<i128>) -> Option<i128> {
    value
}

#[velox_scalar(name="mixed_metadata", arguments=["bigint"], returns="bigint")]
pub fn mixed_metadata_integer(value: i64) -> i64 {
    value
}
#[velox_scalar(name="mixed_metadata", arguments=["varchar"], returns="varchar", deterministic=false)]
pub fn mixed_metadata_string(value: Option<&str>) -> String {
    value.unwrap_or("NULL received").to_owned()
}

#[velox_scalar(arguments=["varchar"],returns="varchar",call_ascii="upper_ascii")]
pub fn upper_dispatch(value: Option<&str>) -> Option<String> {
    value.map(str::to_uppercase)
}
pub fn upper_ascii(value: Option<&str>) -> Option<String> {
    value.map(str::to_ascii_uppercase)
}

#[velox_scalar(arguments=["array(bigint)"],returns="bigint",call_null_free="array_sum_nullfree")]
pub fn array_sum_dispatch(value: Option<ArrayView<'_, i64>>) -> Result<Option<i64>, String> {
    value
        .map(|value| {
            value.skip_nulls().try_fold(0_i64, |sum, v| {
                sum.checked_add(v?).ok_or_else(|| "sum overflow".to_owned())
            })
        })
        .transpose()
}
pub fn array_sum_nullfree(value: NullFreeArrayView<'_, i64>) -> Result<Option<i64>, String> {
    value
        .into_iter()
        .try_fold(0_i64, |sum, v| {
            sum.checked_add(v?).ok_or_else(|| "sum overflow".to_owned())
        })
        .map(Some)
}

#[velox_scalar]
pub fn varchar_bytes_identity(
    value: Option<velox_wasm_sdk::VarcharBytes<&[u8]>>,
) -> Option<velox_wasm_sdk::VarcharBytes<&[u8]>> {
    value
}

#[velox_scalar]
pub fn status_result(value: i64) -> Result<i64, velox_wasm_sdk::RowError> {
    if value == 1 {
        Err(velox_wasm_sdk::RowError::new(
            velox_wasm_sdk::StatusCode::UserError,
            "typed user error",
        ))
    } else if value == 2 {
        Err(velox_wasm_sdk::RowError::new(
            velox_wasm_sdk::StatusCode::TypeError,
            "typed system error",
        ))
    } else {
        Ok(value)
    }
}

#[velox_scalar(arguments=["T"],returns="T",type_variables=["T"])]
pub fn opaque_identity(
    value: Option<velox_wasm_sdk::OpaqueHandle<'_>>,
) -> Option<velox_wasm_sdk::OpaqueHandle<'_>> {
    value
}

#[velox_scalar(arguments=["T"],returns="T",type_variables=["T"])]
pub fn opaque_forge(
    value: Option<velox_wasm_sdk::OpaqueHandle<'_>>,
) -> Option<velox_wasm_sdk::OpaqueHandle<'_>> {
    value.map(|mut v| {
        v.handle = u64::MAX;
        v
    })
}

#[velox_scalar(arguments=["T"],returns="T",type_variables=["T"])]
pub fn codec_increment(
    value: Option<velox_wasm_sdk::CustomValue<&[u8]>>,
) -> Result<Option<velox_wasm_sdk::CustomValue<Vec<u8>>>, String> {
    value
        .map(|v| {
            let n = i64::from_le_bytes(
                v.payload
                    .try_into()
                    .map_err(|_| "expected eight-byte integer codec")?,
            );
            let n = n.checked_add(1).ok_or("codec integer overflow")?;
            Ok(v.map_payload(|_| n.to_le_bytes().to_vec()))
        })
        .transpose()
}

#[velox_scalar(arguments=["T"],returns="T",type_variables=["T"])]
pub fn codec_wrong_version(
    value: Option<velox_wasm_sdk::CustomValue<&[u8]>>,
) -> Option<velox_wasm_sdk::CustomValue<Vec<u8>>> {
    value.map(|mut v| {
        v.version += 1;
        v.map_payload(|p| p.to_vec())
    })
}

fn low8(bytes: &[u8]) -> Result<i64, String> {
    Ok(i64::from_le_bytes(
        bytes
            .try_into()
            .map_err(|_| "expected eight-byte integer codec")?,
    ) & 255)
}
fn compare_low8(
    a: &[u8],
    b: &[u8],
    flags: CompareFlags,
) -> Result<Option<std::cmp::Ordering>, String> {
    let order = low8(a)?.cmp(&low8(b)?);
    Ok(Some(if flags.ascending {
        order
    } else {
        order.reverse()
    }))
}
fn hash_low8(bytes: &[u8]) -> Result<u64, String> {
    velox_wasm_sdk::Value::BigInt(low8(bytes)?).hash()
}
pub fn initialize_low8(_: &velox_wasm_sdk::InitContext<'_>) -> Result<(), String> {
    velox_wasm_sdk::register_codec_semantics(
        "low8_i64",
        1,
        velox_wasm_sdk::CodecSemantics {
            compare: compare_low8,
            hash: hash_low8,
        },
    )
}

#[velox_scalar(arguments=["T"], returns="bigint", type_variables=["T"], default_null_behavior=false,initialize="initialize_low8")]
pub fn codec_hash(value: Generic<'_>) -> Result<i64, String> {
    Ok(value.hash()? as i64)
}

#[velox_scalar(arguments=["T","T"], returns="bigint", type_variables=["T"], default_null_behavior=false,initialize="initialize_low8")]
pub fn codec_compare(left: Generic<'_>, right: Generic<'_>) -> Result<i64, String> {
    Ok(
        match left.compare(&right, CompareFlags::default())?.unwrap() {
            std::cmp::Ordering::Less => -1,
            std::cmp::Ordering::Equal => 0,
            std::cmp::Ordering::Greater => 1,
        },
    )
}

#[velox_scalar(arguments=["T","T"], returns="boolean", type_variables=["T:comparable"], default_null_behavior=false,initialize="initialize_low8")]
pub fn codec_equal(left: Generic<'_>, right: Generic<'_>) -> Result<bool, String> {
    left.equals(&right)
}

#[velox_scalar(arguments=["T","T"], returns="boolean", type_variables=["T:comparable"], default_null_behavior=false,initialize="initialize_low8")]
pub fn codec_equal_sql(left: Generic<'_>, right: Generic<'_>) -> Result<Option<bool>, String> {
    Ok(left
        .compare(
            &right,
            CompareFlags::equality(NullHandling::NullAsIndeterminate),
        )?
        .map(|v| v == std::cmp::Ordering::Equal))
}

/// Generic first value, including a first SQL NULL. The typed intermediate
/// carries T between partial/intermediate/final stages.
pub struct FirstValueWasm;

#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["T"], returns = "T", type_variables = ["T"],
    intermediate = "row(value T,seen boolean)", order_sensitive = true, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for FirstValueWasm {
    type State = (velox_wasm_sdk::Value<'static>, bool);
    type Input<'a> = (Option<velox_wasm_sdk::Generic<'a>>,);
    type Error = String;

    fn create(_context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, String> {
        Ok((velox_wasm_sdk::Value::Null, false))
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), String> {
        if !state.1 {
            state.0 = input
                .0
                .map(|value| value.materialize())
                .transpose()?
                .unwrap_or(velox_wasm_sdk::Value::Null);
            state.1 = true;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, String> {
        Ok(velox_wasm_sdk::Value::Row(vec![
            state.0.materialize()?,
            velox_wasm_sdk::Value::Boolean(state.1),
        ]))
    }
    fn merge(state: &mut Self::State, input: velox_wasm_sdk::Generic<'_>) -> Result<(), String> {
        let row = input.as_row::<velox_wasm_sdk::DynamicRow>()?;
        if !state.1 && row.at(1)?.get::<bool>()? {
            state.0 = row.at(0)?.materialize()?;
            state.1 = true;
        }
        Ok(())
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, String> {
        state.0.materialize()
    }
}

pub struct VariadicSumWasm;

/// Anonymous ROW intermediates bind by position, as native signatures do.
pub struct PositionalSumWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["bigint"], returns = "bigint",
    intermediate = "row(hugeint,bigint)", default_null_behavior = true, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for PositionalSumWasm {
    type State = (i128, i64);
    type Input<'a> = (Option<i64>,);
    type Error = velox_wasm_sdk::RowError;
    fn create(_: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, Self::Error> {
        Ok((0, 0))
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            Self::add(state, value as i128, 1)?;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(velox_wasm_sdk::Value::Row(vec![
            velox_wasm_sdk::Value::HugeInt(state.0),
            velox_wasm_sdk::Value::BigInt(state.1),
        ]))
    }
    fn merge(
        state: &mut Self::State,
        input: velox_wasm_sdk::Generic<'_>,
    ) -> Result<(), Self::Error> {
        let row = input
            .as_row::<velox_wasm_sdk::DynamicRow>()
            .map_err(Self::error)?;
        Self::add(
            state,
            row.at(0)
                .and_then(|v| v.get::<i128>())
                .map_err(Self::error)?,
            row.at(1)
                .and_then(|v| v.get::<i64>())
                .map_err(Self::error)?,
        )
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(if state.1 == 0 {
            velox_wasm_sdk::Value::Null
        } else {
            velox_wasm_sdk::Value::BigInt(i64::try_from(state.0).map_err(Self::error)?)
        })
    }
}
impl PositionalSumWasm {
    fn error(message: impl ToString) -> velox_wasm_sdk::RowError {
        velox_wasm_sdk::RowError::new(velox_wasm_sdk::StatusCode::UserError, message.to_string())
    }
    fn add(
        state: &mut (i128, i64),
        total: i128,
        count: i64,
    ) -> Result<(), velox_wasm_sdk::RowError> {
        state.0 = state
            .0
            .checked_add(total)
            .ok_or_else(|| Self::error("sum overflow"))?;
        state.1 = state
            .1
            .checked_add(count)
            .ok_or_else(|| Self::error("count overflow"))?;
        Ok(())
    }
}
#[derive(velox_wasm_sdk::AggregateState, velox_wasm_sdk::AggregateCheckpoint)]
pub struct VariadicSumState {
    sum: i128,
    count: i64,
    factor: i64,
}

#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["bigint"], variadic = "bigint", returns = "bigint",
    intermediate = "row(total hugeint,count bigint)", constant_arguments = [0],
    config_keys = ["wasm_aggregate_multiplier"], checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for VariadicSumWasm {
    type State = VariadicSumState;
    type Input<'a> = (Option<i64>, velox_wasm_sdk::VariadicView<'a, Option<i64>>);
    type Error = String;

    fn create(context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, String> {
        let constant = context
            .constant_value(0)?
            .map(|value| value.get::<Option<i64>>())
            .transpose()?
            .flatten()
            .unwrap_or(1);
        let multiplier = context
            .config("wasm_aggregate_multiplier")
            .unwrap_or("1")
            .parse::<i64>()
            .map_err(|e| e.to_string())?;
        Ok(VariadicSumState {
            sum: 0,
            count: 0,
            factor: constant
                .checked_mul(multiplier)
                .ok_or("multiplier overflow")?,
        })
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), String> {
        for value in input.1.skip_nulls() {
            let value = value? as i128 * state.factor as i128;
            state.sum = state.sum.checked_add(value).ok_or("sum overflow")?;
            state.count = state.count.checked_add(1).ok_or("count overflow")?;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, String> {
        Ok(velox_wasm_sdk::Value::Row(vec![
            velox_wasm_sdk::Value::HugeInt(state.sum),
            velox_wasm_sdk::Value::BigInt(state.count),
        ]))
    }
    fn merge(state: &mut Self::State, input: velox_wasm_sdk::Generic<'_>) -> Result<(), String> {
        let row = input.as_row::<velox_wasm_sdk::DynamicRow>()?;
        state.sum = state
            .sum
            .checked_add(row.at(0)?.get::<i128>()?)
            .ok_or("sum overflow")?;
        state.count = state
            .count
            .checked_add(row.at(1)?.get::<i64>()?)
            .ok_or("count overflow")?;
        Ok(())
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, String> {
        Ok(if state.count == 0 {
            velox_wasm_sdk::Value::Null
        } else {
            velox_wasm_sdk::Value::BigInt(
                i64::try_from(state.sum).map_err(|_| "BIGINT sum overflow")?,
            )
        })
    }
}

/// Maps a fixed value and a generic variadic tail through one SQL lambda.
/// FUNCTION appears after the fixed values and before the tail in SQL; it is
/// supplied through InitContext, not in the Rust row tuple.
pub struct MapVariadicWasm;
#[derive(velox_wasm_sdk::AggregateState, velox_wasm_sdk::AggregateCheckpoint)]
pub struct MapVariadicState {
    sum: i128,
    count: i64,
    factor: i64,
    map: velox_wasm_sdk::Lambda,
}
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["T", "bigint"], variadic = "T", type_variables = ["T"],
    lambdas = ["function(T,bigint)"], constant_arguments = [1],
    returns = "bigint", intermediate = "row(total hugeint,count bigint)",
    default_null_behavior = false, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for MapVariadicWasm {
    type State = MapVariadicState;
    type Input<'a> = (
        Option<velox_wasm_sdk::Generic<'a>>,
        Option<i64>,
        velox_wasm_sdk::VariadicView<'a, Option<velox_wasm_sdk::Generic<'a>>>,
    );
    type Error = velox_wasm_sdk::RowError;
    fn create(context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, Self::Error> {
        Self::create_state(context, 1)
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            Self::map_values(state, &[velox_wasm_sdk::Value::Borrowed(value)])?;
        }
        for value in input.2.skip_nulls() {
            Self::map_values(
                state,
                &[velox_wasm_sdk::Value::Borrowed(value.map_err(Self::error)?)],
            )?;
        }
        Ok(())
    }
    fn update_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<Self::Input<'a>, String>>,
    ) -> Result<(), velox_wasm_sdk::AggregateBatchError<Self::Error>> {
        let limit = state.map.max_batch_rows()?.min(4096).max(1);
        let mut values = Vec::new();
        for row in rows {
            let (head, _, tail) = row.map_err(velox_wasm_sdk::AggregateBatchError::Input)?;
            for value in head.into_iter().map(Ok).chain(tail.skip_nulls()) {
                values.push(velox_wasm_sdk::Value::Borrowed(
                    value.map_err(velox_wasm_sdk::AggregateBatchError::Input)?,
                ));
                if values.len() == limit {
                    Self::map_values(state, &values)?;
                    values.clear();
                }
            }
        }
        if !values.is_empty() {
            Self::map_values(state, &values)?;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(velox_wasm_sdk::Value::Row(vec![
            velox_wasm_sdk::Value::HugeInt(state.sum),
            velox_wasm_sdk::Value::BigInt(state.count),
        ]))
    }
    fn merge(
        state: &mut Self::State,
        input: velox_wasm_sdk::Generic<'_>,
    ) -> Result<(), Self::Error> {
        let row = input
            .as_row::<velox_wasm_sdk::DynamicRow>()
            .map_err(Self::error)?;
        state.sum = state
            .sum
            .checked_add(
                row.at(0)
                    .and_then(|v| v.get::<i128>())
                    .map_err(Self::error)?,
            )
            .ok_or_else(|| Self::error("sum overflow"))?;
        state.count = state
            .count
            .checked_add(
                row.at(1)
                    .and_then(|v| v.get::<i64>())
                    .map_err(Self::error)?,
            )
            .ok_or_else(|| Self::error("count overflow"))?;
        Ok(())
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(if state.count == 0 {
            velox_wasm_sdk::Value::Null
        } else {
            velox_wasm_sdk::Value::BigInt(i64::try_from(state.sum).map_err(Self::error)?)
        })
    }
}
impl MapVariadicWasm {
    fn create_state(
        context: &velox_wasm_sdk::InitContext<'_>,
        factor_index: usize,
    ) -> Result<MapVariadicState, velox_wasm_sdk::RowError> {
        let factor = context
            .constant_value(factor_index)
            .map_err(Self::error)?
            .map(|value| value.get::<Option<i64>>())
            .transpose()
            .map_err(Self::error)?
            .flatten()
            .unwrap_or(1);
        Ok(MapVariadicState {
            sum: 0,
            count: 0,
            factor,
            map: context.lambda(0).map_err(Self::error)?,
        })
    }
    fn error(message: impl ToString) -> velox_wasm_sdk::RowError {
        velox_wasm_sdk::RowError::new(velox_wasm_sdk::StatusCode::UserError, message.to_string())
    }
    fn map_values(
        state: &mut MapVariadicState,
        values: &[velox_wasm_sdk::Value<'_>],
    ) -> Result<(), velox_wasm_sdk::RowError> {
        for value in state.map.call_batch(values.len(), &[values])? {
            let velox_wasm_sdk::Value::BigInt(value) = value else {
                return Err(Self::error("map lambda must return non-NULL BIGINT"));
            };
            state.sum = state
                .sum
                .checked_add(value as i128 * state.factor as i128)
                .ok_or_else(|| Self::error("sum overflow"))?;
            state.count = state
                .count
                .checked_add(1)
                .ok_or_else(|| Self::error("count overflow"))?;
        }
        Ok(())
    }
}

/// Same mapping contract, with every variadic value required to be constant.
/// Constant argument positions use the Rust value tuple, excluding FUNCTION.
pub struct MapConstVariadicWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["T", "bigint"], variadic = "T", type_variables = ["T"],
    lambdas = ["function(T,bigint)"], constant_arguments = [1,2],
    returns = "bigint", intermediate = "row(total hugeint,count bigint)",
    default_null_behavior = false, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for MapConstVariadicWasm {
    type State = MapVariadicState;
    type Input<'a> = (
        Option<velox_wasm_sdk::Generic<'a>>,
        Option<i64>,
        velox_wasm_sdk::VariadicView<'a, Option<velox_wasm_sdk::Generic<'a>>>,
    );
    type Error = velox_wasm_sdk::RowError;
    fn create(context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, Self::Error> {
        MapVariadicWasm::create(context)
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        MapVariadicWasm::update(state, input)
    }
    fn update_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<Self::Input<'a>, String>>,
    ) -> Result<(), velox_wasm_sdk::AggregateBatchError<Self::Error>> {
        MapVariadicWasm::update_batch(state, rows)
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        MapVariadicWasm::serialize(state)
    }
    fn merge(
        state: &mut Self::State,
        input: velox_wasm_sdk::Generic<'_>,
    ) -> Result<(), Self::Error> {
        MapVariadicWasm::merge(state, input)
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        MapVariadicWasm::finalize(state)
    }
}

/// T is inferred entirely from the values after FUNCTION in the SQL signature.
pub struct TailMapVariadicWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["bigint"], variadic = "T", type_variables = ["T"],
    lambdas = ["function(T,bigint)"], constant_arguments = [0],
    returns = "bigint", intermediate = "row(total hugeint,count bigint)",
    default_null_behavior = false, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for TailMapVariadicWasm {
    type State = MapVariadicState;
    type Input<'a> = (
        Option<i64>,
        velox_wasm_sdk::VariadicView<'a, Option<velox_wasm_sdk::Generic<'a>>>,
    );
    type Error = velox_wasm_sdk::RowError;
    fn create(context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, Self::Error> {
        MapVariadicWasm::create_state(context, 0)
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        MapVariadicWasm::update(state, (None, input.0, input.1))
    }
    fn update_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<Self::Input<'a>, String>>,
    ) -> Result<(), velox_wasm_sdk::AggregateBatchError<Self::Error>> {
        MapVariadicWasm::update_batch(
            state,
            rows.map(|row| row.map(|(factor, tail)| (None, factor, tail))),
        )
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        MapVariadicWasm::serialize(state)
    }
    fn merge(
        state: &mut Self::State,
        input: velox_wasm_sdk::Generic<'_>,
    ) -> Result<(), Self::Error> {
        MapVariadicWasm::merge(state, input)
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        MapVariadicWasm::finalize(state)
    }
}

/// Applies a concrete BIGINT lambda to 1 for each non-NULL heterogeneous value.
/// Values are inspected only for NULL; their bodies remain undecoded in Rust.
pub struct MapNonNullVariadicWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["bigint"], variadic = "any",
    lambdas = ["function(bigint,bigint)"], constant_arguments = [0],
    returns = "bigint", intermediate = "row(total hugeint,count bigint)",
    default_null_behavior = false, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for MapNonNullVariadicWasm {
    type State = MapVariadicState;
    type Input<'a> = (
        Option<i64>,
        velox_wasm_sdk::VariadicView<'a, Option<velox_wasm_sdk::Generic<'a>>>,
    );
    type Error = velox_wasm_sdk::RowError;
    fn create(context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, Self::Error> {
        MapVariadicWasm::create_state(context, 0)
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        for value in input.1.skip_nulls() {
            value.map_err(MapVariadicWasm::error)?;
            MapVariadicWasm::map_values(state, &[velox_wasm_sdk::Value::BigInt(1)])?;
        }
        Ok(())
    }
    fn update_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<Self::Input<'a>, String>>,
    ) -> Result<(), velox_wasm_sdk::AggregateBatchError<Self::Error>> {
        let limit = state.map.max_batch_rows()?.min(4096).max(1);
        let mut values = Vec::new();
        for row in rows {
            let (_, tail) = row.map_err(velox_wasm_sdk::AggregateBatchError::Input)?;
            for value in tail.skip_nulls() {
                value.map_err(velox_wasm_sdk::AggregateBatchError::Input)?;
                values.push(velox_wasm_sdk::Value::BigInt(1));
                if values.len() == limit {
                    MapVariadicWasm::map_values(state, &values)?;
                    values.clear();
                }
            }
        }
        if !values.is_empty() {
            MapVariadicWasm::map_values(state, &values)?;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        MapVariadicWasm::serialize(state)
    }
    fn merge(
        state: &mut Self::State,
        input: velox_wasm_sdk::Generic<'_>,
    ) -> Result<(), Self::Error> {
        MapVariadicWasm::merge(state, input)
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        MapVariadicWasm::finalize(state)
    }
}

pub struct StrictSumWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["hugeint"], intermediate = "hugeint", returns = "hugeint", checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for StrictSumWasm {
    type State = i128;
    type Input<'a> = (i128,);
    type Error = velox_wasm_sdk::RowError;
    fn create(_context: &velox_wasm_sdk::InitContext<'_>) -> Result<i128, Self::Error> {
        Ok(0)
    }
    fn update(state: &mut i128, input: Self::Input<'_>) -> Result<(), Self::Error> {
        *state = state.checked_add(input.0).ok_or_else(|| {
            velox_wasm_sdk::RowError::new(
                velox_wasm_sdk::StatusCode::UserError,
                "HUGEINT sum overflow",
            )
        })?;
        Ok(())
    }
    fn serialize(state: &i128) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(velox_wasm_sdk::Value::HugeInt(*state))
    }
    fn merge(state: &mut i128, input: velox_wasm_sdk::Generic<'_>) -> Result<(), Self::Error> {
        Self::update(
            state,
            (input.get::<i128>().map_err(|message| {
                velox_wasm_sdk::RowError::new(velox_wasm_sdk::StatusCode::TypeError, message)
            })?,),
        )
    }
    fn finalize(state: &i128) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Self::serialize(state)
    }
}

/// BIGINT SUM with a wide typed state and native user errors on overflow.
pub struct CheckedSumWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["bigint"], intermediate = "hugeint", returns = "bigint", checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for CheckedSumWasm {
    type State = i128;
    type Input<'a> = (i64,);
    type Error = velox_wasm_sdk::RowError;
    fn create(_context: &velox_wasm_sdk::InitContext<'_>) -> Result<i128, Self::Error> {
        Ok(0)
    }
    fn update(state: &mut i128, input: Self::Input<'_>) -> Result<(), Self::Error> {
        Self::add(state, input.0 as i128)
    }
    fn serialize(state: &i128) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(velox_wasm_sdk::Value::HugeInt(*state))
    }
    fn merge(state: &mut i128, value: velox_wasm_sdk::Generic<'_>) -> Result<(), Self::Error> {
        Self::add(
            state,
            value.get::<i128>().map_err(|message| {
                velox_wasm_sdk::RowError::new(velox_wasm_sdk::StatusCode::TypeError, message)
            })?,
        )
    }
    fn finalize(state: &i128) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(velox_wasm_sdk::Value::BigInt(
            i64::try_from(*state).map_err(|_| {
                velox_wasm_sdk::RowError::new(
                    velox_wasm_sdk::StatusCode::UserError,
                    "BIGINT sum overflow",
                )
            })?,
        ))
    }
}
impl CheckedSumWasm {
    fn add(state: &mut i128, value: i128) -> Result<(), velox_wasm_sdk::RowError> {
        *state = state.checked_add(value).ok_or_else(|| {
            velox_wasm_sdk::RowError::new(
                velox_wasm_sdk::StatusCode::UserError,
                "HUGEINT state overflow",
            )
        })?;
        Ok(())
    }
}

/// Variable-sized owned state with intentionally spare capacity to demonstrate
/// row-size tracking and compaction without changing its SQL content.
pub struct CollectStringsWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["varchar"], intermediate = "array(varchar)", returns = "array(varchar)",
    order_sensitive = true, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for CollectStringsWasm {
    type State = Vec<String>;
    type Input<'a> = (&'a str,);
    type Error = String;
    fn create(_context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, String> {
        Ok(Vec::with_capacity(64))
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), String> {
        let mut retained = String::with_capacity(256.max(input.0.len()));
        retained.push_str(input.0);
        state.push(retained);
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, String> {
        Ok(velox_wasm_sdk::Value::Array(
            state
                .iter()
                .map(|v| velox_wasm_sdk::Value::Varchar(v.clone()))
                .collect(),
        ))
    }
    fn merge(state: &mut Self::State, input: velox_wasm_sdk::Generic<'_>) -> Result<(), String> {
        let values = input.get::<velox_wasm_sdk::ArrayView<'_, &str>>()?;
        for value in values.iter() {
            Self::update(state, (value?.ok_or("expected non-NULL VARCHAR element")?,))?;
        }
        Ok(())
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, String> {
        Self::serialize(state)
    }
}

/// Generic SQL lambda UDAF. Native operators carry the same expressions into
/// partial/intermediate/final stages; State holds symbolic positions, not pointers.
pub struct ReduceWasm;
#[derive(velox_wasm_sdk::AggregateState, velox_wasm_sdk::AggregateCheckpoint)]
pub struct ReduceState {
    initial: velox_wasm_sdk::Value<'static>,
    initial_set: bool,
    value: velox_wasm_sdk::Value<'static>,
    seen: bool,
    input: velox_wasm_sdk::Lambda,
    combine: velox_wasm_sdk::Lambda,
}
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["T", "S"], type_variables = ["T", "S"], returns = "S",
    intermediate = "row(value S,seen boolean)",
    lambdas = ["function(S,T,S)", "function(S,S,S)"],
    default_null_behavior = false, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for ReduceWasm {
    type State = ReduceState;
    type Input<'a> = (
        Option<velox_wasm_sdk::Generic<'a>>,
        Option<velox_wasm_sdk::Generic<'a>>,
    );
    type Error = velox_wasm_sdk::RowError;
    fn create(context: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, Self::Error> {
        Ok(ReduceState {
            initial: velox_wasm_sdk::Value::Null,
            initial_set: false,
            value: velox_wasm_sdk::Value::Null,
            seen: false,
            input: context.lambda(0).map_err(Self::error)?,
            combine: context.lambda(1).map_err(Self::error)?,
        })
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        let Some(value) = input.0 else {
            return Ok(());
        };
        Self::remember_initial(state, input.1)?;
        let result = state
            .input
            .call(&[&state.initial, &velox_wasm_sdk::Value::Borrowed(value)])?;
        Self::check_batch(std::slice::from_ref(&result))?;
        let result = if state.seen {
            state.combine.call(&[&state.value, &result])?
        } else {
            result
        };
        Self::set(state, result)
    }
    fn update_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<Self::Input<'a>, String>>,
    ) -> Result<(), velox_wasm_sdk::AggregateBatchError<Self::Error>> {
        let limit = state.input.max_batch_rows()?.min(4096).max(1);
        let mut values = Vec::new();
        for row in rows {
            let (value, initial) = row.map_err(velox_wasm_sdk::AggregateBatchError::Input)?;
            let Some(value) = value else {
                continue;
            };
            Self::remember_initial(state, initial)?;
            values.push(velox_wasm_sdk::Value::Borrowed(value));
            if values.len() == limit {
                Self::add_batch(state, &values)?;
                values.clear();
            }
        }
        if !values.is_empty() {
            Self::add_batch(state, &values)?;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(velox_wasm_sdk::Value::Row(vec![
            state.value.clone(),
            velox_wasm_sdk::Value::Boolean(state.seen),
        ]))
    }
    fn merge(
        state: &mut Self::State,
        input: velox_wasm_sdk::Generic<'_>,
    ) -> Result<(), Self::Error> {
        let row = input
            .as_row::<velox_wasm_sdk::DynamicRow>()
            .map_err(Self::error)?;
        if !row
            .at(1)
            .and_then(|v| v.get::<bool>())
            .map_err(Self::error)?
        {
            return Ok(());
        }
        let value = row
            .at(0)
            .and_then(|v| v.materialize())
            .map_err(Self::error)?;
        let result = if state.seen {
            state.combine.call(&[&state.value, &value])?
        } else {
            value
        };
        Self::set(state, result)
    }
    fn merge_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<velox_wasm_sdk::Generic<'a>, String>>,
    ) -> Result<(), velox_wasm_sdk::AggregateBatchError<Self::Error>> {
        let limit = state.combine.max_batch_rows()?.min(4096).max(1);
        let mut values = Vec::new();
        for row in rows {
            let row = row.map_err(velox_wasm_sdk::AggregateBatchError::Input)?;
            let row = row
                .as_row::<velox_wasm_sdk::DynamicRow>()
                .map_err(Self::error)?;
            if !row
                .at(1)
                .and_then(|v| v.get::<bool>())
                .map_err(Self::error)?
            {
                continue;
            }
            values.push(
                row.at(0)
                    .and_then(|v| v.materialize())
                    .map_err(Self::error)?,
            );
            if values.len() == limit {
                Self::combine_batch(state, std::mem::take(&mut values))?;
            }
        }
        if !values.is_empty() {
            Self::combine_batch(state, values)?;
        }
        Ok(())
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(if state.seen {
            state.value.clone()
        } else {
            velox_wasm_sdk::Value::Null
        })
    }
}
impl ReduceWasm {
    fn remember_initial(
        state: &mut ReduceState,
        initial: Option<velox_wasm_sdk::Generic<'_>>,
    ) -> Result<(), velox_wasm_sdk::RowError> {
        let initial = initial.ok_or_else(|| Self::error("reduce initial state cannot be NULL"))?;
        if !state.initial_set {
            state.initial = initial.materialize().map_err(Self::error)?;
            state.initial_set = true;
        } else if !state
            .initial
            .equals(&velox_wasm_sdk::Value::Borrowed(initial))
            .map_err(Self::error)?
        {
            return Err(Self::error(
                "reduce initial value must be constant within a group",
            ));
        }
        Ok(())
    }
    fn add_batch(
        state: &mut ReduceState,
        inputs: &[velox_wasm_sdk::Value<'_>],
    ) -> Result<(), velox_wasm_sdk::RowError> {
        let values = state.input.call_batch(
            inputs.len(),
            &[std::slice::from_ref(&state.initial), inputs],
        )?;
        Self::combine_batch(state, values)
    }
    fn combine_batch(
        state: &mut ReduceState,
        mut values: Vec<velox_wasm_sdk::Value<'static>>,
    ) -> Result<(), velox_wasm_sdk::RowError> {
        Self::check_batch(&values)?;
        if state.seen {
            values.insert(0, state.value.clone());
        }
        while values.len() > 1 {
            let pairs = values.len() / 2;
            let mut left = Vec::with_capacity(pairs);
            let mut right = Vec::with_capacity(pairs);
            let mut input = values.into_iter();
            let mut extra = None;
            while let Some(value) = input.next() {
                if let Some(other) = input.next() {
                    left.push(value);
                    right.push(other);
                } else {
                    extra = Some(value);
                }
            }
            values = state.combine.call_batch(pairs, &[&left, &right])?;
            Self::check_batch(&values)?;
            if let Some(extra) = extra {
                values.push(extra);
            }
        }
        Self::set(
            state,
            values
                .pop()
                .ok_or_else(|| Self::error("missing reduced state"))?,
        )
    }
    fn check_batch(values: &[velox_wasm_sdk::Value<'_>]) -> Result<(), velox_wasm_sdk::RowError> {
        if values.iter().any(|value| value.is_null()) {
            return Err(Self::error("reduce lambda state cannot be NULL"));
        }
        Ok(())
    }
    fn error(message: impl ToString) -> velox_wasm_sdk::RowError {
        velox_wasm_sdk::RowError::new(velox_wasm_sdk::StatusCode::UserError, message.to_string())
    }
    fn set(
        state: &mut ReduceState,
        value: velox_wasm_sdk::Value<'static>,
    ) -> Result<(), velox_wasm_sdk::RowError> {
        if value.is_null() {
            return Err(Self::error("reduce lambda state cannot be NULL"));
        }
        state.value = value;
        state.seen = true;
        Ok(())
    }
}
