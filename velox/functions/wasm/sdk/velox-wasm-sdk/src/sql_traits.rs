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

//! Bounds for Rust generic SQL entrypoints. Velox checks these constraints on
//! the bound SQL types, including children, before invoking a Wasm export.
//! Generic is a runtime view: its Rust type alone does not prove a SQL bound.
use crate::scalar::{RowArgument, RowOutput};
use crate::views::RowShape;
use crate::Materialize;
use crate::{
    ArrayView, CompareFlags, Generic, MapView, NullFreeArrayView, NullFreeMapView, NullFreeRowView,
    RowView, Value,
};
use std::cmp::Ordering;
type Result<T> = std::result::Result<T, String>;

/// A readable and returnable SQL row value. `Option<T>` expresses SQL NULL.
/// The export macro turns this bound into an unconstrained SQL type variable.
pub trait SqlValue<'a>:
    RowArgument<'a> + RowOutput<'a> + Materialize<Owned: RowOutput<'static>>
{
    /// Detach borrowed buffers using the existing Materialize::Owned type.
    /// OPAQUE invocation scope still applies after copying a token.
    /// Returning T directly keeps a borrowed input borrowed instead.
    fn sql_owned(self) -> Result<Self::Owned> {
        self.materialize()
    }
}
impl<'a, T: RowArgument<'a> + RowOutput<'a> + Materialize<Owned: RowOutput<'static>>> SqlValue<'a>
    for T
{
}

/// A SQL variable whose bound root type is not UNKNOWN. ARRAY(UNKNOWN) is known.
/// This is independent of comparability: Velox UNKNOWN is comparable/orderable.
pub trait SqlKnown<'a>: SqlValue<'a> {}

/// SQL equality and hashing, including NaNs and nested NULL values. The export
/// constraint excludes OPAQUE and complex values containing noncomparable types.
pub trait SqlComparable<'a>: SqlValue<'a> {
    fn sql_equals(self, other: Self) -> Result<bool> {
        Value::from_output(self.into_output()).equals(&Value::from_output(other.into_output()))
    }
    /// SQL nullable equality, including indeterminate nested NULLs in MAPs.
    fn sql_equals_with(
        self,
        other: Self,
        null_handling: crate::NullHandling,
    ) -> Result<Option<bool>> {
        Value::from_output(self.into_output())
            .compare(
                &Value::from_output(other.into_output()),
                CompareFlags::equality(null_handling),
            )
            .map(|ordering| ordering.map(|ordering| ordering == Ordering::Equal))
    }
    fn sql_hash(self) -> Result<u64> {
        Value::from_output(self.into_output()).hash()
    }
}

/// SQL ordering, with Velox comparison flags. This excludes MAP and OPAQUE at
/// SQL binding, and checks ARRAY/ROW children recursively.
pub trait SqlOrderable<'a>: SqlComparable<'a> {
    fn sql_compare(self, other: Self, flags: CompareFlags) -> Result<Option<Ordering>> {
        Value::from_output(self.into_output())
            .compare(&Value::from_output(other.into_output()), flags)
    }
}

