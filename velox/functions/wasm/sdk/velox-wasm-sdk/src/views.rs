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

//! Borrowed, typed, lazy single-row views. Nested children are views too.
use crate::scalar::{Generic, OutputValue, RowArgument, RowOutput};
use crate::{ArrayWriter, MapWriter, Value};
use arrow_array::{Array, MapArray, StructArray};
use arrow_schema::{DataType, Fields};
use std::marker::PhantomData;
use std::ops::Range;

type Result<T> = std::result::Result<T, String>;

impl<'a> Generic<'a> {
    pub fn map_len(&self) -> Result<usize> {
        if self.is_null() {
            return Err("cannot read a null map".into());
        }
        let map = self
            .array
            .as_any()
            .downcast_ref::<MapArray>()
            .ok_or("expected map")?;
        Ok(map.value_length(self.row) as usize)
    }
    pub fn map_entry(&self, index: usize) -> Result<(Self, Self)> {
        let length = self.map_len()?;
        if index >= length {
            return Err("map index out of range".into());
        }
        let map = self.array.as_any().downcast_ref::<MapArray>().unwrap();
        let offset = map.value_offsets()[self.row] as usize + index;
        Ok((
            Self::new(map.keys().as_ref(), offset)?,
            Self::new(map.values().as_ref(), offset)?,
        ))
    }
    pub fn row_len(&self) -> Result<usize> {
        if self.is_null() {
            return Err("cannot read a null row".into());
        }
        let row = self
            .array
            .as_any()
            .downcast_ref::<StructArray>()
            .ok_or("expected row")?;
        Ok(row.num_columns())
    }
    pub fn field_at(&self, index: usize) -> Result<Self> {
        if index >= self.row_len()? {
            return Err("row field index out of range".into());
        }
        let row = self.array.as_any().downcast_ref::<StructArray>().unwrap();
        Self::new(row.column(index).as_ref(), self.row)
    }
    pub fn try_cast<T: RowArgument<'a>>(&self) -> Result<Option<T>> {
        if !T::accepts(self.data_type()) {
            return Ok(None);
        }
        self.get::<T>().map(Some)
    }
    pub fn as_array<T: RowArgument<'a>>(&self) -> Result<ArrayView<'a, T>> {
        self.get()
    }
    pub fn as_map<K: RowArgument<'a>, V: RowArgument<'a>>(&self) -> Result<MapView<'a, K, V>> {
        self.get()
    }
    pub fn as_row<T: RowShape<'a>>(&self) -> Result<RowView<'a, T>> {
        self.get()
    }
}

