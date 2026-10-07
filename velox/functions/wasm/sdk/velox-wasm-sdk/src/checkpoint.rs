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

//! Full owned State checkpoints, independent of SQL intermediate values.
use crate::{AggregateState, CustomValue, Date32, Decimal, Timestamp, Value};
use std::mem::size_of;
type Result<T> = std::result::Result<T, String>;

/// Opting an aggregate into Store rebuilding promises all mutable behavior is
/// owned by State. No retained invocation handles, borrowed values, raw guest
/// pointers or mutable module globals may influence subsequent operations.
/// Implement this for custom ownership; derive it for structs of supported fields.
pub trait AggregateCheckpoint: AggregateState + Sized {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()>;
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self>;
}

pub struct CheckpointWriter {
    pub(crate) bytes: Vec<u8>,
    limit: usize,
    depth: u32,
}
impl CheckpointWriter {
    pub fn new(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
            depth: 0,
        }
    }
    pub fn write(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.len() > self.limit.saturating_sub(self.bytes.len()) {
            return Err("aggregate checkpoint exceeds output limit".into());
        }
        self.bytes
            .try_reserve(bytes.len())
            .map_err(|e| e.to_string())?;
        self.bytes.extend_from_slice(bytes);
        Ok(())
    }
    pub fn finish(self) -> Vec<u8> {
        self.bytes
    }
    pub fn nested<T>(&mut self, f: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        if self.depth == 256 {
            return Err("aggregate checkpoint nesting limit".into());
        }
        self.depth += 1;
        let result = f(self);
        self.depth -= 1;
        result
    }
}

pub struct CheckpointReader<'a> {
    bytes: &'a [u8],
    remaining_allocations: u64,
    depth: u32,
}
impl<'a> CheckpointReader<'a> {
    pub fn new(bytes: &'a [u8], allocation_limit: u64) -> Self {
        Self {
            bytes,
            remaining_allocations: allocation_limit,
            depth: 0,
        }
    }
    pub fn read(&mut self, count: usize) -> Result<&'a [u8]> {
        if count > self.bytes.len() {
            return Err("truncated aggregate checkpoint".into());
        }
        let (value, tail) = self.bytes.split_at(count);
        self.bytes = tail;
        Ok(value)
    }
    pub fn charge(&mut self, count: usize) -> Result<()> {
        self.remaining_allocations = self
            .remaining_allocations
            .checked_sub(count as u64)
            .ok_or("aggregate checkpoint allocation limit")?;
        Ok(())
    }
    pub fn finish(&self) -> Result<()> {
        if self.bytes.is_empty() {
            Ok(())
        } else {
            Err("trailing aggregate checkpoint bytes".into())
        }
    }
    pub fn nested<T>(&mut self, f: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        if self.depth == 256 {
            return Err("aggregate checkpoint nesting limit".into());
        }
        self.depth += 1;
        let result = f(self);
        self.depth -= 1;
        result
    }
}
macro_rules! integer {
    ($($ty:ty),*) => {$ (
        impl AggregateCheckpoint for $ty {
            fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> { out.write(&self.to_le_bytes()) }
            fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
                Ok(Self::from_le_bytes(input.read(size_of::<Self>())?.try_into().unwrap()))
            }
        }
    )*};
}
integer!(i8, i16, i32, i64, i128, u8, u16, u32, u64, u128);
impl AggregateCheckpoint for () {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        out.write(&[0])
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        if input.read(1)?[0] != 0 {
            return Err("invalid unit checkpoint".into());
        }
        Ok(())
    }
}
impl AggregateCheckpoint for bool {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        (*self as u8).encode_checkpoint(out)
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        match u8::decode_checkpoint(input)? {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err("invalid boolean checkpoint".into()),
        }
    }
}
macro_rules! mapped {
    ($ty:ty, $wire:ty, $encode:expr, $decode:expr) => {
        impl AggregateCheckpoint for $ty {
            fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
                ($encode)(*self).encode_checkpoint(out)
            }
            fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
                ($decode)(<$wire>::decode_checkpoint(input)?)
            }
        }
    };
}
mapped!(usize, u64, |v: usize| v as u64, |v| usize::try_from(v)
    .map_err(|_| "checkpoint usize overflow".into()));
