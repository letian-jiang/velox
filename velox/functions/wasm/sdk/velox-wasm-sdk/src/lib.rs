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

pub use arrow_array;
pub use arrow_schema;
pub use velox_wasm_macros::{velox_aggregate, velox_arrow_scalar, velox_scalar};

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

/// Number of nanoseconds since 1970-01-01, matching the scalar ABI's Arrow
/// timestamp unit.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Timestamp(pub i64);

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
            fn data_type(&mut self) -> Result<DataType> {
                let name = self.word()?;
                match name.as_str() {
                    "array" => {
                        self.expect(b'<')?;
                        let item = self.data_type()?;
                        self.expect(b'>')?;
                        Ok(DataType::List(Arc::new(Field::new("item", item, true))))
                    }
                    "map" => {
                        self.expect(b'<')?;
                        let key = self.data_type()?;
                        self.expect(b',')?;
                        let value = self.data_type()?;
                        self.expect(b'>')?;
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
                        self.expect(b'<')?;
                        let mut fields = Vec::new();
                        loop {
                            let field = self.word()?;
                            self.expect(b':')?;
                            let data_type = self.data_type()?;
                            fields.push(Arc::new(Field::new(field, data_type, true)));
                            if !self.consume(b',') {
                                break;
                            }
                        }
                        self.expect(b'>')?;
                        Ok(DataType::Struct(fields.into()))
                    }
                    "boolean" => Ok(DataType::Boolean),
                    "tinyint" => Ok(DataType::Int8),
                    "smallint" => Ok(DataType::Int16),
                    "int" | "integer" => Ok(DataType::Int32),
                    "bigint" => Ok(DataType::Int64),
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
        next: u32,
        states: HashMap<u32, S>,
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
        match outcome {
            Ok(output) => {
                let (pointer, length) = allocate_bytes(&output);
                pack(0, pointer, length)
            }
            Err(message) => {
                let (pointer, length) = allocate_bytes(message.as_bytes());
                pack(1, pointer, length)
            }
        }
    }

    #[cfg(target_arch = "wasm32")]
    #[target_feature(enable = "simd128")]
    pub unsafe fn execute_count<F>(count: u32, function: F) -> core::arch::wasm32::v128
    where
        F: FnOnce(u32) -> Result<Vec<u8>>,
    {
        match function(count) {
            Ok(output) => {
                let (pointer, length) = allocate_bytes(&output);
                pack(0, pointer, length)
            }
            Err(message) => {
                let (pointer, length) = allocate_bytes(message.as_bytes());
                pack(1, pointer, length)
            }
        }
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
