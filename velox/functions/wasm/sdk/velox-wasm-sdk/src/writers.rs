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

//! Single-row writers. Children may themselves be writers or Generic values.
use crate::scalar::{OutputValue, RowOutput};
use crate::Value;

#[derive(Clone, Debug)]
pub struct ArrayWriter<T> {
    values: Vec<Option<T>>,
}
impl<T> Default for ArrayWriter<T> {
    fn default() -> Self {
        Self::new()
    }
}
impl<T> ArrayWriter<T> {
    pub fn new() -> Self {
        Self { values: vec![] }
    }
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            values: Vec::with_capacity(capacity),
        }
    }
    pub fn len(&self) -> usize {
        self.values.len()
    }
    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }
    pub fn reserve(&mut self, additional: usize) {
        self.values.reserve(additional);
    }
    pub fn resize(&mut self, size: usize) {
        self.values.resize_with(size, || None);
    }
    pub fn push(&mut self, value: Option<T>) {
        self.values.push(value);
    }
    pub fn add_null(&mut self) {
        self.push(None);
    }
    pub fn add_item(&mut self) -> &mut T
    where
        T: Default,
    {
        self.push(Some(T::default()));
        self.values.last_mut().unwrap().as_mut().unwrap()
    }
    pub fn get_mut(&mut self, index: usize) -> Option<&mut Option<T>> {
        self.values.get_mut(index)
    }
    pub fn add_items(&mut self, values: impl IntoIterator<Item = Option<T>>) {
        self.values.extend(values);
    }
    pub fn copy_from(&mut self, values: impl IntoIterator<Item = Option<T>>) {
        self.values.clear();
        self.add_items(values);
    }
    pub fn into_values(self) -> Vec<Option<T>> {
        self.values
    }
}
impl<T> From<Vec<Option<T>>> for ArrayWriter<T> {
    fn from(values: Vec<Option<T>>) -> Self {
        Self { values }
    }
}
impl<'a, T: RowOutput<'a>> RowOutput<'a> for ArrayWriter<T> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(Value::Array(
            self.values
                .into_iter()
                .map(|v| Value::from_output(v.into_output()))
                .collect(),
        ))
    }
}

#[derive(Clone, Debug)]
pub struct MapWriter<K, V> {
    entries: Vec<(K, Option<V>)>,
}
impl<K, V> Default for MapWriter<K, V> {
    fn default() -> Self {
        Self::new()
    }
}
impl<K, V> MapWriter<K, V> {
    pub fn new() -> Self {
        Self { entries: vec![] }
    }
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            entries: Vec::with_capacity(capacity),
        }
    }
    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
    pub fn reserve(&mut self, additional: usize) {
        self.entries.reserve(additional);
    }
    pub fn emplace(&mut self, key: K, value: Option<V>) {
        self.entries.push((key, value));
    }
    pub fn add_item(&mut self) -> (&mut K, &mut V)
    where
        K: Default,
        V: Default,
    {
        self.entries.push((K::default(), Some(V::default())));
        let (k, v) = self.entries.last_mut().unwrap();
        (k, v.as_mut().unwrap())
    }
    pub fn add_null(&mut self, key: K) {
        self.emplace(key, None);
    }
    pub fn resize(&mut self, size: usize)
    where
        K: Default,
    {
        self.entries.resize_with(size, || (K::default(), None));
    }
    pub fn get_mut(&mut self, index: usize) -> Option<&mut (K, Option<V>)> {
        self.entries.get_mut(index)
    }
    pub fn add_items(&mut self, entries: impl IntoIterator<Item = (K, Option<V>)>) {
        self.entries.extend(entries);
    }
    pub fn copy_from(&mut self, entries: impl IntoIterator<Item = (K, Option<V>)>) {
        self.entries.clear();
        self.add_items(entries);
    }
    pub fn into_entries(self) -> Vec<(K, Option<V>)> {
        self.entries
    }
}
impl<'a, K: RowOutput<'a>, V: RowOutput<'a>> RowOutput<'a> for MapWriter<K, V> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(Value::Map(
            self.entries
                .into_iter()
                .map(|(k, v)| {
                    (
                        Value::from_output(k.into_output()),
                        Value::from_output(v.into_output()),
                    )
                })
                .collect(),
        ))
    }
}

