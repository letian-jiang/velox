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

//! Row values used by scalar UDFs. Arrow batching stays in the generated adapter.
use crate::{Date32, Timestamp};
use arrow_array::*;
use arrow_schema::DataType;
use std::sync::Arc;

type Result<T> = std::result::Result<T, String>;

/// A borrowed, single SQL value whose type is bound by Velox.
#[derive(Clone, Copy)]
pub struct Generic<'a> {
    pub(crate) array: &'a dyn Array,
    pub(crate) row: usize,
}

impl<'a> Generic<'a> {
    pub fn new(array: &'a dyn Array, row: usize) -> Result<Self> {
        if row >= array.len() {
            return Err("generic value index out of range".into());
        }
        Ok(Self { array, row })
    }
    pub fn data_type(&self) -> &DataType {
        self.array.data_type()
    }
    pub fn is_null(&self) -> bool {
        self.array.data_type() == &DataType::Null || self.array.is_null(self.row)
    }
    pub fn get<T: RowArgument<'a>>(&self) -> Result<T> {
        T::read(self.array, self.row)
    }
    pub fn list_len(&self) -> Result<usize> {
        if self.is_null() {
            return Err("cannot read a null array".into());
        }
        let list = self
            .array
            .as_any()
            .downcast_ref::<ListArray>()
            .ok_or("expected array")?;
        Ok(list.value_length(self.row) as usize)
    }
    pub fn element(&self, index: usize) -> Result<Self> {
        if self.is_null() {
            return Err("cannot index a null array".into());
        }
        let list = self
            .array
            .as_any()
            .downcast_ref::<ListArray>()
            .ok_or("expected array")?;
        if index >= list.value_length(self.row) as usize {
            return Err("array index out of range".into());
        }
        Self::new(
            list.values().as_ref(),
            list.value_offsets()[self.row] as usize + index,
        )
    }
    pub fn field(&self, name: &str) -> Result<Self> {
        if self.is_null() {
            return Err("cannot read a null row".into());
        }
        let row = self
            .array
            .as_any()
            .downcast_ref::<StructArray>()
            .ok_or("expected row")?;
        let column = row.column_by_name(name).ok_or("unknown row field")?;
        Self::new(column.as_ref(), self.row)
    }
}

/// Actual variadic values for one row. A Generic tail may be heterogeneous.
pub struct Variadic<T> {
    values: Vec<T>,
}
impl<T> Variadic<T> {
    pub fn from_values(values: Vec<T>) -> Self {
        Self { values }
    }
    pub fn len(&self) -> usize {
        self.values.len()
    }
    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }
    pub fn iter(&self) -> std::slice::Iter<'_, T> {
        self.values.iter()
    }
    pub fn get(&self, index: usize) -> Option<&T> {
        self.values.get(index)
    }
    #[doc(hidden)]
    pub fn from_row<'a>(columns: &'a [ArrayRef], row: usize) -> Result<Self>
    where
        T: RowArgument<'a>,
    {
        columns
            .iter()
            .map(|column| read_row(column.as_ref(), row))
            .collect::<Result<Vec<_>>>()
            .map(Self::from_values)
    }
}
impl<T> IntoIterator for Variadic<T> {
    type Item = T;
    type IntoIter = std::vec::IntoIter<T>;
    fn into_iter(self) -> Self::IntoIter {
        self.values.into_iter()
    }
}
impl<'a, T> IntoIterator for &'a Variadic<T> {
    type Item = &'a T;
    type IntoIter = std::slice::Iter<'a, T>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

/// Conservative batch-level recursive NULL check, using Arrow validity metadata.
/// A false result lets row adapters skip all recursive per-row scans.
#[doc(hidden)]
pub fn array_may_have_recursive_nulls(array: &dyn Array) -> bool {
    if array.logical_null_count() != 0 {
        return true;
    }
    match array.data_type() {
        DataType::List(_) => array_may_have_recursive_nulls(
            array
                .as_any()
                .downcast_ref::<ListArray>()
                .unwrap()
                .values()
                .as_ref(),
        ),
        DataType::Map(_, _) => {
            let map = array.as_any().downcast_ref::<MapArray>().unwrap();
            array_may_have_recursive_nulls(map.keys().as_ref())
                || array_may_have_recursive_nulls(map.values().as_ref())
        }
        DataType::Struct(_) => array
            .as_any()
            .downcast_ref::<StructArray>()
            .unwrap()
            .columns()
            .iter()
            .any(|v| array_may_have_recursive_nulls(v.as_ref())),
        _ => false,
    }
}

/// Native SFI ASCII dispatch uses all top-level VARCHAR inputs in the selected
/// batch. NULLs and nested strings do not disqualify that path. Older hosts may
/// omit metadata; scan the batch once in that case, rather than once per call.
#[doc(hidden)]
pub fn batch_is_ascii(batch: &RecordBatch) -> Result<bool> {
    if let Some(ascii) = batch
        .schema_ref()
        .metadata()
        .get("velox.scalar.ascii_inputs")
    {
        return match ascii.as_str() {
            "true" => Ok(true),
            "false" => Ok(false),
            _ => Err("invalid scalar ASCII metadata".into()),
        };
    }
    for column in batch.columns() {
        if <&str>::accepts(column.data_type()) {
            for row in 0..batch.num_rows() {
                let value = Generic::new(column.as_ref(), row)?;
                if !value.is_null() && !value.get::<crate::VarcharBytes<&[u8]>>()?.0.is_ascii() {
                    return Ok(false);
                }
            }
        }
    }
    Ok(true)
}

// Autoref dispatch preserves compatibility with arbitrary ToString errors
// without requiring authors to implement a new trait. RowError gets a typed
// wire representation; ordinary errors keep their original message bytes.
#[doc(hidden)]
pub trait EncodeRowError {
    fn encode_row_error(self) -> String;
}
impl<T: ToString + ?Sized> EncodeRowError for &&T {
    fn encode_row_error(self) -> String {
        let message = self.to_string();
        if message.starts_with('\u{001e}') {
            format!("\u{001e}velox.message.v1:{message}")
        } else {
            message
        }
    }
}
impl EncodeRowError for &crate::RowError {
    fn encode_row_error(self) -> String {
        format!(
            "\u{001e}velox.status.v1:{}\n{}",
            self.code as u8, self.message
        )
    }
}

/// Unscaled decimal value and its bound precision and scale.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Decimal {
    pub value: i128,
    pub precision: u8,
    pub scale: i8,
}

