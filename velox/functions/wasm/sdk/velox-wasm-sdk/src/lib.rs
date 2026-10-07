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

extern crate self as velox_wasm_sdk;
pub mod checkpoint;
pub mod state;
pub use checkpoint::{AggregateCheckpoint, CheckpointReader, CheckpointWriter};
pub use state::AggregateState;
pub mod lambda;
pub use lambda::Lambda;
pub mod aggregate;
pub use aggregate::{AggregateBatchError, VeloxRowAggregate};
pub mod comparison;
pub mod scalar;
pub mod sql_traits;
pub use comparison::{CompareFlags, NullHandling, SqlKey};
pub use sql_traits::{SqlComparable, SqlKnown, SqlOrderable, SqlValue};
pub mod value;
pub mod views;
pub mod writers;
pub use scalar::{Decimal, Generic, Variadic};
pub mod variadic;
pub use variadic::{VariadicIter, VariadicView};
pub mod custom;
pub use custom::{register_codec_semantics, CodecSemantics, CodecType, CustomValue, OpaqueHandle};
pub use value::Value;
pub use views::{
    ArrayView, DynamicRow, MapView, Materialize, NullFreeArrayView, NullFreeMapView,
    NullFreeRowView, RowView,
};
pub use writers::{ArrayWriter, GenericWriter, MapWriter, RowWriter};
#[cfg(test)]
mod nested_tests;

pub use arrow_array;
pub use arrow_schema;
pub use velox_wasm_macros::{
    velox_aggregate, velox_arrow_scalar, velox_row_aggregate, velox_scalar, AggregateCheckpoint,
    AggregateState,
};

/// Input types available at aggregate factory initialization. Native staged
/// plans can bind raw types for a final operator; this is not the operator step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AggregateInputTypes {
    Raw,
    Intermediate,
}

/// One-row initialization message. Nonconstant columns are null placeholders;
/// constant SQL NULL is distinguished by the constant mask. Types are bound by
/// Velox before this message is sent. Only declared query configuration is sent.
/// Independent merge/extract uses a separate intermediate-constant marker and
/// never supplies intermediate data through raw constant indices.
pub struct InitContext<'a> {
    batch: &'a arrow_array::RecordBatch,
}

impl<'a> InitContext<'a> {
    pub fn new(batch: &'a arrow_array::RecordBatch) -> Result<Self, String> {
        let mask = batch
            .schema_ref()
            .metadata()
            .get("velox.constant_arguments")
            .ok_or("missing initialization constant mask")?;
        if batch.num_rows() != 1
            || mask.len() != batch.num_columns()
            || mask.bytes().any(|value| value != b'0' && value != b'1')
            || !batch
                .schema_ref()
                .metadata()
                .contains_key("velox.return_type")
        {
            return Err("invalid scalar initialization message".to_owned());
        }
        let metadata = batch.schema_ref().metadata();
        let intermediate = metadata.get("velox.aggregate.intermediate_constant");
        match metadata
            .get("velox.aggregate.input_types")
            .map(String::as_str)
        {
            None | Some("raw") => {
                if intermediate.is_some() {
                    return Err("unexpected intermediate initialization constant".into());
                }
            }
            Some("intermediate") => {
                if batch.num_columns() != 1
                    || mask != "0"
                    || !metadata.contains_key("velox.intermediate_type")
                    || !matches!(intermediate.map(String::as_str), Some("true" | "false"))
                    || (intermediate.map(String::as_str) == Some("false")
                        && !batch.column(0).is_null(0))
                {
                    return Err("invalid intermediate initialization message".into());
                }
            }
            _ => return Err("invalid aggregate input type binding".into()),
        }
        Ok(Self { batch })
    }

    /// Whether this factory bound raw or intermediate input types. None means
    /// a scalar initialization or a host predating this aggregate metadata.
    pub fn aggregate_input_types(&self) -> Option<AggregateInputTypes> {
        match self
            .batch
            .schema_ref()
            .metadata()
            .get("velox.aggregate.input_types")
            .map(String::as_str)
        {
            Some("raw") => Some(AggregateInputTypes::Raw),
            Some("intermediate") => Some(AggregateInputTypes::Intermediate),
            _ => None,
        }
    }