pub struct ArrayView<'a, T> {
    value: Generic<'a>,
    elements: &'a dyn Array,
    offset: usize,
    length: usize,
    marker: PhantomData<T>,
}
impl<'a, T> Copy for ArrayView<'a, T> {}
impl<'a, T> Clone for ArrayView<'a, T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, T: RowArgument<'a>> RowArgument<'a> for ArrayView<'a, T> {
    fn accepts(ty: &DataType) -> bool {
        matches!(ty,DataType::List(field) if T::accepts(field.data_type()))
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        let value = Generic::new(array, row)?;
        if !Self::accepts(value.data_type()) {
            return Err("array child type does not match typed view".into());
        }
        let list = array
            .as_any()
            .downcast_ref::<arrow_array::ListArray>()
            .ok_or("expected ARRAY")?;
        if value.is_null() {
            return Err("cannot read a null array".into());
        }
        Ok(Self {
            value,
            elements: list.values().as_ref(),
            offset: list.value_offsets()[row] as usize,
            length: list.value_length(row) as usize,
            marker: PhantomData,
        })
    }
}
impl<'a, T: RowArgument<'a>> ArrayView<'a, T> {
    pub fn len(&self) -> usize {
        self.length
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn get(&self, index: usize) -> Result<Option<T>> {
        if index >= self.length {
            return Err("array index out of range".into());
        }
        <Option<T>>::read(self.elements, self.offset + index)
    }
    pub fn at(&self, index: usize) -> Result<Option<T>> {
        self.get(index)
    }
    pub fn iter(
        &self,
    ) -> impl DoubleEndedIterator<Item = Result<Option<T>>> + ExactSizeIterator + 'a {
        let view = *self;
        (0..self.len()).map(move |i| view.get(i))
    }
    pub fn skip_nulls(&self) -> impl DoubleEndedIterator<Item = Result<T>> + 'a {
        self.iter().filter_map(|item| item.transpose())
    }
    pub fn may_have_nulls(&self) -> bool {
        let DataType::List(_) = self.value.data_type() else {
            unreachable!()
        };
        self.value
            .array
            .as_any()
            .downcast_ref::<arrow_array::ListArray>()
            .unwrap()
            .values()
            .logical_null_count()
            > 0
    }
    pub fn contains_nulls(&self) -> Result<bool> {
        self.value.contains_nulls()
    }
    pub fn materialize(&self) -> Result<ArrayWriter<T::Owned>>
    where
        T: Materialize,
    {
        Ok(ArrayWriter::from(
            self.iter()
                .map(|v| v?.map(Materialize::materialize).transpose())
                .collect::<Result<Vec<_>>>()?,
        ))
    }
    pub fn null_free(&self) -> Result<NullFreeArrayView<'a, T>> {
        if self.contains_nulls()? {
            return Err("null-free array contains nulls".into());
        }
        Ok(NullFreeArrayView(*self))
    }
}
impl<'a, T> RowOutput<'a> for ArrayView<'a, T> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Generic(self.value)
    }
}

pub struct MapView<'a, K, V> {
    value: Generic<'a>,
    marker: PhantomData<(K, V)>,
}
impl<'a, K, V> Copy for MapView<'a, K, V> {}
impl<'a, K, V> Clone for MapView<'a, K, V> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, K: RowArgument<'a>, V: RowArgument<'a>> RowArgument<'a> for MapView<'a, K, V> {
    fn accepts(ty: &DataType) -> bool {
        matches!(ty,DataType::Map(field,_) if matches!(field.data_type(),DataType::Struct(fields) if fields.len()==2 && K::accepts(fields[0].data_type()) && V::accepts(fields[1].data_type())))
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        let value = Generic::new(array, row)?;
        if !Self::accepts(value.data_type()) {
            return Err("map child types do not match typed view".into());
        }
        value.map_len()?;
        Ok(Self {
            value,
            marker: PhantomData,
        })
    }
}
impl<'a, K: RowArgument<'a>, V: RowArgument<'a>> MapView<'a, K, V> {
    pub fn len(&self) -> usize {
        self.value.map_len().unwrap()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn at_index(&self, index: usize) -> Result<(K, Option<V>)> {
        let (k, v) = self.value.map_entry(index)?;
        if k.is_null() {
            return Err("map key is null".into());
        }
        Ok((k.get()?, v.get()?))
    }
    pub fn iter(
        &self,
    ) -> impl DoubleEndedIterator<Item = Result<(K, Option<V>)>> + ExactSizeIterator + 'a {
        let view = *self;
        (0..self.len()).map(move |i| view.at_index(i))
    }
    /// Linear search, matching Velox MapView. Some(None) is a present NULL value.
    pub fn find<'k, Q: RowOutput<'k>>(&self, key: Q) -> Result<Option<Option<V>>> {
        let key = Value::from_output(key.into_output());
        if key.is_null() {
            return Err("lookup key is null".into());
        }
        let DataType::Map(field, _) = self.value.data_type() else {
            unreachable!()
        };
        let DataType::Struct(fields) = field.data_type() else {
            unreachable!()
        };
        crate::value::validate(&key, fields[0].data_type())?;
        for i in 0..self.len() {
            let (k, v) = self.value.map_entry(i)?;
            if Value::Borrowed(k).equals(&key)? {
                return Ok(Some(v.get()?));
            }
        }
        Ok(None)
    }
    pub fn at<'k, Q: RowOutput<'k>>(&self, key: Q) -> Result<Option<V>> {
        self.find(key)?.ok_or_else(|| "map key not found".into())
    }
    pub fn contains<'k, Q: RowOutput<'k>>(&self, key: Q) -> Result<bool> {
        Ok(self.find(key)?.is_some())
    }
    pub fn contains_nulls(&self) -> Result<bool> {
        self.value.contains_nulls()
    }
    pub fn materialize(&self) -> Result<MapWriter<K::Owned, V::Owned>>
    where
        K: Materialize,
        V: Materialize,
    {
        let mut result = MapWriter::with_capacity(self.len());
        for entry in self.iter() {
            let (k, v) = entry?;
            result.emplace(
                k.materialize()?,
                v.map(Materialize::materialize).transpose()?,
            );
        }
        Ok(result)
    }
    pub fn null_free(&self) -> Result<NullFreeMapView<'a, K, V>> {
        if self.contains_nulls()? {
            return Err("null-free map contains nulls".into());
        }
        Ok(NullFreeMapView(*self))
    }
}
impl<'a, K, V> RowOutput<'a> for MapView<'a, K, V> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Generic(self.value)
    }
}