#[doc(hidden)]
pub trait RowArgument<'a>: Sized + 'a {
    fn accepts(data_type: &DataType) -> bool;
    fn read(array: &'a dyn Array, row: usize) -> Result<Self>;
    /// Prepare once per batch. The fallback keeps nested/custom value decoding
    /// deferred until a row is accessed; primitive readers cache their casts.
    fn prepare(array: &'a dyn Array) -> impl Fn(usize) -> Result<Self> + Send + Sync + 'a {
        move |row| read_row(array, row)
    }
    /// Called only after an optional reader has checked validity. The default
    /// remains conservative; primitive implementations avoid a second check.
    fn prepare_non_null(array: &'a dyn Array) -> impl Fn(usize) -> Result<Self> + Send + Sync + 'a {
        Self::prepare(array)
    }
}
#[doc(hidden)]
pub fn prepare_row<'a, T: RowArgument<'a>>(
    array: &'a dyn Array,
) -> impl Fn(usize) -> Result<T> + Send + Sync + 'a {
    T::prepare(array)
}
#[doc(hidden)]
pub fn read_row<'a, T: RowArgument<'a>>(array: &'a dyn Array, row: usize) -> Result<T> {
    if row >= array.len() {
        return Err("scalar row index out of range".into());
    }
    T::read(array, row)
}
impl<'a> RowArgument<'a> for Generic<'a> {
    fn accepts(_: &DataType) -> bool {
        true
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        Self::new(array, row)
    }
}
impl<'a, T: RowArgument<'a>> RowArgument<'a> for Option<T> {
    fn accepts(data_type: &DataType) -> bool {
        T::accepts(data_type)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        if array.data_type() == &DataType::Null || array.is_null(row) {
            Ok(None)
        } else {
            T::read(array, row).map(Some)
        }
    }
    fn prepare(array: &'a dyn Array) -> impl Fn(usize) -> Result<Self> + Send + Sync + 'a {
        let rows = array.len();
        let nulls = array.nulls();
        let reader = (array.data_type() != &DataType::Null).then(|| T::prepare_non_null(array));
        move |row| {
            if row >= rows {
                return Err("scalar row index out of range".into());
            }
            if nulls.is_some_and(|nulls| nulls.is_null(row)) {
                return Ok(None);
            }
            match &reader {
                Some(reader) => reader(row).map(Some),
                None => Ok(None),
            }
        }
    }
}
macro_rules! primitive_argument {
    ($rust:ty, $array:ty, $data_type:expr) => {
        impl<'a> RowArgument<'a> for $rust {
            fn accepts(data_type: &DataType) -> bool {
                data_type == &$data_type
            }
            fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
                let values = array.as_any().downcast_ref::<$array>().ok_or_else(|| {
                    format!(
                        "expected {}, got {:?}",
                        stringify!($rust),
                        array.data_type()
                    )
                })?;
                if values.is_null(row) {
                    return Err("unexpected null scalar argument; use Option<T>".into());
                }
                Ok(values.value(row))
            }
            fn prepare(array: &'a dyn Array) -> impl Fn(usize) -> Result<Self> + Send + Sync + 'a {
                let read = Self::prepare_non_null(array);
                let nulls = array.nulls();
                move |row| {
                    let value = read(row)?;
                    if nulls.is_some_and(|nulls| nulls.is_null(row)) {
                        return Err("unexpected null scalar argument; use Option<T>".into());
                    }
                    Ok(value)
                }
            }
            fn prepare_non_null(
                array: &'a dyn Array,
            ) -> impl Fn(usize) -> Result<Self> + Send + Sync + 'a {
                let rows = array.len();
                let values = array.as_any().downcast_ref::<$array>();
                move |row| {
                    if row >= rows {
                        return Err("scalar row index out of range".into());
                    }
                    let values = values.ok_or_else(|| {
                        format!(
                            "expected {}, got {:?}",
                            stringify!($rust),
                            array.data_type()
                        )
                    })?;
                    Ok(values.value(row))
                }
            }
        }
    };
}
primitive_argument!(bool, BooleanArray, DataType::Boolean);
primitive_argument!(i8, Int8Array, DataType::Int8);
primitive_argument!(i16, Int16Array, DataType::Int16);
primitive_argument!(i32, Int32Array, DataType::Int32);
primitive_argument!(i64, Int64Array, DataType::Int64);
primitive_argument!(f32, Float32Array, DataType::Float32);
primitive_argument!(f64, Float64Array, DataType::Float64);

