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

//! Owned single SQL values and schema-directed nested result construction.
use crate::scalar::{Generic, OutputValue, RowOutput};
use crate::{Date32, Decimal, Timestamp};
use arrow_array::*;
use arrow_buffer::{NullBuffer, OffsetBuffer, ScalarBuffer};
use arrow_schema::DataType;
use std::sync::Arc;

type Result<T> = std::result::Result<T, String>;

/// A single result value. Borrowed children retain their input lifetime;
/// materialize() produces an independent Value<'static>.
#[derive(Clone)]
pub enum Value<'a> {
    Null,
    Boolean(bool),
    TinyInt(i8),
    SmallInt(i16),
    Integer(i32),
    BigInt(i64),
    HugeInt(i128),
    Real(f32),
    Double(f64),
    Varchar(String),
    VarcharBytes(Vec<u8>),
    Varbinary(Vec<u8>),
    Date(Date32),
    Timestamp(Timestamp),
    Decimal(Decimal),
    Array(Vec<Value<'a>>),
    Map(Vec<(Value<'a>, Value<'a>)>),
    Row(Vec<Value<'a>>),
    Custom(Box<crate::CustomValue<Vec<u8>>>),
    Opaque {
        scope: u64,
        handle: u64,
        type_identity: String,
    },
    Borrowed(Generic<'a>),
}
impl<'a> Default for Value<'a> {
    fn default() -> Self {
        Self::Null
    }
}
impl<'a> Value<'a> {
    pub fn is_null(&self) -> bool {
        matches!(self, Self::Null) || matches!(self, Self::Borrowed(v) if v.is_null())
    }
    pub fn contains_nulls(&self) -> Result<bool> {
        match self {
            Self::Borrowed(v) => v.contains_nulls(),
            Self::Null => Ok(true),
            Self::Array(items) | Self::Row(items) => {
                for item in items {
                    if item.contains_nulls()? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            Self::Map(items) => {
                for (k, v) in items {
                    if k.contains_nulls()? || v.contains_nulls()? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            _ => Ok(false),
        }
    }
    pub fn materialize(&self) -> Result<Value<'static>> {
        Ok(match self {
            Self::Borrowed(v) => return v.materialize(),
            Self::Null => Value::Null,
            Self::Boolean(v) => Value::Boolean(*v),
            Self::TinyInt(v) => Value::TinyInt(*v),
            Self::SmallInt(v) => Value::SmallInt(*v),
            Self::Integer(v) => Value::Integer(*v),
            Self::BigInt(v) => Value::BigInt(*v),
            Self::HugeInt(v) => Value::HugeInt(*v),
            Self::Real(v) => Value::Real(*v),
            Self::Double(v) => Value::Double(*v),
            Self::Varchar(v) => Value::Varchar(v.clone()),
            Self::VarcharBytes(v) => Value::VarcharBytes(v.clone()),
            Self::Varbinary(v) => Value::Varbinary(v.clone()),
            Self::Date(v) => Value::Date(*v),
            Self::Timestamp(v) => Value::Timestamp(*v),
            Self::Decimal(v) => Value::Decimal(*v),
            Self::Custom(v) => Value::Custom(v.clone()),
            Self::Opaque {
                scope,
                handle,
                type_identity,
            } => Value::Opaque {
                scope: *scope,
                handle: *handle,
                type_identity: type_identity.clone(),
            },
            Self::Array(v) => {
                Value::Array(v.iter().map(Value::materialize).collect::<Result<_>>()?)
            }
            Self::Row(v) => Value::Row(v.iter().map(Value::materialize).collect::<Result<_>>()?),
            Self::Map(v) => Value::Map(
                v.iter()
                    .map(|(k, v)| Ok((k.materialize()?, v.materialize()?)))
                    .collect::<Result<_>>()?,
            ),
        })
    }
    pub(crate) fn from_output(value: OutputValue<'a>) -> Self {
        match value {
            OutputValue::Null => Self::Null,
            OutputValue::Generic(v) => Self::Borrowed(v),
            OutputValue::Owned(v) => v,
            OutputValue::Bool(v) => Self::Boolean(v),
            OutputValue::I8(v) => Self::TinyInt(v),
            OutputValue::I16(v) => Self::SmallInt(v),
            OutputValue::I32(v) => Self::Integer(v),
            OutputValue::I64(v) => Self::BigInt(v),
            OutputValue::F32(v) => Self::Real(v),
            OutputValue::F64(v) => Self::Double(v),
            OutputValue::String(v) => Self::Varchar(v),
            OutputValue::Binary(v) => Self::Varbinary(v),
            OutputValue::Date(v) => Self::Date(v),
            OutputValue::Time(v) => Self::Timestamp(v),
            OutputValue::Decimal(v) => Self::Decimal(v),
        }
    }
}
impl<'a> RowOutput<'a> for Value<'a> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(self)
    }
}

impl<'a> Generic<'a> {
    pub fn materialize(&self) -> Result<Value<'static>> {
        if self.is_null() {
            return Ok(Value::Null);
        }
        if crate::custom::marker(self.data_type()) == Some("custom_codec_v1") {
            return Ok(Value::Custom(Box::new(
                self.get::<crate::CustomValue<Vec<u8>>>()?,
            )));
        }
        if crate::custom::marker(self.data_type()) == Some("opaque_handle_v1") {
            let value = self.get::<crate::OpaqueHandle>()?;
            return Ok(Value::Opaque {
                scope: value.scope,
                handle: value.handle,
                type_identity: value.type_identity.to_owned(),
            });
        }
        if crate::is_varchar_wire_type(self.data_type()) {
            return Ok(Value::VarcharBytes(
                self.get::<crate::VarcharBytes<&[u8]>>()?.0.to_vec(),
            ));
        }
        if crate::is_hugeint_wire_type(self.data_type()) {
            return Ok(Value::HugeInt(self.get::<i128>()?));
        }
        if Timestamp::is_wire_type(self.data_type()) {
            return Ok(Value::Timestamp(self.get::<Timestamp>()?));
        }
        macro_rules! scalar {
            ($variant:ident, $ty:ty) => {
                Value::$variant(self.get::<$ty>()?)
            };
        }
        Ok(match self.data_type() {
            DataType::Boolean => scalar!(Boolean, bool),
            DataType::Int8 => scalar!(TinyInt, i8),
            DataType::Int16 => scalar!(SmallInt, i16),
            DataType::Int32 => scalar!(Integer, i32),
            DataType::Int64 => scalar!(BigInt, i64),
            DataType::Float32 => scalar!(Real, f32),
            DataType::Float64 => scalar!(Double, f64),
            DataType::Utf8 => scalar!(Varchar, String),
            DataType::Binary => scalar!(Varbinary, Vec<u8>),
            DataType::Date32 => scalar!(Date, Date32),
            DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None) => {
                scalar!(Timestamp, Timestamp)
            }
            DataType::Decimal128(..) => scalar!(Decimal, Decimal),
            DataType::List(_) => Value::Array(
                (0..self.list_len()?)
                    .map(|i| self.element(i)?.materialize())
                    .collect::<Result<_>>()?,
            ),
            DataType::Map(..) => Value::Map(
                (0..self.map_len()?)
                    .map(|i| {
                        let (k, v) = self.map_entry(i)?;
                        Ok((k.materialize()?, v.materialize()?))
                    })
                    .collect::<Result<_>>()?,
            ),
            DataType::Struct(_) => Value::Row(
                (0..self.row_len()?)
                    .map(|i| self.field_at(i)?.materialize())
                    .collect::<Result<_>>()?,
            ),
            ty => return Err(format!("unsupported SQL value type {ty:?}")),
        })
    }
    /// Includes the value itself and recursively all nested children.
    pub fn contains_nulls(&self) -> Result<bool> {
        if !crate::scalar::array_may_have_recursive_nulls(self.array) {
            return Ok(false);
        }
        if self.is_null() {
            return Ok(true);
        }
        match self.data_type() {
            DataType::List(_) => {
                for i in 0..self.list_len()? {
                    if self.element(i)?.contains_nulls()? {
                        return Ok(true);
                    }
                }
            }
            DataType::Map(..) => {
                for i in 0..self.map_len()? {
                    let (k, v) = self.map_entry(i)?;
                    if k.contains_nulls()? || v.contains_nulls()? {
                        return Ok(true);
                    }
                }
            }
            DataType::Struct(_) => {
                for i in 0..self.row_len()? {
                    if self.field_at(i)?.contains_nulls()? {
                        return Ok(true);
                    }
                }
            }
            _ => {}
        }
        Ok(false)
    }
}

/// Compare logical types across old Arrow scalars and marked row-ABI values.
/// Marker recognition precedes STRUCT comparison so ordinary SQL ROWs do not
/// become timestamps, strings, or integers just because their fields match.
pub(crate) fn same_sql_type(a: &DataType, b: &DataType) -> bool {
    // The common bound-type case needs no reconstructed nested schema.
    if a == b {
        return true;
    }
    if crate::custom::is_custom(a) || crate::custom::is_custom(b) {
        return a == b;
    }
    fn normalize(ty: &DataType) -> DataType {
        if crate::is_varchar_wire_type(ty) {
            return DataType::Utf8;
        }
        if crate::Timestamp::is_wire_type(ty) {
            return DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None);
        }
        if crate::is_hugeint_wire_type(ty) {
            return DataType::FixedSizeBinary(16);
        }
        match ty {
            DataType::List(field) => DataType::List(Arc::new(
                field
                    .as_ref()
                    .clone()
                    .with_data_type(normalize(field.data_type())),
            )),
            DataType::Map(field, sorted) => DataType::Map(
                Arc::new(
                    field
                        .as_ref()
                        .clone()
                        .with_data_type(normalize(field.data_type())),
                ),
                *sorted,
            ),
            DataType::Struct(fields) => DataType::Struct(
                fields
                    .iter()
                    .map(|f| Arc::new(f.as_ref().clone().with_data_type(normalize(f.data_type()))))
                    .collect::<Vec<_>>()
                    .into(),
            ),
            ty => ty.clone(),
        }
    }
    fn custom_identities(a: &DataType, b: &DataType) -> bool {
        if crate::custom::is_custom(a) || crate::custom::is_custom(b) {
            return a == b;
        }
        match (a, b) {
            (DataType::List(a), DataType::List(b)) | (DataType::Map(a, _), DataType::Map(b, _)) => {
                custom_identities(a.data_type(), b.data_type())
            }
            (DataType::Struct(a), DataType::Struct(b)) => {
                a.len() == b.len()
                    && a.iter()
                        .zip(b)
                        .all(|(a, b)| custom_identities(a.data_type(), b.data_type()))
            }
            _ => true,
        }
    }
    let (a, b) = (normalize(a), normalize(b));
    a.equals_datatype(&b) && custom_identities(&a, &b)
}