    /// A constant intermediate supplied to an independent merge factory.
    /// Some(NULL) distinguishes constant SQL NULL from a missing/nonconstant
    /// intermediate. It is data for merge, not raw-input configuration.
    pub fn intermediate_constant_value(&self) -> Result<Option<Generic<'_>>, String> {
        if self.aggregate_input_types() == Some(AggregateInputTypes::Intermediate)
            && self
                .batch
                .schema_ref()
                .metadata()
                .get("velox.aggregate.intermediate_constant")
                .map(String::as_str)
                == Some("true")
        {
            Ok(Some(Generic::new(self.batch.column(0).as_ref(), 0)?))
        } else {
            Ok(None)
        }
    }

    pub fn argument_count(&self) -> usize {
        self.batch.num_columns()
    }

    pub fn argument_type(&self, index: usize) -> Option<&arrow_schema::DataType> {
        self.batch
            .schema_ref()
            .fields()
            .get(index)
            .map(|field| field.data_type())
    }

    pub fn argument_sql_type(&self, index: usize) -> Option<&str> {
        self.batch
            .schema_ref()
            .metadata()
            .get(&format!("velox.argument_type.{index}"))
            .map(String::as_str)
    }

    pub fn return_codec(&self) -> Result<Option<CodecType>, String> {
        custom::CodecType::from_type(&runtime::bound_return_type(
            self.batch.schema_ref().metadata(),
        )?)
    }

    pub fn return_sql_type(&self) -> &str {
        &self.batch.schema_ref().metadata()["velox.return_type"]
    }

    pub fn constant_argument(&self, index: usize) -> Option<&arrow_array::ArrayRef> {
        let mask = &self.batch.schema_ref().metadata()["velox.constant_arguments"];
        if mask.as_bytes().get(index) == Some(&b'1') {
            self.batch.columns().get(index)
        } else {
            None
        }
    }

    /// A borrowed scalar constant; None means nonconstant or missing, while
    /// Some(Generic::is_null()) distinguishes a constant SQL NULL.
    pub fn constant_value(&self, index: usize) -> Result<Option<Generic<'_>>, String> {
        self.constant_argument(index)
            .map(|value| Generic::new(value.as_ref(), 0))
            .transpose()
    }

    /// Lambda indexes follow the declaration's lambda list, excluding values.
    /// Independent merge/extract can preserve a symbolic slot whose input-only
    /// types were erased; inspect Lambda::has_bound_types before evaluating it.
    pub fn lambda(&self, index: usize) -> Result<Lambda, String> {
        Lambda::from_context(self.batch.schema_ref().metadata(), index)
    }

    pub fn config(&self, key: &str) -> Option<&str> {
        self.batch
            .schema_ref()
            .metadata()
            .get(&format!("velox.config.{key}"))
            .map(String::as_str)
    }
}

/// A view over the tail columns of an Arrow scalar's arguments. `any` tails may
/// contain different Arrow types; a type variable tail is homogeneous.
pub struct VariadicArgs<'a> {
    columns: &'a [arrow_array::ArrayRef],
}

impl<'a> VariadicArgs<'a> {
    pub fn new(
        batch: &'a arrow_array::RecordBatch,
        fixed_arguments: usize,
    ) -> Result<Self, String> {
        let columns = batch
            .columns()
            .get(fixed_arguments..)
            .ok_or("missing fixed arguments")?;
        Ok(Self { columns })
    }
    pub fn len(&self) -> usize {
        self.columns.len()
    }
    pub fn is_empty(&self) -> bool {
        self.columns.is_empty()
    }
    pub fn iter(&self) -> impl Iterator<Item = &'a arrow_array::ArrayRef> {
        self.columns.iter()
    }
    pub fn get(&self, index: usize) -> Option<&'a arrow_array::ArrayRef> {
        self.columns.get(index)
    }
}

pub trait VeloxAggregate {
    type State;
    type Input<'a>;
    type Output;
    type Error: ToString;

    fn create() -> std::result::Result<Self::State, Self::Error>;
    fn destroy(state: Self::State);
    fn update(
        state: &mut Self::State,
        input: Self::Input<'_>,
    ) -> std::result::Result<(), Self::Error>;
    fn serialize(state: &Self::State) -> std::result::Result<Vec<u8>, Self::Error>;
    fn merge(state: &mut Self::State, intermediate: &[u8]) -> std::result::Result<(), Self::Error>;
    fn finalize(state: &Self::State) -> std::result::Result<Self::Output, Self::Error>;
}

/// Number of days since 1970-01-01, matching Arrow Date32 and Velox DATE.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Date32(pub i32);

