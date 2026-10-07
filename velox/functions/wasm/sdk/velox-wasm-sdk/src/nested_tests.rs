use crate::scalar::{RowArgument, RowOutput, RowOutputBuilder};
use crate::value::build_array;
use crate::*;
use arrow_array::{Array, Int64Array, StringArray};
use arrow_schema::DataType;
use std::cmp::Ordering;

fn sql(text: &str) -> DataType {
    runtime::parse_arrow_type(text).unwrap()
}
fn owned<'a, T: RowOutput<'a>>(value: T) -> Value<'a> {
    Value::from_output(value.into_output())
}

#[test]
fn primitive_columns_preserve_mixed_borrowed_owned_nulls_and_bits() {
    use arrow_array::{Float32Array, Float64Array, Int32Array};
    let cases = [
        (
            DataType::Boolean,
            Value::Boolean(false),
            Value::Boolean(true),
        ),
        (
            DataType::Int8,
            Value::TinyInt(i8::MIN),
            Value::TinyInt(i8::MAX),
        ),
        (
            DataType::Int16,
            Value::SmallInt(i16::MIN),
            Value::SmallInt(i16::MAX),
        ),
        (
            DataType::Int32,
            Value::Integer(i32::MIN),
            Value::Integer(i32::MAX),
        ),
        (
            DataType::Int64,
            Value::BigInt(i64::MIN),
            Value::BigInt(i64::MAX),
        ),
        (
            DataType::Float32,
            Value::Real(-0.0),
            Value::Real(f32::INFINITY),
        ),
        (
            DataType::Float64,
            Value::Double(-0.0),
            Value::Double(f64::INFINITY),
        ),
    ];
    for (ty, first, last) in cases {
        let expected = build_array(
            &ty,
            &[last.clone(), first.clone(), Value::Null, Value::Null],
        )
        .unwrap();
        let output = {
            let source = build_array(&ty, &[first.clone(), Value::Null, last]).unwrap();
            let slice = source.slice(1, 2);
            build_array(
                &ty,
                &[
                    Value::Borrowed(Generic::new(slice.as_ref(), 1).unwrap()),
                    first,
                    Value::Null,
                    Value::Borrowed(Generic::new(slice.as_ref(), 0).unwrap()),
                ],
            )
            .unwrap()
        };
        assert_eq!(output.as_ref(), expected.as_ref());
        assert_eq!(build_array(&ty, &[]).unwrap().len(), 0);
    }
    let f32_bits = [0x7fc0_1234_u32, 0xffc0_5678, 0x8000_0000];
    let floats = Float32Array::from(f32_bits.map(f32::from_bits).to_vec());
    let output = build_array(
        &DataType::Float32,
        &[
            Value::Borrowed(Generic::new(&floats, 0).unwrap()),
            Value::Real(f32::from_bits(f32_bits[1])),
            Value::Borrowed(Generic::new(&floats, 2).unwrap()),
        ],
    )
    .unwrap();
    let output = output.as_any().downcast_ref::<Float32Array>().unwrap();
    for (index, bits) in f32_bits.into_iter().enumerate() {
        assert_eq!(output.value(index).to_bits(), bits);
    }
    let f64_bits = [
        0x7ff8_0000_0000_1234_u64,
        0xfff8_0000_0000_5678,
        0x8000_0000_0000_0000,
    ];
    let floats = Float64Array::from(f64_bits.map(f64::from_bits).to_vec());
    let output = build_array(
        &DataType::Float64,
        &[
            Value::Borrowed(Generic::new(&floats, 0).unwrap()),
            Value::Double(f64::from_bits(f64_bits[1])),
            Value::Borrowed(Generic::new(&floats, 2).unwrap()),
        ],
    )
    .unwrap();
    let output = output.as_any().downcast_ref::<Float64Array>().unwrap();
    for (index, bits) in f64_bits.into_iter().enumerate() {
        assert_eq!(output.value(index).to_bits(), bits);
    }
    let wrong = Int32Array::from(vec![Some(42), None]);
    assert!(build_array(
        &DataType::Int64,
        &[Value::Borrowed(Generic::new(&wrong, 0).unwrap())]
    )
    .is_err());
    assert!(build_array(&DataType::Boolean, &[Value::BigInt(1)]).is_err());
    // A NULL materializes independently of its source type, as before. Public
    // result writers separately validate the bound SQL type before building.
    assert!(build_array(
        &DataType::Int64,
        &[Value::Borrowed(Generic::new(&wrong, 1).unwrap())]
    )
    .unwrap()
    .is_null(0));
}