/// Use DynamicRow (the default) for arbitrary arity; a tuple enables get::<I>().
pub struct DynamicRow;
pub trait RowShape<'a>: 'a {
    fn accepts(fields: &Fields) -> bool;
}
impl<'a> RowShape<'a> for DynamicRow {
    fn accepts(_: &Fields) -> bool {
        true
    }
}
pub trait RowField<'a, const I: usize>: RowShape<'a> {
    type Item: RowArgument<'a>;
}
pub struct RowView<'a, T = DynamicRow> {
    value: Generic<'a>,
    marker: PhantomData<T>,
}
impl<'a, T> Copy for RowView<'a, T> {}
impl<'a, T> Clone for RowView<'a, T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, T: RowShape<'a>> RowArgument<'a> for RowView<'a, T> {
    fn accepts(ty: &DataType) -> bool {
        !crate::custom::is_custom(ty)
            && !crate::is_varchar_wire_type(ty)
            && !crate::Timestamp::is_wire_type(ty)
            && !crate::is_hugeint_wire_type(ty)
            && matches!(ty,DataType::Struct(fields) if T::accepts(fields))
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        let value = Generic::new(array, row)?;
        if !Self::accepts(value.data_type()) {
            return Err("row fields do not match typed view".into());
        }
        value.row_len()?;
        Ok(Self {
            value,
            marker: PhantomData,
        })
    }
}
impl<'a, T: RowShape<'a>> RowView<'a, T> {
    pub fn len(&self) -> usize {
        self.value.row_len().unwrap()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn get<const I: usize>(&self) -> Result<Option<T::Item>>
    where
        T: RowField<'a, I>,
    {
        self.value.field_at(I)?.get()
    }
    pub fn field(&self, name: &str) -> Result<Generic<'a>> {
        self.value.field(name)
    }
    pub fn at(&self, index: usize) -> Result<Generic<'a>> {
        self.value.field_at(index)
    }
    pub fn iter(
        &self,
    ) -> impl DoubleEndedIterator<Item = Result<Generic<'a>>> + ExactSizeIterator + 'a {
        let value = self.value;
        (0..self.len()).map(move |i| value.field_at(i))
    }
    pub fn contains_nulls(&self) -> Result<bool> {
        self.value.contains_nulls()
    }
    pub fn materialize(&self) -> Result<Value<'static>> {
        self.value.materialize()
    }
    pub fn null_free(&self) -> Result<NullFreeRowView<'a, T>> {
        if self.contains_nulls()? {
            return Err("null-free row contains nulls".into());
        }
        Ok(NullFreeRowView(*self))
    }
}
impl<'a, T> RowOutput<'a> for RowView<'a, T> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Generic(self.value)
    }
}
impl<'a> RowShape<'a> for () {
    fn accepts(fields: &Fields) -> bool {
        fields.is_empty()
    }
}