/// Nanoseconds since the Unix epoch. i128 retains Velox's full seconds/nanos range.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct Timestamp(pub i128);
impl Timestamp {
    pub fn from_seconds_nanos(seconds: i64, nanos: u32) -> Result<Self, String> {
        let value = Self(seconds as i128 * 1_000_000_000 + nanos as i128);
        if nanos >= 1_000_000_000 {
            return Err("timestamp nanoseconds exceed one second".into());
        }
        value.validate()?;
        Ok(value)
    }
    pub fn seconds(self) -> i64 {
        self.0.div_euclid(1_000_000_000) as i64
    }
    pub fn nanos(self) -> u32 {
        self.0.rem_euclid(1_000_000_000) as u32
    }
    pub fn validate(self) -> Result<(), String> {
        let seconds = self.0.div_euclid(1_000_000_000);
        if !((i64::MIN / 1000 - 1) as i128..=(i64::MAX / 1000) as i128).contains(&seconds) {
            return Err("timestamp seconds outside Velox range".into());
        }
        Ok(())
    }
    #[doc(hidden)]
    pub fn wire_type() -> arrow_schema::DataType {
        use arrow_schema::{DataType, Field};
        let metadata = std::collections::HashMap::from([(
            "velox.logical_type".to_owned(),
            "timestamp_seconds_nanos_v1".to_owned(),
        )]);
        DataType::Struct(
            vec![
                std::sync::Arc::new(
                    Field::new("seconds", DataType::Int64, true).with_metadata(metadata),
                ),
                std::sync::Arc::new(Field::new("nanos", DataType::Int64, true)),
            ]
            .into(),
        )
    }
    #[doc(hidden)]
    pub fn is_wire_type(ty: &arrow_schema::DataType) -> bool {
        let arrow_schema::DataType::Struct(fields) = ty else {
            return false;
        };
        fields.len() == 2
            && fields[0].name() == "seconds"
            && fields[1].name() == "nanos"
            && fields[0].data_type() == &arrow_schema::DataType::Int64
            && fields[1].data_type() == &arrow_schema::DataType::Int64
            && fields[0]
                .metadata()
                .get("velox.logical_type")
                .map(String::as_str)
                == Some("timestamp_seconds_nanos_v1")
    }
}

/// Error codes mirror velox/common/base/Status.h. Non-user statuses retain
/// native fatal/system behavior; they are not recoverable SQL TRY errors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum StatusCode {
    UserError = 1,
    TypeError = 2,
    IndexError = 3,
    KeyError = 4,
    AlreadyExists = 5,
    OutOfMemory = 6,
    IOError = 7,
    Cancelled = 8,
    Invalid = 9,
    UnknownError = 10,
    NotImplemented = 11,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RowError {
    pub code: StatusCode,
    pub message: String,
}
impl RowError {
    pub fn new(code: StatusCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}
impl std::fmt::Display for RowError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}: {}", self.code, self.message)
    }
}
impl std::error::Error for RowError {}

/// A SQL VARCHAR byte view / owned value, including invalid UTF-8.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VarcharBytes<T>(pub T);
#[doc(hidden)]
pub fn varchar_wire_type() -> arrow_schema::DataType {
    use arrow_schema::{DataType, Field};
    let metadata = std::collections::HashMap::from([(
        "velox.logical_type".to_owned(),
        "varchar_bytes_v1".to_owned(),
    )]);
    DataType::Struct(
        vec![std::sync::Arc::new(
            Field::new("bytes", DataType::Binary, true).with_metadata(metadata),
        )]
        .into(),
    )
}
#[doc(hidden)]
pub fn is_varchar_wire_type(ty: &arrow_schema::DataType) -> bool {
    let arrow_schema::DataType::Struct(fields) = ty else {
        return false;
    };
    fields.len() == 1
        && fields[0].name() == "bytes"
        && fields[0].data_type() == &arrow_schema::DataType::Binary
        && fields[0]
            .metadata()
            .get("velox.logical_type")
            .map(String::as_str)
            == Some("varchar_bytes_v1")
}
// HUGEINT has all 128 bits; Decimal128(38,0) cannot represent its extremes.
#[doc(hidden)]
pub fn hugeint_wire_type() -> arrow_schema::DataType {
    use arrow_schema::{DataType, Field};
    let metadata = std::collections::HashMap::from([(
        "velox.logical_type".to_owned(),
        "hugeint_high_low_v1".to_owned(),
    )]);
    DataType::Struct(
        vec![
            std::sync::Arc::new(Field::new("high", DataType::Int64, true).with_metadata(metadata)),
            std::sync::Arc::new(Field::new("low", DataType::Int64, true)),
        ]
        .into(),
    )
}
#[doc(hidden)]
pub fn is_hugeint_wire_type(ty: &arrow_schema::DataType) -> bool {
    let arrow_schema::DataType::Struct(fields) = ty else {
        return false;
    };
    fields.len() == 2
        && fields[0].name() == "high"
        && fields[1].name() == "low"
        && fields[0].data_type() == &arrow_schema::DataType::Int64
        && fields[1].data_type() == &arrow_schema::DataType::Int64
        && fields[0]
            .metadata()
            .get("velox.logical_type")
            .map(String::as_str)
            == Some("hugeint_high_low_v1")
}