impl<'a> RowArgument<'a> for crate::VarcharBytes<&'a [u8]> {
    fn accepts(ty: &DataType) -> bool {
        crate::is_varchar_wire_type(ty) || ty == &DataType::Utf8
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        if crate::is_varchar_wire_type(array.data_type()) {
            return Ok(Self(Generic::new(array, row)?.field_at(0)?.get::<&[u8]>()?));
        }
        let strings = array
            .as_any()
            .downcast_ref::<StringArray>()
            .ok_or("expected varchar")?;
        if strings.is_null(row) {
            return Err("unexpected NULL varchar".into());
        }
        Ok(Self(strings.value(row).as_bytes()))
    }
}
impl<'a> RowArgument<'a> for crate::VarcharBytes<Vec<u8>> {
    fn accepts(ty: &DataType) -> bool {
        <crate::VarcharBytes<&[u8]>>::accepts(ty)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        Ok(Self(
            <crate::VarcharBytes<&[u8]>>::read(array, row)?.0.to_vec(),
        ))
    }
}
impl<'a> RowArgument<'a> for &'a str {
    fn accepts(ty: &DataType) -> bool {
        <crate::VarcharBytes<&[u8]>>::accepts(ty)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        let value = <crate::VarcharBytes<&[u8]>>::read(array, row)?;
        std::str::from_utf8(value.0)
            .map_err(|_| "VARCHAR is not UTF-8; use VarcharBytes<&[u8]>".into())
    }
}
impl<'a, T: AsRef<[u8]>> RowOutput<'a> for crate::VarcharBytes<T> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(crate::Value::VarcharBytes(self.0.as_ref().to_vec()))
    }
}

primitive_argument!(&'a [u8], BinaryArray, DataType::Binary);
impl<'a> RowArgument<'a> for String {
    fn accepts(data_type: &DataType) -> bool {
        <&str>::accepts(data_type)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        <&str>::read(array, row).map(str::to_owned)
    }
}
impl<'a> RowArgument<'a> for Vec<u8> {
    fn accepts(data_type: &DataType) -> bool {
        data_type == &DataType::Binary
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        <&[u8]>::read(array, row).map(<[u8]>::to_vec)
    }
}
impl<'a> RowArgument<'a> for Date32 {
    fn accepts(data_type: &DataType) -> bool {
        data_type == &DataType::Date32
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        let values = array
            .as_any()
            .downcast_ref::<Date32Array>()
            .ok_or("expected date")?;
        if values.is_null(row) {
            return Err("unexpected null date".into());
        }
        Ok(Self(values.value(row)))
    }
}
impl<'a> RowArgument<'a> for i128 {
    fn accepts(data_type: &DataType) -> bool {
        crate::is_hugeint_wire_type(data_type)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        if !Self::accepts(array.data_type()) {
            return Err("expected hugeint".into());
        }
        let value = Generic::new(array, row)?;
        let high = value.field_at(0)?.get::<i64>()?;
        let low = value.field_at(1)?.get::<i64>()?;
        Ok(((high as u128) << 64 | low as u64 as u128) as i128)
    }
}
impl<'a> RowOutput<'a> for i128 {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(crate::Value::HugeInt(self))
    }
}
impl<'a> RowArgument<'a> for Timestamp {
    fn accepts(data_type: &DataType) -> bool {
        Timestamp::is_wire_type(data_type)
            || data_type == &DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        if Timestamp::is_wire_type(array.data_type()) {
            let value = Generic::new(array, row)?;
            let seconds = value.field_at(0)?.get::<i64>()?;
            let nanos = value.field_at(1)?.get::<i64>()?;
            let nanos = u32::try_from(nanos).map_err(|_| "invalid timestamp nanoseconds")?;
            return Timestamp::from_seconds_nanos(seconds, nanos);
        }
        let values = array
            .as_any()
            .downcast_ref::<TimestampNanosecondArray>()
            .ok_or("expected timestamp")?;
        if values.is_null(row) {
            return Err("unexpected null timestamp".into());
        }
        Ok(Self(values.value(row) as i128))
    }
}
impl<'a> RowArgument<'a> for Decimal {
    fn accepts(data_type: &DataType) -> bool {
        matches!(data_type, DataType::Decimal128(..))
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        let values = array
            .as_any()
            .downcast_ref::<Decimal128Array>()
            .ok_or("expected decimal")?;
        if values.is_null(row) {
            return Err("unexpected null decimal".into());
        }
        Ok(Self {
            value: values.value(row),
            precision: values.precision(),
            scale: values.scale(),
        })
    }
}