#[test]
fn typed_array_views_preserve_slices_nested_nulls_and_cast_contracts() {
    let ty = sql("array(array(bigint))");
    let values = build_array(
        &ty,
        &[
            Value::Array(vec![Value::Array(vec![Value::BigInt(999)])]),
            Value::Array(vec![
                Value::Array(vec![Value::BigInt(42), Value::Null]),
                Value::Null,
                Value::Array(vec![]),
            ]),
            Value::Null,
        ],
    )
    .unwrap()
    .slice(1, 2);
    let view = Generic::new(values.as_ref(), 0)
        .unwrap()
        .as_array::<ArrayView<'_, i64>>()
        .unwrap();
    assert_eq!(view.len(), 3);
    assert_eq!(view.get(0).unwrap().unwrap().get(0).unwrap(), Some(42));
    assert_eq!(view.get(0).unwrap().unwrap().get(1).unwrap(), None);
    assert!(view.get(1).unwrap().is_none());
    assert!(view.get(2).unwrap().unwrap().is_empty());
    assert!(view.get(3).is_err());
    assert!(view.contains_nulls().unwrap());
    assert!(view.null_free().is_err());
    let copied = build_array(&ty, &[owned(view.materialize().unwrap())]).unwrap();
    assert_eq!(copied.as_ref(), values.slice(0, 1).as_ref());
    let empty = build_array(&sql("array(bigint)"), &[Value::Array(vec![])]).unwrap();
    assert!(Generic::new(empty.as_ref(), 0)
        .unwrap()
        .try_cast::<ArrayView<'_, String>>()
        .unwrap()
        .is_none());
    assert!(
        <Option<ArrayView<'_, ArrayView<'_, i64>>>>::read(values.as_ref(), 1)
            .unwrap()
            .is_none()
    );
}

#[test]
fn typed_maps_rows_lookup_and_recursive_materialization() {
    let ty = sql("row(\"Case Name\" bigint,\"values\" map(varchar,array(bigint)))");
    let array = build_array(
        &ty,
        &[Value::Row(vec![
            Value::BigInt(42),
            Value::Map(vec![
                (
                    Value::Varchar("present".into()),
                    Value::Array(vec![Value::BigInt(7), Value::Null]),
                ),
                (Value::Varchar("null".into()), Value::Null),
            ]),
        ])],
    )
    .unwrap();
    let row = Generic::new(array.as_ref(), 0)
        .unwrap()
        .as_row::<(i64, MapView<'_, &str, ArrayView<'_, i64>>)>()
        .unwrap();
    assert_eq!(row.get::<0>().unwrap(), Some(42));
    assert_eq!(row.field("Case Name").unwrap().get::<i64>().unwrap(), 42);
    assert!(row.field("case name").is_err());
    assert!(row.at(2).is_err());
    let map = row.get::<1>().unwrap().unwrap();
    assert!(map.find("missing").unwrap().is_none());
    assert!(matches!(map.find("null").unwrap(), Some(None)));
    assert_eq!(map.at("present").unwrap().unwrap().get(0).unwrap(), Some(7));
    assert!(map.at("missing").is_err());
    assert!(map.find(42_i64).is_err());
    let copy = row.materialize().unwrap();
    assert_eq!(build_array(&ty, &[copy]).unwrap().as_ref(), array.as_ref());
    let map_copy = owned(map.materialize().unwrap());
    assert!(map_copy.contains_nulls().unwrap());
}