pub mod runtime {
    use std::collections::HashMap;
    use std::io::Cursor;
    use std::sync::Arc;

    use arrow_array::{ArrayRef, RecordBatch};
    use arrow_ipc::reader::StreamReader;
    use arrow_ipc::writer::StreamWriter;
    use arrow_schema::{ArrowError, DataType, Field, Schema};
    use arrow_select::concat::concat;

    pub type Result<T> = std::result::Result<T, String>;

    /// Collects one-row nested Arrow values returned by an aggregate.
    pub struct ComplexOutputBuilder {
        values: Vec<ArrayRef>,
        empty_type: DataType,
    }

    impl ComplexOutputBuilder {
        pub fn new(sql_type: &str) -> Result<Self> {
            Ok(Self {
                values: Vec::new(),
                empty_type: parse_arrow_type(sql_type)?,
            })
        }

        pub fn append_value(&mut self, value: ArrayRef) -> Result<()> {
            if value.len() != 1 {
                return Err("complex aggregate result must contain one row".to_owned());
            }
            if let Some(first) = self.values.first() {
                if first.data_type() != value.data_type() {
                    return Err("complex aggregate results have different Arrow types".to_owned());
                }
            }
            self.values.push(value);
            Ok(())
        }

        pub fn finish(self) -> Result<ArrayRef> {
            if self.values.is_empty() {
                return Ok(arrow_array::new_empty_array(&self.empty_type));
            }
            let values: Vec<&dyn arrow_array::Array> =
                self.values.iter().map(|value| value.as_ref()).collect();
            concat(&values).map_err(|error| format!("cannot concatenate complex results: {error}"))
        }
    }

    /// Parses the supported Hive-style SQL types for empty complex results.
    /// Row ABI logical TIMESTAMP uses seconds/nanos at any nesting depth.
    pub fn bound_return_type(
        metadata: &std::collections::HashMap<String, String>,
    ) -> Result<DataType> {
        if let Some(text) = metadata.get("velox.return_wire_type") {
            return crate::custom::parse_description(
                &serde_json::from_str::<serde_json::Value>(text).map_err(|e| e.to_string())?,
            );
        }
        parse_row_arrow_type(
            metadata
                .get("velox.return_type")
                .ok_or("missing bound scalar return type")?,
        )
    }