/// Validate writer contracts before recording the result, so bad map keys or
/// decimal overflow are isolated to the offending row and work with SQL TRY.
pub(crate) fn validate(value: &Value<'_>, ty: &DataType) -> Result<()> {
    use Value::*;
    if let Borrowed(v) = value {
        if !same_sql_type(v.data_type(), ty) {
            return Err("borrowed result does not match bound SQL type".into());
        }
        return Ok(());
    }
    if value.is_null() {
        return Ok(());
    }
    if crate::custom::is_custom(ty) || matches!(value, Custom(_) | Opaque { .. }) {
        return crate::custom::validate(value, ty);
    }
    if matches!(value, Varchar(_) | VarcharBytes(_)) && crate::is_varchar_wire_type(ty) {
        return Ok(());
    }
    if let VarcharBytes(value) = value {
        if ty == &DataType::Utf8 {
            std::str::from_utf8(value).map_err(|_| "legacy VARCHAR requires UTF-8")?;
            return Ok(());
        }
    }
    if matches!(value, HugeInt(_)) && crate::is_hugeint_wire_type(ty) {
        return Ok(());
    }
    if let Timestamp(value) = value {
        value.validate()?;
        if crate::Timestamp::is_wire_type(ty) {
            return Ok(());
        }
        if matches!(
            ty,
            DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None)
        ) {
            i64::try_from(value.0).map_err(|_| "legacy timestamp exceeds nanosecond range")?;
            return Ok(());
        }
    }
    match (value, ty) {
        (Boolean(_), DataType::Boolean)
        | (TinyInt(_), DataType::Int8)
        | (SmallInt(_), DataType::Int16)
        | (Integer(_), DataType::Int32)
        | (BigInt(_), DataType::Int64)
        | (Real(_), DataType::Float32)
        | (Double(_), DataType::Float64)
        | (Varchar(_), DataType::Utf8)
        | (Varbinary(_), DataType::Binary)
        | (Date(_), DataType::Date32)
        | (Timestamp(_), DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None)) => Ok(()),
        (Decimal(v), DataType::Decimal128(p, s)) => {
            if v.precision != *p || v.scale != *s {
                return Err("decimal precision/scale does not match bound SQL type".into());
            }
            let max = 10_i128.pow(*p as u32) - 1;
            if v.value > max || v.value < -max {
                return Err("decimal result exceeds bound precision".into());
            }
            Ok(())
        }
        (Array(values), DataType::List(field)) => {
            for v in values {
                validate(v, field.data_type())?;
            }
            Ok(())
        }
        (Row(values), DataType::Struct(fields)) => {
            if values.len() != fields.len() {
                return Err("row result field count does not match bound SQL type".into());
            }
            for (v, field) in values.iter().zip(fields) {
                validate(v, field.data_type())?;
            }
            Ok(())
        }
        (Map(values), DataType::Map(field, _)) => {
            let DataType::Struct(fields) = field.data_type() else {
                return Err("invalid map schema".into());
            };
            for (k, v) in values {
                if k.is_null() {
                    return Err("map key must not be null".into());
                }
                validate(k, fields[0].data_type())?;
                validate(v, fields[1].data_type())?;
            }
            Ok(())
        }
        _ => Err(format!("result value does not match bound SQL type {ty:?}")),
    }
}