macro_rules! scalar_bounds {
    ($($ty:ty),* $(,)?) => {$(
        impl<'a> SqlKnown<'a> for $ty {}
        impl<'a> SqlComparable<'a> for $ty {}
        impl<'a> SqlOrderable<'a> for $ty {}
    )*};
}
scalar_bounds!(
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
    &'a str,
    &'a [u8],
    crate::Date32,
    crate::Timestamp,
    crate::Decimal,
    Generic<'a>
);
scalar_bounds!(crate::VarcharBytes<&'a [u8]>, crate::VarcharBytes<Vec<u8>>);
impl<'a> SqlKnown<'a> for crate::OpaqueHandle<'a> {}
impl<'a, T: SqlKnown<'a>> SqlKnown<'a> for Option<T> {}
impl<'a, T: SqlComparable<'a>> SqlComparable<'a> for Option<T> {}
impl<'a, T: SqlOrderable<'a>> SqlOrderable<'a> for Option<T> {}

macro_rules! array_bounds {
    ($view:ident) => {
        impl<'a, T: SqlValue<'a>> SqlKnown<'a> for $view<'a, T> {}
        impl<'a, T: SqlComparable<'a>> SqlComparable<'a> for $view<'a, T> {}
        impl<'a, T: SqlOrderable<'a>> SqlOrderable<'a> for $view<'a, T> {}
    };
}
array_bounds!(ArrayView);
array_bounds!(NullFreeArrayView);
macro_rules! map_bounds {
    ($view:ident) => {
        impl<'a, K: SqlValue<'a>, V: SqlValue<'a>> SqlKnown<'a> for $view<'a, K, V> {}
        impl<'a, K: SqlComparable<'a>, V: SqlComparable<'a>> SqlComparable<'a> for $view<'a, K, V> {}
    };
}
map_bounds!(MapView);
map_bounds!(NullFreeMapView);

trait ComparableRow<'a>: RowShape<'a> {}
trait OrderableRow<'a>: ComparableRow<'a> {}
impl<'a> ComparableRow<'a> for crate::DynamicRow {}
impl<'a> OrderableRow<'a> for crate::DynamicRow {}
macro_rules! row_bounds {
    ($($t:ident),*) => {
        impl<'a, $($t: SqlComparable<'a>),*> ComparableRow<'a> for ($($t,)*) {}
        impl<'a, $($t: SqlOrderable<'a>),*> OrderableRow<'a> for ($($t,)*) {}
    };
}
row_bounds!();
row_bounds!(T0);
row_bounds!(T0, T1);
row_bounds!(T0, T1, T2);
row_bounds!(T0, T1, T2, T3);
row_bounds!(T0, T1, T2, T3, T4);
row_bounds!(T0, T1, T2, T3, T4, T5);
row_bounds!(T0, T1, T2, T3, T4, T5, T6);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8, T9);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14);
row_bounds!(T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15);
macro_rules! row_view_bounds {
    ($view:ident) => {
        impl<'a, T: RowShape<'a>> SqlKnown<'a> for $view<'a, T> {}
        impl<'a, T: ComparableRow<'a>> SqlComparable<'a> for $view<'a, T> {}
        impl<'a, T: OrderableRow<'a>> SqlOrderable<'a> for $view<'a, T> {}
    };
}
row_view_bounds!(RowView);
row_view_bounds!(NullFreeRowView);

#[cfg(test)]
mod tests {
    use super::*;
    use arrow_array::{Float64Array, Int64Array};

    #[test]
    fn comparable_map_equality_can_preserve_indeterminate_nulls() {
        use arrow_array::builder::{Int64Builder, MapBuilder, StringBuilder};
        let mut builder = MapBuilder::new(None, StringBuilder::new(), Int64Builder::new());
        builder.keys().append_value("key");
        builder.values().append_null();
        builder.append(true).unwrap();
        let array = builder.finish();
        let map = Generic::new(&array, 0)
            .unwrap()
            .as_map::<&str, i64>()
            .unwrap();
        assert!(map.sql_equals(map).unwrap());
        assert_eq!(
            map.sql_equals_with(map, crate::NullHandling::NullAsIndeterminate)
                .unwrap(),
            None
        );
        assert_eq!(
            map.sql_equals_with(map, crate::NullHandling::NullAsValue)
                .unwrap(),
            Some(true)
        );
    }

    #[test]
    fn sql_operations_match_for_concrete_and_runtime_values() {
        assert!(f64::NAN.sql_equals(f64::NAN).unwrap());
        assert_eq!(
            f64::NAN.sql_compare(1.0, CompareFlags::default()).unwrap(),
            Some(Ordering::Greater)
        );
        assert_eq!((-0.0_f64).sql_hash().unwrap(), 0.0_f64.sql_hash().unwrap());
        let array = Float64Array::from(vec![f64::NAN, f64::NAN, 1.0]);
        let a = Generic::new(&array, 0).unwrap();
        let b = Generic::new(&array, 1).unwrap();
        assert!(a.sql_equals(b).unwrap());
        assert_eq!(a.sql_hash().unwrap(), f64::NAN.sql_hash().unwrap());
        assert_eq!(
            a.sql_compare(Generic::new(&array, 2).unwrap(), CompareFlags::default())
                .unwrap(),
            Some(Ordering::Greater)
        );
        assert!(matches!(
            Generic::new(&Int64Array::from(vec![42]), 0)
                .unwrap()
                .sql_owned()
                .unwrap(),
            Value::BigInt(42)
        ));
    }
}