    pub fn parse_row_arrow_type(text: &str) -> Result<DataType> {
        fn convert(ty: DataType) -> DataType {
            match ty {
                DataType::Utf8 => crate::varchar_wire_type(),
                DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None) => {
                    crate::Timestamp::wire_type()
                }
                DataType::List(field) => DataType::List(Arc::new(
                    field
                        .as_ref()
                        .clone()
                        .with_data_type(convert(field.data_type().clone())),
                )),
                DataType::Map(field, sorted) => DataType::Map(
                    Arc::new(
                        field
                            .as_ref()
                            .clone()
                            .with_data_type(convert(field.data_type().clone())),
                    ),
                    sorted,
                ),
                DataType::Struct(fields) => DataType::Struct(
                    fields
                        .iter()
                        .map(|field| {
                            Arc::new(
                                field
                                    .as_ref()
                                    .clone()
                                    .with_data_type(convert(field.data_type().clone())),
                            )
                        })
                        .collect::<Vec<_>>()
                        .into(),
                ),
                ty => ty,
            }
        }
        Ok(convert(parse_arrow_type(text)?))
    }

    pub fn parse_arrow_type(text: &str) -> Result<DataType> {
        struct Parser<'a> {
            text: &'a [u8],
            offset: usize,
        }
        impl Parser<'_> {
            fn whitespace(&mut self) {
                while self.offset < self.text.len() && self.text[self.offset].is_ascii_whitespace()
                {
                    self.offset += 1;
                }
            }
            fn consume(&mut self, character: u8) -> bool {
                self.whitespace();
                if self.text.get(self.offset) == Some(&character) {
                    self.offset += 1;
                    true
                } else {
                    false
                }
            }
            fn expect(&mut self, character: u8) -> Result<()> {
                if self.consume(character) {
                    Ok(())
                } else {
                    Err(format!("expected '{}' in complex type", character as char))
                }
            }
            fn word(&mut self) -> Result<String> {
                self.whitespace();
                let start = self.offset;
                while self.offset < self.text.len()
                    && (self.text[self.offset].is_ascii_alphanumeric()
                        || self.text[self.offset] == b'_')
                {
                    self.offset += 1;
                }
                if start == self.offset {
                    return Err("expected a name in complex type".to_owned());
                }
                Ok(String::from_utf8_lossy(&self.text[start..self.offset]).to_ascii_lowercase())
            }
            fn open(&mut self) -> Result<u8> {
                if self.consume(b'<') {
                    Ok(b'>')
                } else {
                    self.expect(b'(')?;
                    Ok(b')')
                }
            }
            fn field_name(&mut self) -> Result<String> {
                if !self.consume(b'"') {
                    return self.word();
                }
                let mut name = Vec::new();
                loop {
                    let value = *self
                        .text
                        .get(self.offset)
                        .ok_or("unterminated row field name")?;
                    self.offset += 1;
                    if value == b'"' {
                        if self.text.get(self.offset) == Some(&b'"') {
                            self.offset += 1;
                            name.push(value);
                        } else {
                            break;
                        }
                    } else {
                        name.push(value);
                    }
                }
                String::from_utf8(name).map_err(|error| error.to_string())
            }
            fn data_type(&mut self) -> Result<DataType> {
                let name = self.word()?;
                match name.as_str() {
                    "array" => {
                        let close = self.open()?;
                        let item = self.data_type()?;
                        self.expect(close)?;
                        Ok(DataType::List(Arc::new(Field::new("item", item, true))))
                    }
                    "map" => {
                        let close = self.open()?;
                        let key = self.data_type()?;
                        self.expect(b',')?;
                        let value = self.data_type()?;
                        self.expect(close)?;
                        let entries = DataType::Struct(
                            vec![
                                Arc::new(Field::new("key", key, false)),
                                Arc::new(Field::new("value", value, true)),
                            ]
                            .into(),
                        );
                        Ok(DataType::Map(
                            Arc::new(Field::new("entries", entries, false)),
                            false,
                        ))
                    }
                    "row" | "struct" => {
                        let close = self.open()?;
                        let mut fields = Vec::new();
                        if self.consume(close) {
                            return Ok(DataType::Struct(fields.into()));
                        }
                        loop {
                            let field = self.field_name()?;
                            if close == b'>' {
                                self.expect(b':')?;
                            }
                            let data_type = self.data_type()?;
                            fields.push(Arc::new(Field::new(field, data_type, true)));
                            if !self.consume(b',') {
                                break;
                            }
                        }
                        self.expect(close)?;
                        Ok(DataType::Struct(fields.into()))
                    }
                    "decimal" => {
                        self.expect(b'(')?;
                        let precision = self
                            .word()?
                            .parse::<u8>()
                            .map_err(|error| error.to_string())?;
                        self.expect(b',')?;
                        let scale = self
                            .word()?
                            .parse::<i8>()
                            .map_err(|error| error.to_string())?;
                        self.expect(b')')?;
                        if precision == 0 || precision > 38 || scale < 0 || scale as u8 > precision
                        {
                            return Err("invalid decimal precision/scale".into());
                        }
                        Ok(DataType::Decimal128(precision, scale))
                    }
                    "unknown" => Ok(DataType::Null),
                    "boolean" => Ok(DataType::Boolean),
                    "tinyint" => Ok(DataType::Int8),
                    "smallint" => Ok(DataType::Int16),
                    "int" | "integer" => Ok(DataType::Int32),
                    "bigint" => Ok(DataType::Int64),
                    "hugeint" => Ok(crate::hugeint_wire_type()),
                    "real" | "float" => Ok(DataType::Float32),
                    "double" => Ok(DataType::Float64),
                    "varchar" | "string" => Ok(DataType::Utf8),
                    "varbinary" | "binary" => Ok(DataType::Binary),
                    "date" => Ok(DataType::Date32),
                    "timestamp" => Ok(DataType::Timestamp(
                        arrow_schema::TimeUnit::Nanosecond,
                        None,
                    )),
                    _ => Err(format!("unsupported complex type component '{name}'")),
                }
            }
        }
        let mut parser = Parser {
            text: text.as_bytes(),
            offset: 0,
        };
        let data_type = parser.data_type()?;
        parser.whitespace();
        if parser.offset != parser.text.len() {
            return Err("unexpected suffix in complex type".to_owned());
        }
        Ok(data_type)
    }

    pub struct StateRegistry<S> {
        pub(crate) next: u32,
        pub(crate) states: HashMap<u32, S>,
    }

    impl<S> StateRegistry<S> {
        pub fn new() -> Self {
            Self {
                next: 1,
                states: HashMap::new(),
            }
        }

        pub fn insert(&mut self, state: S) -> std::result::Result<u32, (String, S)> {
            if self.next == 0 {
                return Err((
                    "Wasm UDAF state handle space is exhausted".to_owned(),
                    state,
                ));
            }
            let handle = self.next;
            self.next = self.next.checked_add(1).unwrap_or(0);
            self.states.insert(handle, state);
            Ok(handle)
        }

        pub fn get(&self, handle: u32) -> Result<&S> {
            self.states
                .get(&handle)
                .ok_or_else(|| format!("unknown Wasm UDAF state handle {handle}"))
        }

        pub fn get_mut(&mut self, handle: u32) -> Result<&mut S> {
            self.states
                .get_mut(&handle)
                .ok_or_else(|| format!("unknown Wasm UDAF state handle {handle}"))
        }

        pub fn remove(&mut self, handle: u32) -> Option<S> {
            self.states.remove(&handle)
        }
    }

    pub fn decode_input(bytes: &[u8]) -> Result<RecordBatch> {
        let mut reader = StreamReader::try_new(Cursor::new(bytes), None)
            .map_err(|error| format!("cannot open input Arrow IPC: {error}"))?;
        let batch = reader
            .next()
            .ok_or_else(|| "input Arrow IPC contains no record batch".to_owned())?
            .map_err(|error| format!("cannot read input Arrow IPC: {error}"))?;
        if reader.next().is_some() {
            return Err("input Arrow IPC contains more than one record batch".to_owned());
        }
        Ok(batch)
    }

    pub fn encode_batch(fields: Vec<Field>, arrays: Vec<ArrayRef>) -> Result<Vec<u8>> {
        let schema = Arc::new(Schema::new(fields));
        let batch = RecordBatch::try_new(schema.clone(), arrays)
            .map_err(|error| format!("cannot build output record batch: {error}"))?;
        let mut output = Vec::new();
        {
            let mut writer = StreamWriter::try_new(&mut output, &schema)
                .map_err(|error| format!("cannot create output Arrow IPC: {error}"))?;
            writer
                .write(&batch)
                .map_err(|error| format!("cannot write output Arrow IPC: {error}"))?;
            writer
                .finish()
                .map_err(|error| format!("cannot finish output Arrow IPC: {error}"))?;
        }
        Ok(output)
    }

    pub fn encode_output(field: Field, array: ArrayRef) -> Result<Vec<u8>> {
        encode_batch(vec![field], vec![array])
    }

    pub fn type_error(column: usize, expected: &str) -> String {
        format!("argument {column} is not Arrow {expected}")
    }

    pub fn null_error(row: usize, column: usize) -> String {
        format!("non-null argument {column} is null at compact row {row}")
    }

    #[allow(dead_code)]
    fn _keep_arrow_error(_: ArrowError) {}

    // Typed aggregate errors use a tagged ABI status lane, not a message prefix
    // on the wire. Scalar row errors retain their existing error-column format.
    // The internal encoder escapes ordinary errors beginning with the marker.
    #[cfg_attr(not(target_arch = "wasm32"), allow(dead_code))]
    pub(crate) fn aggregate_failure(message: &str) -> (u32, &str) {
        if let Some(payload) = message.strip_prefix("\u{001e}velox.status.v1:") {
            if let Some((code, text)) = payload.split_once('\n') {
                if let Ok(code @ 1..=11) = code.parse::<u8>() {
                    return (0x100 + u32::from(code), text);
                }
            }
        }
        (
            1,
            message
                .strip_prefix("\u{001e}velox.message.v1:")
                .unwrap_or(message),
        )
    }

    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    unsafe fn pack_outcome(outcome: Result<Vec<u8>>, aggregate: bool) -> core::arch::wasm32::v128 {
        match outcome {
            Ok(output) => {
                let (pointer, length) = allocate_bytes(&output);
                pack(0, pointer, length)
            }
            Err(message) => {
                let (status, message) = if aggregate {
                    aggregate_failure(&message)
                } else {
                    (1, message.as_str())
                };
                let (pointer, length) = allocate_bytes(message.as_bytes());
                pack(status, pointer, length)
            }
        }
    }

    #[cfg(target_arch = "wasm32")]
    unsafe fn allocate_bytes(bytes: &[u8]) -> (u32, u32) {
        if bytes.is_empty() {
            return (0, 0);
        }
        let mut boxed = bytes.to_vec().into_boxed_slice();
        let pointer = boxed.as_mut_ptr() as u32;
        let length = boxed.len() as u32;
        std::mem::forget(boxed);
        (pointer, length)
    }

    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    unsafe fn pack(status: u32, pointer: u32, length: u32) -> core::arch::wasm32::v128 {
        core::arch::wasm32::u32x4(status, pointer, length, 0)
    }

    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    pub unsafe fn execute<F>(pointer: u32, length: u32, function: F) -> core::arch::wasm32::v128
    where
        F: FnOnce(RecordBatch) -> Result<Vec<u8>>,
    {
        let input = std::slice::from_raw_parts(pointer as *const u8, length as usize);
        let outcome = decode_input(input).and_then(function);
        pack_outcome(outcome, false)
    }

    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    pub unsafe fn execute_count<F>(count: u32, function: F) -> core::arch::wasm32::v128
    where
        F: FnOnce(u32) -> Result<Vec<u8>>,
    {
        pack_outcome(function(count), false)
    }

    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    pub unsafe fn execute_aggregate<F>(
        pointer: u32,
        length: u32,
        function: F,
    ) -> core::arch::wasm32::v128
    where
        F: FnOnce(RecordBatch) -> Result<Vec<u8>>,
    {
        let input = std::slice::from_raw_parts(pointer as *const u8, length as usize);
        pack_outcome(decode_input(input).and_then(function), true)
    }

    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    pub unsafe fn execute_aggregate_count<F>(count: u32, function: F) -> core::arch::wasm32::v128
    where
        F: FnOnce(u32) -> Result<Vec<u8>>,
    {
        pack_outcome(function(count), true)
    }
    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    pub unsafe fn execute_aggregate_bytes<F>(
        pointer: u32,
        length: u32,
        function: F,
    ) -> core::arch::wasm32::v128
    where
        F: FnOnce(&[u8]) -> Result<Vec<u8>>,
    {
        let input = std::slice::from_raw_parts(pointer as *const u8, length as usize);
        pack_outcome(function(input), true)
    }
}