#[test]
fn writers_construct_resize_modify_and_copy_arbitrary_nested_results() {
    let mut array = ArrayWriter::<i64>::new();
    array.resize(3);
    *array.get_mut(0).unwrap() = Some(42);
    *array.add_item() = 7;
    array.add_null();
    assert_eq!(
        array.into_values(),
        vec![Some(42), None, None, Some(7), None]
    );
    let mut children = ArrayWriter::<ArrayWriter<String>>::new();
    children.add_item().push(Some("value".into()));
    children.add_null();
    let mut map = MapWriter::<String, ArrayWriter<ArrayWriter<String>>>::new();
    map.emplace("key".into(), Some(children));
    map.add_null("null".into());
    let mut row = RowWriter::new((Some(42_i64), Some(map), Some("remove".to_owned())));
    row.set_null_at::<2>();
    *row.get_mut::<0>() = None;
    let value = owned(row);
    let ty =
        sql("row(\"id\" bigint,\"data\" map(varchar,array(array(varchar))),\"extra\" varchar)");
    let built = build_array(&ty, &[value.clone(), Value::Null]).unwrap();
    let first = Generic::new(built.as_ref(), 0).unwrap();
    assert!(first.field("id").unwrap().is_null());
    assert_eq!(
        first
            .field("data")
            .unwrap()
            .map_entry(0)
            .unwrap()
            .1
            .element(0)
            .unwrap()
            .element(0)
            .unwrap()
            .get::<&str>()
            .unwrap(),
        "value"
    );
    assert!(built.is_null(1));
    assert!(first.materialize().unwrap().equals(&value).unwrap());
    let empty = build_array(&sql("row()"), &[Value::Row(vec![]), Value::Null]).unwrap();
    assert_eq!(empty.len(), 2);
    assert!(!empty.is_null(0));
    assert!(empty.is_null(1));
}

#[test]
fn complex_map_keys_preserve_nested_nulls_and_lookup_without_materializing() {
    let ty = sql("map(row(id bigint,tags array(bigint)),bigint)");
    let key = Value::Row(vec![Value::Null, Value::Array(vec![Value::Null])]);
    let other = Value::Row(vec![Value::BigInt(7), Value::Array(vec![])]);
    let value = Value::Map(vec![(key.clone(), Value::BigInt(42)), (other, Value::Null)]);
    let array = build_array(&ty, &[value.clone()]).unwrap();
    let generic = Generic::new(array.as_ref(), 0).unwrap();
    let map = generic.as_map::<Generic<'_>, i64>().unwrap();
    let borrowed_key = map.at_index(0).unwrap().0;
    assert!(borrowed_key.contains_nulls().unwrap());
    assert!(!borrowed_key.is_null());
    assert_eq!(map.find(borrowed_key).unwrap(), Some(Some(42)));
    assert_eq!(map.find(key).unwrap(), Some(Some(42)));
    assert_eq!(map.find(map.at_index(1).unwrap().0).unwrap(), Some(None));
    assert!(map
        .find(Value::Row(vec![Value::BigInt(99), Value::Array(vec![])]))
        .unwrap()
        .is_none());
    assert!(map.find(Value::Null).is_err());
    assert!(map.null_free().is_err());
    assert!(generic.materialize().unwrap().equals(&value).unwrap());
    let mut output = RowOutputBuilder::new(ty, 2);
    output.append(value);
    output.append(Value::Map(vec![(Value::Null, Value::BigInt(0))]));
    let (values, errors) = output.finish().unwrap();
    assert!(!values.is_null(0) && values.is_null(1));
    let errors = errors.as_any().downcast_ref::<StringArray>().unwrap();
    assert!(errors.is_null(0));
    assert!(errors.value(1).contains("map key"));
}

#[test]
fn writer_errors_are_per_row_and_map_keys_cannot_be_null() {
    let ty = sql("map(array(bigint),varchar)");
    let mut output = RowOutputBuilder::new(ty, 3);
    output.append(Value::Map(vec![(
        Value::Array(vec![Value::BigInt(42)]),
        Value::Varchar("ok".into()),
    )]));
    output.append(Value::Map(vec![(
        Value::Null,
        Value::Varchar("bad".into()),
    )]));
    output.append(Value::Map(vec![]));
    let (values, errors) = output.finish().unwrap();
    assert!(!values.is_null(0) && values.is_null(1) && !values.is_null(2));
    let errors = errors.as_any().downcast_ref::<StringArray>().unwrap();
    assert!(errors.is_null(0) && errors.value(1).contains("map key") && errors.is_null(2));
    let mut decimal = RowOutputBuilder::new(sql("decimal(2,0)"), 2);
    decimal.append(Value::Decimal(Decimal {
        value: 99,
        precision: 2,
        scale: 0,
    }));
    decimal.append(Value::Decimal(Decimal {
        value: 100,
        precision: 2,
        scale: 0,
    }));
    let (values, errors) = decimal.finish().unwrap();
    assert!(!values.is_null(0) && values.is_null(1));
    assert!(!errors.is_null(1));
}