/// T is a tuple of nullable fields, e.g. (Option<i64>, Option<ArrayWriter<String>>).
#[derive(Clone, Debug, Default)]
pub struct RowWriter<T> {
    fields: T,
}
impl<T> RowWriter<T> {
    pub fn new(fields: T) -> Self {
        Self { fields }
    }
    pub fn into_fields(self) -> T {
        self.fields
    }
    pub fn get_mut<const I: usize>(&mut self) -> &mut <T as TupleFieldMut<I>>::Item
    where
        T: TupleFieldMut<I>,
    {
        self.fields.field_mut()
    }
    /// Initialize a nullable field and return its scalar or nested writer.
    pub fn get_writer_at<const I: usize>(
        &mut self,
    ) -> &mut <<T as TupleFieldMut<I>>::Item as NullableSlot>::Value
    where
        T: TupleFieldMut<I>,
        <T as TupleFieldMut<I>>::Item: NullableSlot,
    {
        self.get_mut::<I>().get_or_insert_default()
    }
    pub fn set_null_at<const I: usize>(&mut self)
    where
        T: TupleFieldMut<I>,
        <T as TupleFieldMut<I>>::Item: SetNull,
    {
        self.get_mut::<I>().set_null();
    }
}
pub trait TupleFieldMut<const I: usize> {
    type Item;
    fn field_mut(&mut self) -> &mut Self::Item;
}
pub trait NullableSlot {
    type Value;
    fn get_or_insert_default(&mut self) -> &mut Self::Value;
}
pub trait SetNull {
    fn set_null(&mut self);
}
impl<T> SetNull for Option<T> {
    fn set_null(&mut self) {
        *self = None;
    }
}
impl<T: Default> NullableSlot for Option<T> {
    type Value = T;
    fn get_or_insert_default(&mut self) -> &mut T {
        self.get_or_insert_with(T::default)
    }
}
pub trait RowFieldsOutput<'a> {
    fn into_values(self) -> Vec<Value<'a>>;
}
impl<'a, T: RowFieldsOutput<'a>> RowOutput<'a> for RowWriter<T> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(Value::Row(self.fields.into_values()))
    }
}
impl<'a> RowFieldsOutput<'a> for () {
    fn into_values(self) -> Vec<Value<'a>> {
        vec![]
    }
}

/// A dynamic writer supports any field count and SQL type, including Generic children.
#[derive(Clone, Default)]
pub struct GenericWriter<'a> {
    value: Value<'a>,
}
impl<'a> GenericWriter<'a> {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn set<T: RowOutput<'a>>(&mut self, value: T) {
        self.value = Value::from_output(value.into_output());
    }
    pub fn set_null(&mut self) {
        self.value = Value::Null;
    }
    pub fn value_mut(&mut self) -> &mut Value<'a> {
        &mut self.value
    }
    pub fn into_value(self) -> Value<'a> {
        self.value
    }
}
impl<'a> RowOutput<'a> for GenericWriter<'a> {
    fn into_output(self) -> OutputValue<'a> {
        OutputValue::Owned(self.value)
    }
}

macro_rules! row_outputs {
    (@fields [$($all:ident),*];) => {};
    (@fields [$($all:ident),*];$t:ident:$i:tt $(,$rest:ident:$j:tt)*) => {
        impl<$($all),*> TupleFieldMut<$i> for ($($all,)*) { type Item=$t; fn field_mut(&mut self)->&mut Self::Item { &mut self.$i } }
        row_outputs!(@fields [$($all),*];$($rest:$j),*);
    };
    ($($t:ident:$i:tt),+) => {
        impl<'a,$($t:RowOutput<'a>),*> RowFieldsOutput<'a> for ($($t,)*) {
            fn into_values(self)->Vec<Value<'a>> { vec![$(Value::from_output(self.$i.into_output())),*] }
        }
        row_outputs!(@fields [$($t),*];$($t:$i),*);
    };
}
row_outputs!(T0:0);
row_outputs!(T0:0,T1:1);
row_outputs!(T0:0,T1:1,T2:2);
row_outputs!(T0:0,T1:1,T2:2,T3:3);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12,T13:13);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12,T13:13,T14:14);
row_outputs!(T0:0,T1:1,T2:2,T3:3,T4:4,T5:5,T6:6,T7:7,T8:8,T9:9,T10:10,T11:11,T12:12,T13:13,T14:14,T15:15);
