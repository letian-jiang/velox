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

use std::collections::BTreeMap;
use std::io::Cursor;

use roaring::RoaringBitmap;
use velox_wasm_sdk::{
    velox_aggregate, velox_arrow_scalar, velox_scalar, Date32, Timestamp, VeloxAggregate,
};

#[velox_arrow_scalar(arguments = ["array<bigint>"], returns = "array<bigint>")]
pub fn array_identity(
    batch: &velox_wasm_sdk::arrow_array::RecordBatch,
) -> Result<velox_wasm_sdk::arrow_array::ArrayRef, String> {
    Ok(batch.column(0).clone())
}

#[velox_arrow_scalar(arguments = ["map<varchar,bigint>"], returns = "map<varchar,bigint>")]
pub fn map_identity(
    batch: &velox_wasm_sdk::arrow_array::RecordBatch,
) -> Result<velox_wasm_sdk::arrow_array::ArrayRef, String> {
    Ok(batch.column(0).clone())
}

#[velox_arrow_scalar(arguments = ["row<a:bigint,b:array<varchar>>"], returns = "row<a:bigint,b:array<varchar>>")]
pub fn row_identity(
    batch: &velox_wasm_sdk::arrow_array::RecordBatch,
) -> Result<velox_wasm_sdk::arrow_array::ArrayRef, String> {
    Ok(batch.column(0).clone())
}

pub struct ArraySum;

#[velox_aggregate(argument_types = ["array<bigint>"])]
impl VeloxAggregate for ArraySum {
    type State = i64;
    type Input<'a> = (Option<velox_wasm_sdk::arrow_array::ArrayRef>,);
    type Output = i64;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(0)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        use velox_wasm_sdk::arrow_array::{Array, Int64Array, ListArray};
        let Some(array) = input.0 else {
            return Ok(());
        };
        let list = array
            .as_any()
            .downcast_ref::<ListArray>()
            .ok_or("expected an Arrow list")?;
        let values = list.value(0);
        let values = values
            .as_any()
            .downcast_ref::<Int64Array>()
            .ok_or("expected Arrow Int64 list values")?;
        for index in 0..values.len() {
            if !values.is_null(index) {
                *state = state
                    .checked_add(values.value(index))
                    .ok_or("sum overflow")?;
            }
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        Ok(state.to_le_bytes().to_vec())
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        let bytes: [u8; 8] = intermediate
            .try_into()
            .map_err(|_| "invalid array sum intermediate".to_owned())?;
        *state = state
            .checked_add(i64::from_le_bytes(bytes))
            .ok_or("sum overflow")?;
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok(*state)
    }
}

pub struct ArrayCollect;

#[velox_aggregate(return_type = "array<bigint>")]
impl VeloxAggregate for ArrayCollect {
    type State = Vec<i64>;
    type Input<'a> = (Option<i64>,);
    type Output = velox_wasm_sdk::arrow_array::ArrayRef;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(Vec::new())
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            state.push(value);
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut bytes = Vec::with_capacity(state.len() * 8);
        for value in state {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        Ok(bytes)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() % 8 != 0 {
            return Err("invalid array collect intermediate".to_owned());
        }
        for chunk in intermediate.chunks_exact(8) {
            state.push(i64::from_le_bytes(chunk.try_into().unwrap()));
        }
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        use velox_wasm_sdk::arrow_array::{types::Int64Type, ListArray};
        let values: Vec<Option<i64>> = state.iter().copied().map(Some).collect();
        Ok(std::sync::Arc::new(ListArray::from_iter_primitive::<
            Int64Type,
            _,
            _,
        >(vec![Some(values)])))
    }
}

#[velox_scalar]
pub fn add_i64(left: i64, right: i64) -> i64 {
    left + right
}

#[velox_scalar(name = "overloaded_add")]
pub fn overloaded_add_i64(left: i64, right: i64) -> i64 {
    left + right
}

#[velox_scalar(name = "overloaded_add")]
pub fn overloaded_add_f64(left: f64, right: f64) -> f64 {
    left + right
}

#[velox_scalar]
pub fn prefix(prefix: &str, value: Option<&str>) -> Option<String> {
    value.map(|value| {
        let mut result = String::with_capacity(prefix.len() + value.len());
        result.push_str(prefix);
        result.push_str(value);
        result
    })
}

#[velox_scalar(name = "not_bool")]
pub fn not(value: bool) -> bool {
    !value
}

#[velox_scalar]
pub fn widen(a: i8, b: i16, c: i32) -> i64 {
    i64::from(a) + i64::from(b) + i64::from(c)
}

