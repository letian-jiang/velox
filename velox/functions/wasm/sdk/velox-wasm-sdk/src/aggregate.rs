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

//! Row aggregate authoring over typed, host-bound intermediate states.
use crate::{runtime, AggregateState, Generic, InitContext, Value};
use arrow_array::{Array, RecordBatch, UInt32Array};
use arrow_schema::{DataType, Field};

type Result<T> = std::result::Result<T, String>;

/// Inputs are individual values/views. State must own anything retained after
/// update/merge. Extraction borrows State and must not change its SQL content:
/// native window execution extracts repeatedly from an expanding accumulator.
/// Errors fail the aggregate invocation; there is no scalar per-row TRY ABI.
pub trait VeloxRowAggregate {
    type State: AggregateState;
    type Input<'a>;
    type Error: ToString + 'static;

    fn create(context: &InitContext<'_>) -> std::result::Result<Self::State, Self::Error>;
    fn destroy(_state: Self::State) {}
    fn update(
        state: &mut Self::State,
        input: Self::Input<'_>,
    ) -> std::result::Result<(), Self::Error>;
    /// Optional bulk update over this group's rows in their original order.
    /// Each row still contains the declared values/views, never a RecordBatch.
    /// Override for vectorized algorithms; the default keeps scalar semantics.
    fn update_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<Self::Input<'a>>>,
    ) -> std::result::Result<(), AggregateBatchError<Self::Error>> {
        for row in rows {
            Self::update(state, row.map_err(AggregateBatchError::Input)?)
                .map_err(AggregateBatchError::Function)?;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> std::result::Result<Value<'_>, Self::Error>;
    fn merge(state: &mut Self::State, input: Generic<'_>) -> std::result::Result<(), Self::Error>;
    /// Optional bulk merge of typed intermediate row views for one group.
    fn merge_batch<'a>(
        state: &mut Self::State,
        rows: impl Iterator<Item = Result<Generic<'a>>>,
    ) -> std::result::Result<(), AggregateBatchError<Self::Error>> {
        for row in rows {
            Self::merge(state, row.map_err(AggregateBatchError::Input)?)
                .map_err(AggregateBatchError::Function)?;
        }
        Ok(())
    }
    fn finalize(state: &Self::State) -> std::result::Result<Value<'_>, Self::Error>;
}

/// Keeps input decoding failures separate from an author's typed error.
/// Use `.map_err(AggregateBatchError::Input)?` on a row from the iterator;
/// `?` on an author error automatically selects Function.
#[derive(Debug)]
pub enum AggregateBatchError<E> {
    Input(String),
    Function(E),
}
impl<E> From<E> for AggregateBatchError<E> {
    fn from(error: E) -> Self {
        Self::Function(error)
    }
}
impl<E: ToString + 'static> AggregateBatchError<E> {
    #[doc(hidden)]
    pub fn encode(self) -> String {
        match self {
            Self::Input(message) => {
                use crate::scalar::EncodeRowError;
                (&&message).encode_row_error()
            }
            Self::Function(error) => encode_error(error),
        }
    }
}

// Associated Error is generic inside the adapter, so autoref specialization
// cannot select RowError here. This check runs only on failure, never per row
// on the successful hot path, and also supports direct SDK adapter use.
#[doc(hidden)]
pub fn encode_error<E: ToString + 'static>(error: E) -> String {
    use crate::scalar::EncodeRowError;
    if let Some(status) = (&error as &dyn std::any::Any).downcast_ref::<crate::RowError>() {
        status.encode_row_error()
    } else {
        (&&error).encode_row_error()
    }
}

/// Adapter internals are public for generated exports; UDAF authors implement
/// VeloxRowAggregate and do not receive a RecordBatch.
pub struct AggregateAdapter<A: VeloxRowAggregate> {
    states: runtime::StateRegistry<A::State>,
    initialization: Option<RecordBatch>,
    result_type: Option<DataType>,
    intermediate_type: Option<DataType>,
}
impl<A: VeloxRowAggregate> Default for AggregateAdapter<A> {
    fn default() -> Self {
        Self {
            states: runtime::StateRegistry::new(),
            initialization: None,
            result_type: None,
            intermediate_type: None,
        }
    }
}
impl<A: VeloxRowAggregate> AggregateAdapter<A> {
    pub fn initialize(&mut self, batch: RecordBatch) -> Result<Vec<u8>> {
        if self.initialization.is_some() {
            return Err("aggregate adapter initialized twice".into());
        }
        InitContext::new(&batch)?;
        let result = runtime::bound_return_type(batch.schema_ref().metadata())?;
        let mut metadata = batch.schema_ref().metadata().clone();
        metadata.insert(
            "velox.return_type".into(),
            metadata
                .get("velox.intermediate_type")
                .ok_or("missing bound aggregate intermediate type")?
                .clone(),
        );
        metadata.remove("velox.return_wire_type");
        if let Some(wire) = metadata.get("velox.intermediate_wire_type").cloned() {
            metadata.insert("velox.return_wire_type".into(), wire);
        }
        let intermediate = runtime::bound_return_type(&metadata)?;
        crate::lambda::initialize(batch.schema_ref().metadata())?;
        self.result_type = Some(result);
        self.intermediate_type = Some(intermediate);
        self.initialization = Some(batch);
        Ok(Vec::new())
    }