mapped!(isize, i64, |v: isize| v as i64, |v| isize::try_from(v)
    .map_err(|_| "checkpoint isize overflow".into()));
mapped!(f32, u32, |v: f32| v.to_bits(), |v| Ok(f32::from_bits(v)));
mapped!(f64, u64, |v: f64| v.to_bits(), |v| Ok(f64::from_bits(v)));
mapped!(char, u32, |v: char| v as u32, |v| char::from_u32(v)
    .ok_or_else(|| "invalid checkpoint char".into()));
mapped!(Date32, i32, |v: Date32| v.0, |v| Ok(Date32(v)));
impl AggregateCheckpoint for String {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        (self.len() as u64).encode_checkpoint(out)?;
        out.write(self.as_bytes())
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        let size = usize::decode_checkpoint(input)?;
        let value = std::str::from_utf8(input.read(size)?).map_err(|e| e.to_string())?;
        input.charge(size)?;
        let mut result = String::new();
        result.try_reserve_exact(size).map_err(|e| e.to_string())?;
        result.push_str(value);
        Ok(result)
    }
}
impl<T: AggregateCheckpoint> AggregateCheckpoint for Vec<T> {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        (self.len() as u64).encode_checkpoint(out)?;
        out.nested(|out| {
            for value in self {
                out.write(&[0])?;
                value.encode_checkpoint(out)?;
            }
            Ok(())
        })
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        let count = usize::decode_checkpoint(input)?;
        if count > input.bytes.len() {
            return Err("invalid checkpoint element count".into());
        }
        input.charge(
            count
                .checked_mul(size_of::<T>())
                .ok_or("checkpoint vector size overflow")?,
        )?;
        input.nested(|input| {
            let mut values = Vec::new();
            values.try_reserve_exact(count).map_err(|e| e.to_string())?;
            for _ in 0..count {
                if input.read(1)?[0] != 0 {
                    return Err("invalid checkpoint element marker".into());
                }
                values.push(T::decode_checkpoint(input)?);
            }
            Ok(values)
        })
    }
}
impl<T: AggregateCheckpoint> AggregateCheckpoint for Option<T> {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        self.is_some().encode_checkpoint(out)?;
        if let Some(value) = self {
            out.nested(|out| value.encode_checkpoint(out))?;
        }
        Ok(())
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        if bool::decode_checkpoint(input)? {
            input.nested(|input| T::decode_checkpoint(input)).map(Some)
        } else {
            Ok(None)
        }
    }
}
impl<T: AggregateCheckpoint> AggregateCheckpoint for Box<T> {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        out.nested(|out| self.as_ref().encode_checkpoint(out))
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        input.charge(size_of::<T>())?;
        input
            .nested(|input| T::decode_checkpoint(input))
            .map(Box::new)
    }
}
impl<T: AggregateCheckpoint> AggregateCheckpoint for std::cell::RefCell<T> {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        self.try_borrow()
            .map_err(|_| "borrowed State during checkpoint".to_owned())?
            .encode_checkpoint(out)
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        T::decode_checkpoint(input).map(Self::new)
    }
}
impl<T: AggregateCheckpoint, const N: usize> AggregateCheckpoint for [T; N] {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        out.nested(|out| {
            for value in self {
                value.encode_checkpoint(out)?;
            }
            Ok(())
        })
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        input.charge(
            N.checked_mul(size_of::<T>())
                .ok_or("checkpoint array size overflow")?,
        )?;
        input.nested(|input| {
            let mut values = Vec::new();
            values.try_reserve_exact(N).map_err(|e| e.to_string())?;
            for _ in 0..N {
                values.push(T::decode_checkpoint(input)?);
            }
            values
                .try_into()
                .map_err(|_| "checkpoint array length mismatch".into())
        })
    }
}
macro_rules! tuple {
    ($($ty:ident:$index:tt),+) => {
        impl<$($ty:AggregateCheckpoint),+> AggregateCheckpoint for ($($ty,)+) {
            fn encode_checkpoint(&self,out:&mut CheckpointWriter)->Result<()> {
                out.nested(|out| {$(self.$index.encode_checkpoint(out)?;)+Ok(())})
            }
            fn decode_checkpoint(input:&mut CheckpointReader<'_>)->Result<Self> {
                input.nested(|input|Ok(($( $ty::decode_checkpoint(input)?,)+)))
            }
        }
    };
}
tuple!(A:0);
tuple!(A:0,B:1);
tuple!(A:0,B:1,C:2);
tuple!(A:0,B:1,C:2,D:3);
tuple!(A:0,B:1,C:2,D:3,E:4);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8,J:9);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8,J:9,K:10);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8,J:9,K:10,L:11);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8,J:9,K:10,L:11,M:12);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8,J:9,K:10,L:11,M:12,N:13);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8,J:9,K:10,L:11,M:12,N:13,O:14);
tuple!(A:0,B:1,C:2,D:3,E:4,F:5,G:6,H:7,I:8,J:9,K:10,L:11,M:12,N:13,O:14,P:15);
impl<T: AggregateCheckpoint> AggregateCheckpoint for CustomValue<T> {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        self.codec_id.encode_checkpoint(out)?;
        self.type_identity.encode_checkpoint(out)?;
        self.version.encode_checkpoint(out)?;
        self.payload.encode_checkpoint(out)
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        Ok(Self {
            codec_id: String::decode_checkpoint(input)?,
            type_identity: String::decode_checkpoint(input)?,
            version: u32::decode_checkpoint(input)?,
            payload: T::decode_checkpoint(input)?,
        })
    }
}
mapped!(Timestamp, i128, |v: Timestamp| v.0, |v| Ok(Timestamp(v)));
impl AggregateCheckpoint for Decimal {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        self.value.encode_checkpoint(out)?;
        self.precision.encode_checkpoint(out)?;
        self.scale.encode_checkpoint(out)
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        Ok(Self {
            value: i128::decode_checkpoint(input)?,
            precision: u8::decode_checkpoint(input)?,
            scale: i8::decode_checkpoint(input)?,
        })
    }
}
impl AggregateCheckpoint for Value<'static> {
    fn encode_checkpoint(&self, out: &mut CheckpointWriter) -> Result<()> {
        out.nested(|out| {
            macro_rules! value {
                ($tag:expr,$value:expr) => {{
                    out.write(&[$tag])?;
                    $value.encode_checkpoint(out)
                }};
            }
            match self {
                Self::Null => out.write(&[0]),
                Self::Boolean(v) => value!(1, v),
                Self::TinyInt(v) => value!(2, v),
                Self::SmallInt(v) => value!(3, v),
                Self::Integer(v) => value!(4, v),
                Self::BigInt(v) => value!(5, v),
                Self::HugeInt(v) => value!(6, v),
                Self::Real(v) => value!(7, v),
                Self::Double(v) => value!(8, v),
                Self::Varchar(v) => value!(9, v),
                Self::VarcharBytes(v) => value!(10, v),
                Self::Varbinary(v) => value!(11, v),
                Self::Date(v) => value!(12, v),
                Self::Timestamp(v) => value!(13, v),
                Self::Decimal(v) => value!(14, v),
                Self::Array(v) => value!(15, v),
                Self::Map(v) => value!(16, v),
                Self::Row(v) => value!(17, v),
                Self::Custom(v) => value!(18, v),
                Self::Borrowed(_) | Self::Opaque { .. } => Err(
                    "checkpoint cannot retain borrowed values or invocation OPAQUE handles".into(),
                ),
            }
        })
    }
    fn decode_checkpoint(input: &mut CheckpointReader<'_>) -> Result<Self> {
        input.nested(|input| {
            Ok(match u8::decode_checkpoint(input)? {
                0 => Self::Null,
                1 => Self::Boolean(bool::decode_checkpoint(input)?),
                2 => Self::TinyInt(i8::decode_checkpoint(input)?),
                3 => Self::SmallInt(i16::decode_checkpoint(input)?),
                4 => Self::Integer(i32::decode_checkpoint(input)?),
                5 => Self::BigInt(i64::decode_checkpoint(input)?),
                6 => Self::HugeInt(i128::decode_checkpoint(input)?),
                7 => Self::Real(f32::decode_checkpoint(input)?),
                8 => Self::Double(f64::decode_checkpoint(input)?),
                9 => Self::Varchar(String::decode_checkpoint(input)?),
                10 => Self::VarcharBytes(Vec::<u8>::decode_checkpoint(input)?),
                11 => Self::Varbinary(Vec::<u8>::decode_checkpoint(input)?),
                12 => Self::Date(Date32::decode_checkpoint(input)?),
                13 => Self::Timestamp(Timestamp::decode_checkpoint(input)?),
                14 => Self::Decimal(Decimal::decode_checkpoint(input)?),
                15 => Self::Array(Vec::decode_checkpoint(input)?),
                16 => Self::Map(Vec::decode_checkpoint(input)?),
                17 => Self::Row(Vec::decode_checkpoint(input)?),
                18 => Self::Custom(Box::decode_checkpoint(input)?),
                _ => return Err("unknown checkpoint Value tag".into()),
            })
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn encoded<T: AggregateCheckpoint>(value: &T) -> Vec<u8> {
        let mut writer = CheckpointWriter::new(1 << 20);
        value.encode_checkpoint(&mut writer).unwrap();
        writer.finish()
    }
    fn restored<T: AggregateCheckpoint>(bytes: &[u8], limit: u64) -> Result<T> {
        let mut reader = CheckpointReader::new(bytes, limit);
        let value = T::decode_checkpoint(&mut reader)?;
        reader.finish()?;
        Ok(value)
    }

    #[derive(crate::AggregateState, crate::AggregateCheckpoint, Debug, PartialEq)]
    struct Owned<T: AggregateState> {
        private: i128,
        items: Vec<Option<Box<T>>>,
        cache: std::cell::RefCell<String>,
        fixed: [u64; 3],
    }
    #[derive(crate::AggregateState, crate::AggregateCheckpoint, Debug, PartialEq)]
    struct Tuple(i64, String);
    #[derive(crate::AggregateState, crate::AggregateCheckpoint)]
    struct Unit;

    #[test]
    fn derives_restore_full_generic_owned_structs_and_drop_spare_capacity() {
        let mut cache = String::with_capacity(1024);
        cache.push_str("缓存");
        let value = Owned {
            private: i128::MIN,
            items: vec![Some(Box::new(Tuple(7, "nested".into()))), None],
            cache: std::cell::RefCell::new(cache),
            fixed: [0, 1, u64::MAX],
        };
        let bytes = encoded(&value);
        let copy: Owned<Tuple> = restored(&bytes, 4096).unwrap();
        assert_eq!(copy, value);
        assert!(copy.cache.borrow().capacity() < value.cache.borrow().capacity());
        assert!(restored::<Owned<Tuple>>(&bytes, 1).is_err());
        let units = vec![Unit, Unit];
        assert_eq!(restored::<Vec<Unit>>(&encoded(&units), 0).unwrap().len(), 2);
    }

    #[test]
    fn values_preserve_nested_types_arbitrary_varchar_and_float_bits() {
        let value = Value::Row(vec![
            Value::Null,
            Value::Boolean(true),
            Value::TinyInt(i8::MIN),
            Value::SmallInt(i16::MIN),
            Value::Integer(i32::MIN),
            Value::BigInt(i64::MIN),
            Value::HugeInt(i128::MAX),
            Value::Real(f32::from_bits(0x7fc00001)),
            Value::Double(-0.0),
            Value::Varchar("字符串".into()),
            Value::VarcharBytes(vec![0, 255]),
            Value::Varbinary(vec![1, 254]),
            Value::Date(Date32(i32::MAX)),
            Value::Timestamp(Timestamp(i128::MIN)),
            Value::Decimal(Decimal {
                value: i128::MIN,
                precision: 38,
                scale: 7,
            }),
            Value::Array(vec![Value::Null, Value::BigInt(17)]),
            Value::Map(vec![(Value::Integer(3), Value::Array(vec![Value::Null]))]),
            Value::Custom(Box::new(CustomValue {
                codec_id: "custom".into(),
                type_identity: "test".into(),
                version: 2,
                payload: vec![0, 255],
            })),
        ]);
        let bytes = encoded(&value);
        let copy: Value<'static> = restored(&bytes, 1 << 20).unwrap();
        assert_eq!(encoded(&copy), bytes);
        let scalar = (u128::MAX, char::MAX, usize::MAX, isize::MIN);
        assert_eq!(
            restored::<(u128, char, usize, isize)>(&encoded(&scalar), 0).unwrap(),
            scalar
        );
    }

    #[test]
    fn rejects_malformed_lengths_markers_tags_and_trailing_bytes() {
        let impossible = u64::MAX.to_le_bytes();
        assert!(restored::<String>(&impossible, u64::MAX).is_err());
        assert!(restored::<Vec<Unit>>(&impossible, u64::MAX).is_err());
        assert!(restored::<bool>(&[2], 0).is_err());
        assert!(restored::<()>(&[1], 0).is_err());
        assert!(restored::<Value<'static>>(&[255], 1024).is_err());
        assert!(restored::<char>(&0xd800u32.to_le_bytes(), 0).is_err());
        let mut invalid_utf8 = 1u64.to_le_bytes().to_vec();
        invalid_utf8.push(255);
        assert!(restored::<String>(&invalid_utf8, 1024).is_err());
        let mut invalid_marker = 1u64.to_le_bytes().to_vec();
        invalid_marker.push(1);
        assert!(restored::<Vec<Unit>>(&invalid_marker, 0).is_err());
        let bytes = encoded(&vec![Some("hello".to_owned()), None]);
        for length in 0..bytes.len() {
            assert!(restored::<Vec<Option<String>>>(&bytes[..length], 1024).is_err());
        }
        let mut trailing = bytes;
        trailing.push(0);
        assert!(restored::<Vec<Option<String>>>(&trailing, 1024).is_err());
    }

    #[test]
    fn reader_writer_budgets_and_depth_limits_are_symmetric() {
        let mut writer = CheckpointWriter::new(2);
        writer.write(&[1, 2]).unwrap();
        assert!(writer.write(&[3]).is_err());
        assert_eq!(writer.finish(), vec![1, 2]);
        let mut value = Value::Null;
        for _ in 0..64 {
            value = Value::Array(vec![value]);
        }
        let bytes = encoded(&value);
        assert_eq!(
            encoded(&restored::<Value<'static>>(&bytes, 1 << 20).unwrap()),
            bytes
        );
        for _ in 0..200 {
            value = Value::Array(vec![value]);
        }
        assert!(value
            .encode_checkpoint(&mut CheckpointWriter::new(1 << 20))
            .is_err());
        // Forge deep input, with one child per array, rather than rely on encode.
        let mut deep = Vec::new();
        for _ in 0..264 {
            deep.push(15);
            deep.extend_from_slice(&1u64.to_le_bytes());
            deep.push(0);
        }
        deep.push(0);
        assert!(restored::<Value<'static>>(&deep, 1 << 20).is_err());
        let mut reader = CheckpointReader::new(&[], 0);
        assert!(reader.charge(1).is_err());
        assert!(reader.nested::<()>(|_| Err("failure".into())).is_err());
        assert!(reader.nested(|_| Ok(())).is_ok());
    }

    #[test]
    fn retained_invocation_handles_and_borrowed_cells_cannot_checkpoint() {
        let value = Value::Opaque {
            scope: 1,
            handle: 2,
            type_identity: "test".into(),
        };
        assert!(value
            .encode_checkpoint(&mut CheckpointWriter::new(1024))
            .is_err());
        let cell = std::cell::RefCell::new("value".to_owned());
        let _borrow = cell.borrow_mut();
        assert!(cell
            .encode_checkpoint(&mut CheckpointWriter::new(1024))
            .is_err());
    }
}
