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

//! Velox comparison and 64-bit hash semantics for standard SQL value types.
//! Algorithms follow ComplexVector.cpp, FloatingPointUtil.h and BitUtil.cpp.
use crate::{Generic, Value};
use std::cmp::Ordering;
type Result<T> = std::result::Result<T, String>;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NullHandling {
    NullAsValue,
    NullAsIndeterminate,
}
#[derive(Clone, Copy, Debug)]
pub struct CompareFlags {
    pub nulls_first: bool,
    pub ascending: bool,
    pub equals_only: bool,
    pub null_handling: NullHandling,
}
impl Default for CompareFlags {
    fn default() -> Self {
        Self {
            nulls_first: true,
            ascending: true,
            equals_only: false,
            null_handling: NullHandling::NullAsValue,
        }
    }
}
impl CompareFlags {
    pub fn equality(null_handling: NullHandling) -> Self {
        Self {
            equals_only: true,
            null_handling,
            ..Self::default()
        }
    }
}
impl Generic<'_> {
    pub fn compare(&self, other: &Generic<'_>, flags: CompareFlags) -> Result<Option<Ordering>> {
        if !crate::value::same_sql_type(self.data_type(), other.data_type()) {
            return Err("cannot compare different SQL types".into());
        }
        compare_generic(*self, *other, flags)
    }
    pub fn equals(&self, other: &Generic<'_>) -> Result<bool> {
        Ok(
            self.compare(other, CompareFlags::equality(NullHandling::NullAsValue))?
                == Some(Ordering::Equal),
        )
    }
    pub fn hash(&self) -> Result<u64> {
        hash_generic(*self)
    }
}
// The root type check is performed once by Generic::compare. Children are read
// only when reached, without constructing owned strings or nested Value trees.
fn compare_generic(
    lhs: Generic<'_>,
    rhs: Generic<'_>,
    flags: CompareFlags,
) -> Result<Option<Ordering>> {
    use arrow_schema::DataType;
    let opaque = |v: Generic<'_>| {
        !v.is_null() && crate::custom::marker(v.data_type()) == Some("opaque_handle_v1")
    };
    if opaque(lhs) || opaque(rhs) {
        return Err("OPAQUE handles have no SQL comparison semantics".into());
    }
    if !lhs.is_null()
        && matches!(lhs.data_type(), DataType::Map(..))
        && flags.null_handling == NullHandling::NullAsIndeterminate
        && !flags.equals_only
    {
        return Err("map is not an orderable SQL type".into());
    }
    if lhs.is_null() || rhs.is_null() {
        let a = if lhs.is_null() {
            Value::Null
        } else {
            Value::Boolean(false)
        };
        let b = if rhs.is_null() {
            Value::Null
        } else {
            Value::Boolean(false)
        };
        return compare(&a, &b, flags);
    }
    if crate::custom::marker(lhs.data_type()) == Some("custom_codec_v1") {
        let a = lhs.get::<crate::CustomValue<&[u8]>>()?;
        let b = rhs.get::<crate::CustomValue<&[u8]>>()?;
        return (crate::custom::semantics(&a)?.compare)(a.payload, b.payload, flags);
    }
    if crate::is_varchar_wire_type(lhs.data_type()) || lhs.data_type() == &DataType::Utf8 {
        return Ok(ordered(
            lhs.get::<crate::VarcharBytes<&[u8]>>()?
                .0
                .cmp(rhs.get::<crate::VarcharBytes<&[u8]>>()?.0),
            flags,
        ));
    }
    if lhs.data_type() == &DataType::Binary {
        return Ok(ordered(lhs.get::<&[u8]>()?.cmp(rhs.get::<&[u8]>()?), flags));
    }
    match lhs.data_type() {
        DataType::List(_) => sequence_by(lhs.list_len()?, rhs.list_len()?, flags, |i| {
            compare_generic(lhs.element(i)?, rhs.element(i)?, flags)
        }),
        DataType::Map(..) => {
            let (alen, blen) = (lhs.map_len()?, rhs.map_len()?);
            if flags.equals_only && alen != blen {
                return Ok(Some(Ordering::Greater));
            }
            let a = sorted_generic_entries(lhs)?;
            let b = sorted_generic_entries(rhs)?;
            let keys = sequence_by(alen, blen, flags, |i| {
                compare_generic(lhs.map_entry(a[i])?.0, rhs.map_entry(b[i])?.0, flags)
            })?;
            if keys != Some(Ordering::Equal) {
                return Ok(keys);
            }
            sequence_by(alen, blen, flags, |i| {
                compare_generic(lhs.map_entry(a[i])?.1, rhs.map_entry(b[i])?.1, flags)
            })
        }
        DataType::Struct(_)
            if !crate::Timestamp::is_wire_type(lhs.data_type())
                && !crate::is_hugeint_wire_type(lhs.data_type()) =>
        {
            sequence_by(lhs.row_len()?, rhs.row_len()?, flags, |i| {
                compare_generic(lhs.field_at(i)?, rhs.field_at(i)?, flags)
            })
        }
        // Remaining scalar variants contain no heap-backed payloads.
        _ => compare(&lhs.materialize()?, &rhs.materialize()?, flags),
    }
}
fn sorted_generic_entries(map: Generic<'_>) -> Result<Vec<usize>> {
    sorted_indices(map.map_len()?, |a, b| {
        compare_generic(
            map.map_entry(a)?.0,
            map.map_entry(b)?.0,
            CompareFlags::default(),
        )
    })
}
fn hash_generic(value: Generic<'_>) -> Result<u64> {
    use arrow_schema::DataType;
    if value.is_null() {
        return Ok(1);
    }
    match crate::custom::marker(value.data_type()) {
        Some("opaque_handle_v1") => return Err("OPAQUE handles have no SQL hash semantics".into()),
        Some("custom_codec_v1") => {
            let v = value.get::<crate::CustomValue<&[u8]>>()?;
            return (crate::custom::semantics(&v)?.hash)(v.payload);
        }
        _ => {}
    }
    if crate::is_varchar_wire_type(value.data_type()) || value.data_type() == &DataType::Utf8 {
        return Ok(hash_bytes(value.get::<crate::VarcharBytes<&[u8]>>()?.0));
    }
    if value.data_type() == &DataType::Binary {
        return Ok(hash_bytes(value.get::<&[u8]>()?));
    }
    Ok(match value.data_type() {
        DataType::List(_) => {
            let mut hash = 1;
            for i in 0..value.list_len()? {
                hash = mix(hash, hash_generic(value.element(i)?)?);
            }
            hash
        }
        DataType::Map(..) => {
            let mut hash = 1_u64;
            for i in 0..value.map_len()? {
                let (key, item) = value.map_entry(i)?;
                hash = map_hash_combine(hash, mix(hash_generic(key)?, hash_generic(item)?));
            }
            hash
        }
        DataType::Struct(_)
            if !crate::Timestamp::is_wire_type(value.data_type())
                && !crate::is_hugeint_wire_type(value.data_type()) =>
        {
            let len = value.row_len()?;
            let mut hash = if len == 0 {
                1
            } else {
                hash_generic(value.field_at(0)?)?
            };
            for i in 1..len {
                hash = mix(hash, hash_generic(value.field_at(i)?)?);
            }
            hash
        }
        _ => return hash(&value.materialize()?),
    })
}
impl Value<'_> {
    pub fn compare(&self, other: &Value<'_>, flags: CompareFlags) -> Result<Option<Ordering>> {
        compare(self, other, flags)
    }
    pub fn equals(&self, other: &Value<'_>) -> Result<bool> {
        Ok(
            self.compare(other, CompareFlags::equality(NullHandling::NullAsValue))?
                == Some(Ordering::Equal),
        )
    }
    pub fn hash(&self) -> Result<u64> {
        hash(self)
    }
}
fn ordered(value: Ordering, flags: CompareFlags) -> Option<Ordering> {
    Some(if flags.ascending {
        value
    } else {
        value.reverse()
    })
}
fn floating(lhs: f64, rhs: f64) -> Ordering {
    match (lhs.is_nan(), rhs.is_nan()) {
        (true, true) => Ordering::Equal,
        (true, false) => Ordering::Greater,
        (false, true) => Ordering::Less,
        _ => lhs.partial_cmp(&rhs).unwrap(),
    }
}
fn sequence_by(
    alen: usize,
    blen: usize,
    flags: CompareFlags,
    mut element: impl FnMut(usize) -> Result<Option<Ordering>>,
) -> Result<Option<Ordering>> {
    if flags.equals_only && alen != blen {
        return Ok(Some(Ordering::Greater));
    }
    let mut indeterminate = false;
    for i in 0..alen.min(blen) {
        match element(i)? {
            None => indeterminate = true,
            Some(Ordering::Equal) => {}
            result => return Ok(result),
        }
    }
    if indeterminate {
        Ok(None)
    } else {
        Ok(ordered(alen.cmp(&blen), flags))
    }
}
fn sequence(lhs: &[Value<'_>], rhs: &[Value<'_>], flags: CompareFlags) -> Result<Option<Ordering>> {
    sequence_by(lhs.len(), rhs.len(), flags, |i| {
        compare(&lhs[i], &rhs[i], flags)
    })
}
fn sorted_indices(
    len: usize,
    mut key_compare: impl FnMut(usize, usize) -> Result<Option<Ordering>>,
) -> Result<Vec<usize>> {
    let mut indices: Vec<usize> = (0..len).collect();
    let mut error = None;
    indices.sort_by(|a, b| match key_compare(*a, *b) {
        Ok(Some(order)) => order,
        Ok(None) => {
            error = Some("indeterminate map key ordering".into());
            Ordering::Equal
        }
        Err(e) => {
            error = Some(e);
            Ordering::Equal
        }
    });
    if let Some(e) = error {
        return Err(e);
    }
    Ok(indices)
}
fn sorted_entries(entries: &[(Value<'_>, Value<'_>)]) -> Result<Vec<usize>> {
    sorted_indices(entries.len(), |a, b| {
        compare(&entries[a].0, &entries[b].0, CompareFlags::default())
    })
}

