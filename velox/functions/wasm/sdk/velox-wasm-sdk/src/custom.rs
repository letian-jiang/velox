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

//! Portable custom values and invocation-scoped OPAQUE capabilities.
use crate::scalar::{Generic, OutputValue, RowArgument, RowOutput};
use crate::{CompareFlags, Value};
use arrow_array::{Array, ArrayRef, BinaryArray, Int64Array, StructArray};
use arrow_buffer::NullBuffer;
use arrow_schema::{DataType, Field};
use std::{
    cmp::Ordering,
    collections::HashMap,
    sync::{Arc, OnceLock, RwLock},
};

type Result<T> = std::result::Result<T, String>;

/// Bound codec header, suitable for constructing values without exposing batches.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CodecType {
    pub codec_id: String,
    pub version: u32,
    pub type_identity: String,
}
impl CodecType {
    pub fn value<T>(&self, payload: T) -> CustomValue<T> {
        CustomValue::new(&self.codec_id, self.version, &self.type_identity, payload)
    }
    pub(crate) fn from_type(ty: &DataType) -> Result<Option<Self>> {
        if marker(ty) != Some("custom_codec_v1") {
            return Ok(None);
        }
        let id = property(ty, "velox.codec_id")
            .filter(|v| !v.is_empty())
            .ok_or("missing codec ID")?;
        let version = property(ty, "velox.codec_version")
            .ok_or("missing codec version")?
            .parse::<u32>()
            .map_err(|e| e.to_string())?;
        if version == 0 {
            return Err("invalid codec version".into());
        }
        let identity = property(ty, "velox.type_identity")
            .filter(|v| !v.is_empty())
            .ok_or("missing codec type identity")?;
        Ok(Some(Self {
            codec_id: id.to_owned(),
            version,
            type_identity: identity.to_owned(),
        }))
    }
}
impl Generic<'_> {
    pub fn codec_type(&self) -> Result<Option<CodecType>> {
        CodecType::from_type(self.data_type())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CustomValue<T> {
    pub codec_id: String,
    pub version: u32,
    pub type_identity: String,
    pub payload: T,
}
impl<T> CustomValue<T> {
    pub fn new(
        codec_id: impl Into<String>,
        version: u32,
        type_identity: impl Into<String>,
        payload: T,
    ) -> Self {
        Self {
            codec_id: codec_id.into(),
            version,
            type_identity: type_identity.into(),
            payload,
        }
    }
    pub fn map_payload<U>(self, f: impl FnOnce(T) -> U) -> CustomValue<U> {
        CustomValue::new(
            self.codec_id,
            self.version,
            self.type_identity,
            f(self.payload),
        )
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OpaqueHandle<'a> {
    pub scope: u64,
    pub handle: u64,
    pub type_identity: &'a str,
}
impl<'a> RowArgument<'a> for OpaqueHandle<'a> {
    fn accepts(ty: &DataType) -> bool {
        marker(ty) == Some("opaque_handle_v1")
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        if !Self::accepts(array.data_type()) {
            return Err("expected OPAQUE handle".into());
        }
        let value = Generic::new(array, row)?;
        Ok(Self {
            scope: value.field_at(0)?.get::<i64>()? as u64,
            handle: value.field_at(1)?.get::<i64>()? as u64,
            type_identity: property(array.data_type(), "velox.type_identity")
                .ok_or("missing OPAQUE identity")?,
        })
    }
}
impl<'a> RowOutput<'a> for OpaqueHandle<'a> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(Value::Opaque {
            scope: self.scope,
            handle: self.handle,
            type_identity: self.type_identity.to_owned(),
        })
    }
}
impl<'a> RowArgument<'a> for CustomValue<&'a [u8]> {
    fn accepts(ty: &DataType) -> bool {
        marker(ty) == Some("custom_codec_v1")
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        if !Self::accepts(array.data_type()) {
            return Err("expected custom codec value".into());
        }
        Ok(Self::new(
            property(array.data_type(), "velox.codec_id").ok_or("missing codec ID")?,
            property(array.data_type(), "velox.codec_version")
                .ok_or("missing codec version")?
                .parse::<u32>()
                .map_err(|e| e.to_string())?,
            property(array.data_type(), "velox.type_identity").ok_or("missing type identity")?,
            Generic::new(array, row)?.field_at(0)?.get::<&[u8]>()?,
        ))
    }
}
impl<'a> RowArgument<'a> for CustomValue<Vec<u8>> {
    fn accepts(ty: &DataType) -> bool {
        <CustomValue<&[u8]>>::accepts(ty)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        Ok(<CustomValue<&[u8]>>::read(array, row)?.map_payload(|v| v.to_vec()))
    }
}
impl<'a, T: AsRef<[u8]>> RowOutput<'a> for CustomValue<T> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(Value::Custom(Box::new(
            self.map_payload(|v| v.as_ref().to_vec()),
        )))
    }
}
impl<'a> crate::Materialize for CustomValue<&'a [u8]> {
    type Owned = CustomValue<Vec<u8>>;
    fn materialize(self) -> Result<Self::Owned> {
        Ok(self.map_payload(|v| v.to_vec()))
    }
}
impl crate::Materialize for CustomValue<Vec<u8>> {
    type Owned = Self;
    fn materialize(self) -> Result<Self> {
        Ok(self)
    }
}

