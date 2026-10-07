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

//! SQL aggregate lambdas evaluated through the bound native evaluator.
use crate::{runtime, Generic, RowError, StatusCode, Value};
use arrow_schema::{DataType, Field};
use std::{cell::RefCell, collections::HashMap};

type Result<T> = std::result::Result<T, String>;
struct Binding {
    input: DataType,
    output: DataType,
    max_rows: usize,
}
thread_local! { static BINDINGS: RefCell<Vec<Option<Binding>>> = const { RefCell::new(Vec::new()) }; }

fn bound_binding(
    bindings: &[Option<Binding>],
    index: u32,
) -> std::result::Result<&Binding, RowError> {
    bindings
        .get(index as usize)
        .ok_or_else(|| {
            RowError::new(
                StatusCode::IndexError,
                "aggregate lambda is not initialized",
            )
        })?
        .as_ref()
        .ok_or_else(|| {
            RowError::new(
                StatusCode::Invalid,
                "aggregate lambda types are not bound in this stage",
            )
        })
}

/// A symbolic lambda position in this aggregate's immutable InitContext. It is
/// not a host pointer or invocation OPAQUE handle. Types are bound by native SQL.
#[derive(Clone, Copy, crate::AggregateState, crate::AggregateCheckpoint)]
pub struct Lambda {
    index: u32,
}
impl Lambda {
    pub(crate) fn from_context(metadata: &HashMap<String, String>, index: usize) -> Result<Self> {
        let count = count(metadata)?;
        if index >= count {
            return Err("aggregate lambda index out of bounds".into());
        }
        Ok(Self {
            index: index as u32,
        })
    }
    /// Whether this stage knows the lambda's types. Independent merge/extract
    /// may erase input-only generic types while preserving this symbolic slot.
    /// A true result does not grant expression authority: calls also require
    /// this query's native evaluator and bound expressions.
    pub fn has_bound_types(&self) -> std::result::Result<bool, RowError> {
        BINDINGS.with(|bindings| {
            bindings
                .borrow()
                .get(self.index as usize)
                .map(Option::is_some)
                .ok_or_else(|| {
                    RowError::new(
                        StatusCode::IndexError,
                        "aggregate lambda is not initialized",
                    )
                })
        })
    }
    /// Evaluate once, with nullable/nested row values, and own the returned value.
    /// Requests/results remain internal IPC; UDAF author signatures are row-based.
    pub fn call(&self, arguments: &[&Value<'_>]) -> std::result::Result<Value<'static>, RowError> {
        BINDINGS.with(|bindings| {
            let bindings = bindings.borrow();
            let binding = bound_binding(&bindings, self.index)?;
            let request = encode_request(binding, arguments)
                .map_err(|e| RowError::new(StatusCode::TypeError, e))?;
            let bytes = invoke(self.index, &request)?;
            let batch = runtime::decode_input(&bytes)
                .map_err(|e| RowError::new(StatusCode::TypeError, e))?;
            if batch.num_rows() != 1
                || batch.num_columns() != 1
                || batch.column(0).data_type() != &binding.output
            {
                return Err(RowError::new(
                    StatusCode::TypeError,
                    "invalid aggregate lambda response",
                ));
            }
            Generic::new(batch.column(0).as_ref(), 0)
                .and_then(|v| v.materialize())
                .map_err(|e| RowError::new(StatusCode::TypeError, e))
        })
    }

    /// Maximum rows allowed by this query host in one batch. Large operations
    /// can submit chunks without changing the aggregate's row input signature.
    pub fn max_batch_rows(&self) -> std::result::Result<usize, RowError> {
        BINDINGS.with(|bindings| Ok(bound_binding(&bindings.borrow(), self.index)?.max_rows))
    }

    /// Evaluate a batch of rows. Each argument is a borrowed column of Values;
    /// a length-one column is broadcast. No RecordBatch is exposed to the author.
    /// The returned nullable/nested Values are owned. A zero-row call validates
    /// shape but does not invoke the host.
    pub fn call_batch(
        &self,
        rows: usize,
        arguments: &[&[Value<'_>]],
    ) -> std::result::Result<Vec<Value<'static>>, RowError> {
        BINDINGS.with(|bindings| {
            let bindings = bindings.borrow();
            let binding = bound_binding(&bindings, self.index)?;
            if rows > binding.max_rows {
                return Err(RowError::new(
                    StatusCode::Invalid,
                    "lambda batch exceeds row limit",
                ));
            }
            let request = encode_batch(binding, rows, arguments)
                .map_err(|e| RowError::new(StatusCode::TypeError, e))?;
            if rows == 0 {
                return Ok(Vec::new());
            }
            let bytes = invoke_batch(self.index, &request, rows as u32)?;
            let batch = runtime::decode_input(&bytes)
                .map_err(|e| RowError::new(StatusCode::TypeError, e))?;
            if batch.num_rows() != rows
                || batch.num_columns() != 1
                || batch.column(0).data_type() != &binding.output
            {
                return Err(RowError::new(
                    StatusCode::TypeError,
                    "invalid aggregate lambda batch response",
                ));
            }
            (0..rows)
                .map(|row| {
                    Generic::new(batch.column(0).as_ref(), row)
                        .and_then(|value| value.materialize())
                        .map_err(|e| RowError::new(StatusCode::TypeError, e))
                })
                .collect()
        })
    }
}
fn count(metadata: &HashMap<String, String>) -> Result<usize> {
    let count = metadata
        .get("velox.lambda.count")
        .map(|v| v.parse::<usize>())
        .transpose()
        .map_err(|_| "invalid aggregate lambda count")?
        .unwrap_or(0);
    if count > 64 {
        return Err("aggregate lambda count exceeds 64".into());
    }
    Ok(count)
}
fn data_type(metadata: &HashMap<String, String>, kind: &str, index: usize) -> Result<DataType> {
    let key = format!("velox.lambda.{kind}.{index}");
    let mut bound = HashMap::from([(
        "velox.return_type".into(),
        metadata
            .get(&key)
            .ok_or("missing aggregate lambda type")?
            .clone(),
    )]);
    if let Some(wire) = metadata.get(&format!("{key}.wire")) {
        bound.insert("velox.return_wire_type".into(), wire.clone());
    }
    runtime::bound_return_type(&bound)
}
pub(crate) fn initialize(metadata: &HashMap<String, String>) -> Result<()> {
    let mut bindings = Vec::new();
    let max_rows = metadata
        .get("velox.lambda.max_rows")
        .map(|v| v.parse::<usize>())
        .transpose()
        .map_err(|_| "invalid lambda batch row limit")?
        .unwrap_or(65_536);
    if max_rows > i32::MAX as usize {
        return Err("lambda row limit exceeds native index range".into());
    }
    for index in 0..count(metadata)? {
        let marker = format!("velox.lambda.types_bound.{index}");
        match metadata.get(&marker).map(String::as_str) {
            Some("false") => {
                for kind in ["input", "output"] {
                    let key = format!("velox.lambda.{kind}.{index}");
                    if metadata.contains_key(&key) || metadata.contains_key(&format!("{key}.wire"))
                    {
                        return Err("unbound aggregate lambda cannot contain type metadata".into());
                    }
                }
                bindings.push(None);
                continue;
            }
            None | Some("true") => {}
            _ => return Err("invalid aggregate lambda type-binding marker".into()),
        }
        let input = data_type(metadata, "input", index)?;
        if !matches!(input, DataType::Struct(_)) {
            return Err("aggregate lambda input must be ROW".into());
        }
        bindings.push(Some(Binding {
            input,
            output: data_type(metadata, "output", index)?,
            max_rows,
        }));
    }
    BINDINGS.with(|current| *current.borrow_mut() = bindings);
    Ok(())
}
fn encode_batch(binding: &Binding, rows: usize, arguments: &[&[Value<'_>]]) -> Result<Vec<u8>> {
    let DataType::Struct(fields) = &binding.input else {
        return Err("lambda input must be ROW".into());
    };
    if fields.len() != arguments.len() {
        return Err("lambda argument count mismatch".into());
    }
    let mut columns = Vec::with_capacity(fields.len());
    let mut broadcast = None;
    for (field, values) in fields.iter().zip(arguments) {
        if values.len() != rows && values.len() != 1 {
            return Err("lambda column row count mismatch".into());
        }
        let values = crate::value::build_array(field.data_type(), values)?;
        columns.push(if values.len() != rows {
            let indices =
                broadcast.get_or_insert_with(|| arrow_array::UInt32Array::from(vec![0; rows]));
            arrow_select::take::take(values.as_ref(), indices, None).map_err(|e| e.to_string())?
        } else {
            values
        });
    }
    let array = if fields.is_empty() {
        arrow_array::StructArray::new_empty_fields(rows, None)
    } else {
        arrow_array::StructArray::try_new(fields.clone(), columns, None)
            .map_err(|e| e.to_string())?
    };
    runtime::encode_output(
        Field::new("arguments", binding.input.clone(), true),
        std::sync::Arc::new(array),
    )
}
fn encode_request(binding: &Binding, arguments: &[&Value<'_>]) -> Result<Vec<u8>> {
    let DataType::Struct(fields) = &binding.input else {
        return Err("lambda input must be ROW".into());
    };
    if fields.len() != arguments.len() {
        return Err("lambda argument count mismatch".into());
    }
    let mut columns = Vec::with_capacity(fields.len());
    for (field, value) in fields.iter().zip(arguments) {
        columns.push(crate::value::build_array(
            field.data_type(),
            std::slice::from_ref(*value),
        )?);
    }
    let array = if fields.is_empty() {
        arrow_array::StructArray::new_empty_fields(1, None)
    } else {
        arrow_array::StructArray::new(fields.clone(), columns, None)
    };
    runtime::encode_output(
        Field::new("arguments", binding.input.clone(), true),
        std::sync::Arc::new(array),
    )
}
#[cfg(target_arch = "wasm32")]
fn invoke(index: u32, request: &[u8]) -> std::result::Result<Vec<u8>, RowError> {
    #[link(wasm_import_module = "velox_udf_v1")]
    unsafe extern "C" {
        fn lambda_call(index: u32, pointer: u32, length: u32) -> u64;
    }
    let length = u32::try_from(request.len())
        .map_err(|_| RowError::new(StatusCode::OutOfMemory, "lambda request too large"))?;
    // Native keeps one bounded, charged response. Allocate guest storage without
    // host reentry into a guest allocator; result consumes the native token once.
    let packed = unsafe { lambda_call(index, request.as_ptr() as u32, length) };
    receive(packed)
}
#[cfg(target_arch = "wasm32")]
fn invoke_batch(index: u32, request: &[u8], rows: u32) -> std::result::Result<Vec<u8>, RowError> {
    #[link(wasm_import_module = "velox_udf_v1")]
    unsafe extern "C" {
        fn lambda_call_batch(index: u32, pointer: u32, length: u32, rows: u32) -> u64;
    }
    let length = u32::try_from(request.len())
        .map_err(|_| RowError::new(StatusCode::OutOfMemory, "lambda request too large"))?;
    receive(unsafe { lambda_call_batch(index, request.as_ptr() as u32, length, rows) })
}
#[cfg(target_arch = "wasm32")]
fn receive(packed: u64) -> std::result::Result<Vec<u8>, RowError> {
    #[link(wasm_import_module = "velox_udf_v1")]
    unsafe extern "C" {
        fn lambda_result(token: u32, pointer: u32, length: u32) -> u32;
    }
    let size = packed as u32 as usize;
    let token = (packed >> 32) as u32;
    let mut output = Vec::new();
    output
        .try_reserve_exact(size)
        .map_err(|e| RowError::new(StatusCode::OutOfMemory, e.to_string()))?;
    output.resize(size, 0);
    let status = unsafe { lambda_result(token, output.as_mut_ptr() as u32, size as u32) };
    if status != 0 {
        return Err(RowError::new(
            StatusCode::UnknownError,
            "lambda response failed",
        ));
    }
    Ok(output)
}
#[cfg(not(target_arch = "wasm32"))]
fn invoke_batch(_: u32, _: &[u8], _: u32) -> std::result::Result<Vec<u8>, RowError> {
    Err(RowError::new(
        StatusCode::NotImplemented,
        "native lambda callback requires a WASM execution host",
    ))
}
#[cfg(not(target_arch = "wasm32"))]
fn invoke(_: u32, _: &[u8]) -> std::result::Result<Vec<u8>, RowError> {
    Err(RowError::new(
        StatusCode::NotImplemented,
        "native lambda callback requires a WASM execution host",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn metadata() -> HashMap<String, String> {
        HashMap::from([
            ("velox.lambda.count".into(), "1".into()),
            (
                "velox.lambda.input.0".into(),
                "row(arg0 bigint,arg1 array(varchar))".into(),
            ),
            ("velox.lambda.output.0".into(), "bigint".into()),
        ])
    }
    #[test]
    fn request_values_are_borrowed_nullable_nested_and_type_checked() {
        let m = metadata();
        initialize(&m).unwrap();
        BINDINGS.with(|bindings| {
            let bindings = bindings.borrow();
            let binding = bindings[0].as_ref().unwrap();
            let value = Value::Array(vec![Value::Varchar("hello".into()), Value::Null]);
            let request = encode_request(binding, &[&Value::Null, &value]).unwrap();
            let batch = runtime::decode_input(&request).unwrap();
            let row = Generic::new(batch.column(0).as_ref(), 0)
                .unwrap()
                .as_row::<crate::DynamicRow>()
                .unwrap();
            assert!(row.at(0).unwrap().is_null());
            assert_eq!(
                row.at(1)
                    .unwrap()
                    .as_array::<&str>()
                    .unwrap()
                    .at(0)
                    .unwrap(),
                Some("hello")
            );
            assert!(encode_request(binding, &[&value]).is_err());
            assert!(encode_request(binding, &[&Value::Varchar("bad".into()), &value]).is_err());
        });
    }
    #[test]
    fn missing_binding_and_malformed_initialization_are_explicit() {
        assert!(Lambda::from_context(&metadata(), 1).is_err());
        let mut bad = metadata();
        bad.remove("velox.lambda.output.0");
        assert!(initialize(&bad).is_err());
        bad = metadata();
        bad.insert("velox.lambda.count".into(), "65".into());
        assert!(initialize(&bad).is_err());
        initialize(&HashMap::new()).unwrap();
        assert_eq!(
            Lambda { index: 0 }.call(&[]).err().unwrap().code,
            StatusCode::IndexError
        );
        initialize(&metadata()).unwrap();
        assert_eq!(
            Lambda { index: 0 }
                .call(&[&Value::BigInt(1), &Value::Array(vec![])])
                .err()
                .unwrap()
                .code,
            StatusCode::NotImplemented
        );
    }
    #[test]
    fn erased_merge_types_preserve_slots_and_reject_evaluation() {
        let mut m = metadata();
        m.insert("velox.lambda.count".into(), "2".into());
        m.insert("velox.lambda.types_bound.0".into(), "false".into());
        m.remove("velox.lambda.input.0");
        m.remove("velox.lambda.output.0");
        m.insert("velox.lambda.input.1".into(), "row(arg0 bigint)".into());
        m.insert("velox.lambda.output.1".into(), "bigint".into());
        initialize(&m).unwrap();
        let erased = Lambda::from_context(&m, 0).unwrap();
        let bound = Lambda::from_context(&m, 1).unwrap();
        assert!(!erased.has_bound_types().unwrap());
        assert!(bound.has_bound_types().unwrap());
        assert_eq!(erased.call(&[]).err().unwrap().code, StatusCode::Invalid);
        assert_eq!(
            erased.call_batch(0, &[]).err().unwrap().code,
            StatusCode::Invalid
        );
        assert_eq!(
            erased.max_batch_rows().err().unwrap().code,
            StatusCode::Invalid
        );
        assert_eq!(
            bound.call(&[&Value::BigInt(3)]).err().unwrap().code,
            StatusCode::NotImplemented
        );
        let mut output = crate::CheckpointWriter::new(1024);
        crate::AggregateCheckpoint::encode_checkpoint(&erased, &mut output).unwrap();
        let bytes = output.finish();
        let mut input = crate::CheckpointReader::new(&bytes, 0);
        let restored: Lambda = crate::AggregateCheckpoint::decode_checkpoint(&mut input).unwrap();
        input.finish().unwrap();
        assert!(!restored.has_bound_types().unwrap());
        assert_eq!(restored.call(&[]).err().unwrap().code, StatusCode::Invalid);
        initialize(&HashMap::new()).unwrap();
        assert_eq!(
            erased.has_bound_types().err().unwrap().code,
            StatusCode::IndexError
        );
    }
    #[test]
    fn erased_type_metadata_is_explicit_and_cannot_hide_conflicts() {
        let mut m = metadata();
        m.insert("velox.lambda.types_bound.0".into(), "false".into());
        assert!(initialize(&m).is_err());
        m.remove("velox.lambda.input.0");
        m.remove("velox.lambda.output.0");
        initialize(&m).unwrap();
        m.insert("velox.lambda.output.0.wire".into(), "{}".into());
        assert!(initialize(&m).is_err());
        m.remove("velox.lambda.output.0.wire");
        m.insert("velox.lambda.types_bound.0".into(), "0".into());
        assert!(initialize(&m).is_err());
        m.insert("velox.lambda.types_bound.0".into(), "true".into());
        assert!(initialize(&m).is_err());
        initialize(&metadata()).unwrap();
        assert!(Lambda::from_context(&metadata(), 0)
            .unwrap()
            .has_bound_types()
            .unwrap());
    }
    #[test]
    fn symbolic_positions_checkpoint_without_retaining_host_pointers() {
        let value = Lambda::from_context(&metadata(), 0).unwrap();
        assert_eq!(crate::AggregateState::heap_bytes(&value).unwrap(), 0);
        let mut output = crate::CheckpointWriter::new(1024);
        crate::AggregateCheckpoint::encode_checkpoint(&value, &mut output).unwrap();
        let bytes = output.finish();
        let mut input = crate::CheckpointReader::new(&bytes, 0);
        let restored: Lambda = crate::AggregateCheckpoint::decode_checkpoint(&mut input).unwrap();
        input.finish().unwrap();
        assert_eq!(restored.index, 0);
    }

    #[test]
    fn batch_columns_broadcast_nested_values_and_preserve_nulls() {
        initialize(&metadata()).unwrap();
        BINDINGS.with(|bindings| {
            let bindings = bindings.borrow();
            let binding = bindings[0].as_ref().unwrap();
            let first = [Value::BigInt(3), Value::Null, Value::BigInt(7)];
            let nested = [Value::Array(vec![Value::Varchar("a".into()), Value::Null])];
            let bytes = encode_batch(binding, 3, &[&first, &nested]).unwrap();
            let batch = runtime::decode_input(&bytes).unwrap();
            assert_eq!(batch.num_rows(), 3);
            for i in 0..3 {
                let row = Generic::new(batch.column(0).as_ref(), i)
                    .unwrap()
                    .as_row::<crate::DynamicRow>()
                    .unwrap();
                assert_eq!(row.at(0).unwrap().is_null(), i == 1);
                let array = row.at(1).unwrap().as_array::<&str>().unwrap();
                assert_eq!(array.at(0).unwrap(), Some("a"));
                assert_eq!(array.at(1).unwrap(), None);
            }
            assert!(encode_batch(binding, 3, &[&first[..2], &nested]).is_err());
            assert!(encode_batch(binding, 3, &[&first]).is_err());
        });
    }
    #[test]
    fn batch_zero_rows_and_limits_do_not_invoke_host() {
        let mut m = metadata();
        m.insert("velox.lambda.max_rows".into(), "2".into());
        initialize(&m).unwrap();
        let lambda = Lambda::from_context(&m, 0).unwrap();
        assert_eq!(lambda.max_batch_rows().unwrap(), 2);
        assert!(lambda.call_batch(0, &[&[], &[]]).unwrap().is_empty());
        assert_eq!(
            lambda.call_batch(3, &[&[], &[]]).err().unwrap().code,
            StatusCode::Invalid
        );
        assert_eq!(
            lambda
                .call_batch(1, &[&[Value::BigInt(1)], &[Value::Array(vec![])]])
                .err()
                .unwrap()
                .code,
            StatusCode::NotImplemented
        );
        m.insert("velox.lambda.max_rows".into(), "no".into());
        assert!(initialize(&m).is_err());
        m.insert(
            "velox.lambda.max_rows".into(),
            (i32::MAX as u64 + 1).to_string(),
        );
        assert!(initialize(&m).is_err());
    }
    #[test]
    fn zero_argument_batch_preserves_explicit_row_count() {
        let binding = Binding {
            input: DataType::Struct(Vec::<Field>::new().into()),
            output: DataType::Int64,
            max_rows: 8,
        };
        let bytes = encode_batch(&binding, 5, &[]).unwrap();
        let batch = runtime::decode_input(&bytes).unwrap();
        assert_eq!(batch.num_rows(), 5);
    }
}