fn compare(lhs: &Value<'_>, rhs: &Value<'_>, flags: CompareFlags) -> Result<Option<Ordering>> {
    match (lhs, rhs) {
        (Value::Borrowed(a), Value::Borrowed(b)) => return a.compare(b, flags),
        (Value::Borrowed(a), b) => return compare_mixed(*a, b, flags, true),
        (a, Value::Borrowed(b)) => return compare_mixed(*b, a, flags, false),
        _ => {}
    }
    if let (Value::Custom(a), Value::Custom(b)) = (lhs, rhs) {
        if a.codec_id != b.codec_id || a.version != b.version || a.type_identity != b.type_identity
        {
            return Err("cannot compare different custom types".into());
        }
        return (crate::custom::semantics(a)?.compare)(&a.payload, &b.payload, flags);
    }
    if matches!(lhs, Value::Opaque { .. }) || matches!(rhs, Value::Opaque { .. }) {
        return Err("OPAQUE handles have no SQL comparison semantics".into());
    }

    use Value::*;
    if matches!(lhs, Map(_))
        && flags.null_handling == NullHandling::NullAsIndeterminate
        && !flags.equals_only
    {
        return Err("map is not an orderable SQL type".into());
    }
    if lhs.is_null() || rhs.is_null() {
        if flags.null_handling == NullHandling::NullAsIndeterminate {
            return if flags.equals_only {
                Ok(None)
            } else {
                Err("cannot order a null SQL value".into())
            };
        }
        let result = match (lhs.is_null(), rhs.is_null()) {
            (true, true) => Ordering::Equal,
            (true, false) => {
                if flags.nulls_first {
                    Ordering::Less
                } else {
                    Ordering::Greater
                }
            }
            _ => {
                if flags.nulls_first {
                    Ordering::Greater
                } else {
                    Ordering::Less
                }
            }
        };
        // Velox compareNulls uses nullsFirst independently of ascending.
        return Ok(Some(result));
    }
    let result = match (lhs, rhs) {
        (Boolean(a), Boolean(b)) => a.cmp(b),
        (TinyInt(a), TinyInt(b)) => a.cmp(b),
        (SmallInt(a), SmallInt(b)) => a.cmp(b),
        (Integer(a), Integer(b)) => a.cmp(b),
        (BigInt(a), BigInt(b)) => a.cmp(b),
        (HugeInt(a), HugeInt(b)) => a.cmp(b),
        (Real(a), Real(b)) => floating(*a as f64, *b as f64),
        (Double(a), Double(b)) => floating(*a, *b),
        (Varchar(a), Varchar(b)) => a.as_bytes().cmp(b.as_bytes()),
        (VarcharBytes(a), VarcharBytes(b)) => a.cmp(b),
        (Varchar(a), VarcharBytes(b)) => a.as_bytes().cmp(b),
        (VarcharBytes(a), Varchar(b)) => a.as_slice().cmp(b.as_bytes()),
        (Varbinary(a), Varbinary(b)) => a.cmp(b),
        (Date(a), Date(b)) => a.0.cmp(&b.0),
        (Timestamp(a), Timestamp(b)) => a.0.cmp(&b.0),
        (Decimal(a), Decimal(b)) if a.precision == b.precision && a.scale == b.scale => {
            a.value.cmp(&b.value)
        }
        (Array(a), Array(b)) | (Row(a), Row(b)) => return sequence(a, b, flags),
        (Map(a), Map(b)) => {
            if flags.equals_only && a.len() != b.len() {
                return Ok(Some(Ordering::Greater));
            }
            let ai = sorted_entries(a)?;
            let bi = sorted_entries(b)?;
            let keys = sequence_by(a.len(), b.len(), flags, |i| {
                compare(&a[ai[i]].0, &b[bi[i]].0, flags)
            })?;
            if keys != Some(Ordering::Equal) {
                return Ok(keys);
            }
            return sequence_by(a.len(), b.len(), flags, |i| {
                compare(&a[ai[i]].1, &b[bi[i]].1, flags)
            });
        }
        _ => return Err("cannot compare different SQL value types".into()),
    };
    Ok(ordered(result, flags))
}

