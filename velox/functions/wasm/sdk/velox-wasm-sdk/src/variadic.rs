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

//! Borrowed variadic arguments. Values are read only when accessed.
use crate::scalar::{read_row, RowArgument};
use arrow_array::{Array, ArrayRef};
use arrow_schema::DataType;
use std::marker::PhantomData;
use std::sync::Arc;

type Result<T> = std::result::Result<T, String>;

enum Source<'a, T> {
    Columns(&'a [ArrayRef], usize, fn(&'a dyn Array, usize) -> Result<T>),
    Prepared(
        &'a [ArrayRef],
        Arc<Vec<Box<dyn Fn(usize) -> Result<T> + Send + Sync + 'a>>>,
        usize,
    ),
    Values(&'a [T], fn(&T) -> T),
}
impl<T> Clone for Source<'_, T> {
    fn clone(&self) -> Self {
        match self {
            Self::Columns(columns, row, read) => Self::Columns(columns, *row, *read),
            Self::Prepared(columns, readers, row) => Self::Prepared(columns, readers.clone(), *row),
            Self::Values(values, clone) => Self::Values(values, *clone),
        }
    }
}

/// A single row's borrowed variadic tail. Constructing the view and asking for
/// its length do not read values or allocate a per-row argument vector.
/// `iter` and `at` return Result so invalid values cannot silently be skipped.
/// Owned element types (e.g. String) still allocate when an element is read.
pub struct VariadicView<'a, T> {
    source: Source<'a, T>,
}
impl<T> Clone for VariadicView<'_, T> {
    fn clone(&self) -> Self {
        Self {
            source: self.source.clone(),
        }
    }
}
impl<'a, T> VariadicView<'a, T> {
    /// Borrow ordinary Rust values for direct per-row unit tests. Only accessed
    /// elements are cloned; borrowed Generic/string/nested views remain borrowed.
    pub fn from_values(values: &'a [T]) -> Self
    where
        T: Clone,
    {
        Self {
            source: Source::Values(values, T::clone),
        }
    }
    pub fn len(&self) -> usize {
        match &self.source {
            Source::Columns(columns, _, _) | Source::Prepared(columns, _, _) => columns.len(),
            Source::Values(values, _) => values.len(),
        }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}
impl<'a, T: RowArgument<'a>> VariadicView<'a, T> {
    /// Construct a view over individual Arrow columns without a RecordBatch.
    /// The generated adapter checks column types/lengths once per batch instead.
    pub fn new(columns: &'a [ArrayRef], row: usize) -> Result<Self> {
        let rows = columns
            .first()
            .map_or(row.saturating_add(1), |column| column.len());
        validate_columns::<T>(columns, rows)?;
        if row >= rows {
            return Err("variadic row index out of range".into());
        }
        Ok(Self {
            source: Source::Columns(columns, row, read_row::<T>),
        })
    }
}
impl<'a, T: 'a> VariadicView<'a, T> {
    /// Return None only for an out-of-bounds index. For nullable T, SQL NULL is
    /// represented by the inner Option, e.g. Ok(Some(None)).
    pub fn get(&self, index: usize) -> Result<Option<T>> {
        if index >= self.len() {
            return Ok(None);
        }
        self.at(index).map(Some)
    }
    pub fn at(&self, index: usize) -> Result<T> {
        match &self.source {
            Source::Columns(columns, row, read) => read(
                columns
                    .get(index)
                    .ok_or("variadic argument index out of range")?
                    .as_ref(),
                *row,
            ),
            Source::Prepared(_, readers, row) => {
                readers
                    .get(index)
                    .ok_or("variadic argument index out of range")?(*row)
            }
            Source::Values(values, clone) => values
                .get(index)
                .map(clone)
                .ok_or_else(|| "variadic argument index out of range".into()),
        }
    }
    pub fn iter(&self) -> VariadicIter<'a, T> {
        VariadicIter {
            view: self.clone(),
            indices: 0..self.len(),
        }
    }
}
impl<'a, T: 'a> VariadicView<'a, Option<T>> {
    /// Check an argument's outer SQL NULL without converting its value.
    /// Nested child NULLs are separate, as in native optional accessors.
    pub fn is_null(&self, index: usize) -> Result<bool> {
        match &self.source {
            Source::Columns(columns, row, _) | Source::Prepared(columns, _, row) => {
                let column = columns
                    .get(index)
                    .ok_or("variadic argument index out of range")?;
                Ok(column.data_type() == &DataType::Null || column.is_null(*row))
            }
            Source::Values(values, _) => values
                .get(index)
                .map(Option::is_none)
                .ok_or_else(|| "variadic argument index out of range".into()),
        }
    }
    /// Conservative NULL metadata across the source batch (or test-value slice).
    pub fn may_have_nulls(&self) -> bool {
        match &self.source {
            Source::Columns(columns, _, _) | Source::Prepared(columns, _, _) => columns
                .iter()
                .any(|column| column.logical_null_count() != 0),
            Source::Values(values, _) => values.iter().any(Option::is_none),
        }
    }
    /// Skip SQL NULLs while preserving every decoding error as Err.
    pub fn skip_nulls(&self) -> impl DoubleEndedIterator<Item = Result<T>> + 'a {
        self.iter().filter_map(|value| match value {
            Ok(Some(value)) => Some(Ok(value)),
            Ok(None) => None,
            Err(error) => Some(Err(error)),
        })
    }
}