    pub fn create(&mut self, count: u32) -> Result<Vec<u8>> {
        let context = InitContext::new(
            self.initialization
                .as_ref()
                .ok_or("aggregate adapter not initialized")?,
        )?;
        let mut handles = Vec::with_capacity(count as usize);
        for _ in 0..count {
            let created = A::create(&context).map_err(encode_error).and_then(|state| {
                self.states.insert(state).map_err(|(e, state)| {
                    A::destroy(state);
                    e
                })
            });
            match created {
                Ok(handle) => handles.push(handle),
                Err(error) => {
                    for handle in handles {
                        if let Some(state) = self.states.remove(handle) {
                            A::destroy(state);
                        }
                    }
                    return Err(error);
                }
            }
        }
        match self.usage(handles.iter().copied()) {
            Ok(report) => Ok(report),
            Err(error) => {
                for handle in handles {
                    if let Some(state) = self.states.remove(handle) {
                        A::destroy(state);
                    }
                }
                Err(error)
            }
        }
    }

    fn handles<'a>(&self, batch: &'a RecordBatch) -> Result<&'a UInt32Array> {
        let column = batch
            .columns()
            .first()
            .ok_or("missing aggregate state handles")?;
        let handles = column
            .as_any()
            .downcast_ref::<UInt32Array>()
            .ok_or("aggregate state handles must be UInt32")?;
        for row in 0..batch.num_rows() {
            if handles.is_null(row) {
                return Err("NULL aggregate state handle".into());
            }
            self.states.get(handles.value(row))?;
        }
        Ok(handles)
    }

    pub fn destroy(&mut self, batch: &RecordBatch) -> Result<Vec<u8>> {
        if batch.num_columns() != 1 {
            return Err("destroy expects one handle column".into());
        }
        let handles = self.handles(batch)?;
        // Validate the entire batch before dropping states. Duplicates are a
        // malformed request, not a second call to an author's destructor.
        let mut unique = std::collections::HashSet::new();
        for row in 0..batch.num_rows() {
            if !unique.insert(handles.value(row)) {
                return Err("duplicate destroy handle".into());
            }
        }
        for handle in unique {
            A::destroy(self.states.remove(handle).ok_or("unknown destroy handle")?);
        }
        Ok(Vec::new())
    }

    pub fn update<F>(
        &mut self,
        batch: &RecordBatch,
        single: Option<u32>,
        mut update: F,
    ) -> Result<Vec<u8>>
    where
        F: FnMut(&mut A::State, usize) -> Result<()>,
    {
        if let Some(handle) = single {
            let state = self.states.get_mut(handle)?;
            for row in 0..batch.num_rows() {
                update(state, row)?;
            }
        } else {
            // Validate the complete handle column before the first mutation.
            // Clustered inputs then need one state lookup per contiguous run.
            let handles = self.handles(batch)?;
            let mut row = 0;
            while row < batch.num_rows() {
                let handle = handles.value(row);
                let state = self.states.get_mut(handle)?;
                loop {
                    update(state, row)?;
                    row += 1;
                    if row == batch.num_rows() || handles.value(row) != handle {
                        break;
                    }
                }
            }
        }
        if let Some(handle) = single {
            self.usage(std::iter::once(handle))
        } else {
            let handles = self.handles(batch)?;
            // Only inspect run boundaries for clustered input; report each
            // affected state once, even when it occurs in several runs.
            let mut seen = std::collections::HashSet::new();
            let mut previous = None;
            let unique = (0..batch.num_rows()).filter_map(|row| {
                let handle = handles.value(row);
                if previous == Some(handle) {
                    return None;
                }
                previous = Some(handle);
                seen.insert(handle).then_some(handle)
            });
            self.usage(unique)
        }
    }

    /// Generated opt-in bulk hook. Group rows stably without exposing IPC.
    /// Validate every handle before any state is mutated; grouping across
    /// noncontiguous runs preserves order within each independent State.
    pub fn update_batches<F>(
        &mut self,
        batch: &RecordBatch,
        single: Option<u32>,
        mut update: F,
    ) -> Result<Vec<u8>>
    where
        F: FnMut(&mut A::State, &[usize]) -> Result<()>,
    {
        if let Some(handle) = single {
            let state = self.states.get_mut(handle)?;
            let rows = (0..batch.num_rows()).collect::<Vec<_>>();
            update(state, &rows)?;
            return self.usage(std::iter::once(handle));
        }
        let handles = self.handles(batch)?;
        let mut indices = std::collections::HashMap::new();
        let mut groups: Vec<(u32, Vec<usize>)> = Vec::new();
        for row in 0..batch.num_rows() {
            let handle = handles.value(row);
            let index = *indices.entry(handle).or_insert_with(|| {
                groups.push((handle, Vec::new()));
                groups.len() - 1
            });
            groups[index].1.push(row);
        }
        for (handle, rows) in &groups {
            update(self.states.get_mut(*handle)?, rows)?;
        }
        self.usage(groups.iter().map(|(handle, _)| *handle))
    }

    fn usage(&self, handles: impl Iterator<Item = u32>) -> Result<Vec<u8>> {
        let mut report = Vec::new();
        for handle in handles {
            let bytes = crate::state::state_bytes(self.states.get(handle)?)?;
            report.extend_from_slice(&handle.to_le_bytes());
            report.extend_from_slice(&bytes.to_le_bytes());
        }
        Ok(report)
    }

    pub fn compact(&mut self, batch: &RecordBatch) -> Result<Vec<u8>> {
        if batch.num_columns() != 1 {
            return Err("compact expects one handle column".into());
        }
        let handles = self.handles(batch)?;
        let mut unique = std::collections::HashSet::new();
        for row in 0..batch.num_rows() {
            if !unique.insert(handles.value(row)) {
                return Err("duplicate compact handle".into());
            }
        }
        // Validate all handles/duplicates before mutating any State.
        for row in 0..batch.num_rows() {
            self.states.get_mut(handles.value(row))?.compact()?;
        }
        self.usage((0..batch.num_rows()).map(|row| handles.value(row)))
    }

    pub fn merge(
        &mut self,
        batch: &RecordBatch,
        single: Option<u32>,
        skip_nulls: bool,
    ) -> Result<Vec<u8>> {
        let index = usize::from(single.is_none());
        if batch.num_columns() != index + 1 {
            return Err("merge column count mismatch".into());
        }
        let input = batch.column(index);
        if input.data_type()
            != self
                .intermediate_type
                .as_ref()
                .ok_or("adapter not initialized")?
        {
            return Err("merge intermediate type mismatch".into());
        }
        self.update(batch, single, |state, row| {
            if !skip_nulls || !input.is_null(row) {
                A::merge(state, Generic::new(input.as_ref(), row)?).map_err(encode_error)?;
            }
            Ok(())
        })
    }

    pub fn merge_batches(
        &mut self,
        batch: &RecordBatch,
        single: Option<u32>,
        skip_nulls: bool,
    ) -> Result<Vec<u8>> {
        let index = usize::from(single.is_none());
        if batch.num_columns() != index + 1 {
            return Err("merge column count mismatch".into());
        }
        let input = batch.column(index);
        if input.data_type()
            != self
                .intermediate_type
                .as_ref()
                .ok_or("adapter not initialized")?
        {
            return Err("merge intermediate type mismatch".into());
        }
        self.update_batches(batch, single, |state, rows| {
            A::merge_batch(
                state,
                rows.iter()
                    .copied()
                    .filter(|row| !skip_nulls || !input.is_null(*row))
                    .map(|row| Generic::new(input.as_ref(), row)),
            )
            .map_err(AggregateBatchError::encode)
        })
    }

    pub fn to_intermediate<F>(&self, batch: &RecordBatch, mut update: F) -> Result<Vec<u8>>
    where
        F: FnMut(&mut A::State, usize) -> Result<()>,
    {
        let context = InitContext::new(
            self.initialization
                .as_ref()
                .ok_or("adapter not initialized")?,
        )?;
        let ty = self
            .intermediate_type
            .as_ref()
            .ok_or("adapter not initialized")?;
        let mut values = Vec::with_capacity(batch.num_rows());
        for row in 0..batch.num_rows() {
            let mut state = A::create(&context).map_err(encode_error)?;
            let value = update(&mut state, row)
                .and_then(|_| A::serialize(&state).map_err(encode_error))
                .and_then(|value| value.materialize());
            A::destroy(state);
            values.push(value?);
        }
        runtime::encode_output(
            Field::new("result", ty.clone(), true),
            crate::value::build_array(ty, &values)?,
        )
    }

    pub fn extract(&self, batch: &RecordBatch, intermediate: bool) -> Result<Vec<u8>> {
        if batch.num_columns() != 1 {
            return Err("extract expects one handle column".into());
        }
        let handles = self.handles(batch)?;
        let ty = if intermediate {
            &self.intermediate_type
        } else {
            &self.result_type
        }
        .as_ref()
        .ok_or("aggregate adapter not initialized")?;
        let values = (0..batch.num_rows())
            .map(|row| {
                let state = self.states.get(handles.value(row))?;
                if intermediate {
                    A::serialize(state)
                } else {
                    A::finalize(state)
                }
                .map_err(encode_error)
            })
            .collect::<Result<Vec<_>>>()?;
        let output = crate::value::build_array(ty, &values)?;
        let ipc = runtime::encode_output(Field::new("result", ty.clone(), true), output)?;
        // Finalizers may use interior caches. Report retained state after
        // extraction too, so const author methods cannot hide capacity growth.
        let mut seen = std::collections::HashSet::new();
        let mut report = self.usage(
            (0..batch.num_rows())
                .map(|row| handles.value(row))
                .filter(|h| seen.insert(*h)),
        )?;
        report.extend_from_slice(&ipc);
        Ok(report)
    }
}