pub struct NullFreeArrayView<'a, T>(ArrayView<'a, T>);
pub struct NullFreeMapView<'a, K, V>(MapView<'a, K, V>);
pub struct NullFreeRowView<'a, T = DynamicRow>(RowView<'a, T>);
impl<T> Copy for NullFreeArrayView<'_, T> {}
impl<T> Clone for NullFreeArrayView<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<K, V> Copy for NullFreeMapView<'_, K, V> {}
impl<K, V> Clone for NullFreeMapView<'_, K, V> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for NullFreeRowView<'_, T> {}
impl<T> Clone for NullFreeRowView<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<'a, T: RowArgument<'a>> NullFreeArrayView<'a, T> {
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn get(&self, index: usize) -> Result<T> {
        if index >= self.0.length {
            return Err("array index out of range".into());
        }
        T::read(self.0.elements, self.0.offset + index)
    }
    pub fn iter(&self) -> impl DoubleEndedIterator<Item = Result<T>> + ExactSizeIterator + 'a {
        let view = self.0;
        (0..view.len()).map(move |i| T::read(view.elements, view.offset + i))
    }
}
impl<'a, K: RowArgument<'a>, V: RowArgument<'a>> NullFreeMapView<'a, K, V> {
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn at_index(&self, index: usize) -> Result<(K, V)> {
        let (k, v) = self.0.value.map_entry(index)?;
        Ok((k.get()?, v.get()?))
    }
    pub fn iter(&self) -> impl DoubleEndedIterator<Item = Result<(K, V)>> + ExactSizeIterator + 'a {
        let view = self.0;
        (0..view.len()).map(move |i| {
            let (k, v) = view.value.map_entry(i)?;
            Ok((k.get()?, v.get()?))
        })
    }
    pub fn find<'k, Q: RowOutput<'k>>(&self, key: Q) -> Result<Option<V>> {
        self.0
            .find(key)?
            .map(|value| value.ok_or_else(|| "unexpected null map value".into()))
            .transpose()
    }
    pub fn contains<'k, Q: RowOutput<'k>>(&self, key: Q) -> Result<bool> {
        Ok(self.find(key)?.is_some())
    }
    pub fn at<'k, Q: RowOutput<'k>>(&self, key: Q) -> Result<V> {
        self.0
            .at(key)?
            .ok_or_else(|| "unexpected null map value".into())
    }
}
impl<'a, T: RowShape<'a>> NullFreeRowView<'a, T> {
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn get<const I: usize>(&self) -> Result<T::Item>
    where
        T: RowField<'a, I>,
    {
        self.0.value.field_at(I)?.get()
    }
    pub fn at(&self, index: usize) -> Result<Generic<'a>> {
        self.0.at(index)
    }
}