pub(crate) fn build_array(ty: &DataType, values: &[Value<'_>]) -> Result<ArrayRef> {
    if crate::custom::is_custom(ty) {
        return crate::custom::build(ty, values);
    }

    // Primitive columns need no owned Value tree. Read borrowed leaves and
    // copy owned scalars directly into the Arrow builder's typed input.
    macro_rules! primitive {
        ($variant:ident, $native:ty, $array:ty) => {{
            let data = values
                .iter()
                .map(|value| {
                    if value.is_null() {
                        return Ok(None);
                    }
                    match value {
                        Value::$variant(value) => Ok(Some(*value)),
                        Value::Borrowed(value) => value.get::<$native>().map(Some),
                        _ => Err(format!("result value does not match bound SQL type {ty:?}")),
                    }
                })
                .collect::<Result<Vec<_>>>()?;
            return Ok(Arc::new(<$array>::from(data)));
        }};
    }
    match ty {
        DataType::Boolean => primitive!(Boolean, bool, BooleanArray),
        DataType::Int8 => primitive!(TinyInt, i8, Int8Array),
        DataType::Int16 => primitive!(SmallInt, i16, Int16Array),
        DataType::Int32 => primitive!(Integer, i32, Int32Array),
        DataType::Int64 => primitive!(BigInt, i64, Int64Array),
        DataType::Float32 => primitive!(Real, f32, Float32Array),
        DataType::Float64 => primitive!(Double, f64, Float64Array),
        _ => {}
    }

    let values = values
        .iter()
        .map(Value::materialize)
        .collect::<Result<Vec<_>>>()?;
    for v in &values {
        validate(v, ty)?;
    }
    let validity = || NullBuffer::from(values.iter().map(|v| !v.is_null()).collect::<Vec<_>>());
    let offsets = |lengths: Vec<usize>| -> Result<OffsetBuffer<i32>> {
        let mut offsets = vec![0_i32];
        for length in lengths {
            offsets.push(
                offsets
                    .last()
                    .unwrap()
                    .checked_add(i32::try_from(length).map_err(|_| "nested result too large")?)
                    .ok_or("nested result exceeds Arrow offset range")?,
            );
        }
        Ok(OffsetBuffer::new(ScalarBuffer::from(offsets)))
    };
    if crate::is_varchar_wire_type(ty) {
        let DataType::Struct(fields) = ty else {
            unreachable!()
        };
        let bytes = values
            .iter()
            .map(|v| match v {
                Value::Varchar(v) => Value::Varbinary(v.as_bytes().to_vec()),
                Value::VarcharBytes(v) => Value::Varbinary(v.clone()),
                _ => Value::Null,
            })
            .collect::<Vec<_>>();
        return Ok(Arc::new(
            StructArray::try_new(
                fields.clone(),
                vec![build_array(&DataType::Binary, &bytes)?],
                Some(validity()),
            )
            .map_err(|e| e.to_string())?,
        ));
    }
    if crate::is_hugeint_wire_type(ty) {
        let DataType::Struct(fields) = ty else {
            unreachable!()
        };
        let halves = [64, 0].map(|shift| {
            values
                .iter()
                .map(|v| match v {
                    Value::HugeInt(v) => Value::BigInt((*v >> shift) as i64),
                    _ => Value::Null,
                })
                .collect::<Vec<_>>()
        });
        return Ok(Arc::new(
            StructArray::try_new(
                fields.clone(),
                vec![
                    build_array(&DataType::Int64, &halves[0])?,
                    build_array(&DataType::Int64, &halves[1])?,
                ],
                Some(validity()),
            )
            .map_err(|e| e.to_string())?,
        ));
    }
    if Timestamp::is_wire_type(ty) {
        let DataType::Struct(fields) = ty else {
            unreachable!()
        };
        let seconds = values
            .iter()
            .map(|v| {
                if let Value::Timestamp(v) = v {
                    Value::BigInt(v.seconds())
                } else {
                    Value::Null
                }
            })
            .collect::<Vec<_>>();
        let nanos = values
            .iter()
            .map(|v| {
                if let Value::Timestamp(v) = v {
                    Value::BigInt(v.nanos() as i64)
                } else {
                    Value::Null
                }
            })
            .collect::<Vec<_>>();
        return Ok(Arc::new(
            StructArray::try_new(
                fields.clone(),
                vec![
                    build_array(&DataType::Int64, &seconds)?,
                    build_array(&DataType::Int64, &nanos)?,
                ],
                Some(validity()),
            )
            .map_err(|e| e.to_string())?,
        ));
    }
    Ok(match ty {
        DataType::Null => new_null_array(ty, values.len()),
        DataType::Utf8 => Arc::new(StringArray::from_iter(values.iter().map(|v| match v {
            Value::Varchar(v) => Some(v.as_str()),
            Value::VarcharBytes(v) => Some(std::str::from_utf8(v).unwrap()),
            _ => None,
        }))),
        DataType::Binary => Arc::new(BinaryArray::from_iter(values.iter().map(|v| match v {
            Value::Varbinary(v) => Some(v.as_slice()),
            _ => None,
        }))),
        DataType::Date32 => Arc::new(Date32Array::from(
            values
                .iter()
                .map(|v| match v {
                    Value::Date(v) => Some(v.0),
                    _ => None,
                })
                .collect::<Vec<_>>(),
        )),
        DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None) => {
            Arc::new(TimestampNanosecondArray::from(
                values
                    .iter()
                    .map(|v| match v {
                        Value::Timestamp(v) => Some(v.0 as i64),
                        _ => None,
                    })
                    .collect::<Vec<_>>(),
            ))
        }
        DataType::Decimal128(p, s) => Arc::new(
            Decimal128Array::from(
                values
                    .iter()
                    .map(|v| match v {
                        Value::Decimal(v) => Some(v.value),
                        _ => None,
                    })
                    .collect::<Vec<_>>(),
            )
            .with_precision_and_scale(*p, *s)
            .map_err(|e| e.to_string())?,
        ),
        DataType::List(field) => {
            let mut children = vec![];
            let mut lengths = vec![];
            for v in &values {
                if let Value::Array(items) = v {
                    lengths.push(items.len());
                    children.extend(items.iter().cloned());
                } else {
                    lengths.push(0);
                }
            }
            Arc::new(
                ListArray::try_new(
                    field.clone(),
                    offsets(lengths)?,
                    build_array(field.data_type(), &children)?,
                    Some(validity()),
                )
                .map_err(|e| e.to_string())?,
            )
        }
        DataType::Struct(fields) => {
            let mut columns = vec![];
            for (i, field) in fields.iter().enumerate() {
                let children = values
                    .iter()
                    .map(|v| {
                        if let Value::Row(items) = v {
                            items[i].clone()
                        } else {
                            Value::Null
                        }
                    })
                    .collect::<Vec<_>>();
                columns.push(build_array(field.data_type(), &children)?);
            }
            // Explicit length is required for row() with zero fields.
            Arc::new(
                StructArray::try_new_with_length(
                    fields.clone(),
                    columns,
                    Some(validity()),
                    values.len(),
                )
                .map_err(|e| e.to_string())?,
            )
        }
        DataType::Map(field, sorted) => {
            let DataType::Struct(fields) = field.data_type() else {
                return Err("invalid map schema".into());
            };
            let mut keys = vec![];
            let mut items = vec![];
            let mut lengths = vec![];
            for v in &values {
                if let Value::Map(entries) = v {
                    lengths.push(entries.len());
                    for (k, v) in entries {
                        keys.push(k.clone());
                        items.push(v.clone());
                    }
                } else {
                    lengths.push(0);
                }
            }
            let entries = StructArray::try_new(
                fields.clone(),
                vec![
                    build_array(fields[0].data_type(), &keys)?,
                    build_array(fields[1].data_type(), &items)?,
                ],
                None,
            )
            .map_err(|e| e.to_string())?;
            Arc::new(
                MapArray::try_new(
                    field.clone(),
                    offsets(lengths)?,
                    entries,
                    Some(validity()),
                    *sorted,
                )
                .map_err(|e| e.to_string())?,
            )
        }
        _ => return Err(format!("unsupported bound SQL result type {ty:?}")),
    })
}