#[velox_scalar]
pub fn hypotenuse(a: f32, b: f64) -> f64 {
    f64::from(a).hypot(b)
}

#[velox_scalar]
pub fn binary_length(value: &[u8]) -> i32 {
    value.len() as i32
}

#[velox_scalar]
pub fn next_date(value: Date32) -> Date32 {
    Date32(value.0 + 1)
}

#[velox_scalar]
pub fn timestamp_identity(value: Timestamp) -> Timestamp {
    value
}

#[velox_scalar]
pub fn checked_divide(dividend: i64, divisor: i64) -> Result<i64, String> {
    if divisor == 0 {
        Err("division by zero".to_owned())
    } else {
        Ok(dividend / divisor)
    }
}

fn deserialize_bitmap(bytes: &[u8]) -> Result<RoaringBitmap, String> {
    let mut input = Cursor::new(bytes);
    let bitmap = RoaringBitmap::deserialize_from(&mut input)
        .map_err(|error| format!("invalid RoaringBitmap payload: {error}"))?;
    if input.position() != bytes.len() as u64 {
        return Err("RoaringBitmap payload contains trailing bytes".to_owned());
    }
    Ok(bitmap)
}

fn serialize_bitmap(bitmap: &RoaringBitmap) -> Result<Vec<u8>, String> {
    let mut output = Vec::with_capacity(bitmap.serialized_size());
    bitmap
        .serialize_into(&mut output)
        .map_err(|error| format!("cannot serialize RoaringBitmap: {error}"))?;
    Ok(output)
}

#[velox_scalar]
pub fn bitmap_union(left: &[u8], right: &[u8]) -> Result<Vec<u8>, String> {
    let mut result = deserialize_bitmap(left)?;
    result |= deserialize_bitmap(right)?;
    serialize_bitmap(&result)
}

#[velox_scalar]
pub fn bitmap_intersection(left: &[u8], right: &[u8]) -> Result<Vec<u8>, String> {
    let mut result = deserialize_bitmap(left)?;
    result &= deserialize_bitmap(right)?;
    serialize_bitmap(&result)
}

#[velox_scalar]
pub fn bitmap_difference(left: &[u8], right: &[u8]) -> Result<Vec<u8>, String> {
    let mut result = deserialize_bitmap(left)?;
    result -= deserialize_bitmap(right)?;
    serialize_bitmap(&result)
}

pub struct Average;

pub struct AverageState {
    sum: f64,
    count: u64,
}

#[velox_aggregate(prefix = "avg_f64")]
impl VeloxAggregate for Average {
    type State = AverageState;
    type Input<'a> = (Option<f64>,);
    type Output = Option<f64>;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(AverageState { sum: 0.0, count: 0 })
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            state.sum += value;
            state.count += 1;
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut output = Vec::with_capacity(16);
        output.extend_from_slice(&state.sum.to_le_bytes());
        output.extend_from_slice(&state.count.to_le_bytes());
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() != 16 {
            return Err("average intermediate must contain 16 bytes".to_owned());
        }
        state.sum += f64::from_le_bytes(intermediate[0..8].try_into().unwrap());
        state.count += u64::from_le_bytes(intermediate[8..16].try_into().unwrap());
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok((state.count != 0).then(|| state.sum / state.count as f64))
    }
}

pub struct SumI64;

#[velox_aggregate]
impl VeloxAggregate for SumI64 {
    type State = Option<i64>;
    type Input<'a> = (Option<i64>,);
    type Output = Option<i64>;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(None)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            *state = Some(
                state
                    .unwrap_or(0)
                    .checked_add(value)
                    .ok_or("sum overflow")?,
            );
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut output = Vec::with_capacity(9);
        output.push(u8::from(state.is_some()));
        if let Some(value) = state {
            output.extend_from_slice(&value.to_le_bytes());
        }
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        match intermediate {
            [0] => Ok(()),
            [1, bytes @ ..] if bytes.len() == 8 => Self::update(
                state,
                (Some(i64::from_le_bytes(bytes.try_into().unwrap())),),
            ),
            _ => Err("invalid sum intermediate".to_owned()),
        }
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok(*state)
    }
}

pub struct LongestString;