/// Iterator that reads exactly one argument per next/next_back call.
pub struct VariadicIter<'a, T> {
    view: VariadicView<'a, T>,
    indices: std::ops::Range<usize>,
}
impl<'a, T: 'a> Iterator for VariadicIter<'a, T> {
    type Item = Result<T>;
    fn next(&mut self) -> Option<Self::Item> {
        self.indices.next().map(|index| self.view.at(index))
    }
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.indices.size_hint()
    }
    fn nth(&mut self, n: usize) -> Option<Self::Item> {
        self.indices.nth(n).map(|index| self.view.at(index))
    }
}
impl<'a, T: 'a> DoubleEndedIterator for VariadicIter<'a, T> {
    fn next_back(&mut self) -> Option<Self::Item> {
        self.indices.next_back().map(|index| self.view.at(index))
    }
    fn nth_back(&mut self, n: usize) -> Option<Self::Item> {
        self.indices.nth_back(n).map(|index| self.view.at(index))
    }
}
impl<'a, T: 'a> ExactSizeIterator for VariadicIter<'a, T> {}
impl<'a, T: 'a> std::iter::FusedIterator for VariadicIter<'a, T> {}
impl<'a, T: 'a> IntoIterator for VariadicView<'a, T> {
    type Item = Result<T>;
    type IntoIter = VariadicIter<'a, T>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}
impl<'a, T: 'a> IntoIterator for &VariadicView<'a, T> {
    type Item = Result<T>;
    type IntoIter = VariadicIter<'a, T>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