impl<A: VeloxRowAggregate> AggregateAdapter<A>
where
    A::State: crate::AggregateCheckpoint,
{
    pub fn checkpoint(&self, limit: u32) -> Result<Vec<u8>> {
        use crate::AggregateCheckpoint;
        let mut out = crate::CheckpointWriter::new(limit as usize);
        out.write(b"VWS1")?;
        self.states.next.encode_checkpoint(&mut out)?;
        u32::try_from(self.states.states.len())
            .map_err(|_| "too many checkpoint states")?
            .encode_checkpoint(&mut out)?;
        let mut handles = self.states.states.keys().copied().collect::<Vec<_>>();
        handles.sort_unstable();
        for handle in handles {
            handle.encode_checkpoint(&mut out)?;
            let start = out.bytes.len();
            0u32.encode_checkpoint(&mut out)?;
            self.states.get(handle)?.encode_checkpoint(&mut out)?;
            let size = u32::try_from(out.bytes.len() - start - 4)
                .map_err(|_| "checkpoint State too large")?;
            out.bytes[start..start + 4].copy_from_slice(&size.to_le_bytes());
        }
        Ok(out.finish())
    }
    pub fn restore(&mut self, bytes: &[u8], limit: u64) -> Result<Vec<u8>> {
        use crate::AggregateCheckpoint;
        if self.initialization.is_none() || !self.states.states.is_empty() {
            return Err("restore requires an initialized empty aggregate adapter".into());
        }
        let mut header = crate::CheckpointReader::new(bytes, 0);
        if header.read(4)? != b"VWS1" {
            return Err("invalid aggregate checkpoint format".into());
        }
        let next = u32::decode_checkpoint(&mut header)?;
        let count = u32::decode_checkpoint(&mut header)? as usize;
        if count > bytes.len().saturating_sub(12) / 8 {
            return Err("invalid aggregate checkpoint state count".into());
        }
        // Validate all handles, framing and the high-water mark before decoding.
        let mut records = Vec::with_capacity(count);
        let mut seen = std::collections::HashSet::new();
        for _ in 0..count {
            let handle = u32::decode_checkpoint(&mut header)?;
            let size = u32::decode_checkpoint(&mut header)? as usize;
            if handle == 0 || !seen.insert(handle) || (next != 0 && handle >= next) {
                return Err("invalid or duplicate checkpoint handle".into());
            }
            records.push((handle, header.read(size)?));
        }
        header.finish()?;
        let mut candidate = runtime::StateRegistry::new();
        candidate.next = next;
        let mut remaining = limit;
        let mut report = Vec::new();
        let outcome = (|| {
            for (handle, bytes) in &records {
                let mut input = crate::CheckpointReader::new(bytes, remaining);
                let state = A::State::decode_checkpoint(&mut input)?;
                let checked = (|| {
                    input.finish()?;
                    let size = crate::state::state_bytes(&state)?;
                    let budget = remaining
                        .checked_sub(size)
                        .ok_or("restored state exceeds Store limit")?;
                    Ok::<_, String>((size, budget))
                })();
                let (size, budget) = match checked {
                    Ok(value) => value,
                    Err(error) => {
                        A::destroy(state);
                        return Err(error);
                    }
                };
                remaining = budget;
                report.extend_from_slice(&handle.to_le_bytes());
                report.extend_from_slice(&size.to_le_bytes());
                candidate.states.insert(*handle, state);
            }
            Ok(())
        })();
        if let Err(error) = outcome {
            for (_, state) in candidate.states {
                A::destroy(state);
            }
            return Err(error);
        }
        self.states = candidate;
        Ok(report)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use arrow_array::{ArrayRef, Int64Array};
    use arrow_schema::Schema;
    use std::{collections::HashMap, sync::Arc};

    struct TypedFailures;
    impl VeloxRowAggregate for TypedFailures {
        type State = i64;
        type Input<'a> = (i64,);
        type Error = crate::RowError;
        fn create(context: &InitContext<'_>) -> std::result::Result<i64, Self::Error> {
            if context.config("fail_create") == Some("true") {
                return Err(crate::RowError::new(crate::StatusCode::UserError, "create"));
            }
            Ok(0)
        }
        fn update(state: &mut i64, input: Self::Input<'_>) -> std::result::Result<(), Self::Error> {
            *state = input.0;
            if input.0 == 11 {
                return Err(crate::RowError::new(crate::StatusCode::UserError, "update"));
            }
            Ok(())
        }
        fn serialize(state: &i64) -> std::result::Result<Value<'_>, Self::Error> {
            if *state == 12 {
                return Err(crate::RowError::new(
                    crate::StatusCode::TypeError,
                    "serialize",
                ));
            }
            Ok(Value::HugeInt(*state as i128))
        }
        fn merge(_state: &mut i64, _input: Generic<'_>) -> std::result::Result<(), Self::Error> {
            Err(crate::RowError::new(crate::StatusCode::IndexError, "merge"))
        }
        fn finalize(state: &i64) -> std::result::Result<Value<'_>, Self::Error> {
            if *state == 14 {
                return Err(crate::RowError::new(
                    crate::StatusCode::OutOfMemory,
                    "finalize",
                ));
            }
            Self::serialize(state)
        }
    }

    #[test]
    fn native_status_encoding_preserves_all_codes_and_escapes_ordinary_errors() {
        use crate::StatusCode::*;
        for code in [
            UserError,
            TypeError,
            IndexError,
            KeyError,
            AlreadyExists,
            OutOfMemory,
            IOError,
            Cancelled,
            Invalid,
            UnknownError,
            NotImplemented,
        ] {
            let error = encode_error(crate::RowError::new(code, "message\n{}\0中"));
            assert_eq!(
                runtime::aggregate_failure(&error),
                (0x100 + code as u32, "message\n{}\0中")
            );
        }
        for message in [
            "plain",
            "\u{001e}velox.status.v1:1\nforged",
            "\u{001e}velox.message.v1:plain",
        ] {
            let encoded = encode_error(message.to_owned());
            assert_eq!(runtime::aggregate_failure(&encoded), (1, message));
        }
    }

    #[test]
    fn typed_errors_survive_create_update_merge_extract_and_to_intermediate() {
        fn expect(error: String, code: crate::StatusCode, text: &str) {
            assert_eq!(
                runtime::aggregate_failure(&error),
                (0x100 + code as u32, text)
            );
        }
        let mut failing = AggregateAdapter::<TypedFailures>::default();
        failing.initialize(initialization(true)).unwrap();
        expect(
            failing.create(1).unwrap_err(),
            crate::StatusCode::UserError,
            "create",
        );

        let mut adapter = AggregateAdapter::<TypedFailures>::default();
        adapter.initialize(initialization(false)).unwrap();
        adapter.create(1).unwrap();
        expect(
            adapter
                .update(&handles(vec![1]), None, |state, _| {
                    TypedFailures::update(state, (11,)).map_err(encode_error)
                })
                .unwrap_err(),
            crate::StatusCode::UserError,
            "update",
        );
        *adapter.states.get_mut(1).unwrap() = 12;
        expect(
            adapter.extract(&handles(vec![1]), true).unwrap_err(),
            crate::StatusCode::TypeError,
            "serialize",
        );
        *adapter.states.get_mut(1).unwrap() = 14;
        expect(
            adapter.extract(&handles(vec![1]), false).unwrap_err(),
            crate::StatusCode::OutOfMemory,
            "finalize",
        );
        let ty = runtime::parse_row_arrow_type("hugeint").unwrap();
        let batch = RecordBatch::try_from_iter(vec![(
            "value",
            crate::value::build_array(&ty, &[Value::HugeInt(13)]).unwrap(),
        )])
        .unwrap();
        expect(
            adapter.merge(&batch, Some(1), false).unwrap_err(),
            crate::StatusCode::IndexError,
            "merge",
        );
        expect(
            adapter
                .to_intermediate(&batch, |state, _| {
                    TypedFailures::update(state, (11,)).map_err(encode_error)
                })
                .unwrap_err(),
            crate::StatusCode::UserError,
            "update",
        );
        expect(
            adapter
                .to_intermediate(&batch, |state, _| {
                    TypedFailures::update(state, (12,)).map_err(encode_error)
                })
                .unwrap_err(),
            crate::StatusCode::TypeError,
            "serialize",
        );
        assert_eq!(*adapter.states.get(1).unwrap(), 14);
    }

    struct Sum;
    impl VeloxRowAggregate for Sum {
        type State = i128;
        type Input<'a> = (i128,);
        type Error = String;
        fn create(context: &InitContext<'_>) -> Result<i128> {
            if context.config("fail_create") == Some("true") {
                return Err("create failed".into());
            }
            Ok(0)
        }
        fn update(state: &mut i128, input: Self::Input<'_>) -> Result<()> {
            *state += input.0;
            Ok(())
        }
        fn serialize(state: &i128) -> Result<Value<'_>> {
            Ok(Value::HugeInt(*state))
        }
        fn merge(state: &mut i128, value: Generic<'_>) -> Result<()> {
            *state += if value.is_null() {
                17
            } else {
                value.get::<i128>()?
            };
            Ok(())
        }
        fn finalize(state: &i128) -> Result<Value<'_>> {
            Self::serialize(state)
        }
    }
    fn initialization(fail: bool) -> RecordBatch {
        let metadata = HashMap::from([
            ("velox.return_type".into(), "hugeint".into()),
            ("velox.intermediate_type".into(), "hugeint".into()),
            ("velox.constant_arguments".into(), "0".into()),
            ("velox.config.fail_create".into(), fail.to_string()),
        ]);
        RecordBatch::try_new(
            Arc::new(Schema::new_with_metadata(
                vec![Field::new("input", DataType::Int64, true)],
                metadata,
            )),
            vec![Arc::new(Int64Array::from(vec![None]))],
        )
        .unwrap()
    }
    fn handles(values: Vec<u32>) -> RecordBatch {
        RecordBatch::try_from_iter(vec![(
            "handles",
            Arc::new(UInt32Array::from(values)) as ArrayRef,
        )])
        .unwrap()
    }
    fn adapter() -> AggregateAdapter<Sum> {
        let mut adapter = AggregateAdapter::<Sum>::default();
        adapter.initialize(initialization(false)).unwrap();
        adapter.create(2).unwrap();
        adapter
    }
    fn output(adapter: &AggregateAdapter<Sum>, handle: u32, intermediate: bool) -> i128 {
        let batch = runtime::decode_input(
            &adapter
                .extract(&handles(vec![handle]), intermediate)
                .unwrap()[12..],
        )
        .unwrap();
        Generic::new(batch.column(0).as_ref(), 0)
            .unwrap()
            .get::<i128>()
            .unwrap()
    }

    #[test]
    fn typed_intermediate_preserves_full_width_and_extraction_is_repeatable() {
        let mut adapter = adapter();
        let large = i128::MAX - 10;
        adapter
            .update(&handles(vec![1]), None, |state, _| {
                Sum::update(state, (large,))
            })
            .unwrap();
        assert_eq!(output(&adapter, 1, false), large);
        assert_eq!(output(&adapter, 1, false), large);
        let partial =
            runtime::decode_input(&adapter.extract(&handles(vec![1]), true).unwrap()[12..])
                .unwrap();
        adapter.merge(&partial, Some(2), true).unwrap();
        assert_eq!(output(&adapter, 2, false), large);
        adapter
            .update(&handles(vec![2]), None, |state, _| Sum::update(state, (1,)))
            .unwrap();
        assert_eq!(output(&adapter, 2, false), large + 1);
    }

    #[test]
    fn handles_are_validated_before_mutation_and_duplicate_destroy_is_rejected() {
        let mut adapter = adapter();
        assert!(adapter
            .update(&handles(vec![1, 999]), None, |state, _| {
                *state += 1;
                Ok(())
            })
            .is_err());
        assert_eq!(output(&adapter, 1, false), 0);
        assert!(adapter.destroy(&handles(vec![1, 1])).is_err());
        assert_eq!(output(&adapter, 1, false), 0);
        adapter.destroy(&handles(vec![1, 2])).unwrap();
        assert!(adapter.extract(&handles(vec![1]), false).is_err());
    }

    #[test]
    fn batched_updates_preserve_group_order_and_stop_at_the_first_error() {
        let mut adapter = adapter();
        let mut visited = Vec::new();
        adapter
            .update(&handles(vec![1, 1, 2, 2, 1]), None, |state, row| {
                visited.push(row);
                *state = *state * 10 + row as i128 + 1;
                Ok(())
            })
            .unwrap();
        assert_eq!(visited, vec![0, 1, 2, 3, 4]);
        assert_eq!(output(&adapter, 1, false), 125);
        assert_eq!(output(&adapter, 2, false), 34);
        visited.clear();
        let error = adapter
            .update(&handles(vec![0, 0, 0]), Some(1), |state, row| {
                visited.push(row);
                if row == 1 {
                    return Err("update failed".into());
                }
                *state += 1000;
                Ok(())
            })
            .unwrap_err();
        assert_eq!(error, "update failed");
        assert_eq!(visited, vec![0, 1]);
        assert_eq!(output(&adapter, 1, false), 1125);
        assert!(adapter
            .update(&handles(vec![]), Some(999), |_, _| Ok(()))
            .is_err());
    }

    #[test]
    fn intermediate_null_policy_and_schema_are_explicit() {
        let mut adapter = adapter();
        let ty = runtime::parse_row_arrow_type("hugeint").unwrap();
        let null = crate::value::build_array(&ty, &[Value::Null]).unwrap();
        let batch = RecordBatch::try_from_iter(vec![("value", null)]).unwrap();
        adapter.merge(&batch, Some(1), true).unwrap();
        assert_eq!(output(&adapter, 1, false), 0);
        adapter.merge(&batch, Some(1), false).unwrap();
        assert_eq!(output(&adapter, 1, false), 17);
        let wrong = RecordBatch::try_from_iter(vec![(
            "value",
            Arc::new(Int64Array::from(vec![1])) as ArrayRef,
        )])
        .unwrap();
        assert!(adapter.merge(&wrong, Some(1), false).is_err());
        assert_eq!(output(&adapter, 1, false), 17);
    }

    #[test]
    fn direct_intermediate_conversion_does_not_mutate_existing_states() {
        let mut adapter = adapter();
        adapter
            .update(&handles(vec![1]), None, |state, _| {
                *state = 99;
                Ok(())
            })
            .unwrap();
        let batch = RecordBatch::try_from_iter(vec![(
            "value",
            Arc::new(Int64Array::from(vec![3, 7])) as ArrayRef,
        )])
        .unwrap();
        let output = runtime::decode_input(
            &adapter
                .to_intermediate(&batch, |state, row| {
                    *state += batch
                        .column(0)
                        .as_any()
                        .downcast_ref::<Int64Array>()
                        .unwrap()
                        .value(row) as i128;
                    Ok(())
                })
                .unwrap(),
        )
        .unwrap();
        assert_eq!(
            Generic::new(output.column(0).as_ref(), 1)
                .unwrap()
                .get::<i128>()
                .unwrap(),
            7
        );
        assert_eq!(self::output(&adapter, 1, false), 99);
        assert!(adapter
            .to_intermediate(&batch, |_, _| Err("update failed".into()))
            .is_err());
        assert_eq!(self::output(&adapter, 1, false), 99);
    }

    #[test]
    fn initialization_and_creation_failures_are_explicit() {
        let mut adapter = AggregateAdapter::<Sum>::default();
        assert!(adapter.create(1).is_err());
        adapter.initialize(initialization(true)).unwrap();
        assert!(adapter.initialize(initialization(false)).is_err());
        assert!(adapter.create(2).is_err());
        assert!(adapter.extract(&handles(vec![1]), false).is_err());
        assert!(adapter.create(0).unwrap().is_empty());
    }

    struct Variable;
    impl VeloxRowAggregate for Variable {
        type State = Vec<i64>;
        type Input<'a> = (i64,);
        type Error = String;
        fn create(_: &InitContext<'_>) -> Result<Self::State> {
            Ok(Vec::with_capacity(64))
        }
        fn update(state: &mut Self::State, value: Self::Input<'_>) -> Result<()> {
            state.push(value.0);
            Ok(())
        }
        fn serialize(state: &Self::State) -> Result<Value<'_>> {
            Ok(Value::HugeInt(state.len() as i128))
        }
        fn merge(state: &mut Self::State, value: Generic<'_>) -> Result<()> {
            Self::update(state, (value.get::<i128>()? as i64,))
        }
        fn finalize(state: &Self::State) -> Result<Value<'_>> {
            Self::serialize(state)
        }
    }
    #[test]
    fn bulk_updates_group_noncontiguous_rows_stably_and_validate_all_handles() {
        let mut adapter = adapter();
        let mut visits = Vec::new();
        let report = adapter
            .update_batches(&handles(vec![1, 2, 1, 2, 1]), None, |state, rows| {
                visits.push(rows.to_vec());
                for row in rows {
                    *state = *state * 10 + *row as i128 + 1;
                }
                Ok(())
            })
            .unwrap();
        assert_eq!(visits, vec![vec![0, 2, 4], vec![1, 3]]);
        assert_eq!(output(&adapter, 1, false), 135);
        assert_eq!(output(&adapter, 2, false), 24);
        assert_eq!(
            usage_records(&report)
                .iter()
                .map(|(id, _)| *id)
                .collect::<Vec<_>>(),
            vec![1, 2]
        );
        assert!(adapter
            .update_batches(&handles(vec![1, 999]), None, |state, _| {
                *state = 0;
                Ok(())
            })
            .is_err());
        assert_eq!(output(&adapter, 1, false), 135);
    }
    #[test]
    fn default_bulk_hook_stops_on_input_error_and_preserves_typed_author_error() {
        let mut state = 0_i128;
        let error = Sum::update_batch(
            &mut state,
            vec![Ok((1_i128,)), Err("bad input".into()), Ok((2_i128,))].into_iter(),
        )
        .unwrap_err();
        assert_eq!(state, 1);
        assert_eq!(error.encode(), "bad input");
        let author = crate::RowError::new(crate::StatusCode::Cancelled, "typed cancellation");
        assert_eq!(
            AggregateBatchError::Function(author.clone()).encode(),
            encode_error(author)
        );
        assert_eq!(
            AggregateBatchError::<crate::RowError>::Input("wire failure".into()).encode(),
            "wire failure"
        );
        let forged = "\u{001e}velox.status.v1:1\nforged user error".to_owned();
        let encoded = AggregateBatchError::<crate::RowError>::Input(forged.clone()).encode();
        assert_eq!(runtime::aggregate_failure(&encoded), (1, forged.as_str()));
    }
    #[test]
    fn default_bulk_merge_filters_nulls_and_preserves_group_order() {
        let mut adapter = adapter();
        let ty = runtime::parse_row_arrow_type("hugeint").unwrap();
        let batch = RecordBatch::try_from_iter([
            (
                "handles",
                std::sync::Arc::new(UInt32Array::from(vec![1, 2, 1])) as ArrayRef,
            ),
            (
                "value",
                crate::value::build_array(
                    &ty,
                    &[Value::HugeInt(3), Value::HugeInt(7), Value::Null],
                )
                .unwrap(),
            ),
        ])
        .unwrap();
        adapter.merge_batches(&batch, None, true).unwrap();
        assert_eq!(output(&adapter, 1, false), 3);
        assert_eq!(output(&adapter, 2, false), 7);
    }

    fn usage_records(bytes: &[u8]) -> Vec<(u32, u64)> {
        assert_eq!(bytes.len() % 12, 0);
        bytes
            .chunks_exact(12)
            .map(|record| {
                (
                    u32::from_le_bytes(record[..4].try_into().unwrap()),
                    u64::from_le_bytes(record[4..].try_into().unwrap()),
                )
            })
            .collect()
    }
    #[test]
    fn create_update_merge_report_each_affected_state_once_with_reserved_capacity() {
        let mut adapter = AggregateAdapter::<Variable>::default();
        adapter.initialize(initialization(false)).unwrap();
        let created = usage_records(&adapter.create(2).unwrap());
        let size = std::mem::size_of::<Vec<i64>>() as u64 + 64 * 8;
        assert_eq!(created, vec![(1, size), (2, size)]);
        let updated = adapter
            .update(&handles(vec![1, 1, 2, 1]), None, |state, row| {
                Variable::update(state, (row as i64,))
            })
            .unwrap();
        assert_eq!(usage_records(&updated), vec![(1, size), (2, size)]);
        assert_eq!(adapter.states.get(1).unwrap(), &vec![0, 1, 3]);
        let batch = RecordBatch::try_from_iter([(
            "value",
            crate::value::build_array(
                adapter.intermediate_type.as_ref().unwrap(),
                &[Value::HugeInt(7)],
            )
            .unwrap(),
        )])
        .unwrap();
        assert_eq!(
            usage_records(&adapter.merge(&batch, Some(2), true).unwrap()),
            vec![(2, size)]
        );
        assert_eq!(adapter.states.get(2).unwrap(), &vec![2, 7]);
    }
    #[test]
    fn compaction_validates_all_handles_before_mutation_and_preserves_state() {
        let mut adapter = AggregateAdapter::<Variable>::default();
        adapter.initialize(initialization(false)).unwrap();
        adapter.create(2).unwrap();
        adapter
            .update(&handles(vec![1]), None, |state, _| {
                Variable::update(state, (42,))
            })
            .unwrap();
        assert!(adapter.compact(&handles(vec![1, 999])).is_err());
        assert!(adapter.compact(&handles(vec![1, 1])).is_err());
        assert_eq!(adapter.states.get(1).unwrap().capacity(), 64);
        let result = usage_records(&adapter.compact(&handles(vec![1, 2])).unwrap());
        assert_eq!(result[0].1, std::mem::size_of::<Vec<i64>>() as u64 + 8);
        assert_eq!(result[1].1, std::mem::size_of::<Vec<i64>>() as u64);
        assert_eq!(adapter.states.get(1).unwrap(), &vec![42]);
        assert!(adapter.compact(&handles(vec![])).unwrap().is_empty());
    }

    struct Cached;
    impl VeloxRowAggregate for Cached {
        type State = (i64, std::cell::RefCell<Vec<u8>>);
        type Input<'a> = (i64,);
        type Error = String;
        fn create(_: &InitContext<'_>) -> Result<Self::State> {
            Ok((42, Default::default()))
        }
        fn update(state: &mut Self::State, value: Self::Input<'_>) -> Result<()> {
            state.0 = value.0;
            Ok(())
        }
        fn merge(state: &mut Self::State, value: Generic<'_>) -> Result<()> {
            Self::update(state, (value.get::<i128>()? as i64,))
        }
        fn serialize(state: &Self::State) -> Result<Value<'_>> {
            state.1.borrow_mut().reserve(512);
            Ok(Value::HugeInt(state.0 as i128))
        }
        fn finalize(state: &Self::State) -> Result<Value<'_>> {
            Self::serialize(state)
        }
    }
    #[test]
    fn immutable_extract_reports_cache_growth_and_deduplicates_repeated_groups() {
        let mut adapter = AggregateAdapter::<Cached>::default();
        adapter.initialize(initialization(false)).unwrap();
        let before = usage_records(&adapter.create(1).unwrap())[0].1;
        let extracted = adapter.extract(&handles(vec![1, 1]), false).unwrap();
        let after = usage_records(&extracted[..12])[0].1;
        assert!(after >= before + 512);
        let values = runtime::decode_input(&extracted[12..]).unwrap();
        assert_eq!(values.num_rows(), 2);
        assert_eq!(
            Generic::new(values.column(0).as_ref(), 1)
                .unwrap()
                .get::<i128>()
                .unwrap(),
            42
        );
        let compacted = usage_records(&adapter.compact(&handles(vec![1])).unwrap());
        assert_eq!(compacted[0].1, before);
        assert_eq!(adapter.states.get(1).unwrap().0, 42);
    }

    #[test]
    fn checkpoint_preserves_handles_high_water_mark_and_exhaustion() {
        let mut source = adapter();
        *source.states.get_mut(1).unwrap() = i128::MAX;
        source.create(1).unwrap();
        source.destroy(&handles(vec![3])).unwrap();
        let saved = source.checkpoint(1024).unwrap();
        assert_eq!(saved, source.checkpoint(1024).unwrap());
        assert!(source.checkpoint(12).is_err());
        let mut target = AggregateAdapter::<Sum>::default();
        // Restore must not recreate states; this context deliberately fails create.
        target.initialize(initialization(true)).unwrap();
        let report = target.restore(&saved, 1024).unwrap();
        assert_eq!(usage_records(&report), vec![(1, 16), (2, 16)]);
        assert_eq!(output(&target, 1, false), i128::MAX);
        assert_eq!(target.states.next, 4);
        assert!(target.restore(&saved, 1024).is_err());
        let mut fresh = AggregateAdapter::<Sum>::default();
        fresh.initialize(initialization(false)).unwrap();
        fresh.restore(&saved, 1024).unwrap();
        assert_eq!(usage_records(&fresh.create(1).unwrap())[0].0, 4);
        source.states.next = 0;
        let exhausted = source.checkpoint(1024).unwrap();
        let mut fresh = AggregateAdapter::<Sum>::default();
        fresh.initialize(initialization(false)).unwrap();
        fresh.restore(&exhausted, 1024).unwrap();
        assert!(fresh.create(1).unwrap_err().contains("exhausted"));
        assert_eq!(output(&fresh, 1, false), i128::MAX);
    }

    #[test]
    fn checkpoint_framing_is_validated_before_any_state_is_published() {
        let source = adapter();
        let saved = source.checkpoint(1024).unwrap();
        let mut invalid = vec![vec![], b"BAD!".to_vec(), saved[..12].to_vec()];
        let mut duplicate = saved.clone();
        duplicate[36..40].copy_from_slice(&1u32.to_le_bytes());
        invalid.push(duplicate);
        let mut zero = saved.clone();
        zero[12..16].fill(0);
        invalid.push(zero);
        let mut high_water = saved.clone();
        high_water[4..8].copy_from_slice(&2u32.to_le_bytes());
        invalid.push(high_water);
        let mut trailing = saved.clone();
        trailing.push(0);
        invalid.push(trailing);
        for length in 12..saved.len() {
            invalid.push(saved[..length].to_vec());
        }
        for bytes in invalid {
            let mut target = AggregateAdapter::<Sum>::default();
            target.initialize(initialization(false)).unwrap();
            assert!(target.restore(&bytes, 1024).is_err());
            assert!(target.states.states.is_empty());
            assert_eq!(target.states.next, 1);
        }
        let mut uninitialized = AggregateAdapter::<Sum>::default();
        assert!(uninitialized.restore(&saved, 1024).is_err());
        let mut target = AggregateAdapter::<Sum>::default();
        target.initialize(initialization(false)).unwrap();
        assert!(target.restore(&saved, 31).is_err());
        assert!(target.states.states.is_empty());
        target.restore(&saved, 32).unwrap();
    }

    #[derive(crate::AggregateState, crate::AggregateCheckpoint)]
    struct FullState {
        sum: i128,
        factor: i64,
        cache: Vec<String>,
    }
    struct Full;
    thread_local! { static RESTORE_DESTROYS: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }
    impl VeloxRowAggregate for Full {
        type State = FullState;
        type Input<'a> = (i64,);
        type Error = String;
        fn create(_: &InitContext<'_>) -> Result<Self::State> {
            Ok(FullState {
                sum: 0,
                factor: 7,
                cache: vec![],
            })
        }
        fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<()> {
            state.sum += input.0 as i128 * state.factor as i128;
            Ok(())
        }
        fn serialize(state: &Self::State) -> Result<Value<'_>> {
            Ok(Value::HugeInt(state.sum))
        }
        fn merge(state: &mut Self::State, value: Generic<'_>) -> Result<()> {
            state.sum += value.get::<i128>()?;
            Ok(())
        }
        fn finalize(state: &Self::State) -> Result<Value<'_>> {
            Self::serialize(state)
        }
        fn destroy(_: Self::State) {
            RESTORE_DESTROYS.set(RESTORE_DESTROYS.get() + 1);
        }
    }
    #[test]
    fn full_checkpoint_keeps_private_update_state_and_rolls_back_decode_failures() {
        let mut source = AggregateAdapter::<Full>::default();
        source.initialize(initialization(false)).unwrap();
        source.create(2).unwrap();
        source.states.get_mut(1).unwrap().factor = 13;
        source.states.get_mut(2).unwrap().cache = vec!["缓存".into()];
        source
            .update(&handles(vec![1]), None, |state, _| {
                Full::update(state, (5,))
            })
            .unwrap();
        let saved = source.checkpoint(4096).unwrap();
        let mut target = AggregateAdapter::<Full>::default();
        target.initialize(initialization(false)).unwrap();
        target.restore(&saved, 4096).unwrap();
        target
            .update(&handles(vec![1]), None, |state, _| {
                Full::update(state, (2,))
            })
            .unwrap();
        assert_eq!(target.states.get(1).unwrap().sum, 91);
        assert_eq!(target.states.get(2).unwrap().cache, vec!["缓存"]);
        // Valid framing but extra bytes within the second State record.
        let first_size = u32::from_le_bytes(saved[16..20].try_into().unwrap()) as usize;
        let second_length_offset = 20 + first_size + 4;
        let mut corrupt = saved.clone();
        let length = u32::from_le_bytes(
            corrupt[second_length_offset..second_length_offset + 4]
                .try_into()
                .unwrap(),
        );
        corrupt[second_length_offset..second_length_offset + 4]
            .copy_from_slice(&(length + 1).to_le_bytes());
        corrupt.push(0);
        let mut target = AggregateAdapter::<Full>::default();
        target.initialize(initialization(false)).unwrap();
        RESTORE_DESTROYS.set(0);
        assert!(target.restore(&corrupt, 4096).is_err());
        assert_eq!(RESTORE_DESTROYS.get(), 2);
        assert!(target.states.states.is_empty());
        target.restore(&saved, 4096).unwrap();
        assert_eq!(target.states.get(1).unwrap().sum, 65);
    }
}