#[velox_aggregate(ignore_duplicates = true)]
impl VeloxAggregate for LongestString {
    type State = Option<String>;
    type Input<'a> = (Option<&'a str>,);
    type Output = Option<String>;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(None)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            if state
                .as_ref()
                .map_or(true, |current| value.len() > current.len())
            {
                *state = Some(value.to_owned());
            }
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let mut output = Vec::new();
        output.push(u8::from(state.is_some()));
        if let Some(value) = state {
            output.extend_from_slice(value.as_bytes());
        }
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        let Some((&present, bytes)) = intermediate.split_first() else {
            return Err("invalid longest-string intermediate".to_owned());
        };
        match present {
            0 if bytes.is_empty() => Ok(()),
            1 => Self::update(
                state,
                (Some(
                    std::str::from_utf8(bytes).map_err(|error| error.to_string())?,
                ),),
            ),
            _ => Err("invalid longest-string intermediate".to_owned()),
        }
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        Ok(state.clone())
    }
}

pub struct FunnelSteps;

#[velox_aggregate(ignore_duplicates = true)]
impl VeloxAggregate for FunnelSteps {
    type State = u64;
    type Input<'a> = (Option<i64>,);
    type Output = i64;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(0)
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(stage) = input.0 {
            if !(1..=63).contains(&stage) {
                return Err("funnel stage must be between 1 and 63".to_owned());
            }
            *state |= 1_u64 << (stage - 1);
        }
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        Ok(state.to_le_bytes().to_vec())
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() != 8 {
            return Err("funnel intermediate must contain 8 bytes".to_owned());
        }
        *state |= u64::from_le_bytes(intermediate.try_into().unwrap());
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        let completed = (!state).trailing_zeros().min(63);
        Ok(i64::from(completed))
    }
}

pub struct WindowFunnel;

pub struct WindowFunnelState {
    window: Option<i64>,
    events: BTreeMap<i64, u64>,
}

#[velox_aggregate(ignore_duplicates = true)]
impl VeloxAggregate for WindowFunnel {
    type State = WindowFunnelState;
    type Input<'a> = (Option<i64>, Option<i64>, Option<i64>);
    type Output = i64;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error> {
        Ok(WindowFunnelState {
            window: None,
            events: BTreeMap::new(),
        })
    }

    fn destroy(_state: Self::State) {}

    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        let (Some(timestamp), Some(stage), Some(window)) = input else {
            return Ok(());
        };
        if window < 0 {
            return Err("window must be non-negative".to_owned());
        }
        if !(1..=63).contains(&stage) {
            return Err("funnel stage must be between 1 and 63".to_owned());
        }
        Self::set_window(state, window)?;
        *state.events.entry(timestamp).or_default() |= 1_u64 << (stage - 1);
        Ok(())
    }

    fn serialize(state: &Self::State) -> Result<Vec<u8>, Self::Error> {
        let event_count = u32::try_from(state.events.len())
            .map_err(|_| "too many window-funnel events".to_owned())?;
        let mut output = Vec::with_capacity(12 + state.events.len() * 16);
        output.extend_from_slice(&state.window.unwrap_or(-1).to_le_bytes());
        output.extend_from_slice(&event_count.to_le_bytes());
        for (&timestamp, &stage_mask) in &state.events {
            output.extend_from_slice(&timestamp.to_le_bytes());
            output.extend_from_slice(&stage_mask.to_le_bytes());
        }
        Ok(output)
    }

    fn merge(state: &mut Self::State, intermediate: &[u8]) -> Result<(), Self::Error> {
        if intermediate.len() < 12 || (intermediate.len() - 12) % 16 != 0 {
            return Err("invalid window-funnel intermediate state".to_owned());
        }
        let window = i64::from_le_bytes(intermediate[0..8].try_into().unwrap());
        let event_count = u32::from_le_bytes(intermediate[8..12].try_into().unwrap()) as usize;
        if intermediate.len() != 12 + event_count * 16 {
            return Err("invalid window-funnel intermediate state".to_owned());
        }
        if window == -1 && event_count == 0 {
            return Ok(());
        }
        if window < 0 {
            return Err("invalid window-funnel window".to_owned());
        }
        Self::set_window(state, window)?;
        for event in intermediate[12..].chunks_exact(16) {
            let timestamp = i64::from_le_bytes(event[0..8].try_into().unwrap());
            let stage_mask = u64::from_le_bytes(event[8..16].try_into().unwrap());
            *state.events.entry(timestamp).or_default() |= stage_mask;
        }
        Ok(())
    }

    fn finalize(state: &Self::State) -> Result<Self::Output, Self::Error> {
        let Some(window) = state.window else {
            return Ok(0);
        };
        let mut starts = [None; 63];
        let mut completed = 0_usize;
        for (&timestamp, &stage_mask) in &state.events {
            for stage in 0..63 {
                if stage_mask & (1_u64 << stage) == 0 {
                    continue;
                }
                if stage == 0 {
                    starts[0] = Some(timestamp);
                    completed = completed.max(1);
                    continue;
                }
                let Some(start) = starts[stage - 1] else {
                    continue;
                };
                if timestamp
                    .checked_sub(start)
                    .is_some_and(|elapsed| elapsed <= window)
                {
                    starts[stage] = Some(starts[stage].map_or(start, |current| current.max(start)));
                    completed = completed.max(stage + 1);
                }
            }
        }
        Ok(completed as i64)
    }
}