#[test]
fn comparison_matches_velox_nested_null_nan_and_map_semantics() {
    let flags = CompareFlags::equality(NullHandling::NullAsIndeterminate);
    let a = Value::Array(vec![Value::Null, Value::BigInt(1)]);
    let b = Value::Array(vec![Value::Null, Value::BigInt(2)]);
    assert_eq!(a.compare(&b, flags).unwrap(), Some(Ordering::Less));
    assert_eq!(a.compare(&a, flags).unwrap(), None);
    assert!(a.equals(&a).unwrap());
    assert!(a
        .compare(
            &a,
            CompareFlags {
                null_handling: NullHandling::NullAsIndeterminate,
                ..CompareFlags::default()
            }
        )
        .is_err());
    let map_a = Value::Map(vec![
        (Value::BigInt(2), Value::Null),
        (Value::BigInt(1), Value::BigInt(42)),
    ]);
    let map_b = Value::Map(vec![
        (Value::BigInt(1), Value::BigInt(42)),
        (Value::BigInt(2), Value::Null),
    ]);
    assert!(map_a.equals(&map_b).unwrap());
    assert_eq!(map_a.hash().unwrap(), map_b.hash().unwrap());
    assert_eq!(map_a.compare(&map_b, flags).unwrap(), None);
    let map_c = Value::Map(vec![(Value::BigInt(3), Value::Null)]);
    assert!(map_a.compare(&map_c, flags).unwrap().is_some());
    let nan1 = Value::Double(f64::NAN);
    let nan2 = Value::Double(f64::from_bits(0xfff8000000000001));
    assert!(nan1.equals(&nan2).unwrap());
    assert_eq!(nan1.hash().unwrap(), nan2.hash().unwrap());
    assert_eq!(
        nan1.compare(&Value::Double(f64::INFINITY), CompareFlags::default())
            .unwrap(),
        Some(Ordering::Greater)
    );
    assert_eq!(
        Value::Double(0.0).hash().unwrap(),
        Value::Double(-0.0).hash().unwrap()
    );
    assert_eq!(
        Value::Null
            .compare(
                &Value::BigInt(1),
                CompareFlags {
                    ascending: false,
                    ..CompareFlags::default()
                }
            )
            .unwrap(),
        Some(Ordering::Less)
    );
}

#[test]
fn complex_key_lookup_and_null_free_views() {
    let ty = sql("map(array(bigint),varchar)");
    let array = build_array(
        &ty,
        &[Value::Map(vec![(
            Value::Array(vec![Value::BigInt(42)]),
            Value::Varchar("found".into()),
        )])],
    )
    .unwrap();
    let map = Generic::new(array.as_ref(), 0)
        .unwrap()
        .as_map::<ArrayView<'_, i64>, &str>()
        .unwrap();
    let key = ArrayWriter::from(vec![Some(42_i64)]);
    assert_eq!(map.at(key).unwrap(), Some("found"));
    let nullfree = map.null_free().unwrap();
    assert_eq!(nullfree.at_index(0).unwrap().1, "found");
    let ty = sql("array(bigint)");
    let array = build_array(
        &ty,
        &[Value::Array(vec![Value::BigInt(20), Value::BigInt(22)])],
    )
    .unwrap();
    let view = Generic::new(array.as_ref(), 0)
        .unwrap()
        .get::<NullFreeArrayView<'_, i64>>()
        .unwrap();
    assert_eq!(
        view.iter().collect::<Result<Vec<_>, _>>().unwrap(),
        vec![20, 22]
    );
    let nan_map = build_array(
        &sql("map(double,bigint)"),
        &[Value::Map(vec![(
            Value::Double(f64::NAN),
            Value::BigInt(42),
        )])],
    )
    .unwrap();
    let map = Generic::new(nan_map.as_ref(), 0)
        .unwrap()
        .as_map::<f64, i64>()
        .unwrap();
    assert_eq!(
        map.at(f64::from_bits(0x7ff8000000000042)).unwrap(),
        Some(42)
    );
    let empty = Int64Array::from(vec![1]);
    assert!(Generic::new(&empty, 0).unwrap().as_row::<()>().is_err());
}