// Aggregate State owns retained values, whereas update/merge inputs are borrowed.
// Comparing the two must not first copy every string or decode a nested tail.
// Keep the operand direction: equals-only length mismatches and NULL ordering
// cannot be implemented by reversing an already-computed comparison result.
fn compare_mixed(
    borrowed: Generic<'_>,
    owned: &Value<'_>,
    flags: CompareFlags,
    borrowed_left: bool,
) -> Result<Option<Ordering>> {
    use arrow_schema::DataType;
    use Value::*;
    let borrowed_null = borrowed.is_null();
    if matches!(owned, Opaque { .. })
        || (!borrowed_null
            && crate::custom::marker(borrowed.data_type()) == Some("opaque_handle_v1"))
    {
        return Err("OPAQUE handles have no SQL comparison semantics".into());
    }
    let lhs_is_map = if borrowed_left {
        !borrowed_null && matches!(borrowed.data_type(), DataType::Map(..))
    } else {
        matches!(owned, Map(_))
    };
    if lhs_is_map && flags.null_handling == NullHandling::NullAsIndeterminate && !flags.equals_only
    {
        return Err("map is not an orderable SQL type".into());
    }
    if borrowed_null || owned.is_null() {
        let a = if borrowed_null { Null } else { Boolean(false) };
        let b = if owned.is_null() {
            Null
        } else {
            Boolean(false)
        };
        return if borrowed_left {
            compare(&a, &b, flags)
        } else {
            compare(&b, &a, flags)
        };
    }
    let scalar_order = |result: Ordering| {
        Ok(ordered(
            if borrowed_left {
                result
            } else {
                result.reverse()
            },
            flags,
        ))
    };
    let mismatch = || Err("cannot compare different SQL value types".into());
    let child = |value: Generic<'_>, other: &Value<'_>| {
        if borrowed_left {
            compare(&Borrowed(value), other, flags)
        } else {
            compare(other, &Borrowed(value), flags)
        }
    };
    macro_rules! scalar {
        ($variant:ident, $ty:ty) => {
            if let $variant(other) = owned {
                scalar_order(borrowed.get::<$ty>()?.cmp(other))
            } else {
                mismatch()
            }
        };
    }
    if crate::is_varchar_wire_type(borrowed.data_type()) || borrowed.data_type() == &DataType::Utf8
    {
        let other = match owned {
            Varchar(s) => s.as_bytes(),
            VarcharBytes(bytes) => bytes.as_slice(),
            _ => return mismatch(),
        };
        return scalar_order(borrowed.get::<crate::VarcharBytes<&[u8]>>()?.0.cmp(other));
    }
    if crate::is_hugeint_wire_type(borrowed.data_type()) {
        return scalar!(HugeInt, i128);
    }
    if crate::Timestamp::is_wire_type(borrowed.data_type()) {
        return if let Timestamp(other) = owned {
            scalar_order(borrowed.get::<crate::Timestamp>()?.0.cmp(&other.0))
        } else {
            mismatch()
        };
    }
    // Custom codecs retain their registered semantics and identity checks.
    if crate::custom::marker(borrowed.data_type()) == Some("custom_codec_v1") {
        let value = borrowed.materialize()?;
        return if borrowed_left {
            compare(&value, owned, flags)
        } else {
            compare(owned, &value, flags)
        };
    }
    match borrowed.data_type() {
        DataType::Boolean => scalar!(Boolean, bool),
        DataType::Int8 => scalar!(TinyInt, i8),
        DataType::Int16 => scalar!(SmallInt, i16),
        DataType::Int32 => scalar!(Integer, i32),
        DataType::Int64 => scalar!(BigInt, i64),
        DataType::Float32 => match owned {
            Real(other) => scalar_order(floating(borrowed.get::<f32>()? as f64, *other as f64)),
            _ => mismatch(),
        },
        DataType::Float64 => match owned {
            Double(other) => scalar_order(floating(borrowed.get::<f64>()?, *other)),
            _ => mismatch(),
        },
        DataType::Binary => match owned {
            Varbinary(other) => scalar_order(borrowed.get::<&[u8]>()?.cmp(other)),
            _ => mismatch(),
        },
        DataType::Date32 => match owned {
            Date(other) => scalar_order(borrowed.get::<crate::Date32>()?.0.cmp(&other.0)),
            _ => mismatch(),
        },
        DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, None) => match owned {
            Timestamp(other) => scalar_order(borrowed.get::<crate::Timestamp>()?.0.cmp(&other.0)),
            _ => mismatch(),
        },
        DataType::Decimal128(precision, scale) => match owned {
            Decimal(other) if precision == &other.precision && scale == &other.scale => {
                scalar_order(borrowed.get::<crate::Decimal>()?.value.cmp(&other.value))
            }
            _ => mismatch(),
        },
        DataType::List(_) => match owned {
            Array(items) => {
                let len = borrowed.list_len()?;
                let (alen, blen) = if borrowed_left {
                    (len, items.len())
                } else {
                    (items.len(), len)
                };
                sequence_by(alen, blen, flags, |i| {
                    child(borrowed.element(i)?, &items[i])
                })
            }
            _ => mismatch(),
        },
        DataType::Struct(_) => match owned {
            Row(items) => {
                let len = borrowed.row_len()?;
                let (alen, blen) = if borrowed_left {
                    (len, items.len())
                } else {
                    (items.len(), len)
                };
                sequence_by(alen, blen, flags, |i| {
                    child(borrowed.field_at(i)?, &items[i])
                })
            }
            _ => mismatch(),
        },
        DataType::Map(..) => match owned {
            Map(items) => {
                let len = borrowed.map_len()?;
                if flags.equals_only && len != items.len() {
                    return Ok(Some(Ordering::Greater));
                }
                let bi = sorted_generic_entries(borrowed)?;
                let oi = sorted_entries(items)?;
                let (alen, blen) = if borrowed_left {
                    (len, items.len())
                } else {
                    (items.len(), len)
                };
                let keys = sequence_by(alen, blen, flags, |i| {
                    child(borrowed.map_entry(bi[i])?.0, &items[oi[i]].0)
                })?;
                if keys != Some(Ordering::Equal) {
                    return Ok(keys);
                }
                sequence_by(alen, blen, flags, |i| {
                    child(borrowed.map_entry(bi[i])?.1, &items[oi[i]].1)
                })
            }
            _ => mismatch(),
        },
        ty => Err(format!("unsupported SQL value type {ty:?}")),
    }
}
fn mix(upper: u64, lower: u64) -> u64 {
    let mul = 0x9ddfea08eb382d69_u64;
    let mut a = (lower ^ upper).wrapping_mul(mul);
    a ^= a >> 47;
    let mut b = (upper ^ a).wrapping_mul(mul);
    b ^= b >> 47;
    b.wrapping_mul(mul)
}
fn twang(mut k: u64) -> u64 {
    k = (!k).wrapping_add(k << 21);
    k ^= k >> 24;
    k = k.wrapping_add(k << 3).wrapping_add(k << 8);
    k ^= k >> 14;
    k = k.wrapping_add(k << 2).wrapping_add(k << 4);
    k ^= k >> 28;
    k.wrapping_add(k << 31)
}
fn jenkins(mut k: u32) -> u64 {
    k = k.wrapping_add(k << 12);
    k ^= k >> 22;
    k = k.wrapping_add(k << 4);
    k ^= k >> 9;
    k = k.wrapping_add(k << 10);
    k ^= k >> 2;
    k = k.wrapping_add(k << 7);
    k.wrapping_add(k << 12) as u64
}
fn crc32_u64(seed: u64, word: u64) -> u64 {
    let mut crc = seed as u32;
    for byte in word.to_le_bytes() {
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ if crc & 1 != 0 { 0x82f63b78 } else { 0 };
        }
    }
    crc as u64
}
fn hash_bytes(data: &[u8]) -> u64 {
    let word = |data: &[u8]| {
        let mut bytes = [0u8; 8];
        bytes[..data.len()].copy_from_slice(data);
        u64::from_le_bytes(bytes)
    };
    if data.len() < 8 {
        let v = word(data);
        return crc32_u64(1, v) | (crc32_u64(1, v >> 32) << 32);
    }
    let (mut a0, mut a1, mut a2) = (1_u64, 1_u64 << 32, 0_u64);
    let mut data = data;
    while data.len() >= 24 {
        a0 = crc32_u64(a0, word(&data[..8]));
        a1 = crc32_u64(a1, word(&data[8..16]));
        a2 = crc32_u64(a2, word(&data[16..24]));
        data = &data[24..];
    }
    if data.len() > 16 {
        a0 = crc32_u64(a0, word(&data[..8]));
        a1 = crc32_u64(a1, word(&data[8..16]));
        a2 = crc32_u64(a2, word(&data[16..]));
    } else if data.len() > 8 {
        a0 = crc32_u64(a0, word(&data[..8]));
        a1 = crc32_u64(a1, word(&data[8..]));
    } else if !data.is_empty() {
        a0 = crc32_u64(a0, word(data));
    }
    a0 ^ a1.wrapping_mul(0x9ddfea08eb382d69) ^ a2.wrapping_mul(0x9ddfea08eb382d69)
}
fn map_hash_combine(hash: u64, next: u64) -> u64 {
    3860031_u64
        .wrapping_add(hash.wrapping_add(next).wrapping_mul(2779))
        .wrapping_add(hash.wrapping_mul(next).wrapping_mul(2))
}
fn hash(value: &Value<'_>) -> Result<u64> {
    use Value::*;
    Ok(match value {
        Null => 1,
        Boolean(v) => {
            if *v {
                u64::MAX
            } else {
                0
            }
        }
        TinyInt(v) => jenkins(*v as i32 as u32),
        SmallInt(v) => jenkins(*v as i32 as u32),
        Integer(v) => jenkins(*v as u32),
        BigInt(v) => twang(*v as u64),
        HugeInt(v) => mix((*v as u128 >> 64) as u64, *v as u64),
        Real(v) => {
            if *v == 0.0 {
                0
            } else {
                twang(if v.is_nan() {
                    f32::NAN.to_bits() as u64
                } else {
                    v.to_bits() as u64
                })
            }
        }
        Double(v) => {
            if *v == 0.0 {
                0
            } else {
                twang(if v.is_nan() {
                    f64::NAN.to_bits()
                } else {
                    v.to_bits()
                })
            }
        }
        Varchar(v) => hash_bytes(v.as_bytes()),
        VarcharBytes(v) => hash_bytes(v),
        Varbinary(v) => hash_bytes(v),
        Date(v) => jenkins(v.0 as u32),
        Timestamp(v) => mix(
            v.0.div_euclid(1_000_000_000) as u64,
            v.0.rem_euclid(1_000_000_000) as u64,
        ),
        Decimal(v) => {
            if v.precision <= 18 {
                twang(v.value as u64)
            } else {
                mix((v.value as u128 >> 64) as u64, v.value as u64)
            }
        }
        Array(items) => {
            let mut hash = 1;
            for item in items {
                hash = mix(hash, self::hash(item)?);
            }
            hash
        }
        Row(items) => {
            let mut iter = items.iter();
            let mut hash = match iter.next() {
                Some(item) => self::hash(item)?,
                None => 1,
            };
            for item in iter {
                hash = mix(hash, self::hash(item)?);
            }
            hash
        }
        Map(items) => {
            let mut hash = 1_u64;
            for (k, v) in items {
                let next = mix(self::hash(k)?, self::hash(v)?);
                hash = map_hash_combine(hash, next);
            }
            hash
        }
        Borrowed(v) => return v.hash(),
        Custom(v) => return (crate::custom::semantics(v)?.hash)(&v.payload),
        Opaque { .. } => return Err("OPAQUE handles have no SQL hash semantics".into()),
    })
}