impl WindowFunnel {
    fn set_window(state: &mut WindowFunnelState, window: i64) -> Result<(), String> {
        match state.window {
            None => state.window = Some(window),
            Some(current) if current != window => {
                return Err("window must be constant within each group".to_owned());
            }
            Some(_) => {}
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn row_function_remains_directly_testable() {
        assert_eq!(add_i64(20, 22), 42);
        assert_eq!(checked_divide(84, 2), Ok(42));
        assert_eq!(checked_divide(84, 0), Err("division by zero".to_owned()));

        let left: RoaringBitmap = [1, 2, 65_537].into_iter().collect();
        let right: RoaringBitmap = [2, 3, 65_537].into_iter().collect();
        let left = serialize_bitmap(&left).unwrap();
        let right = serialize_bitmap(&right).unwrap();
        assert_eq!(
            deserialize_bitmap(&bitmap_union(&left, &right).unwrap()).unwrap(),
            [1, 2, 3, 65_537].into_iter().collect()
        );
        assert_eq!(
            deserialize_bitmap(&bitmap_intersection(&left, &right).unwrap()).unwrap(),
            [2, 65_537].into_iter().collect()
        );
        assert_eq!(
            deserialize_bitmap(&bitmap_difference(&left, &right).unwrap()).unwrap(),
            [1].into_iter().collect()
        );
    }

    #[test]
    fn aggregate_row_apis_remain_directly_testable() {
        let mut average = Average::create().unwrap();
        Average::update(&mut average, (Some(2.0),)).unwrap();
        Average::update(&mut average, (None,)).unwrap();
        Average::update(&mut average, (Some(4.0),)).unwrap();
        let serialized = Average::serialize(&average).unwrap();
        let mut merged_average = Average::create().unwrap();
        Average::merge(&mut merged_average, &serialized).unwrap();
        assert_eq!(Average::finalize(&merged_average).unwrap(), Some(3.0));

        let mut sum = SumI64::create().unwrap();
        SumI64::update(&mut sum, (Some(20),)).unwrap();
        SumI64::update(&mut sum, (Some(22),)).unwrap();
        assert_eq!(SumI64::finalize(&sum).unwrap(), Some(42));

        let mut longest = LongestString::create().unwrap();
        LongestString::update(&mut longest, (Some("short"),)).unwrap();
        LongestString::update(&mut longest, (Some("a longer value"),)).unwrap();
        assert_eq!(
            LongestString::finalize(&longest).unwrap().as_deref(),
            Some("a longer value")
        );

        let mut funnel = FunnelSteps::create().unwrap();
        FunnelSteps::update(&mut funnel, (Some(1),)).unwrap();
        FunnelSteps::update(&mut funnel, (Some(3),)).unwrap();
        assert_eq!(FunnelSteps::finalize(&funnel).unwrap(), 1);
        let serialized = FunnelSteps::serialize(&funnel).unwrap();
        let mut merged_funnel = FunnelSteps::create().unwrap();
        FunnelSteps::merge(&mut merged_funnel, &serialized).unwrap();
        FunnelSteps::update(&mut merged_funnel, (Some(2),)).unwrap();
        assert_eq!(FunnelSteps::finalize(&merged_funnel).unwrap(), 3);

        let mut window_funnel = WindowFunnel::create().unwrap();
        WindowFunnel::update(&mut window_funnel, (Some(100), Some(1), Some(30))).unwrap();
        WindowFunnel::update(&mut window_funnel, (Some(120), Some(2), Some(30))).unwrap();
        WindowFunnel::update(&mut window_funnel, (Some(140), Some(3), Some(30))).unwrap();
        assert_eq!(WindowFunnel::finalize(&window_funnel).unwrap(), 2);

        let serialized = WindowFunnel::serialize(&window_funnel).unwrap();
        let mut merged_window_funnel = WindowFunnel::create().unwrap();
        WindowFunnel::merge(&mut merged_window_funnel, &serialized).unwrap();
        WindowFunnel::update(&mut merged_window_funnel, (Some(125), Some(3), Some(30))).unwrap();
        assert_eq!(WindowFunnel::finalize(&merged_window_funnel).unwrap(), 3);
    }
}