#[cfg(target_arch = "wasm32")]
#[no_mangle]
pub extern "C" fn velox_wasm_alloc(length: u32) -> u32 {
    let mut boxed = vec![0u8; length as usize].into_boxed_slice();
    let pointer = boxed.as_mut_ptr() as u32;
    std::mem::forget(boxed);
    pointer
}

#[cfg(target_arch = "wasm32")]
#[no_mangle]
pub unsafe extern "C" fn velox_wasm_free(pointer: u32, length: u32) {
    if length != 0 {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
            pointer as *mut u8,
            length as usize,
        )));
    }
}

#[cfg(test)]
mod initialization_tests {
    use super::*;
    use arrow_array::{Array, ArrayRef, RecordBatch, StringArray};
    use arrow_schema::{DataType, Field, Schema};
    use std::collections::HashMap;
    use std::sync::Arc;

    #[test]
    fn parses_bound_sql_types_and_preserves_quoted_row_names() {
        for (sql, hive) in [
            ("array(bigint)", "array<bigint>"),
            (
                "map(varchar,array(decimal(20,4)))",
                "map<varchar,array<decimal(20,4)>>",
            ),
            (
                "row(id bigint,values array(varchar))",
                "row<id:bigint,values:array<varchar>>",
            ),
        ] {
            assert_eq!(
                runtime::parse_arrow_type(sql).unwrap(),
                runtime::parse_arrow_type(hive).unwrap()
            );
        }
        let row = runtime::parse_arrow_type("row(\"Case Name\" bigint,\"quote\"\"name\" unknown)")
            .unwrap();
        let DataType::Struct(fields) = row else {
            panic!("expected row")
        };
        assert_eq!(fields[0].name(), "Case Name");
        assert_eq!(fields[1].name(), "quote\"name");
        assert_eq!(fields[1].data_type(), &DataType::Null);
        for invalid in [
            "decimal(0,0)",
            "decimal(39,0)",
            "decimal(2,3)",
            "array(T)",
            "bigint suffix",
            "row(\"unterminated bigint)",
        ] {
            assert!(runtime::parse_arrow_type(invalid).is_err(), "{invalid}");
        }
    }