pub(crate) fn property<'a>(ty: &'a DataType, key: &str) -> Option<&'a str> {
    let DataType::Struct(fields) = ty else {
        return None;
    };
    fields.first()?.metadata().get(key).map(String::as_str)
}
pub(crate) fn marker(ty: &DataType) -> Option<&str> {
    property(ty, "velox.logical_type")
}
pub(crate) fn is_custom(ty: &DataType) -> bool {
    matches!(marker(ty), Some("custom_codec_v1" | "opaque_handle_v1"))
}
pub(crate) fn codec_type(id: &str, version: u32, identity: &str) -> DataType {
    DataType::Struct(
        vec![Arc::new(
            Field::new("payload", DataType::Binary, true).with_metadata(HashMap::from([
                ("velox.logical_type".into(), "custom_codec_v1".into()),
                ("velox.codec_id".into(), id.into()),
                ("velox.codec_version".into(), version.to_string()),
                ("velox.type_identity".into(), identity.into()),
            ])),
        )]
        .into(),
    )
}
pub(crate) fn opaque_type(identity: &str) -> DataType {
    DataType::Struct(
        vec![
            Arc::new(
                Field::new("scope", DataType::Int64, true).with_metadata(HashMap::from([
                    ("velox.logical_type".into(), "opaque_handle_v1".into()),
                    ("velox.type_identity".into(), identity.into()),
                ])),
            ),
            Arc::new(Field::new("handle", DataType::Int64, true)),
        ]
        .into(),
    )
}
pub(crate) fn validate(value: &Value<'_>, ty: &DataType) -> Result<()> {
    let matches = match value {
        Value::Custom(v) => ty == &codec_type(&v.codec_id, v.version, &v.type_identity),
        Value::Opaque { type_identity, .. } => ty == &opaque_type(type_identity),
        _ => false,
    };
    if matches {
        Ok(())
    } else {
        Err("custom/OPAQUE result identity or codec version does not match bound type".into())
    }
}
pub(crate) fn build(ty: &DataType, values: &[Value<'_>]) -> Result<ArrayRef> {
    let DataType::Struct(fields) = ty else {
        return Err("expected bridge struct".into());
    };
    let values = values
        .iter()
        .map(Value::materialize)
        .collect::<Result<Vec<_>>>()?;
    let nulls = Some(NullBuffer::from(
        values.iter().map(|v| !v.is_null()).collect::<Vec<_>>(),
    ));
    let columns: Vec<ArrayRef> = if marker(ty) == Some("custom_codec_v1") {
        vec![Arc::new(BinaryArray::from_iter(values.iter().map(
            |v| match v {
                Value::Custom(v) => Some(v.payload.as_slice()),
                _ => None,
            },
        )))]
    } else {
        vec![
            Arc::new(Int64Array::from_iter(values.iter().map(|v| match v {
                Value::Opaque { scope, .. } => Some(*scope as i64),
                _ => None,
            }))),
            Arc::new(Int64Array::from_iter(values.iter().map(|v| match v {
                Value::Opaque { handle, .. } => Some(*handle as i64),
                _ => None,
            }))),
        ]
    };
    StructArray::try_new(fields.clone(), columns, nulls)
        .map(|v| Arc::new(v) as ArrayRef)
        .map_err(|e| e.to_string())
}

/// Portable implementations must reproduce the corresponding native SQL type.
/// Register once during initialization, before comparison or hashing.
#[derive(Clone, Copy)]
pub struct CodecSemantics {
    pub compare: fn(&[u8], &[u8], CompareFlags) -> Result<Option<Ordering>>,
    pub hash: fn(&[u8]) -> Result<u64>,
}
static SEMANTICS: OnceLock<RwLock<HashMap<(String, u32), CodecSemantics>>> = OnceLock::new();
pub fn register_codec_semantics(id: &str, version: u32, semantics: CodecSemantics) -> Result<()> {
    if id.is_empty() || version == 0 {
        return Err("codec ID/version must be nonempty/positive".into());
    }
    let mut registry = SEMANTICS
        .get_or_init(Default::default)
        .write()
        .map_err(|_| "codec registry poisoned")?;
    let key = (id.to_owned(), version);
    if registry.contains_key(&key) {
        return Err("codec semantics already registered".into());
    }
    registry.insert(key, semantics);
    Ok(())
}
pub(crate) fn semantics<T>(value: &CustomValue<T>) -> Result<CodecSemantics> {
    SEMANTICS
        .get_or_init(Default::default)
        .read()
        .map_err(|_| "codec registry poisoned")?
        .get(&(value.codec_id.clone(), value.version))
        .copied()
        .ok_or_else(|| "no comparison/hash semantics registered for custom codec".into())
}

pub(crate) fn parse_description(node: &serde_json::Value) -> Result<DataType> {
    let string = |key: &str| {
        node.get(key)
            .and_then(|v| v.as_str())
            .ok_or_else(|| format!("missing wire type {key}"))
    };
    let children = || {
        node.get("children")
            .and_then(|v| v.as_array())
            .ok_or("missing wire children".to_owned())
    };
    Ok(match string("kind")? {
        "sql" => crate::runtime::parse_row_arrow_type(string("type")?)?,
        "codec" => {
            let version = node
                .get("version")
                .and_then(|v| v.as_u64())
                .filter(|v| *v > 0 && *v <= u32::MAX as u64)
                .ok_or("invalid codec version")?;
            codec_type(string("id")?, version as u32, string("identity")?)
        }
        "opaque" => opaque_type(string("identity")?),
        "array" => {
            let c = children()?;
            if c.len() != 1 {
                return Err("invalid ARRAY wire description".into());
            }
            DataType::List(Arc::new(Field::new(
                "item",
                parse_description(&c[0])?,
                true,
            )))
        }
        "map" => {
            let c = children()?;
            if c.len() != 2 {
                return Err("invalid MAP wire description".into());
            }
            let entries = DataType::Struct(
                vec![
                    Arc::new(Field::new("key", parse_description(&c[0])?, false)),
                    Arc::new(Field::new("value", parse_description(&c[1])?, true)),
                ]
                .into(),
            );
            DataType::Map(Arc::new(Field::new("entries", entries, false)), false)
        }
        "row" => {
            let c = children()?;
            let names = node
                .get("names")
                .and_then(|v| v.as_array())
                .ok_or("missing ROW names")?;
            if names.len() != c.len() {
                return Err("ROW wire arity mismatch".into());
            }
            let fields = c
                .iter()
                .zip(names)
                .map(|(ty, name)| {
                    Ok(Arc::new(Field::new(
                        name.as_str().ok_or("invalid ROW name")?,
                        parse_description(ty)?,
                        true,
                    )))
                })
                .collect::<Result<Vec<_>>>()?;
            DataType::Struct(fields.into())
        }
        _ => return Err("unknown wire type kind".into()),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scalar::RowOutputBuilder;
    #[test]
    fn codec_identity_is_preserved_in_nested_results() {
        let ty = codec_type("test", 1, "sql:custom");
        let v = Value::Custom(Box::new(CustomValue::new(
            "test",
            1,
            "sql:custom",
            vec![0, 255],
        )));
        let array = build(&ty, &[v, Value::Null]).unwrap();
        assert_eq!(
            Generic::new(array.as_ref(), 0)
                .unwrap()
                .get::<CustomValue<&[u8]>>()
                .unwrap()
                .payload,
            &[0, 255]
        );
        assert!(Generic::new(array.as_ref(), 1).unwrap().is_null());
        let other = codec_type("test", 2, "sql:custom");
        let a = DataType::List(Arc::new(Field::new("item", ty.clone(), true)));
        let b = DataType::List(Arc::new(Field::new("item", other.clone(), true)));
        assert!(!crate::value::same_sql_type(&a, &b));
        assert!(!crate::value::same_sql_type(&ty, &other));
        let mut builder = RowOutputBuilder::new(ty, 1);
        builder.append(CustomValue::new("test", 2, "sql:custom", vec![0]));
        let (values, errors) = builder.finish().unwrap();
        assert!(values.is_null(0) && !errors.is_null(0));
    }
    #[test]
    fn opaque_handles_are_distinct_from_rows() {
        let ty = opaque_type("opaque:example");
        let array = build(
            &ty,
            &[Value::Opaque {
                scope: 7,
                handle: 3,
                type_identity: "opaque:example".into(),
            }],
        )
        .unwrap();
        let handle = Generic::new(array.as_ref(), 0)
            .unwrap()
            .get::<OpaqueHandle>()
            .unwrap();
        assert_eq!((handle.scope, handle.handle), (7, 3));
        assert!(!<crate::RowView<'_, crate::DynamicRow>>::accepts(&ty));
        assert!(Generic::new(array.as_ref(), 0).unwrap().hash().is_err());
    }
    #[test]
    fn custom_semantics_require_explicit_registration() {
        fn cmp(a: &[u8], b: &[u8], flags: CompareFlags) -> Result<Option<Ordering>> {
            let v = a.cmp(b);
            Ok(Some(if flags.ascending { v } else { v.reverse() }))
        }
        fn hash(v: &[u8]) -> Result<u64> {
            Ok(v.iter().map(|v| *v as u64).sum())
        }
        let value = Value::Custom(Box::new(CustomValue::new(
            "unit_custom_semantics",
            1,
            "custom",
            vec![1, 2],
        )));
        assert!(value.hash().is_err());
        register_codec_semantics(
            "unit_custom_semantics",
            1,
            CodecSemantics { compare: cmp, hash },
        )
        .unwrap();
        assert_eq!(value.hash().unwrap(), 3);
        assert!(value.equals(&value).unwrap());
        assert!(register_codec_semantics(
            "unit_custom_semantics",
            1,
            CodecSemantics { compare: cmp, hash }
        )
        .is_err());
    }
    #[test]
    fn wire_descriptions_validate_custom_versions_and_shape() {
        let valid = serde_json::json!({"kind":"array","children":[{"kind":"codec","id":"v","version":1,"identity":"custom"}]});
        assert!(parse_description(&valid).is_ok());
        assert!(parse_description(
            &serde_json::json!({"kind":"codec","id":"v","version":0,"identity":"custom"})
        )
        .is_err());
        assert!(parse_description(
            &serde_json::json!({"kind":"row","names":[],"children":[valid]})
        )
        .is_err());
    }
}