#[test]
fn timestamps_retain_full_velox_range_at_any_nesting_depth() {
    let values = [
        Timestamp::from_seconds_nanos(-14831769600, 123456789).unwrap(),
        Timestamp::from_seconds_nanos(16725225600, 987654321).unwrap(),
        Timestamp::from_seconds_nanos(i64::MIN / 1000 - 1, 0).unwrap(),
        Timestamp::from_seconds_nanos(i64::MAX / 1000, 999999999).unwrap(),
    ];
    let ty = runtime::parse_row_arrow_type("array(timestamp)").unwrap();
    let input = Value::Array(
        values
            .iter()
            .copied()
            .map(Value::Timestamp)
            .chain([Value::Null])
            .collect(),
    );
    let array = build_array(&ty, &[input.clone()]).unwrap();
    let view = Generic::new(array.as_ref(), 0)
        .unwrap()
        .as_array::<Timestamp>()
        .unwrap();
    for (i, expected) in values.iter().enumerate() {
        assert_eq!(view.get(i).unwrap(), Some(*expected));
    }
    assert_eq!(view.get(4).unwrap(), None);
    assert!(Generic::new(array.as_ref(), 0)
        .unwrap()
        .materialize()
        .unwrap()
        .equals(&input)
        .unwrap());
    assert!(Timestamp::from_seconds_nanos(0, 1_000_000_000).is_err());
    assert!(Timestamp::from_seconds_nanos(i64::MAX, 0).is_err());
    // A user SQL row with matching field names has no timestamp marker.
    assert!(!Timestamp::is_wire_type(&sql(
        "row(\"seconds\" bigint,\"nanos\" bigint)"
    )));
}

#[test]
fn borrowed_iteration_mutable_row_fields_and_owned_sql_hash_keys() {
    let array = build_array(
        &sql("array(bigint)"),
        &[Value::Array(vec![
            Value::BigInt(42),
            Value::Null,
            Value::BigInt(7),
        ])],
    )
    .unwrap();
    let view = Generic::new(array.as_ref(), 0)
        .unwrap()
        .as_array::<i64>()
        .unwrap();
    let mut iter = (&view).into_iter();
    assert_eq!(iter.len(), 3);
    assert_eq!(iter.next_back().unwrap().unwrap(), Some(7));
    assert_eq!(iter.next().unwrap().unwrap(), Some(42));
    assert_eq!(iter.next().unwrap().unwrap(), None);
    assert!(iter.next().is_none());
    let nonnull = build_array(
        &sql("array(bigint)"),
        &[Value::Array(vec![Value::BigInt(42)])],
    )
    .unwrap();
    let nullfree = Generic::new(nonnull.as_ref(), 0)
        .unwrap()
        .as_array::<i64>()
        .unwrap()
        .null_free()
        .unwrap();
    assert_eq!(nullfree.into_iter().next().unwrap().unwrap(), 42);
    assert_eq!(
        nullfree.materialize().unwrap().into_values(),
        vec![Some(42)]
    );
    let mut writer = RowWriter::<(Option<i64>, Option<ArrayWriter<String>>)>::default();
    *writer.get_writer_at::<0>() = 42;
    writer.get_writer_at::<1>().add_item().push_str("hello");
    writer.set_null_at::<0>();
    let result = owned(writer);
    assert!(result.contains_nulls().unwrap());
    let numbers = arrow_array::Float64Array::from(vec![
        Some(f64::NAN),
        Some(-f64::NAN),
        Some(0.0),
        Some(-0.0),
        None,
        None,
    ]);
    let mut set = std::collections::HashSet::new();
    for row in 0..numbers.len() {
        set.insert(Generic::new(&numbers, row).unwrap().sql_key().unwrap());
    }
    assert_eq!(set.len(), 3); // NaN, zero, NULL use SQL value equality.
    drop(numbers); // Keys own their contents independently of the input batch.
    assert_eq!(set.len(), 3);
}