/// Materialize primitives and recursively typed views into independent values.
pub trait Materialize {
    type Owned;
    fn materialize(self) -> Result<Self::Owned>;
}
impl<T: Materialize> Materialize for Option<T> {
    type Owned = Option<T::Owned>;
    fn materialize(self) -> Result<Self::Owned> {
        self.map(Materialize::materialize).transpose()
    }
}
impl Materialize for crate::OpaqueHandle<'_> {
    type Owned = Value<'static>;
    fn materialize(self) -> Result<Self::Owned> {
        Value::from_output(self.into_output()).materialize()
    }
}
macro_rules! owned_materialize { ($($t:ty),*) => { $(impl Materialize for $t { type Owned=Self; fn materialize(self)->Result<Self> { Ok(self) } })* }; }
owned_materialize!(
    bool,
    i8,
    i16,
    i32,
    i64,
    i128,
    f32,
    f64,
    String,
    Vec<u8>,
    crate::Date32,
    crate::Timestamp,
    crate::Decimal
);
impl Materialize for &str {
    type Owned = String;
    fn materialize(self) -> Result<String> {
        Ok(self.to_owned())
    }
}
impl Materialize for &[u8] {
    type Owned = Vec<u8>;
    fn materialize(self) -> Result<Vec<u8>> {
        Ok(self.to_vec())
    }
}
impl Materialize for Generic<'_> {
    type Owned = Value<'static>;
    fn materialize(self) -> Result<Self::Owned> {
        Generic::materialize(&self)
    }
}
impl<'a, T: RowArgument<'a> + Materialize> Materialize for ArrayView<'a, T> {
    type Owned = ArrayWriter<T::Owned>;
    fn materialize(self) -> Result<Self::Owned> {
        ArrayView::materialize(&self)
    }
}
impl<'a, K: RowArgument<'a> + Materialize, V: RowArgument<'a> + Materialize> Materialize
    for MapView<'a, K, V>
{
    type Owned = MapWriter<K::Owned, V::Owned>;
    fn materialize(self) -> Result<Self::Owned> {
        MapView::materialize(&self)
    }
}
impl<'a, T: RowShape<'a>> Materialize for RowView<'a, T> {
    type Owned = Value<'static>;
    fn materialize(self) -> Result<Self::Owned> {
        RowView::materialize(&self)
    }
}

macro_rules! row_shapes {
    (@fields [$($all:ident),*];) => {};
    (@fields [$($all:ident),*];$t:ident:$i:tt $(,$rest:ident:$j:tt)*) => {
        impl<'a,$($all:RowArgument<'a>),*> RowField<'a,$i> for ($($all,)*) { type Item=$t; }
        row_shapes!(@fields [$($all),*];$($rest:$j),*);
    };
    ($($t:ident:$i:tt),+) => {
        impl<'a,$($t:RowArgument<'a>),*> RowShape<'a> for ($($t,)*) {
            fn accepts(fields:&Fields)->bool { fields.len()==[$(stringify!($t)),*].len() && $($t::accepts(fields[$i].data_type()) &&)* true }
        }
        row_shapes!(@fields [$($t),*];$($t:$i),*);
    };
}
row_shapes!(T0:0);
row_shapes!(T0:0,T1:1);
row_shapes!(T0:0,T1:1,T2:2);
row_shapes!(T0:0,T1:1,T2:2,T3:3);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12,T13:13);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12,T13:13,T14:14);
row_shapes!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12,T13:13,T14:14,T15:15);

impl<'a, T: RowArgument<'a>> RowArgument<'a> for NullFreeArrayView<'a, T> {
    fn accepts(ty: &DataType) -> bool {
        ArrayView::<'a, T>::accepts(ty)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        ArrayView::read(array, row)?.null_free()
    }
}
impl<'a, K: RowArgument<'a>, V: RowArgument<'a>> RowArgument<'a> for NullFreeMapView<'a, K, V> {
    fn accepts(ty: &DataType) -> bool {
        MapView::<'a, K, V>::accepts(ty)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        MapView::read(array, row)?.null_free()
    }
}
impl<'a, T: RowShape<'a>> RowArgument<'a> for NullFreeRowView<'a, T> {
    fn accepts(ty: &DataType) -> bool {
        RowView::<'a, T>::accepts(ty)
    }
    fn read(array: &'a dyn Array, row: usize) -> Result<Self> {
        RowView::read(array, row)?.null_free()
    }
}
impl<'a, T> RowOutput<'a> for NullFreeArrayView<'a, T> {
    fn into_output(self) -> OutputValue<'a> {
        self.0.into_output()
    }
}
impl<'a, K, V> RowOutput<'a> for NullFreeMapView<'a, K, V> {
    fn into_output(self) -> OutputValue<'a> {
        self.0.into_output()
    }
}
impl<'a, T> RowOutput<'a> for NullFreeRowView<'a, T> {
    fn into_output(self) -> OutputValue<'a> {
        self.0.into_output()
    }
}