#[doc(hidden)]
pub enum OutputValue<'a> {
    Null,
    Owned(crate::Value<'a>),
    Generic(Generic<'a>),
    Bool(bool),
    I8(i8),
    I16(i16),
    I32(i32),
    I64(i64),
    F32(f32),
    F64(f64),
    String(String),
    Binary(Vec<u8>),
    Date(Date32),
    Time(Timestamp),
    Decimal(Decimal),
}
#[doc(hidden)]
pub trait RowOutput<'a> {
    fn into_output(self) -> OutputValue<'a>;
}
macro_rules! scalar_output {
    ($rust:ty, $variant:ident) => {
        impl<'a> RowOutput<'a> for $rust {
            fn into_output(self) -> OutputValue<'a> {
                OutputValue::$variant(self)
            }
        }
    };
}
scalar_output!(bool, Bool);
scalar_output!(i8, I8);
scalar_output!(i16, I16);
scalar_output!(i32, I32);
scalar_output!(i64, I64);
scalar_output!(f32, F32);
scalar_output!(f64, F64);
scalar_output!(String, String);
scalar_output!(Vec<u8>, Binary);
scalar_output!(Date32, Date);
scalar_output!(Timestamp, Time);
scalar_output!(Decimal, Decimal);
impl<'a> RowOutput<'a> for Generic<'a> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Generic(self)
    }
}
impl<'a> RowOutput<'a> for &'a str {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::String(self.to_owned())
    }
}
impl<'a> RowOutput<'a> for &'a [u8] {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Binary(self.to_vec())
    }
}
impl<'a, T: RowOutput<'a>> RowOutput<'a> for Option<T> {
    fn into_output(self) -> OutputValue<'a> {
        self.map(RowOutput::into_output)
            .unwrap_or(OutputValue::Null)
    }
}