#[test]
fn hugeints_roundtrip_all_bits_in_typed_and_nested_values() {
    let ty = runtime::parse_row_arrow_type("array(hugeint)").unwrap();
    let ints = [i128::MIN, i128::MAX, -(1_i128 << 100), -1, 0];
    let array = build_array(
        &ty,
        &[Value::Array(
            ints.iter()
                .map(|v| Value::HugeInt(*v))
                .chain([Value::Null])
                .collect(),
        )],
    )
    .unwrap();
    let view = Generic::new(array.as_ref(), 0)
        .unwrap()
        .as_array::<i128>()
        .unwrap();
    assert_eq!(
        view.materialize().unwrap().into_values(),
        ints.into_iter().map(Some).chain([None]).collect::<Vec<_>>()
    );
    let input = build_array(
        &hugeint_wire_type(),
        &[Value::HugeInt(i128::MIN), Value::Null],
    )
    .unwrap();
    assert_eq!(
        Generic::new(input.as_ref(), 0)
            .unwrap()
            .get::<i128>()
            .unwrap(),
        i128::MIN
    );
    assert!(Generic::new(input.as_ref(), 0)
        .unwrap()
        .try_cast::<RowView<'_, (i64, i64)>>()
        .unwrap()
        .is_none());
    let mut output = RowOutputBuilder::new(hugeint_wire_type(), 2);
    output.append(Some(i128::MAX));
    output.append(None::<i128>);
    let (output, errors) = output.finish().unwrap();
    assert_eq!(
        Generic::new(output.as_ref(), 0)
            .unwrap()
            .get::<i128>()
            .unwrap(),
        i128::MAX
    );
    assert!(output.is_null(1));
    assert!(errors.is_null(0));
}

#[test]
fn varchar_bytes_preserve_invalid_utf8_and_are_distinct_from_sql_rows() {
    let ty = runtime::parse_row_arrow_type("array(varchar)").unwrap();
    let input = build_array(
        &ty,
        &[Value::Array(vec![
            Value::Varchar("velox".into()),
            Value::VarcharBytes(vec![0xff, 0, 0xfe]),
            Value::Null,
        ])],
    )
    .unwrap();
    let view = Generic::new(input.as_ref(), 0)
        .unwrap()
        .as_array::<VarcharBytes<&[u8]>>()
        .unwrap();
    assert_eq!(view.get(1).unwrap().unwrap().0, &[0xff, 0, 0xfe]);
    assert_eq!(
        view.materialize().unwrap().into_values()[1]
            .as_ref()
            .unwrap()
            .0,
        vec![0xff, 0, 0xfe]
    );
    let invalid = Generic::new(input.as_ref(), 0).unwrap().element(1).unwrap();
    assert!(invalid.get::<&str>().is_err());
    assert!(invalid
        .try_cast::<RowView<'_, (Vec<u8>,)>>()
        .unwrap()
        .is_none());
    let legacy = StringArray::from(vec!["velox"]);
    let encoded = Generic::new(input.as_ref(), 0).unwrap().element(0).unwrap();
    let legacy = Generic::new(&legacy, 0).unwrap();
    assert!(encoded.equals(&legacy).unwrap());
    assert_eq!(encoded.hash().unwrap(), legacy.hash().unwrap());
    assert!(encoded.sql_key().unwrap() == legacy.sql_key().unwrap());
    let mut output = RowOutputBuilder::new(varchar_wire_type(), 2);
    output.append(VarcharBytes(&[0xff, 0, 0xfe][..]));
    output.append(None::<String>);
    let (output, errors) = output.finish().unwrap();
    assert_eq!(
        Generic::new(output.as_ref(), 0)
            .unwrap()
            .get::<VarcharBytes<&[u8]>>()
            .unwrap()
            .0,
        &[0xff, 0, 0xfe]
    );
    assert!(output.is_null(1) && errors.is_null(0));
}

#[test]
fn structured_status_encoding_preserves_codes_and_plain_error_messages() {
    use crate::scalar::EncodeRowError;
    let typed = RowError::new(StatusCode::KeyError, "missing\nkey");
    assert_eq!(
        (&typed).encode_row_error(),
        "\u{001e}velox.status.v1:4\nmissing\nkey"
    );
    assert_eq!(
        (&"plain error".to_owned()).encode_row_error(),
        "plain error"
    );
    assert_eq!(typed.to_string(), "KeyError: missing\nkey");
    let plain = "\u{001e}velox.status.v1:2\nplain text".to_owned();
    assert_eq!(
        (&plain).encode_row_error(),
        format!("\u{001e}velox.message.v1:{plain}")
    );
}