impl<'a, T: RowArgument<'a> + Materialize> Materialize for NullFreeArrayView<'a, T> {
    type Owned = ArrayWriter<T::Owned>;
    fn materialize(self) -> Result<Self::Owned> {
        self.0.materialize()
    }
}
impl<'a, K: RowArgument<'a> + Materialize, V: RowArgument<'a> + Materialize> Materialize
    for NullFreeMapView<'a, K, V>
{
    type Owned = MapWriter<K::Owned, V::Owned>;
    fn materialize(self) -> Result<Self::Owned> {
        self.0.materialize()
    }
}
impl<'a, T: RowShape<'a>> Materialize for NullFreeRowView<'a, T> {
    type Owned = Value<'static>;
    fn materialize(self) -> Result<Self::Owned> {
        self.0.materialize()
    }
}

#[doc(hidden)]
pub trait IndexedView {
    type Item;
    fn item(&self, index: usize) -> Self::Item;
}
/// An allocation-free, exact-size iterator over one nested SQL value.
pub struct ViewIter<V> {
    view: V,
    indices: Range<usize>,
}
impl<V: IndexedView> Iterator for ViewIter<V> {
    type Item = V::Item;
    fn next(&mut self) -> Option<Self::Item> {
        self.indices.next().map(|i| self.view.item(i))
    }
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.indices.size_hint()
    }
    fn nth(&mut self, n: usize) -> Option<Self::Item> {
        self.indices.nth(n).map(|i| self.view.item(i))
    }
}
impl<V: IndexedView> DoubleEndedIterator for ViewIter<V> {
    fn next_back(&mut self) -> Option<Self::Item> {
        self.indices.next_back().map(|i| self.view.item(i))
    }
}
impl<V: IndexedView> ExactSizeIterator for ViewIter<V> {}
impl<V: IndexedView> std::iter::FusedIterator for ViewIter<V> {}
macro_rules! view_iteration {
    ($view:ident<$($t:ident:$bound:ident),+>, $item:ty, $getter:ident) => {
        impl<'a, $($t:$bound<'a>),+> IndexedView for $view<'a,$($t),+> {
            type Item = Result<$item>;
            fn item(&self,index:usize)->Self::Item { self.$getter(index) }
        }
        impl<'a, $($t:$bound<'a>),+> IntoIterator for $view<'a,$($t),+> {
            type Item = Result<$item>;
            type IntoIter = ViewIter<Self>;
            fn into_iter(self)->Self::IntoIter { ViewIter { indices: 0..self.len(), view:self } }
        }
        impl<'a, $($t:$bound<'a>),+> IntoIterator for &$view<'a,$($t),+> {
            type Item = Result<$item>;
            type IntoIter = ViewIter<$view<'a,$($t),+>>;
            fn into_iter(self)->Self::IntoIter { (*self).into_iter() }
        }
    };
}
view_iteration!(ArrayView<T:RowArgument>, Option<T>, get);
view_iteration!(MapView<K:RowArgument,V:RowArgument>, (K,Option<V>), at_index);
view_iteration!(RowView<T:RowShape>, Generic<'a>, at);
view_iteration!(NullFreeArrayView<T:RowArgument>, T, get);
view_iteration!(NullFreeMapView<K:RowArgument,V:RowArgument>, (K,V), at_index);
view_iteration!(NullFreeRowView<T:RowShape>, Generic<'a>, at);

impl<T: AsRef<[u8]>> Materialize for crate::VarcharBytes<T> {
    type Owned = crate::VarcharBytes<Vec<u8>>;
    fn materialize(self) -> Result<Self::Owned> {
        Ok(crate::VarcharBytes(self.0.as_ref().to_vec()))
    }
}