    #[test]
    fn constant_null_is_distinct_from_a_nonconstant_argument() {
        let metadata = HashMap::from([
            ("velox.constant_arguments".to_owned(), "10".to_owned()),
            ("velox.return_type".to_owned(), "varchar".to_owned()),
            ("velox.argument_type.0".to_owned(), "varchar".to_owned()),
            ("velox.config.timezone".to_owned(), "UTC".to_owned()),
        ]);
        let schema = Arc::new(Schema::new_with_metadata(
            vec![
                Field::new("arg0", DataType::Utf8, true),
                Field::new("arg1", DataType::Utf8, true),
            ],
            metadata,
        ));
        let null: ArrayRef = Arc::new(StringArray::from(vec![None::<&str>]));
        let batch = RecordBatch::try_new(schema, vec![null.clone(), null]).unwrap();
        let context = InitContext::new(&batch).unwrap();
        assert!(context.constant_argument(0).unwrap().is_null(0));
        assert!(context.constant_argument(1).is_none());
        assert!(context.constant_argument(2).is_none());
        assert_eq!(context.argument_type(0), Some(&DataType::Utf8));
        assert_eq!(context.argument_sql_type(0), Some("varchar"));
        assert_eq!(context.return_sql_type(), "varchar");
        assert_eq!(context.config("timezone"), Some("UTC"));
        assert_eq!(context.config("missing"), None);
        assert_eq!(VariadicArgs::new(&batch, 1).unwrap().len(), 1);
        assert!(VariadicArgs::new(&batch, 2).unwrap().is_empty());
        assert!(VariadicArgs::new(&batch, 3).is_err());
    }

