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

//! Retained aggregate state accounting, separate from transport buffers and the
//! Store's linear capacity charge. All sizes include reserved capacity, not just
//! live element lengths. Shared/global allocations require an explicit ownership
//! policy in a custom implementation; counting every Arc is not correct.
use crate::{CustomValue, Value};
use std::mem::size_of;

type Result<T> = std::result::Result<T, String>;

/// Owned bytes below the inline state. Derive this for structs of supported
/// owned fields, or implement it for custom allocators/shared ownership. Inline
/// size is added by the adapter exactly once. Compaction must preserve SQL state.
pub trait AggregateState {
    fn heap_bytes(&self) -> Result<u64>;
    fn compact(&mut self) -> Result<()>;
}

#[doc(hidden)]
pub fn add_bytes(left: u64, right: u64) -> Result<u64> {
    left.checked_add(right)
        .ok_or_else(|| "aggregate state size overflow".into())
}

pub fn state_bytes<T: AggregateState>(state: &T) -> Result<u64> {
    add_bytes(size_of::<T>() as u64, state.heap_bytes()?)
}

macro_rules! fixed {
    ($($ty:ty),*) => {$ (
        impl AggregateState for $ty {
            fn heap_bytes(&self) -> Result<u64> { Ok(0) }
            fn compact(&mut self) -> Result<()> { Ok(()) }
        }
    )*};
}
fixed!(
    (),
    bool,
    i8,
    i16,
    i32,
    i64,
    i128,
    isize,
    u8,
    u16,
    u32,
    u64,
    u128,
    usize,
    f32,
    f64,
    char,
    crate::Date32,
    crate::Timestamp,
    crate::Decimal
);

impl AggregateState for String {
    fn heap_bytes(&self) -> Result<u64> {
        Ok(self.capacity() as u64)
    }
    fn compact(&mut self) -> Result<()> {
        self.shrink_to_fit();
        Ok(())
    }
}
impl<T: AggregateState> AggregateState for Vec<T> {
    fn heap_bytes(&self) -> Result<u64> {
        let bytes = (self.capacity() as u64)
            .checked_mul(size_of::<T>() as u64)
            .ok_or("aggregate state size overflow")?;
        self.iter()
            .try_fold(bytes, |n, v| add_bytes(n, v.heap_bytes()?))
    }
    fn compact(&mut self) -> Result<()> {
        for value in self.iter_mut() {
            value.compact()?;
        }
        self.shrink_to_fit();
        Ok(())
    }
}
impl<T: AggregateState> AggregateState for Box<T> {
    fn heap_bytes(&self) -> Result<u64> {
        state_bytes(self.as_ref())
    }
    fn compact(&mut self) -> Result<()> {
        self.as_mut().compact()
    }
}
impl<T: AggregateState> AggregateState for Option<T> {
    fn heap_bytes(&self) -> Result<u64> {
        self.as_ref().map_or(Ok(0), AggregateState::heap_bytes)
    }
    fn compact(&mut self) -> Result<()> {
        self.as_mut().map_or(Ok(()), AggregateState::compact)
    }
}
impl<T: AggregateState> AggregateState for std::cell::RefCell<T> {
    fn heap_bytes(&self) -> Result<u64> {
        self.try_borrow()
            .map_err(|_| "aggregate State is borrowed during accounting".to_owned())?
            .heap_bytes()
    }
    fn compact(&mut self) -> Result<()> {
        self.get_mut().compact()
    }
}
impl<T: AggregateState, const N: usize> AggregateState for [T; N] {
    fn heap_bytes(&self) -> Result<u64> {
        self.iter()
            .try_fold(0, |n, v| add_bytes(n, v.heap_bytes()?))
    }
    fn compact(&mut self) -> Result<()> {
        for value in self.iter_mut() {
            value.compact()?;
        }
        Ok(())
    }
}
macro_rules! tuple {
    ($($ty:ident:$index:tt),+) => {
        impl<$($ty: AggregateState),+> AggregateState for ($($ty,)+) {
            fn heap_bytes(&self) -> Result<u64> {
                let mut bytes = 0;
                $(bytes = add_bytes(bytes, self.$index.heap_bytes()?)?;)+
                Ok(bytes)
            }
            fn compact(&mut self) -> Result<()> {
                $(self.$index.compact()?;)+
                Ok(())
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

impl<T: AggregateState> AggregateState for CustomValue<T> {
    fn heap_bytes(&self) -> Result<u64> {
        [
            self.codec_id.heap_bytes()?,
            self.type_identity.heap_bytes()?,
            self.payload.heap_bytes()?,
        ]
        .into_iter()
        .try_fold(0, add_bytes)
    }
    fn compact(&mut self) -> Result<()> {
        self.codec_id.compact()?;
        self.type_identity.compact()?;
        self.payload.compact()
    }
}
impl AggregateState for Value<'static> {
    fn heap_bytes(&self) -> Result<u64> {
        match self {
            Self::Varchar(v) => v.heap_bytes(),
            Self::VarcharBytes(v) | Self::Varbinary(v) => v.heap_bytes(),
            Self::Array(v) | Self::Row(v) => v.heap_bytes(),
            Self::Map(v) => v.heap_bytes(),
            Self::Custom(v) => v.heap_bytes(),
            Self::Opaque { .. } => {
                Err("aggregate state cannot retain invocation OPAQUE handles".into())
            }
            Self::Borrowed(_) => Err("aggregate state must materialize borrowed values".into()),
            _ => Ok(0),
        }
    }
    fn compact(&mut self) -> Result<()> {
        match self {
            Self::Varchar(v) => v.compact(),
            Self::VarcharBytes(v) | Self::Varbinary(v) => v.compact(),
            Self::Array(v) | Self::Row(v) => v.compact(),
            Self::Map(v) => v.compact(),
            Self::Custom(v) => v.compact(),
            Self::Opaque { .. } | Self::Borrowed(_) => self.heap_bytes().map(|_| ()),
            _ => Ok(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[derive(crate::AggregateState)]
    struct Owned {
        text: String,
        nested: Vec<Option<Box<String>>>,
    }
    #[test]
    fn reserved_nested_capacity_is_counted_and_compaction_preserves_values() {
        let mut state = Owned {
            text: String::with_capacity(128),
            nested: Vec::with_capacity(16),
        };
        state.text.push_str("hello");
        let mut child = String::with_capacity(64);
        child.push_str("world");
        state.nested.push(Some(Box::new(child)));
        let expected = size_of::<Owned>() as u64
            + 128
            + 16 * size_of::<Option<Box<String>>>() as u64
            + size_of::<String>() as u64
            + 64;
        assert_eq!(state_bytes(&state).unwrap(), expected);
        state.compact().unwrap();
        assert_eq!(state.text, "hello");
        assert_eq!(state.nested[0].as_ref().unwrap().as_str(), "world");
        assert!(state_bytes(&state).unwrap() < expected);
    }
    #[test]
    fn nested_value_storage_counts_capacity_without_double_counting_inline_children() {
        let mut children = Vec::with_capacity(8);
        children.push(Value::Varbinary(Vec::with_capacity(100)));
        let value = Value::Array(children);
        assert_eq!(
            state_bytes(&value).unwrap(),
            size_of::<Value>() as u64 * 9 + 100
        );
        assert!(add_bytes(u64::MAX, 1).is_err());
    }
}