/// Internal accumulator for row results, including generic/nested values.
#[doc(hidden)]
pub struct RowOutputBuilder<'a> {
    data_type: DataType,
    values: Vec<OutputValue<'a>>,
    errors: Vec<Option<String>>,
}
impl<'a> RowOutputBuilder<'a> {
    pub fn new(data_type: DataType, capacity: usize) -> Self {
        Self {
            data_type,
            values: Vec::with_capacity(capacity),
            errors: Vec::with_capacity(capacity),
        }
    }
    pub fn append<T: RowOutput<'a>>(&mut self, value: T) {
        let value = value.into_output();
        let contract = match &value {
            OutputValue::Owned(value) => crate::value::validate(value, &self.data_type),
            OutputValue::Decimal(value) => {
                crate::value::validate(&crate::Value::Decimal(*value), &self.data_type)
            }
            OutputValue::Time(value) => {
                crate::value::validate(&crate::Value::Timestamp(*value), &self.data_type)
            }
            OutputValue::Generic(value) => {
                crate::value::validate(&crate::Value::Borrowed(*value), &self.data_type)
            }
            _ => Ok(()),
        };
        if let Err(error) = contract {
            self.append_error(error);
            return;
        }
        self.values.push(value);
        self.errors.push(None);
    }
    pub fn append_error(&mut self, error: String) {
        self.values.push(OutputValue::Null);
        self.errors.push(Some(error));
    }
    // Preserve buffers for an identity/projection, and gather other borrowed
    // results by source ranges. No per-row ArrayData/slice is needed. Exact wire
    // types (including field names and metadata) are required for buffer reuse;
    // values needing a representation conversion use the ordinary builder.
    fn borrowed_array(&self) -> Option<Result<ArrayRef>> {
        if self.values.is_empty() {
            return Some(Ok(new_empty_array(&self.data_type)));
        }
        if let OutputValue::Generic(first) = &self.values[0] {
            if first.data_type() == &self.data_type
                && self.values.iter().enumerate().all(|(index, value)| {
                    matches!(value, OutputValue::Generic(value)
                        if std::ptr::eq(value.array, first.array)
                            && value.row == first.row + index)
                })
            {
                return Some(Ok(first.array.slice(first.row, self.values.len())));
            }
        }
        if !self.values.iter().all(|value| match value {
            OutputValue::Null => true,
            OutputValue::Generic(value) => value.data_type() == &self.data_type,
            _ => false,
        }) {
            return None;
        }
        enum Range {
            Source(usize, usize, usize), // source index, start, exclusive end
            Null(usize),
        }
        let mut sources: Vec<&dyn Array> = Vec::new();
        let mut indices = std::collections::HashMap::new();
        let mut ranges = Vec::new();
        for value in &self.values {
            match value {
                OutputValue::Null => match ranges.last_mut() {
                    Some(Range::Null(len)) => *len += 1,
                    _ => ranges.push(Range::Null(1)),
                },
                OutputValue::Generic(value) => {
                    let source = *indices
                        .entry(value.array as *const dyn Array)
                        .or_insert_with(|| {
                            sources.push(value.array);
                            sources.len() - 1
                        });
                    match ranges.last_mut() {
                        Some(Range::Source(index, _, end))
                            if *index == source && *end == value.row =>
                        {
                            *end += 1;
                        }
                        _ => ranges.push(Range::Source(source, value.row, value.row + 1)),
                    }
                }
                _ => unreachable!(),
            }
        }
        if sources.is_empty() {
            return Some(Ok(new_null_array(&self.data_type, self.values.len())));
        }
        let data: Vec<_> = sources.iter().map(|source| source.to_data()).collect();
        let mut output = arrow_data::transform::MutableArrayData::new(
            data.iter().collect(),
            true,
            self.values.len(),
        );
        for range in ranges {
            let result = match range {
                Range::Source(index, start, end) => output.try_extend(index, start, end),
                Range::Null(len) => output.try_extend_nulls(len),
            };
            if let Err(error) = result {
                return Some(Err(error.to_string()));
            }
        }
        Some(Ok(make_array(output.freeze())))
    }
    pub fn finish(self) -> Result<(ArrayRef, ArrayRef)> {
        if let Some(array) = self.borrowed_array() {
            return Ok((array?, Arc::new(StringArray::from(self.errors))));
        }
        // Common VARCHAR results can build the wire binary buffer directly.
        // Avoid materializing/cloning every string into an intermediate Value.
        if crate::is_varchar_wire_type(&self.data_type)
            && !self
                .values
                .iter()
                .any(|v| matches!(v, OutputValue::Owned(_)))
        {
            let bytes = self
                .values
                .iter()
                .map(|value| match value {
                    OutputValue::Null => Ok(None),
                    OutputValue::String(v) => Ok(Some(v.as_bytes())),
                    OutputValue::Generic(v) if v.is_null() => Ok(None),
                    OutputValue::Generic(v) => {
                        v.get::<crate::VarcharBytes<&[u8]>>().map(|v| Some(v.0))
                    }
                    _ => Err("expected VARCHAR row result".into()),
                })
                .collect::<Result<Vec<_>>>()?;
            let binary = BinaryArray::from(bytes);
            let nulls = binary.nulls().cloned();
            let DataType::Struct(fields) = &self.data_type else {
                unreachable!()
            };
            let result = StructArray::try_new(fields.clone(), vec![Arc::new(binary)], nulls)
                .map_err(|e| e.to_string())?;
            return Ok((Arc::new(result), Arc::new(StringArray::from(self.errors))));
        }
        if crate::custom::is_custom(&self.data_type)
            || Timestamp::is_wire_type(&self.data_type)
            || crate::is_varchar_wire_type(&self.data_type)
            || crate::is_hugeint_wire_type(&self.data_type)
            || self
                .values
                .iter()
                .any(|value| matches!(value, OutputValue::Owned(_)))
        {
            let values = self
                .values
                .into_iter()
                .map(crate::Value::from_output)
                .collect::<Vec<_>>();
            return Ok((
                crate::value::build_array(&self.data_type, &values)?,
                Arc::new(StringArray::from(self.errors)),
            ));
        }
        macro_rules! collect_values {
            ($variant:ident, $rust:ty) => {
                self.values
                    .iter()
                    .map(|value| match value {
                        OutputValue::Null => Ok(None),
                        OutputValue::$variant(value) => Ok(Some(value.clone())),
                        OutputValue::Generic(value) if value.is_null() => Ok(None),
                        OutputValue::Generic(value) => value.get::<$rust>().map(Some),
                        _ => Err("scalar result does not match the bound return type".to_owned()),
                    })
                    .collect::<Result<Vec<Option<$rust>>>>()?
            };
        }
        let result: ArrayRef = match &self.data_type {
            DataType::Boolean => Arc::new(BooleanArray::from(collect_values!(Bool, bool))),
            DataType::Int8 => Arc::new(Int8Array::from(collect_values!(I8, i8))),
            DataType::Int16 => Arc::new(Int16Array::from(collect_values!(I16, i16))),
            DataType::Int32 => Arc::new(Int32Array::from(collect_values!(I32, i32))),
            DataType::Int64 => Arc::new(Int64Array::from(collect_values!(I64, i64))),
            DataType::Float32 => Arc::new(Float32Array::from(collect_values!(F32, f32))),
            DataType::Float64 => Arc::new(Float64Array::from(collect_values!(F64, f64))),
            DataType::Utf8 => Arc::new(StringArray::from(collect_values!(String, String))),
            DataType::Binary => {
                let values = collect_values!(Binary, Vec<u8>);
                Arc::new(BinaryArray::from_iter(
                    values.iter().map(|value| value.as_deref()),
                ))
            }
            DataType::Date32 => Arc::new(Date32Array::from(
                collect_values!(Date, Date32)
                    .into_iter()
                    .map(|value| value.map(|value| value.0))
                    .collect::<Vec<_>>(),
            )),
            DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None) => {
                Arc::new(TimestampNanosecondArray::from(
                    collect_values!(Time, Timestamp)
                        .into_iter()
                        .map(|value| {
                            value
                                .map(|value| {
                                    i64::try_from(value.0).map_err(|_| {
                                        "legacy timestamp exceeds nanosecond range".to_owned()
                                    })
                                })
                                .transpose()
                        })
                        .collect::<Result<Vec<_>>>()?,
                ))
            }
            DataType::Decimal128(precision, scale) => {
                let values = collect_values!(Decimal, Decimal);
                for value in values.iter().flatten() {
                    if value.precision != *precision || value.scale != *scale {
                        return Err(
                            "decimal result precision/scale does not match bound type".into()
                        );
                    }
                }
                Arc::new(
                    Decimal128Array::from(
                        values
                            .into_iter()
                            .map(|value| value.map(|value| value.value))
                            .collect::<Vec<_>>(),
                    )
                    .with_precision_and_scale(*precision, *scale)
                    .map_err(|error| error.to_string())?,
                )
            }
            _ => {
                let mut chunks: Vec<ArrayRef> = Vec::new();
                for value in &self.values {
                    match value {
                        OutputValue::Null => chunks.push(new_null_array(&self.data_type, 1)),
                        OutputValue::Generic(value) => {
                            if !value.data_type().equals_datatype(&self.data_type) {
                                return Err(
                                    "generic result does not match bound return type".into()
                                );
                            }
                            chunks.push(value.array.slice(value.row, 1));
                        }
                        _ => return Err("expected a generic nested result".into()),
                    }
                }
                if chunks.is_empty() {
                    new_empty_array(&self.data_type)
                } else {
                    let references: Vec<_> = chunks.iter().map(|array| array.as_ref()).collect();
                    arrow_select::concat::concat(&references).map_err(|error| error.to_string())?
                }
            }
        };
        Ok((result, Arc::new(StringArray::from(self.errors))))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use arrow_array::types::Int64Type;

    // A transparent test wrapper counts dynamic casts. All Arrow storage and
    // validity methods delegate to the same immutable primitive array.
    #[derive(Debug)]
    struct CountedInt64 {
        values: Int64Array,
        casts: std::sync::atomic::AtomicUsize,
    }
    // SAFETY: every Arrow layout/length/validity operation delegates to values;
    // as_any exposes that same concrete array, without changing its buffers.
    unsafe impl Array for CountedInt64 {
        fn as_any(&self) -> &dyn std::any::Any {
            self.casts
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            &self.values
        }
        fn to_data(&self) -> arrow_data::ArrayData {
            self.values.to_data()
        }
        fn into_data(self) -> arrow_data::ArrayData {
            self.values.into_data()
        }
        fn data_type(&self) -> &DataType {
            self.values.data_type()
        }
        fn slice(&self, offset: usize, length: usize) -> ArrayRef {
            Arc::new(self.values.slice(offset, length))
        }
        fn len(&self) -> usize {
            self.values.len()
        }
        fn is_empty(&self) -> bool {
            self.values.is_empty()
        }
        fn offset(&self) -> usize {
            self.values.offset()
        }
        fn nulls(&self) -> Option<&arrow_buffer::NullBuffer> {
            self.values.nulls()
        }
        fn get_buffer_memory_size(&self) -> usize {
            self.values.get_buffer_memory_size()
        }
        fn get_array_memory_size(&self) -> usize {
            self.values.get_array_memory_size()
        }
    }

    #[test]
    fn prepared_primitive_reader_casts_once_and_preserves_sliced_validity() {
        let array = CountedInt64 {
            values: Int64Array::from(vec![Some(99), Some(20), None, Some(22)]).slice(1, 3),
            casts: std::sync::atomic::AtomicUsize::new(0),
        };
        let read = prepare_row::<Option<i64>>(&array);
        assert_eq!(array.casts.load(std::sync::atomic::Ordering::Relaxed), 1);
        let (sum, allocations) = crate::comparison::tests::allocation_count(|| {
            let mut sum = 0;
            for _ in 0..100 {
                for row in 0..3 {
                    sum += read(row).unwrap().unwrap_or(0);
                }
            }
            sum
        });
        assert_eq!(sum, 4200);
        assert_eq!(allocations, 0);
        assert_eq!(array.casts.load(std::sync::atomic::Ordering::Relaxed), 1);
        assert!(read(3).is_err() && read(usize::MAX).is_err());
        let read = prepare_row::<i64>(&array);
        assert_eq!(read(0).unwrap(), 20);
        assert!(read(1).is_err());
        assert_eq!(read(2).unwrap(), 22);
        assert_eq!(array.casts.load(std::sync::atomic::Ordering::Relaxed), 2);
    }

    #[test]
    fn prepared_reader_defers_conversion_errors_and_handles_untyped_nulls() {
        let strings = StringArray::from(vec![None, Some("wrong type")]);
        let read = prepare_row::<Option<i64>>(&strings);
        assert_eq!(read(0).unwrap(), None);
        assert_eq!(
            read(1).unwrap_err(),
            read_row::<Option<i64>>(&strings, 1).unwrap_err()
        );
        let nulls = NullArray::new(2);
        let read = prepare_row::<Option<Option<i64>>>(&nulls);
        assert_eq!(read(0).unwrap(), None);
        assert_eq!(read(1).unwrap(), None);
        assert!(read(2).is_err());
        let empty = Int64Array::from(Vec::<i64>::new());
        assert!(prepare_row::<i64>(&empty)(0).is_err());
        let booleans = BooleanArray::from(vec![Some(true), None, Some(false)]).slice(1, 2);
        let read = prepare_row::<Option<bool>>(&booleans);
        assert_eq!(read(0).unwrap(), None);
        assert_eq!(read(1).unwrap(), Some(false));
        let floats = Float64Array::from(vec![f64::NAN, -0.0, f64::INFINITY]);
        let read = prepare_row::<f64>(&floats);
        assert!(read(0).unwrap().is_nan());
        assert_eq!(read(1).unwrap().to_bits(), (-0.0f64).to_bits());
        assert_eq!(read(2).unwrap(), f64::INFINITY);
    }

    #[test]
    fn ascii_dispatch_uses_batch_metadata_and_matches_native_input_rules() {
        let strings: ArrayRef = Arc::new(StringArray::from(vec![Some("velox"), None, Some("")]));
        let batch = RecordBatch::try_from_iter(vec![("text", strings)]).unwrap();
        assert!(batch_is_ascii(&batch).unwrap());
        let strings: ArrayRef = Arc::new(StringArray::from(vec!["velox", "élan"]));
        let mixed = RecordBatch::try_from_iter(vec![("text", strings)]).unwrap();
        assert!(!batch_is_ascii(&mixed).unwrap());
        // Host-provided false is conservative, even when the bytes are ASCII.
        // A true flag is trusted input from the host, never guest output.
        for flag in ["true", "false", "invalid"] {
            let schema = Arc::new(batch.schema().as_ref().clone().with_metadata(
                std::collections::HashMap::from([(
                    "velox.scalar.ascii_inputs".into(),
                    flag.into(),
                )]),
            ));
            let batch = RecordBatch::try_new(schema, batch.columns().to_vec()).unwrap();
            match flag {
                "true" => assert!(batch_is_ascii(&batch).unwrap()),
                "false" => assert!(!batch_is_ascii(&batch).unwrap()),
                _ => assert!(batch_is_ascii(&batch).is_err()),
            }
        }
        let ty = crate::runtime::parse_row_arrow_type("varchar").unwrap();
        let bytes =
            crate::value::build_array(&ty, &[crate::Value::VarcharBytes(vec![0xff])]).unwrap();
        assert!(
            !batch_is_ascii(&RecordBatch::try_from_iter(vec![("text", bytes)]).unwrap()).unwrap()
        );
        let ty = crate::runtime::parse_row_arrow_type("array(varchar)").unwrap();
        let nested = crate::value::build_array(
            &ty,
            &[crate::Value::Array(vec![crate::Value::Varchar(
                "élan".into(),
            )])],
        )
        .unwrap();
        assert!(
            batch_is_ascii(&RecordBatch::try_from_iter(vec![("nested", nested)]).unwrap()).unwrap()
        );
        assert!(batch_is_ascii(&RecordBatch::new_empty(Arc::new(
            arrow_schema::Schema::empty()
        )))
        .unwrap());
    }

    #[test]
    fn variadic_reads_one_row_and_preserves_nulls_and_heterogeneous_types() {
        let columns: Vec<ArrayRef> = vec![
            Arc::new(Int64Array::from(vec![Some(42), None])),
            Arc::new(StringArray::from(vec!["first", "second"])),
        ];
        let values = Variadic::<Option<Generic<'_>>>::from_row(&columns, 1).unwrap();
        assert!(values.get(0).unwrap().is_none());
        assert_eq!(
            values.get(1).unwrap().unwrap().get::<&str>().unwrap(),
            "second"
        );
        assert!(Variadic::<i64>::from_row(&columns[..1], 1).is_err());
        assert!(Variadic::<Option<i64>>::from_row(&columns[..0], 1)
            .unwrap()
            .is_empty());
        assert!(Generic::new(columns[0].as_ref(), 2).is_err());
        assert!(Variadic::<Option<i64>>::from_row(&columns[..1], 2).is_err());
        let unknown = NullArray::new(1);
        let unknown = Generic::new(&unknown, 0).unwrap();
        assert!(unknown.is_null());
        assert_eq!(unknown.get::<Option<i64>>().unwrap(), None);
    }

    #[test]
    fn generic_nested_results_preserve_children_and_row_errors() {
        let lists = ListArray::from_iter_primitive::<Int64Type, _, _>([
            Some(vec![Some(42), None]),
            Some(vec![]),
            None,
        ]);
        let first = Generic::new(&lists, 0).unwrap();
        assert_eq!(first.list_len().unwrap(), 2);
        assert!(first
            .element(1)
            .unwrap()
            .get::<Option<i64>>()
            .unwrap()
            .is_none());
        assert!(first.element(2).is_err());
        assert!(Generic::new(&lists, 2).unwrap().element(0).is_err());
        let mut builder = RowOutputBuilder::new(lists.data_type().clone(), 3);
        builder.append(Some(first));
        builder.append_error("empty array".into());
        builder.append(None::<Generic<'_>>);
        let (values, errors) = builder.finish().unwrap();
        let values = values.as_any().downcast_ref::<ListArray>().unwrap();
        assert_eq!(values.value(0).as_ref(), lists.value(0).as_ref());
        assert!(values.is_null(1) && values.is_null(2));
        let errors = errors.as_any().downcast_ref::<StringArray>().unwrap();
        assert!(errors.is_null(0) && errors.is_null(2));
        assert_eq!(errors.value(1), "empty array");
    }

    #[test]
    fn contiguous_borrowed_outputs_retain_buffers_and_wire_metadata() {
        use crate::Value;
        let cases = [
            ("bigint", Value::BigInt(42)),
            ("varchar", Value::VarcharBytes(vec![0xff, 0, 0xfe])),
            ("hugeint", Value::HugeInt(i128::MAX)),
            (
                "timestamp",
                Value::Timestamp(Timestamp::from_seconds_nanos(123, 456).unwrap()),
            ),
            (
                "array(bigint)",
                Value::Array(vec![Value::BigInt(42), Value::Null]),
            ),
            ("row(x bigint)", Value::Row(vec![Value::BigInt(42)])),
            (
                "map(bigint,bigint)",
                Value::Map(vec![(Value::BigInt(42), Value::Null)]),
            ),
        ];
        fn same_buffers(left: &arrow_data::ArrayData, right: &arrow_data::ArrayData) {
            for (a, b) in left.buffers().iter().zip(right.buffers()) {
                assert_eq!(a.as_ptr(), b.as_ptr());
            }
            for (a, b) in left.child_data().iter().zip(right.child_data()) {
                same_buffers(a, b);
            }
        }
        for (sql, value) in cases {
            let ty = crate::runtime::parse_row_arrow_type(sql).unwrap();
            let input =
                crate::value::build_array(&ty, &[value.clone(), Value::Null, value.clone(), value])
                    .unwrap();
            // A sliced input has its own offsets and retains NULL parent rows.
            let input = input.slice(1, 3);
            let mut builder = RowOutputBuilder::new(ty.clone(), 3);
            for row in 0..3 {
                builder.append(Generic::new(input.as_ref(), row).unwrap());
            }
            let (output, errors) = builder.finish().unwrap();
            assert_eq!(output.data_type(), &ty, "{sql}");
            assert_eq!(output.as_ref(), input.as_ref(), "{sql}");
            assert!(output.is_null(0));
            assert_eq!(errors.null_count(), 3);
            same_buffers(&input.to_data(), &output.to_data());
        }
    }

    #[test]
    fn borrowed_gather_handles_reordering_multiple_sources_and_error_nulls() {
        let first = ListArray::from_iter_primitive::<Int64Type, _, _>([
            Some(vec![Some(10), None]),
            None,
            Some(vec![Some(30)]),
        ]);
        let second =
            ListArray::from_iter_primitive::<Int64Type, _, _>([Some(vec![]), Some(vec![Some(40)])]);
        let first = first.slice(1, 2);
        let mut builder = RowOutputBuilder::new(first.data_type().clone(), 6);
        builder.append(Generic::new(&first, 1).unwrap());
        builder.append_error("rejected row".into());
        builder.append(None::<Generic<'_>>);
        builder.append(Generic::new(&second, 0).unwrap());
        builder.append(Generic::new(&second, 1).unwrap());
        builder.append(Generic::new(&first, 0).unwrap());
        let (output, errors) = builder.finish().unwrap();
        let expected = ListArray::from_iter_primitive::<Int64Type, _, _>([
            Some(vec![Some(30)]),
            None,
            None,
            Some(vec![]),
            Some(vec![Some(40)]),
            None,
        ]);
        assert_eq!(output.as_ref(), &expected);
        let errors = errors.as_any().downcast_ref::<StringArray>().unwrap();
        assert_eq!(errors.value(1), "rejected row");
        assert_eq!(errors.null_count(), 5);
    }

    #[test]
    fn borrowed_fast_path_does_not_bypass_type_errors_or_wire_conversion() {
        let ty = crate::runtime::parse_row_arrow_type("varchar").unwrap();
        let input = StringArray::from(vec!["legacy"]);
        let mut builder = RowOutputBuilder::new(ty.clone(), 2);
        builder.append(Generic::new(&input, 0).unwrap());
        let wrong = Int64Array::from(vec![42]);
        builder.append(Generic::new(&wrong, 0).unwrap());
        let (output, errors) = builder.finish().unwrap();
        assert_eq!(output.data_type(), &ty);
        assert_eq!(
            Generic::new(output.as_ref(), 0)
                .unwrap()
                .get::<&str>()
                .unwrap(),
            "legacy"
        );
        assert!(output.is_null(1));
        assert!(!errors.is_null(1));
    }

    #[test]
    fn bound_type_builds_empty_null_and_decimal_results() {
        for data_type in [DataType::Int64, DataType::Decimal128(20, 4), DataType::Null] {
            let (empty, errors) = RowOutputBuilder::new(data_type.clone(), 0)
                .finish()
                .unwrap();
            assert_eq!(empty.data_type(), &data_type);
            assert_eq!(empty.len(), 0);
            assert_eq!(errors.len(), 0);
            let mut builder = RowOutputBuilder::new(data_type.clone(), 1);
            builder.append(None::<Generic<'_>>);
            let (nulls, _) = builder.finish().unwrap();
            assert_eq!(nulls.data_type(), &data_type);
            assert_eq!(nulls.logical_null_count(), 1);
        }
        let mut builder = RowOutputBuilder::new(DataType::Decimal128(20, 4), 1);
        builder.append(Decimal {
            value: 12345,
            precision: 20,
            scale: 4,
        });
        let (values, _) = builder.finish().unwrap();
        assert_eq!(
            values
                .as_any()
                .downcast_ref::<Decimal128Array>()
                .unwrap()
                .value(0),
            12345
        );
        let mut builder = RowOutputBuilder::new(DataType::Decimal128(20, 4), 1);
        builder.append(Decimal {
            value: 12345,
            precision: 10,
            scale: 4,
        });
        let (values, errors) = builder.finish().unwrap();
        assert!(values.is_null(0));
        assert!(errors
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap()
            .value(0)
            .contains("precision/scale"));
    }
}