    #[test]
    fn rejects_missing_or_inconsistent_initialization_metadata() {
        let fields = vec![Field::new("arg0", DataType::Utf8, true)];
        for mask in [None, Some(""), Some("2"), Some("11")] {
            let mut metadata =
                HashMap::from([("velox.return_type".to_owned(), "varchar".to_owned())]);
            if let Some(mask) = mask {
                metadata.insert("velox.constant_arguments".to_owned(), mask.to_owned());
            }
            let schema = Arc::new(Schema::new_with_metadata(fields.clone(), metadata));
            let batch =
                RecordBatch::try_new(schema, vec![Arc::new(StringArray::from(vec!["hello"]))])
                    .unwrap();
            assert!(InitContext::new(&batch).is_err());
        }
    }

    #[test]
    fn raw_and_intermediate_constants_use_distinct_channels() {
        let make = |mode: Option<&str>, marker: Option<&str>, mask: &str, null: bool| {
            let mut metadata = HashMap::from([
                ("velox.constant_arguments".into(), mask.into()),
                ("velox.return_type".into(), "bigint".into()),
                ("velox.intermediate_type".into(), "bigint".into()),
            ]);
            if let Some(mode) = mode {
                metadata.insert("velox.aggregate.input_types".into(), mode.into());
            }
            if let Some(marker) = marker {
                metadata.insert(
                    "velox.aggregate.intermediate_constant".into(),
                    marker.into(),
                );
            }
            RecordBatch::try_new(
                Arc::new(Schema::new_with_metadata(
                    vec![Field::new("arg0", DataType::Int64, true)],
                    metadata,
                )),
                vec![Arc::new(arrow_array::Int64Array::from(vec![if null {
                    None
                } else {
                    Some(7)
                }]))],
            )
            .unwrap()
        };
        for mode in [None, Some("raw")] {
            let batch = make(mode, None, "1", false);
            let context = InitContext::new(&batch).unwrap();
            assert_eq!(
                context
                    .constant_value(0)
                    .unwrap()
                    .unwrap()
                    .get::<i64>()
                    .unwrap(),
                7
            );
            assert!(context.intermediate_constant_value().unwrap().is_none());
            assert_eq!(
                context.aggregate_input_types(),
                mode.map(|_| AggregateInputTypes::Raw)
            );
        }
        for null in [false, true] {
            let batch = make(Some("intermediate"), Some("true"), "0", null);
            let context = InitContext::new(&batch).unwrap();
            assert_eq!(
                context.aggregate_input_types(),
                Some(AggregateInputTypes::Intermediate)
            );
            assert!(context.constant_value(0).unwrap().is_none());
            assert_eq!(
                context
                    .intermediate_constant_value()
                    .unwrap()
                    .unwrap()
                    .get::<Option<i64>>()
                    .unwrap(),
                if null { None } else { Some(7) }
            );
        }
        let batch = make(Some("intermediate"), Some("false"), "0", true);
        assert!(InitContext::new(&batch)
            .unwrap()
            .intermediate_constant_value()
            .unwrap()
            .is_none());
    }

    #[test]
    fn intermediate_binding_rejects_ambiguous_or_malformed_markers() {
        for (mode, marker, mask, value) in [
            ("intermediate", None, "0", None),
            ("intermediate", Some("yes"), "0", None),
            ("intermediate", Some("true"), "1", Some(7)),
            ("intermediate", Some("false"), "0", Some(7)),
            ("raw", Some("true"), "1", Some(7)),
            ("other", None, "0", None),
        ] {
            let mut metadata = HashMap::from([
                ("velox.constant_arguments".into(), mask.into()),
                ("velox.return_type".into(), "bigint".into()),
                ("velox.intermediate_type".into(), "bigint".into()),
                ("velox.aggregate.input_types".into(), mode.into()),
            ]);
            if let Some(marker) = marker {
                metadata.insert(
                    "velox.aggregate.intermediate_constant".into(),
                    marker.into(),
                );
            }
            let batch = RecordBatch::try_new(
                Arc::new(Schema::new_with_metadata(
                    vec![Field::new("arg0", DataType::Int64, true)],
                    metadata,
                )),
                vec![Arc::new(arrow_array::Int64Array::from(vec![value]))],
            )
            .unwrap();
            assert!(InitContext::new(&batch).is_err());
        }
    }
}