/// An owned, validated SQL key for Rust HashMap / HashSet. NULL and NaN use
/// Velox null-as-value equality. Keep fallible value decoding outside Hash/Eq.
#[derive(Clone)]
pub struct SqlKey {
    data_type: arrow_schema::DataType,
    value: Value<'static>,
    hash: u64,
}
impl Generic<'_> {
    pub fn sql_key(&self) -> Result<SqlKey> {
        let value = self.materialize()?;
        // Reject unsupported comparison domains when creating the key.
        value.equals(&value)?;
        let hash = value.hash()?;
        Ok(SqlKey {
            data_type: self.data_type().clone(),
            value,
            hash,
        })
    }
}
impl SqlKey {
    pub fn value(&self) -> &Value<'static> {
        &self.value
    }
    pub fn velox_hash(&self) -> u64 {
        self.hash
    }
}
impl PartialEq for SqlKey {
    fn eq(&self, other: &Self) -> bool {
        crate::value::same_sql_type(&self.data_type, &other.data_type)
            && self.value.equals(&other.value).unwrap_or(false)
    }
}
impl Eq for SqlKey {}
impl std::hash::Hash for SqlKey {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        state.write_u64(self.hash);
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::value::build_array;
    use arrow_array::{Array, ArrayRef, Int64Array, ListArray, StructArray};
    use arrow_buffer::{OffsetBuffer, ScalarBuffer};
    use arrow_schema::DataType;
    use std::{
        alloc::{GlobalAlloc, Layout, System},
        cell::Cell,
        sync::Arc,
    };