/// Batch schema checks run even if user code never accesses its tail. Readers
/// cache primitive casts/validity once, sharing that metadata with row views.
/// Creating/cloning a view allocates nothing and converts no row values.
#[doc(hidden)]
pub struct VariadicReader<'a, T> {
    columns: &'a [ArrayRef],
    readers: Arc<Vec<Box<dyn Fn(usize) -> Result<T> + Send + Sync + 'a>>>,
    rows: usize,
    marker: PhantomData<T>,
}
fn validate_columns<'a, T: RowArgument<'a>>(columns: &'a [ArrayRef], rows: usize) -> Result<()> {
    for column in columns {
        if column.len() != rows {
            return Err("variadic column length mismatch".into());
        }
        if column.data_type() != &DataType::Null && !T::accepts(column.data_type()) {
            return Err(format!(
                "unexpected variadic argument type: {:?}",
                column.data_type()
            ));
        }
    }
    Ok(())
}
impl<'a, T: RowArgument<'a>> VariadicReader<'a, T> {
    pub fn new(columns: &'a [ArrayRef], rows: usize) -> Result<Self> {
        validate_columns::<T>(columns, rows)?;
        let readers = columns
            .iter()
            .map(|column| {
                Box::new(T::prepare(column.as_ref()))
                    as Box<dyn Fn(usize) -> Result<T> + Send + Sync + 'a>
            })
            .collect();
        Ok(Self {
            columns,
            readers: Arc::new(readers),
            rows,
            marker: PhantomData,
        })
    }
    pub fn row(&self, row: usize) -> Result<VariadicView<'a, T>> {
        if row >= self.rows {
            return Err("variadic row index out of range".into());
        }
        Ok(VariadicView {
            source: Source::Prepared(self.columns, self.readers.clone(), row),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{comparison::tests::allocation_count, ArrayView, Generic, Value};
    use arrow_array::{Int64Array, ListArray, NullArray, StringArray};

    #[test]
    fn prepared_views_share_readers_and_outlive_batch_reader_metadata() {
        let columns: Vec<ArrayRef> = vec![
            Arc::new(Int64Array::from(vec![Some(20), None, Some(22)]).slice(1, 2)),
            Arc::new(Int64Array::from(vec![Some(42), Some(1)])),
        ];
        let reader = VariadicReader::<Option<i64>>::new(&columns, 2).unwrap();
        let view = reader.row(0).unwrap();
        let copied = view.clone();
        fn assert_send_sync<T: Send + Sync>(_: &T) {}
        assert_send_sync(&copied);
        drop(reader);
        let (sum, allocations) = allocation_count(|| {
            copied
                .skip_nulls()
                .try_fold(0, |sum, value| value.map(|v| sum + v))
                .unwrap()
        });
        assert_eq!(sum, 42);
        assert_eq!(allocations, 0);
        assert_eq!(view.get(0).unwrap(), Some(None));
        assert_eq!(view.get(1).unwrap(), Some(Some(42)));
        assert_eq!(view.get(2).unwrap(), None);
    }

    #[test]
    fn prepared_variadic_keeps_unused_utf8_validation_deferred() {
        let ty = crate::runtime::parse_row_arrow_type("varchar").unwrap();
        let columns: Vec<ArrayRef> = [
            Value::Varchar("first".into()),
            Value::VarcharBytes(vec![0xff]),
            Value::Null,
        ]
        .into_iter()
        .map(|value| crate::value::build_array(&ty, &[value]).unwrap())
        .collect();
        let reader = VariadicReader::<Option<&str>>::new(&columns, 1).unwrap();
        let view = reader.row(0).unwrap();
        let mut iter = view.skip_nulls();
        assert_eq!(iter.next().unwrap().unwrap(), "first");
        assert!(!view.is_null(1).unwrap());
        assert!(iter.next().unwrap().is_err());
        assert!(iter.next().is_none());
        drop(reader);
        assert_eq!(view.at(0).unwrap(), Some("first"));
    }

    #[test]
    fn borrowed_variadic_preserves_nulls_bounds_and_empty_tails() {
        let columns: Vec<ArrayRef> = vec![
            Arc::new(Int64Array::from(vec![Some(20), None])),
            Arc::new(Int64Array::from(vec![None, Some(22)])),
        ];
        let reader = VariadicReader::<Option<i64>>::new(&columns, 2).unwrap();
        let first = reader.row(0).unwrap();
        assert!(first.may_have_nulls());
        assert!(!first.is_null(0).unwrap());
        assert!(first.is_null(1).unwrap());
        assert!(first.is_null(2).is_err());
        assert_eq!(first.get(0).unwrap(), Some(Some(20)));
        assert_eq!(first.get(1).unwrap(), Some(None));
        assert_eq!(first.get(2).unwrap(), None);
        assert!(first.at(2).is_err());
        assert_eq!(
            reader
                .row(1)
                .unwrap()
                .skip_nulls()
                .collect::<Result<Vec<_>>>()
                .unwrap(),
            vec![22]
        );
        assert!(reader.row(2).is_err());
        assert!(VariadicView::<i64>::new(&columns, 1)
            .unwrap()
            .at(0)
            .is_err());
        let empty = VariadicReader::<Option<i64>>::new(&[], 3).unwrap();
        assert!(empty.row(2).unwrap().is_empty());
        assert!(!empty.row(2).unwrap().may_have_nulls());
        assert_eq!(empty.row(2).unwrap().iter().next(), None);
        assert!(empty.row(3).is_err());
        assert!(VariadicReader::<i64>::new(&[], 0).unwrap().row(0).is_err());
        let unknown: Vec<ArrayRef> = vec![Arc::new(NullArray::new(1))];
        assert_eq!(
            VariadicView::<Option<i64>>::new(&unknown, 0)
                .unwrap()
                .at(0)
                .unwrap(),
            None
        );
        assert_eq!(
            VariadicView::<Option<i64>>::from_values(&[Some(20), None, Some(22)])
                .skip_nulls()
                .collect::<Result<Vec<_>>>()
                .unwrap(),
            vec![20, 22]
        );
    }

    #[test]
    fn batch_reader_checks_unused_argument_types_and_column_lengths() {
        let wrong: Vec<ArrayRef> = vec![Arc::new(StringArray::from(vec![None::<&str>]))];
        assert!(VariadicReader::<Option<i64>>::new(&wrong, 1).is_err());
        assert!(VariadicReader::<Option<i64>>::new(
            &[Arc::new(StringArray::from(Vec::<&str>::new()))],
            0
        )
        .is_err());
        assert!(
            VariadicView::<Option<i64>>::new(&[Arc::new(Int64Array::from(vec![1]))], 1).is_err()
        );
        let lengths: Vec<ArrayRef> = vec![
            Arc::new(Int64Array::from(vec![1])),
            Arc::new(Int64Array::from(vec![2, 3])),
        ];
        assert!(VariadicReader::<i64>::new(&lengths, 1).is_err());
    }

    #[test]
    fn iteration_reads_only_accessed_values_and_never_discards_errors() {
        let ty = crate::runtime::parse_row_arrow_type("varchar").unwrap();
        let columns: Vec<ArrayRef> = [
            Value::Varchar("first".into()),
            Value::VarcharBytes(vec![0xff]),
            Value::Null,
            Value::Varchar("last".into()),
        ]
        .into_iter()
        .map(|value| crate::value::build_array(&ty, &[value]).unwrap())
        .collect();
        let view = VariadicView::<Option<&str>>::new(&columns, 0).unwrap();
        // Like native OptionalAccessor::has_value, metadata checks do not
        // validate UTF-8 in non-NULL values.
        let (non_nulls, allocations) = allocation_count(|| {
            (0..view.len())
                .filter(|&index| !view.is_null(index).unwrap())
                .count()
        });
        assert_eq!(non_nulls, 3);
        assert_eq!(allocations, 0);
        assert!(view.may_have_nulls());
        assert_eq!(
            view.iter().take(1).collect::<Result<Vec<_>>>().unwrap(),
            vec![Some("first")]
        );
        // nth jumps over the invalid second argument, rather than decoding it.
        assert_eq!(view.iter().nth(3).unwrap().unwrap(), Some("last"));
        assert_eq!(view.iter().nth_back(3).unwrap().unwrap(), Some("first"));
        assert!(view.at(1).unwrap_err().contains("not UTF-8"));
        let mut skip = view.skip_nulls();
        assert_eq!(skip.next().unwrap().unwrap(), "first");
        assert!(skip.next().unwrap().is_err());
        assert_eq!(skip.next().unwrap().unwrap(), "last");
        assert!(skip.next().is_none());
        let mut iter = (&view).into_iter();
        assert_eq!(iter.len(), 4);
        assert_eq!(iter.next_back().unwrap().unwrap(), Some("last"));
        assert_eq!(iter.next().unwrap().unwrap(), Some("first"));
        assert_eq!(iter.len(), 2);
        assert!(iter.next().unwrap().is_err());
        assert_eq!(iter.next().unwrap().unwrap(), None);
        assert!(iter.next().is_none() && iter.next_back().is_none());
    }

    #[test]
    fn borrowed_variadic_is_allocation_free_and_keeps_nested_and_sliced_views() {
        let nested: ArrayRef = Arc::new(ListArray::from_iter_primitive::<
            arrow_array::types::Int64Type,
            _,
            _,
        >([
            Some(vec![Some(99)]),
            Some(vec![Some(20), None, Some(22)]),
        ]));
        let nested = nested.slice(1, 1);
        let text: ArrayRef = Arc::new(StringArray::from(vec!["discard", "borrowed"]));
        let text = text.slice(1, 1);
        let columns = vec![nested, text];
        let reader = VariadicReader::<Option<Generic<'_>>>::new(&columns, 1).unwrap();
        let ((len, sum, text), allocations) = allocation_count(|| {
            let view = reader.row(0).unwrap();
            let array = view
                .at(0)
                .unwrap()
                .unwrap()
                .get::<ArrayView<'_, i64>>()
                .unwrap();
            let sum = array
                .skip_nulls()
                .try_fold(0, |sum, value| value.map(|v| sum + v))
                .unwrap();
            let text = view.at(1).unwrap().unwrap().get::<&str>().unwrap();
            (view.len(), sum, text)
        });
        assert_eq!((len, sum, text), (2, 42, "borrowed"));
        assert_eq!(allocations, 0);
        let values = [Some("first"), None, Some("last")];
        let view = VariadicView::from_values(&values);
        assert!(!view.is_null(0).unwrap() && view.is_null(1).unwrap());
        assert!(view.may_have_nulls());
        assert!(view.is_null(3).is_err());
        let (sum, allocations) = allocation_count(|| {
            VariadicView::from_values(&values)
                .skip_nulls()
                .try_fold(0, |len, value| value.map(|v| len + v.len()))
                .unwrap()
        });
        assert_eq!(sum, 9);
        assert_eq!(allocations, 0);
    }
}