    // Count allocations only on the calling test thread. Other parallel tests
    // and the test harness cannot affect the result.
    struct CountingAllocator;
    thread_local! { static ALLOCATIONS: Cell<Option<usize>> = const { Cell::new(None) }; }
    fn allocated() {
        let _ = ALLOCATIONS.try_with(|n| {
            if let Some(count) = n.get() {
                n.set(Some(count + 1));
            }
        });
    }
    unsafe impl GlobalAlloc for CountingAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            allocated();
            System.alloc(layout)
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            allocated();
            System.alloc_zeroed(layout)
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            System.dealloc(ptr, layout)
        }
        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
            allocated();
            System.realloc(ptr, layout, size)
        }
    }
    #[global_allocator]
    static ALLOCATOR: CountingAllocator = CountingAllocator;
    pub(crate) fn allocation_count<T>(f: impl FnOnce() -> T) -> (T, usize) {
        struct Reset;
        impl Drop for Reset {
            fn drop(&mut self) {
                ALLOCATIONS.with(|n| n.set(None));
            }
        }
        ALLOCATIONS.with(|n| {
            assert!(n.get().is_none());
            n.set(Some(0));
        });
        let _reset = Reset;
        let value = f();
        (value, ALLOCATIONS.with(|n| n.get().unwrap()))
    }

    #[test]
    fn mixed_scalar_comparison_matches_owned_semantics() {
        let cases = [
            ("boolean", vec![Value::Boolean(false), Value::Boolean(true)]),
            (
                "tinyint",
                vec![Value::TinyInt(i8::MIN), Value::TinyInt(i8::MAX)],
            ),
            (
                "smallint",
                vec![Value::SmallInt(i16::MIN), Value::SmallInt(i16::MAX)],
            ),
            (
                "integer",
                vec![Value::Integer(i32::MIN), Value::Integer(i32::MAX)],
            ),
            (
                "bigint",
                vec![Value::BigInt(i64::MIN), Value::BigInt(i64::MAX)],
            ),
            (
                "hugeint",
                vec![Value::HugeInt(i128::MIN), Value::HugeInt(i128::MAX)],
            ),
            (
                "real",
                vec![
                    Value::Real(-0.0),
                    Value::Real(0.0),
                    Value::Real(f32::NEG_INFINITY),
                    Value::Real(f32::NAN),
                ],
            ),
            (
                "double",
                vec![
                    Value::Double(-0.0),
                    Value::Double(0.0),
                    Value::Double(f64::INFINITY),
                    Value::Double(f64::NAN),
                ],
            ),
            (
                "varchar",
                vec![
                    Value::Varchar("a".into()),
                    Value::VarcharBytes(vec![0xff, 0x00]),
                ],
            ),
            (
                "varbinary",
                vec![Value::Varbinary(vec![]), Value::Varbinary(vec![0xff, 0x00])],
            ),
            (
                "date",
                vec![
                    Value::Date(crate::Date32(i32::MIN)),
                    Value::Date(crate::Date32(i32::MAX)),
                ],
            ),
            (
                "timestamp",
                vec![
                    Value::Timestamp(crate::Timestamp(-1)),
                    Value::Timestamp(crate::Timestamp(
                        (i64::MAX / 1000) as i128 * 1_000_000_000 + 999_999_999,
                    )),
                ],
            ),
            (
                "decimal(38,5)",
                vec![
                    Value::Decimal(crate::Decimal {
                        value: -12345,
                        precision: 38,
                        scale: 5,
                    }),
                    Value::Decimal(crate::Decimal {
                        value: 12345,
                        precision: 38,
                        scale: 5,
                    }),
                ],
            ),
        ];
        for (sql, mut values) in cases {
            values.push(Value::Null);
            let ty = crate::runtime::parse_row_arrow_type(sql).unwrap();
            let array = build_array(&ty, &values).unwrap();
            for (i, a) in values.iter().enumerate() {
                let av = Value::Borrowed(Generic::new(array.as_ref(), i).unwrap());
                for b in &values {
                    for nulls_first in [false, true] {
                        for ascending in [false, true] {
                            for equals_only in [false, true] {
                                for null_handling in
                                    [NullHandling::NullAsValue, NullHandling::NullAsIndeterminate]
                                {
                                    let flags = CompareFlags {
                                        nulls_first,
                                        ascending,
                                        equals_only,
                                        null_handling,
                                    };
                                    assert_eq!(
                                        av.compare(b, flags),
                                        a.compare(b, flags),
                                        "borrowed/owned {sql}: {flags:?}"
                                    );
                                    assert_eq!(
                                        b.compare(&av, flags),
                                        b.compare(a, flags),
                                        "owned/borrowed {sql}: {flags:?}"
                                    );
                                }
                            }
                        }
                    }
                }
                if !a.is_null() {
                    assert!(av
                        .compare(&Value::Row(vec![]), CompareFlags::default())
                        .is_err());
                    assert!(Value::Row(vec![])
                        .compare(&av, CompareFlags::default())
                        .is_err());
                }
            }
        }
        let ty = DataType::Int32;
        let array = build_array(&ty, &[Value::Integer(1)]).unwrap();
        let borrowed = Value::Borrowed(Generic::new(array.as_ref(), 0).unwrap());
        assert!(borrowed.equals(&Value::BigInt(1)).is_err());
        assert!(Value::BigInt(1).equals(&borrowed).is_err());
        let decimal = build_array(
            &DataType::Decimal128(38, 5),
            &[Value::Decimal(crate::Decimal {
                value: 1,
                precision: 38,
                scale: 5,
            })],
        )
        .unwrap();
        let borrowed = Value::Borrowed(Generic::new(decimal.as_ref(), 0).unwrap());
        assert!(borrowed
            .equals(&Value::Decimal(crate::Decimal {
                value: 1,
                precision: 38,
                scale: 4
            }))
            .is_err());
    }

    #[test]
    fn mixed_comparison_preserves_custom_identity_and_opaque_errors() {
        let ty = crate::custom::opaque_type("mixed_opaque");
        let opaque = Value::Opaque {
            scope: 1,
            handle: 2,
            type_identity: "mixed_opaque".into(),
        };
        let array = build_array(&ty, &[opaque.clone(), Value::Null]).unwrap();
        for index in 0..2 {
            let av = Value::Borrowed(Generic::new(array.as_ref(), index).unwrap());
            let owned = if index == 0 { &opaque } else { &Value::Null };
            for b in [&opaque, &Value::Null, &Value::BigInt(1)] {
                assert_eq!(
                    av.compare(b, CompareFlags::default()),
                    owned.compare(b, CompareFlags::default())
                );
                assert_eq!(
                    b.compare(&av, CompareFlags::default()),
                    b.compare(owned, CompareFlags::default())
                );
            }
        }
        crate::register_codec_semantics(
            "mixed_compare_codec",
            1,
            crate::CodecSemantics {
                compare: |a, b, flags| Ok(ordered(a.cmp(b), flags)),
                hash: |_| Ok(1),
            },
        )
        .unwrap();
        let ty = crate::custom::codec_type("mixed_compare_codec", 1, "mixed_custom");
        let value = Value::Custom(Box::new(crate::CustomValue::new(
            "mixed_compare_codec",
            1,
            "mixed_custom",
            vec![1, 2],
        )));
        let other = Value::Custom(Box::new(crate::CustomValue::new(
            "mixed_compare_codec",
            1,
            "other_custom",
            vec![1, 2],
        )));
        let array = build_array(&ty, &[value.clone()]).unwrap();
        let borrowed = Value::Borrowed(Generic::new(array.as_ref(), 0).unwrap());
        assert!(value.equals(&borrowed).unwrap());
        assert!(borrowed.equals(&value).unwrap());
        assert!(other.equals(&borrowed).is_err());
        assert!(borrowed.equals(&other).is_err());
    }

    #[test]
    fn borrowed_nested_hash_and_compare_do_not_allocate_value_trees() {
        let ty = crate::runtime::parse_row_arrow_type(
            "row(data array(varchar), number bigint, time timestamp)",
        )
        .unwrap();
        let value = Value::Row(vec![
            Value::Array(vec![
                Value::VarcharBytes(vec![0xff; 8192]),
                Value::Null,
                Value::Varchar("tail".into()),
            ]),
            Value::BigInt(42),
            Value::Timestamp(crate::Timestamp(-1)),
        ]);
        let array = build_array(&ty, &[value.clone(), value.clone()]).unwrap();
        let (a, b) = (
            Generic::new(array.as_ref(), 0).unwrap(),
            Generic::new(array.as_ref(), 1).unwrap(),
        );
        let (result, allocations) = allocation_count(|| {
            (
                a.hash(),
                a.compare(&b, CompareFlags::default()),
                Value::Borrowed(a).hash(),
            )
        });
        assert_eq!(result.0.unwrap(), value.hash().unwrap());
        assert_eq!(result.1.unwrap(), Some(Ordering::Equal));
        assert_eq!(result.2.unwrap(), value.hash().unwrap());
        assert_eq!(
            allocations, 0,
            "strings and nested values must stay borrowed"
        );
        let (result, allocations) =
            allocation_count(|| (value.hash(), value.compare(&value, CompareFlags::default())));
        assert_eq!(result.0.unwrap(), a.hash().unwrap());
        assert_eq!(result.1.unwrap(), Some(Ordering::Equal));
        assert_eq!(allocations, 0, "owned values must not be cloned either");
        let (results, allocations) = allocation_count(|| {
            (
                value.compare(&Value::Borrowed(a), CompareFlags::default()),
                Value::Borrowed(b).compare(&value, CompareFlags::default()),
            )
        });
        assert_eq!(results.0.unwrap(), Some(Ordering::Equal));
        assert_eq!(results.1.unwrap(), Some(Ordering::Equal));
        assert_eq!(
            allocations, 0,
            "aggregate State/input comparison must stay borrowed"
        );
    }

    #[test]
    fn comparison_does_not_decode_unvisited_elements() {
        let time = crate::runtime::parse_row_arrow_type("timestamp").unwrap();
        let DataType::Struct(fields) = &time else {
            panic!("timestamp wire struct")
        };
        let times: ArrayRef = Arc::new(StructArray::new(
            fields.clone(),
            vec![
                Arc::new(Int64Array::from(vec![0, 0, 1, 0])),
                // Invalid timestamp data in both tails proves they aren't decoded.
                Arc::new(Int64Array::from(vec![0, 1_000_000_000, 0, 1_000_000_000])),
            ],
            None,
        ));
        let list = ListArray::new(
            Arc::new(arrow_schema::Field::new("item", time, true)),
            OffsetBuffer::new(ScalarBuffer::from(vec![0_i32, 2, 4])),
            times,
            None,
        );
        let (a, b) = (
            Generic::new(&list, 0).unwrap(),
            Generic::new(&list, 1).unwrap(),
        );
        assert!(a.materialize().is_err());
        assert!(b.materialize().is_err());
        assert_eq!(
            a.compare(&b, CompareFlags::default()).unwrap(),
            Some(Ordering::Less)
        );
        assert!(!a.equals(&b).unwrap());
        let first = Value::Array(vec![Value::Timestamp(crate::Timestamp(0))]);
        assert_eq!(
            first
                .compare(&Value::Borrowed(b), CompareFlags::default())
                .unwrap(),
            Some(Ordering::Less)
        );
        assert_eq!(
            Value::Borrowed(b)
                .compare(&first, CompareFlags::default())
                .unwrap(),
            Some(Ordering::Greater)
        );
        // Hash visits every element, so it must still reject the invalid tail.
        assert!(a.hash().is_err());
    }

    #[test]
    fn borrowed_comparison_preserves_all_flags_and_map_order_independence() {
        let cases = [
            (
                "array(double)",
                vec![
                    Value::Null,
                    Value::Array(vec![]),
                    Value::Array(vec![Value::Double(-0.0), Value::Null]),
                    Value::Array(vec![Value::Double(0.0), Value::Double(f64::NAN)]),
                    Value::Array(vec![
                        Value::Double(0.0),
                        Value::Double(f64::from_bits(0x7ff8000000000042)),
                    ]),
                ],
            ),
            (
                "map(array(bigint),array(varchar))",
                vec![
                    Value::Null,
                    Value::Map(vec![]),
                    Value::Map(vec![
                        (
                            Value::Array(vec![Value::BigInt(2)]),
                            Value::Array(vec![Value::Null]),
                        ),
                        (
                            Value::Array(vec![Value::BigInt(1)]),
                            Value::Array(vec![Value::Varchar("a".into())]),
                        ),
                    ]),
                    Value::Map(vec![
                        (
                            Value::Array(vec![Value::BigInt(1)]),
                            Value::Array(vec![Value::Varchar("a".into())]),
                        ),
                        (
                            Value::Array(vec![Value::BigInt(2)]),
                            Value::Array(vec![Value::Null]),
                        ),
                    ]),
                ],
            ),
            (
                "row(a array(bigint), b varchar)",
                vec![
                    Value::Null,
                    Value::Row(vec![
                        Value::Array(vec![Value::Null, Value::BigInt(1)]),
                        Value::Varchar("a".into()),
                    ]),
                    Value::Row(vec![
                        Value::Array(vec![Value::Null, Value::BigInt(2)]),
                        Value::Varchar("b".into()),
                    ]),
                ],
            ),
        ];
        for (sql, values) in cases {
            let ty = crate::runtime::parse_row_arrow_type(sql).unwrap();
            // Slice exercises nonzero Arrow offsets, including map entry offsets.
            let array = build_array(&ty, &[vec![values[0].clone()], values.clone()].concat())
                .unwrap()
                .slice(1, values.len());
            for (i, a) in values.iter().enumerate() {
                let av = Generic::new(array.as_ref(), i).unwrap();
                assert_eq!(av.hash().unwrap(), a.hash().unwrap());
                for (j, b) in values.iter().enumerate() {
                    let bv = Generic::new(array.as_ref(), j).unwrap();
                    for nulls_first in [false, true] {
                        for ascending in [false, true] {
                            for equals_only in [false, true] {
                                for null_handling in
                                    [NullHandling::NullAsValue, NullHandling::NullAsIndeterminate]
                                {
                                    let flags = CompareFlags {
                                        nulls_first,
                                        ascending,
                                        equals_only,
                                        null_handling,
                                    };
                                    assert_eq!(
                                        av.compare(&bv, flags),
                                        a.compare(b, flags),
                                        "{sql}, rows {i}/{j}, {flags:?}"
                                    );
                                    assert_eq!(
                                        Value::Borrowed(av).compare(b, flags),
                                        a.compare(b, flags),
                                        "borrowed/owned {sql}, rows {i}/{j}, {flags:?}"
                                    );
                                    assert_eq!(
                                        a.compare(&Value::Borrowed(bv), flags),
                                        a.compare(b, flags),
                                        "owned/borrowed {sql}, rows {i}/{j}, {flags:?}"
                                    );
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
