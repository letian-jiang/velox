# Velox WebAssembly UDF and UDAF SDK

The SDK keeps user code row-oriented and generates the Arrow IPC batch export
expected by the Velox Wasm function wrappers.

Create a Rust library with `crate-type = ["cdylib"]` and add the local SDK as a
dependency:

```toml
[lib]
crate-type = ["cdylib"]

[dependencies]
velox-wasm-sdk = { path = "/path/to/velox/functions/wasm/sdk/velox-wasm-sdk" }
```

```rust
use velox_wasm_sdk::velox_scalar;

#[velox_scalar]
fn add_i64(left: i64, right: i64) -> i64 {
    left + right
}

#[velox_scalar]
fn checked_divide(left: i64, right: i64) -> Result<i64, String> {
    (right != 0)
        .then(|| left / right)
        .ok_or_else(|| "division by zero".to_owned())
}
```

Build a `cdylib` for the bare WebAssembly target with SIMD enabled:

```shell
RUSTFLAGS="-C target-feature=+simd128" \
  cargo build --release --target wasm32-unknown-unknown
```

The macro supports `bool`, `i8`, `i16`, `i32`, `i64`, `i128` (`HUGEINT`),
`f32`, `f64`, owned or borrowed UTF-8 strings, `VarcharBytes<&[u8]>` /
`VarcharBytes<Vec<u8>>` for byte-preserving VARCHAR, owned or borrowed binary values,
`Date32`, and `Timestamp`. Use `Option<T>` for nullable inputs and outputs.
Return `Result<T, E>` with `E: ToString` for a per-row error; `TRY` makes only the
failing rows null. Panics and traps are batch errors and do not identify a single failing row. `Date32` stores epoch days.
`Timestamp(pub i128)` stores epoch nanoseconds and covers Velox's complete
seconds/nanoseconds range. Prefer `Timestamp::from_seconds_nanos(seconds,nanos)`
for validated construction; `seconds()` and `nanos()` recover its components.

## Nested values: typed views and writers

All new `velox_scalar` declarations use the same row adapter, including
primitive-only signatures. Scalar authors receive **one SQL row**. `ArrayView`, `MapView`, `RowView`, and
`Generic` borrow values from that row. Arrow IPC, batch iteration, and result
encoding are handled by the adapter; user functions do not accept `RecordBatch`
or return `ArrayRef`.

```rust
use velox_wasm_sdk::{velox_scalar, ArrayView, ArrayWriter};

#[velox_scalar]
fn increment(values: Option<ArrayView<'_, i64>>)
    -> Result<Option<ArrayWriter<i64>>, String>
{
    let Some(values) = values else { return Ok(None); };
    let mut result = ArrayWriter::with_capacity(values.len());
    for value in values {
        result.push(value?.map(|v| v.checked_add(1).ok_or("overflow"))
                         .transpose()?);
    }
    Ok(Some(result))
}
```

Concrete ARRAY/MAP types are inferred recursively from Rust views and writers.
ROW tuple inference names fields `field0`, `field1`, etc.; declare an explicit
SQL signature to preserve application field names:

```rust
use velox_wasm_sdk::{velox_scalar, ArrayView, ArrayWriter, RowView, RowWriter};

#[velox_scalar(
    arguments=["row(\"id\" bigint,\"values\" array(varchar))"],
    returns="row(\"id\" bigint,\"values\" array(varchar))",
)]
fn copy_row(value: Option<RowView<'_, (i64, ArrayView<'_, &str>)>>)
    -> Result<Option<RowWriter<(Option<i64>, Option<ArrayWriter<String>>)>>, String>
{
    let Some(value) = value else { return Ok(None); };
    Ok(Some(RowWriter::new((
        value.get::<0>()?,
        value.get::<1>()?.map(|array| array.materialize()).transpose()?,
    ))))
}
```

| SQL value | Read one value | Construct or modify one result |
| --- | --- | --- |
| ARRAY(T) | `ArrayView<'a,T>`: `len`, `get`/`at`, `iter`, `skip_nulls`, `contains_nulls`, `materialize` | `ArrayWriter<T>`: `push`, `add_item`, `add_null`, `reserve`, `resize`, `get_mut`, `copy_from` |
| MAP(K,V) | `MapView<'a,K,V>`: `at_index`, `iter`, `find`, `at`, `contains`, `materialize` | `MapWriter<K,V>`: `emplace`, `add_item`, `add_null(key)`, `resize`, `get_mut`, `copy_from` |
| ROW(fields) | `RowView<'a,(T0,T1,...)>`: `get::<I>`, `field(name)`, `at(index)`, `iter` | `RowWriter<(Option<T0>,...)>`: `get_mut::<I>`, `get_writer_at::<I>`, `set_null_at::<I>` |
| Generic or dynamic ROW | `Generic`: `get::<T>`, `try_cast::<T>`, `as_array`, `as_map`, `as_row`, `materialize` | `GenericWriter`, or `Value::Array`/`Map`/`Row` with mixed owned and borrowed children |

Nested children can themselves be views or writers. For example,
`MapView<'_, &str, ArrayView<'_, i64>>` reads `map(varchar,array(bigint))`;
`MapWriter<String, ArrayWriter<i64>>` constructs that type. Views implement
`IntoIterator`, including iteration through a borrowed view. Iterators are
fallible, double-ended, and exact-size. Materialization recursively copies
values so the result can outlive the input; returning a view or `Generic`
preserves the value without author-side materialization. IPC still copies
buffers across the host/guest boundary.

The host can export fully selected ordinary nested inputs without copying their
plain sibling buffers when only scalar leaves have dictionary/constant
encodings supported by Arrow. Encoded complex values, sparse selections and
unloaded lazy bases still use the native gather fallback. This changes host
preparation cost, not the author API: views decode accessed values on demand,
while IPC serialization and transfer remain eager.

Primitive BOOLEAN/TINYINT/SMALLINT/INTEGER/BIGINT/REAL/DOUBLE output columns
build directly from owned scalars or borrowed views without an intermediate
owned `Vec<Value>`. This also applies to primitive children of nested results
and internal lambda argument columns. NULLs, type checks and floating-point
bit patterns are preserved; authors still use the same row/view interfaces.

`MapView::find(key)` returns `None` for a missing key, `Some(None)` for a present
NULL value, and `Some(Some(value))` otherwise. `at(key)` errors on a missing key.
Lookup is linear, as in the native view. ARRAY/ROW keys and NaN use SQL value
equality with NULL children treated as values. A key itself must be non-NULL;
ARRAY/ROW key children may contain NULL, matching native Simple Function views,
writers and `map_from_entries`. The SQL `map()` constructor separately rejects
indeterminate keys; authors implementing that function can use
`key.contains_nulls()` to enforce its policy. Map writers reject a NULL key
before recording a row result and do not enforce key uniqueness, as with native
`MapWriter`. `find` compares borrowed keys directly rather than materializing
every candidate's nested payload.

The generic `map_lookup` example accepts complex keys without exposing
RecordBatch parameters:

```rust
#[velox_scalar(
    arguments = ["map(K,V)", "K"], returns = "V", type_variables = ["K", "V"]
)]
pub fn map_lookup<'a>(
    map: Option<MapView<'a, Generic<'a>, Generic<'a>>>,
    key: Option<Generic<'a>>,
) -> Result<Option<Generic<'a>>, String> {
    let (Some(map), Some(key)) = (map, key) else {
        return Ok(None);
    };
    Ok(map.find(key)?.flatten())
}
```

With `map_from_entries(array[row(row(NULL, 1), 42)])` as a native MAP value,
lookup of the non-NULL ROW key `(NULL, 1)` returns 42. Missing keys and present
NULL values both produce SQL NULL here; use `find`'s nested Option directly when
your function needs to distinguish them. `contains_nulls` and null-free views
still detect nested NULLs. Owned materialization preserves them for UDAF State;
`owned_generic_identity` demonstrates the copy path.

`ArrayView::may_have_nulls()` is a conservative check of its child array bitmap;
`contains_nulls()` checks the actual value recursively. A nullable nested child
is distinct from an empty ARRAY/MAP, and all-null/empty results retain the bound
SQL schema. The SDK validates writer types, ROW arity, decimal precision and
scale, and decimal overflow per row.

Typed ROW tuples support up to 16 fields. `RowView<'_, DynamicRow>`, `Generic`,
`GenericWriter`, and `Value::Row` support arbitrary field counts with explicit
SQL signatures. The tuple count is an SDK implementation choice, not a WASM
restriction. Concrete Hive types such as `array<bigint>` remain supported.
See [the executable examples](examples/add/src/lib.rs) and
[nested SDK tests](velox-wasm-sdk/src/nested_tests.rs).

## Generic operations and specialized calls

`Generic::compare(other, CompareFlags)` returns `Result<Option<Ordering>,String>`.
`equals` uses null-as-value equality; SQL three-valued equality uses
`CompareFlags::equality(NullHandling::NullAsIndeterminate)`. Nested NULLs may
make equality indeterminate, but a definite unequal child still yields false.
NaNs compare equal to one another and above non-NaN values; signed zeros are
equal. MAP comparison sorts keys and ignores insertion order. SQL orderable
constraints exclude MAP; the internal default comparator still orders maps,
as does Velox's vector comparator.

`Generic::hash()` returns a full `u64` with the standard Velox value hash,
including nested values, NULLs, NaNs, decimals, timestamps, and HUGEINT. The
smoke test compares it directly with native `BaseVector::hashValueAt` on the
built host. `Generic::sql_key()` validates and materializes an owned `SqlKey`
for Rust `HashSet`/`HashMap`; its equality and hash use these SQL value semantics.
Rust's container hasher may transform the value hash; it is not itself the
Velox value hash. Custom logical types use explicitly registered codec comparison/hash semantics,
as described below.
`Generic` comparison/hash reads borrowed buffers directly. ARRAY/ROW comparison
stops at the first decisive difference; hash visits every element. Standard
ARRAY/ROW/string paths with identical schemas do not allocate value trees.
MAP comparison allocates sorted entry indices. Custom callbacks borrow payloads,
with small codec-header/registry-key allocations. Owned `Value` operations avoid
tree cloning. Mixed owned/borrowed comparisons also read standard scalars,
strings and nested children directly, including retained aggregate State
compared with a current input. ARRAY/ROW stop before unvisited tails; MAP
allocates sorted entry indices. Mixed custom codec comparisons retain payload
materialization and registered identity/semantics checks. `sql_key()` intentionally
owns its value. Host gather and IPC remain
eager, and these improvements do not establish native performance parity.

`Result<T, RowError>` preserves a native `StatusCode` and message. For example,
`RowError::new(StatusCode::UserError, "bad input")` is a recoverable row error.
Outside `TRY`, validated business errors retain the native `VeloxUserError`
category and a healthy instance remains usable. Malformed status payloads are
validated before any user error is reported and invalidate the Store.
Other status categories go through native `EvalCtx::setStatus`; in this Velox
checkout they are system errors and `TRY` does not suppress them. Ordinary
`String` or other `ToString` errors remain user errors. Status information is
serialized inside the optional error column with a versioned encoding; no C++
exception object crosses the boundary. Initialization and trap errors remain
invocation-level errors.

`NullFreeArrayView`, `NullFreeMapView`, and `NullFreeRowView` expose children
without `Option`. A function whose input includes a null-free view follows the
native **only-callNullFree** behavior: any recursive input NULL produces NULL
without executing user code. `view.null_free()` performs a checked conversion
inside ordinary user code and returns an error when NULLs exist.

A regular function can supply alternate paths with explicit SQL signatures:

```rust
use velox_wasm_sdk::{velox_scalar, ArrayView, NullFreeArrayView};

#[velox_scalar(arguments=["array(bigint)"], returns="bigint",
               call_null_free="sum_nullfree")]
fn sum(values: Option<ArrayView<'_,i64>>) -> Result<Option<i64>,String> {
    values.map(|v| v.skip_nulls().try_fold(0_i64, |s,x| {
        s.checked_add(x?).ok_or_else(|| "overflow".to_owned())
    })).transpose()
}
fn sum_nullfree(values: NullFreeArrayView<'_,i64>) -> Result<Option<i64>,String> {
    values.into_iter().try_fold(0_i64, |s,x| {
        s.checked_add(x?).ok_or_else(|| "overflow".to_owned())
    }).map(Some)
}
```

`call_null_free="path"` uses the alternate when all input values are recursively
non-null; otherwise it uses the ordinary function. `call_ascii="path"` uses an
alternate for the entire selected batch when top-level VARCHAR inputs are known
ASCII, matching native SFI. NULL and nested strings do not disqualify this path.
A non-ASCII or unknown host flag selects the ordinary function for the batch. Paths have the same argument count and result type;
Rust infers alternate reader types from the referenced function. Alternate
implementations must preserve the ordinary function's SQL semantics. A batch-level recursive validity check skips per-row NULL scans when the
input columns have no recursive NULLs; mixed batches retain checked per-row
dispatch. The generated manifest declares `has_ascii`; the host requests native
input ASCII caching and sends `velox.scalar.ascii_inputs=true|false` in IPC schema
metadata. An older host may omit this key; the SDK scans input bytes once per
batch then. The host computes and caches actual VARCHAR output encoding for
all scalar exports. It never trusts a guest ASCII-preservation claim. Expression
dictionary wrappers maintain separate caches, as with native vectors. Native
adaptive loops and end-to-end demand loading remain additional work.

## Scalar wire representation and compatibility

New row adapters set `row_api=true` and receive the resolved return SQL type.
TIMESTAMP uses a marked Arrow STRUCT of `seconds: Int64` and `nanos: Int64`;
HUGEINT uses marked `high: Int64` and `low: Int64`; VARCHAR uses marked
`bytes: Binary` to preserve arbitrary bytes. Field metadata
`velox.logical_type` distinguishes these values from ordinary SQL ROWs with
identical field names. Conversion is recursive through ARRAY/MAP/ROW, and the
host validates markers and timestamp component ranges on the return path.
Timestamp precision remains nanoseconds; its range matches Velox rather than
Arrow's narrower `Int64` nanosecond timestamp. `VarcharBytes` preserves invalid
UTF-8 and embedded NUL bytes, including in nested values and MAP keys. Reading
such a VARCHAR through `&str` or `String` returns a per-row decoding error;
`TRY` preserves other rows. `Generic` compare/hash/roundtrip work on raw bytes.
An ordinary ROW named `(bytes varbinary)` remains a ROW. `InitContext::constant_value`
exposes a one-value constant without requiring an Arrow-specific downcast.

Existing ABI v1 exports remain compatible. The explicit `velox_arrow_scalar`
batch macro and aggregate adapter still use legacy Arrow timestamp encoding;
their timestamp range is limited to `i64` epoch nanoseconds. Full-width HUGEINT and arbitrary-byte VARCHAR
require the scalar row adapter. Custom values use the codec protocol below;
OPAQUE values can use codecs or invocation-scoped handles. Raw C++ pointers
remain outside the guest value ABI. The detailed
[capability assessment](../../../docs/designs/wasm-scalar-sfi-parity.md)
separates implementation gaps from sandbox constraints.

## Custom types and OPAQUE

Register host codecs before running queries, using
[`TypeBridge.h`](../TypeBridge.h). A codec connects a **specific native SQL type**
to a versioned byte format implemented by the Rust function:

```cpp
using namespace facebook::velox::functions::wasm;
registerWasmTypeCodec({
    MY_CUSTOM_SQL_TYPE(), "my_value", 1,
    [](const BaseVector& values, vector_size_t row) -> std::string {
      // Encode one non-null native value using a portable byte format.
      return encodeMyValue(values, row);
    },
    [](std::string_view bytes, BaseVector& values, vector_size_t row) {
      // Decode and set one non-null result in the supplied native vector.
      decodeMyValue(bytes, values, row);
    },
});
```

Scalar nested outputs can defer host codec decoding in ROW fields, ARRAY
elements and MAP values. This requires no Rust authoring change: return
CustomValue or nested writers as before. The native LazyDereference query path
can project an ordinary field without decoding an unused codec field. Other
native ROW readers or sparse result copying can materialize the columns.
First access decodes the entire codec column, preserving results for later row
consumers under native LazyVector's single-load contract. MAP keys, OPAQUE
handles and top-level scalar codec results remain eager; aggregate/borrowed IPC
contracts are unchanged. This does not add host input projection or selective
row decoding. The host validates wire/schema/identity/payload presence eagerly,
then interprets codec bytes on load. A native codec failure is a fatal bridge
error that bypasses SQL TRY and invalidates a live Store. A Rust Result/RowError
still uses the recoverable per-row channel. Pending results retain IPC and pool
ownership and can outlive their producer via a weak Store failure callback.

The host passes codec ID, positive version and canonical logical type identity
in field metadata. The payload is a marked binary struct rather than an
ordinary VARBINARY or ROW. ARRAY/MAP/ROW nesting, NULLs, constants and generic
return construction preserve that identity. A codec is explicit: a custom type
without a codec is rejected instead of silently becoming its underlying
integer/string type. Use the type's native registered SQL name for concrete
signatures, or bind it through SQL type variables for generic functions.

Read `CustomValue<&[u8]>`, return `CustomValue<Vec<u8>>`, or use `Generic` inside
nested views/writers. For an eight-byte little-endian integer codec:

```rust
use velox_wasm_sdk::{velox_scalar, CustomValue};

#[velox_scalar(arguments=["T"], returns="T", type_variables=["T"])]
fn increment(value: Option<CustomValue<&[u8]>>)
    -> Result<Option<CustomValue<Vec<u8>>>, String>
{
    value.map(|value| {
        let n = i64::from_le_bytes(value.payload.try_into()
            .map_err(|_| "expected eight bytes")?);
        let n = n.checked_add(1).ok_or("overflow")?;
        Ok(value.map_payload(|_| n.to_le_bytes().to_vec()))
    }).transpose()
}
```

This function intentionally uses the declared codec format; applications should
also check the expected `codec_id` and `version` when multiple formats are
eligible. `map_payload` retains the bound header. `Generic::codec_type()`
returns an owned `CodecType`; `InitContext::return_codec()` exposes the bound
return codec during initialization. `codec.value(payload)` constructs a new
result without manually reproducing logical type metadata. Wrong result
identity/version is a row error compatible with TRY. Malformed IPC, invalid
handles or host decoder failures are invocation-level failures.

For generic comparison and hashing, call `register_codec_semantics(id, version,
CodecSemantics { compare, hash })` from the initializer. The callbacks operate
on valid non-null payloads and receive `CompareFlags`; they must implement the
native type's comparison and full 64-bit value hash, including ascending order
and equality/hash consistency. NULL handling and recursive ARRAY/MAP/ROW
composition remain SDK responsibilities. Unregistered semantics return an
error; serialized bytes are never implicitly treated as SQL value order/hash.
The executable `low8_i64` example is checked against a native type that compares
and hashes only the lowest eight bits of its BIGINT value.

OPAQUE provides two choices:

- **Serialized value:** register native `OpaqueType::registerSerialization<T>`
  callbacks, then call `registerWasmOpaqueSerializationCodec(OPAQUE<T>(), id,
  version)`. Rust receives a `CustomValue`, can inspect/modify its portable
  payload, and the host reconstructs the result object. This can create a new
  object and does not promise pointer identity.
- **Object identity:** register a stable `registerOpaqueType<T>(alias)`; an
  OPAQUE type without an explicit codec uses `OpaqueHandle<'a>`. A named native
  OPAQUE custom type can instead supply identity via its native type serializer.
  The host retains `shared_ptr` ownership in an invocation scope. Rust can pass
  the handle through or place it in nested results; the host resolves the
  original object without copying its contents.

```rust
use velox_wasm_sdk::{velox_scalar, OpaqueHandle};
#[velox_scalar(arguments=["T"], returns="T", type_variables=["T"])]
fn pass_handle(value: Option<OpaqueHandle<'_>>) -> Option<OpaqueHandle<'_>> {
    value
}
```

A handle is not a host address and exposes no object methods. Scope, handle
membership, native C++ `type_index`, and logical wire identity are checked on
return. OpaqueType's ordinary native equivalence is insufficient for choosing
an object decoder; the bridge explicitly checks `type_index` too. Handles are
valid only for the originating invocation, including initialization. Storing
numeric handle parts or an owned `Value::Opaque` does not extend validity;
a later call rejects expired/foreign handles. The returned host vector owns the
resolved object independently of the temporary scope. General host-method
imports and retained query-lifetime guest handles are not implemented.

The host/guest IPC boundary still copies bytes. Within the host, owned output
IPC storage is transferred to Arrow and retained by imported vectors; VARCHAR
wire views share host buffers and all-selected results reuse the compact
vector. The borrowed-buffer decode API keeps its deep-copy behavior. Sparse
scatter and encoded inputs retain gather/copy paths. This avoids redundant
host copies without giving the guest access to pre-existing host pointers.

### Row UDAFs

Use `VeloxRowAggregate` and `#[velox_row_aggregate]` for new aggregate functions.
User code receives individual scalar/borrowed-view arguments, not RecordBatch.
State owns everything retained across update calls; serialize/finalize borrow it
and must preserve its SQL contents for repeated window extraction.

```rust
use velox_wasm_sdk::{velox_row_aggregate, Generic, InitContext, RowError, StatusCode, Value, VeloxRowAggregate};

pub struct SumWide;
#[velox_row_aggregate(arguments = ["hugeint"], intermediate = "hugeint", returns = "hugeint", checkpoint = true)]
impl VeloxRowAggregate for SumWide {
    type State = i128;
    type Input<'a> = (i128,);
    type Error = RowError;
    fn create(_context: &InitContext<'_>) -> Result<i128, RowError> { Ok(0) }
    fn update(state: &mut i128, input: Self::Input<'_>) -> Result<(), RowError> {
        *state = state.checked_add(input.0)
            .ok_or_else(|| RowError::new(StatusCode::UserError, "sum overflow"))?;
        Ok(())
    }
    fn serialize(state: &i128) -> Result<Value<'_>, RowError> { Ok(Value::HugeInt(*state)) }
    fn merge(state: &mut i128, value: Generic<'_>) -> Result<(), RowError> {
        let input = value.get::<i128>()
            .map_err(|message| RowError::new(StatusCode::TypeError, message))?;
        Self::update(state, (input,))
    }
    fn finalize(state: &i128) -> Result<Value<'_>, RowError> { Self::serialize(state) }
}
```

The nonnullable input infers default NULL behavior: NULL raw inputs are skipped,
empty/all-NULL groups return NULL, and NULL intermediates are skipped. Use Option
inputs or explicit `default_null_behavior = false` to receive NULL raw values;
merge then also receives outer-NULL Generic values and must define their meaning.
Nested child NULLs remain distinct from outer NULL.

For an orderable generic min-like aggregate, `update` and `merge` can share
this helper. Compare the borrowed candidate directly with owned State; copy
only a value that must survive the current invocation. Declare `T:orderable`
and use a typed intermediate carrying T. This helper ignores outer NULLs;
an empty State is represented by `Value::Null`.

```rust
use velox_wasm_sdk::{CompareFlags, Generic, Value};
use std::cmp::Ordering;

fn retain_smaller(state: &mut Value<'static>, input: Generic<'_>) -> Result<(), String> {
    if input.is_null() {
        return Ok(());
    }
    let candidate = Value::Borrowed(input);
    if state.is_null()
        || candidate.compare(state, CompareFlags::default())? == Some(Ordering::Less)
    {
        *state = input.materialize()?;
    }
    Ok(())
}
```

The executable examples add:

- `FirstValueWasm`: `arguments = ["T"]`, `type_variables = ["T"]`,
  `intermediate = "row(value T,seen boolean)"`, `returns = "T"`;
  Input is `(Option<Generic<'a>>,)`. It calls `materialize()` only when retaining
  the first input, including a first NULL. The typed intermediate carries T
  across partial/intermediate/final stages and supports nested values.
- `VariadicSumWasm`: one constant bigint plus `variadic = "bigint"`;
  Input is `(Option<i64>, VariadicView<'a, Option<i64>>)`. The borrowed tail
  supports zero arguments, indexing and iteration without a per-row Vec.
  `create(&InitContext)` reads `constant_value(0)` and declared
  `wasm_aggregate_multiplier`; typed ROW(HUGEINT,BIGINT) holds sum/count.
- `StrictSumWasm`: the full-width/default-NULL example above, including inputs
  beyond DECIMAL(38,0) and native `toIntermediate` support.
- `CheckedSumWasm`: BIGINT input/result with a HUGEINT intermediate; checked
  accumulation and BIGINT extraction return typed UserError on overflow.
- `CollectStringsWasm`: VARCHAR → ARRAY(VARCHAR) intermediate/result. State is
  Vec<String>; borrowed ARRAY merge reads avoid materializing its whole input.
  Spare container capacities demonstrate row-size accounting and compaction.

```sql
SELECT first_value_wasm(items ORDER BY ordinal) FROM input;
SELECT variadic_sum_wasm(2, a, b) FROM input;
SELECT variadic_sum_wasm(2, a) OVER (
  PARTITION BY key ORDER BY ordinal
  ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) FROM input;
```

`arguments`, `returns`, `intermediate`, `type_variables`, `integer_variables`,
`variadic`, `constant_arguments` and `config_keys` use the host signature binder.
`name` overrides the SQL name; `export` overrides the lifecycle export prefix.
`order_sensitive`/`ignore_duplicates` describe semantics, not automatic user-code
validation. State creation occurs per group; adapter initialization occurs once
per Store. Raw constants are not automatically carried to downstream merge
operators: encode their effect in the intermediate and keep merge initialization
neutral. Config access exposes declared strings/default strings, as for scalars.

Generated row UDAFs declare ABI 3; existing scalar and legacy aggregate exports
remain ABI 1. The host accepts ABI 1/2/3, preserving old payloads. Older hosts
reject new row declarations. ABI 3 adds retained-state size reports and a required
compact export, while preserving ABI 2 typed Status behavior.
Use `type Error = RowError` to preserve native Status behavior throughout the
aggregate lifecycle: UserError becomes a native user exception and all other
Status codes become system exceptions, as with native EvalCtx::setStatus.
String/other ToString errors retain legacy fatal export-error behavior. Error
types must be `'static`; borrowed Input values keep their input lifetime.
All errors fail the aggregate export and invalidate Store, since earlier rows
may have already changed state. Aggregate Err does not use scalar's per-row TRY
channel and cannot permit continuing on that Store. OPAQUE retained state requires a
serialization codec; invocation-scoped handles cannot survive an update call.
`toIntermediate` constructs/destroys temporary per-row states and is a correctness
fallback. DISTINCT, masks, ordering, partial/final and window framing use native
operators. Grouped spill uses the declared intermediate type.

`State` must implement `AggregateState`. Supported owned fields include fixed
primitives, String, Vec, Box, Option, arrays, tuples (through arity 16), RefCell,
CustomValue and owned Value trees. Derive for a struct:

```rust
#[derive(velox_wasm_sdk::AggregateState)]
pub struct MyState {
    total: i128,
    labels: Vec<String>,
}
// In impl VeloxRowAggregate: type State = MyState;
```

The adapter adds inline State size once; heap_bytes counts reserved capacity
and recursively owned child allocations. A Vec counts capacity * size_of(element),
then child heaps, avoiding counting inline children twice. Compact recursively
shrinks excess capacities without changing SQL values. Custom/shared allocators
implement `heap_bytes` and `compact` explicitly; shared allocations need one
ownership policy, so the SDK does not blindly count every Arc/Rc. Footprint
arithmetic is checked. Retained borrowed Value and invocation OPAQUE handles
are rejected; materialize retained inputs first.

Size reports travel in create/update/merge's existing success payloads and are
also prefixed to serialize/finalize output, including interior-cache growth.
The author continues to receive row values, not protocol records or RecordBatch.
The host updates the aggregate's contribution to the native shared row-size
counter used in row/spill sizing, with full report validation before publication.
Store-wide bookkeeping/fragmentation are covered by the separate accessible
linear-page charge. ABI 1/2 modules keep their legacy payloads; custom structs in
source using the new SDK add a derive or manual implementation.

Guest capacity shrinking makes space reusable inside a live Store. Standard
Wasm linear memory cannot shrink. An aggregate can opt into full owned-State
checkpointing so the host can rebuild a fragmented Store while keeping groups
active. Logical size reduction alone returns zero reclaimed bytes; successful
rebuilding returns the reduction in accessible pages charged to native MemoryPool.
Successful last-group retirement also releases the Store. Failed Stores remain
poisoned. Native spill/flush continues to use SQL intermediates.

### Full-State checkpointing for active-group compaction

```rust
#[derive(velox_wasm_sdk::AggregateState, velox_wasm_sdk::AggregateCheckpoint)]
pub struct MyState {
    total: i128,
    // Private update configuration must survive even if SQL intermediate omits it.
    factor: i64,
    labels: Vec<String>,
}
// Add checkpoint = true to #[velox_row_aggregate(...)] and use MyState.
```

`checkpoint = true` is an explicit author promise: all mutable behavior that
influences future aggregate operations belongs to State and is reconstructible.
The SDK supports primitive fields, String, Vec, Box, Option, arrays, tuples up
to arity 16, RefCell, serialized CustomValue and owned Value trees. Derive supports
named/tuple/unit structs, including generic fields. Implement `AggregateCheckpoint`
manually for enums/custom ownership; `CheckpointWriter::write/nested` and
`CheckpointReader::read/charge/nested` provide bounded framing. Do not opt in if
behavior depends on mutable module globals, retained borrowed values, invocation
OPAQUE handles or guest addresses. Shared ownership needs a deliberate full-graph
reconstruction policy. Rebuilding initializes the same InitContext but restores
State directly without calling the author's create/merge methods.

Checkpoint-enabled row examples include generic nested first-value State,
the positional sum and the variadic sum's private factor. No author method receives RecordBatch
or checkpoint protocol records. SQL intermediate and full-State checkpoint serve
different purposes: serialize/merge must support distributed stages/spill; the
private checkpoint migrates all live State and its handle high-water mark within
the same module/context.

The host first compacts the supplied subset, then may rebuild **all** groups in
the Store. It validates the full snapshot before discarding old pages, restores
identical handles, and publishes every group's new size report without changing
native NULL flags or other aggregates' row sizes. A failed decode/report/export
invalidates the whole aggregate; cleanup removes its own contributions safely.
Unknown protocol, missing paired exports and non-ABI-3 declarations are rejected.
The feature is additive to ABI 3: existing ABI-3 hosts can ignore it and continue
logical compaction without physical reclaim.

Current rebuilding requires excess pages beyond the initialized Store plus twice
live reported State and one 64-KiB page, and enough native free/reserved capacity
for an estimated `4 * live State + 32 * groups + 64 KiB` temporary workspace.
If those conditions do not hold, compact returns zero and native can continue to
spill. The estimate is a heuristic; actual allocations still obey native/Store
limits, fuel, cancellation and input/output size limits. The owned host snapshot
is charged before copying, and the old Store is released before creating a new
one. This is whole-Store migration, with serialization work and temporary
allocations; streaming/segmented reclamation under zero spare capacity remains an
optimization obligation. Pool page accounting is not a measured RSS claim.

The runtime now charges accessible pages before creation/growth through Wasmtime
host memory callbacks instead of reserving the configured maximum. Guard and
virtual address reservations remain inaccessible; they are not query allocation
bytes or an RSS measurement. Native pool failures are fatal even if guest code
ignores memory.grow's -1. Standard 64-KiB pages and isolated, unshared guest
memories are supported; shared/threaded memory needs a separately accounted
runtime. JIT/table/non-linear overhead remains a deployment/accounting obligation.

Registration generates companion functions using native Velox adapters and
signature rules. The SDK author does not write extra exports or declarations:

```sql
-- Each partition produces the declared HUGEINT intermediate.
WITH partials AS (
  SELECT partition_key, checked_sum_wasm_partial(v) AS state
  FROM input GROUP BY partition_key
)
SELECT checked_sum_wasm_merge_extract(state) FROM partials;

-- Merge intermediates while keeping their type for another stage.
SELECT checked_sum_wasm_merge(state) FROM partials;

-- Finalize an individual HUGEINT intermediate to BIGINT.
SELECT checked_sum_wasm_extract(state) FROM partials;
```

The latter two SELECTs use the same `partials` relation; supply its CTE again
when executing them independently. `_partial` uses raw input and returns
intermediate, `_merge` combines intermediates, `_merge_extract` combines and
finalizes, and scalar `_extract` finalizes each selected intermediate. Empty and
NULL semantics still come from the aggregate. Raw companions receive constants;
merge uses the encoded intermediate and its own initialization.

Initialization exposes factory type binding through
`context.aggregate_input_types()`: `Some(AggregateInputTypes::Raw)` means the
argument types are the raw value signature, and `Intermediate` means a direct
merge/extract binding. `None` identifies scalar initialization or an older host
without this metadata. This is not the current operator step: native staged
plans can construct a final operator's aggregate using the original raw types.

`constant_value(index)` and `constant_argument(index)` refer to raw configuration.
An independent merge's constant ROW/ARRAY/scalar intermediate never occupies
those constant indices, even when its physical type happens to match a raw
argument. `intermediate_constant_value()` exposes a supplied intermediate
constant separately; `Some(NULL)` differs from a missing/nonconstant input.
The shared native scalar extract adapter does not forward intermediate constants
to its aggregate factory, so this accessor is normally `None` for `_extract`.
Actual data still arrives at `merge` once per selected input row. Use an
initialization hint for validation/capacity planning, not to seed the accumulated
total and then add the same value again. For a sum/count intermediate:

```rust
if let Some(value) = context.intermediate_constant_value()? {
    if !value.is_null() {
        let count_hint = value.as_row::<DynamicRow>()?.at(1)?.get::<i64>()?;
        // An optional capacity hint; merge still accounts for each input row.
    }
}
```

Do not retain that borrowed view in State. Existing raw constant behavior is
unchanged. Older SDKs ignore the new binding metadata but receive a cleared raw
constant mask for direct merges, so they cannot reinterpret intermediate values
as raw configuration. New SDKs validate mode/constant markers and reject
contradictory masks or non-NULL placeholders marked as nonconstant.

These are primarily planner adapters. When overloads share an intermediate
type, native return-type suffixes distinguish extract/merge-extract names.
Generic variables must be inferable from the intermediate under native rules;
`row(value T,seen boolean)` preserves T while an erased VARBINARY payload cannot
supply that binding. Adding/replacing an overload refreshes its companion family;
a generated-name collision rejects the entire module, even with overwrite.
The registration count includes declared SQL names only, not generated aliases.

A companion `_extract` UserError throws the whole call even under SQL TRY,
matching native extract behavior; it has no scalar row-error column. Failure
releases the temporary aggregate instance and failed Store. A later API retry
gets a fresh instance. Ordinary aggregate lifecycle errors also stop that Store.

See [UDAF capability alignment](../../../docs/designs/wasm-udaf-parity.md) for
the current memory, callback and optimizer contracts and remaining gaps.

### ROW field names in aggregate intermediates

Native signature rules apply to both scalar and aggregate declarations:
`row(total hugeint,count bigint)` requires those field names when binding the
signature; `row(hugeint,bigint)` binds fields by position. Generic `T` retains
its bound input's names. Do not infer signature matching from
`Type::equivalent()`, which ignores ROW names more broadly.

The `PositionalSumWasm` example declares the anonymous intermediate:

```rust
/// Anonymous ROW intermediates bind by position, as native signatures do.
pub struct PositionalSumWasm;
#[velox_wasm_sdk::velox_row_aggregate(
    arguments = ["bigint"], returns = "bigint",
    intermediate = "row(hugeint,bigint)", default_null_behavior = true, checkpoint = true
)]
impl velox_wasm_sdk::VeloxRowAggregate for PositionalSumWasm {
    type State = (i128, i64);
    type Input<'a> = (Option<i64>,);
    type Error = velox_wasm_sdk::RowError;
    fn create(_: &velox_wasm_sdk::InitContext<'_>) -> Result<Self::State, Self::Error> {
        Ok((0, 0))
    }
    fn update(state: &mut Self::State, input: Self::Input<'_>) -> Result<(), Self::Error> {
        if let Some(value) = input.0 {
            Self::add(state, value as i128, 1)?;
        }
        Ok(())
    }
    fn serialize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(velox_wasm_sdk::Value::Row(vec![
            velox_wasm_sdk::Value::HugeInt(state.0),
            velox_wasm_sdk::Value::BigInt(state.1),
        ]))
    }
    fn merge(
        state: &mut Self::State,
        input: velox_wasm_sdk::Generic<'_>,
    ) -> Result<(), Self::Error> {
        let row = input
            .as_row::<velox_wasm_sdk::DynamicRow>()
            .map_err(Self::error)?;
        Self::add(
            state,
            row.at(0)
                .and_then(|v| v.get::<i128>())
                .map_err(Self::error)?,
            row.at(1)
                .and_then(|v| v.get::<i64>())
                .map_err(Self::error)?,
        )
    }
    fn finalize(state: &Self::State) -> Result<velox_wasm_sdk::Value<'_>, Self::Error> {
        Ok(if state.1 == 0 {
            velox_wasm_sdk::Value::Null
        } else {
            velox_wasm_sdk::Value::BigInt(i64::try_from(state.0).map_err(Self::error)?)
        })
    }
}
impl PositionalSumWasm {
    fn error(message: impl ToString) -> velox_wasm_sdk::RowError {
        velox_wasm_sdk::RowError::new(velox_wasm_sdk::StatusCode::UserError, message.to_string())
    }
    fn add(
        state: &mut (i128, i64),
        total: i128,
        count: i64,
    ) -> Result<(), velox_wasm_sdk::RowError> {
        state.0 = state
            .0
            .checked_add(total)
            .ok_or_else(|| Self::error("sum overflow"))?;
        state.1 = state
            .1
            .checked_add(count)
            .ok_or_else(|| Self::error("count overflow"))?;
        Ok(())
    }
}

```

Its `_merge`, `_merge_extract` and `_extract` companions accept compatible ROWs
with other field names. Ordinary final-stage merge follows the already bound
native intermediate contract. Before either single-group or grouped merge, the
host canonicalizes only the intermediate's Arrow field descriptors, recursively
through ARRAY/MAP/ROW. It preserves physical wire markers, leaf types,
DECIMAL parameters, codec identity and OPAQUE type identity. Field conversion
shares the existing value buffers; caller vectors and shared children keep their
original types. Selection/export and IPC still incur their normal transport
costs. Explicitly named signatures are not broadened by this conversion.

For VARCHAR bytes in a nested typed view, use
`ArrayView<'_, VarcharBytes<&[u8]>>`; `&[u8]` alone denotes VARBINARY.
`named_row_length` demonstrates `RowView::field("id")` and `field("values")`
without converting each string's bytes. Its explicitly named signature requires
`id` and `values`; generic row views expose their bound input names.

### SQL lambdas in row UDAFs

The `ReduceWasm` example supports a generic input T and state S. Declare
functions separately from ordinary value arguments:

```rust
#[velox_row_aggregate(
    arguments = ["T", "S"], type_variables = ["T", "S"], returns = "S",
    intermediate = "row(value S,seen boolean)",
    lambdas = ["function(S,T,S)", "function(S,S,S)"],
    default_null_behavior = false, checkpoint = true
)]
```

`Input<'a>` is `(Option<Generic<'a>>, Option<Generic<'a>>)`. The generated native
signature appends two FUNCTION arguments; they never become RecordBatch or
value arguments in Rust. In `create`, save `context.lambda(0)?` and
`context.lambda(1)?` in State. Each is a four-byte symbolic index implementing
`AggregateState` and `AggregateCheckpoint`, with no retained host pointer.
Convert context errors to your aggregate's error type as in the full example.
With `variadic`, the native signature places FUNCTION arguments between the
fixed value prefix and the final value tail. Rust `Input` and constant indices
still exclude FUNCTION. The SQL resolver accepts expanded/empty tails and binds
known value types after unresolved lambdas; see the examples below.
In `update`, map a value from the initial state, then combine with any existing
accumulator and store the owned result (see `ReduceWasm` for validation):

```rust
let mapped = state.input.call(&[&state.initial, &Value::Borrowed(value)])?;
let result = if state.seen {
    state.combine.call(&[&state.value, &mapped])?
} else {
    mapped
};
state.value = result;
state.seen = true;
```

SQL usage, with native binding of T/S and lambda types:

```sql
SELECT reduce_wasm(value, 0, (s,x) -> s+x, (a,b) -> a+b) FROM input;
SELECT key, reduce_wasm(value, 0, (s,x) -> s+x, (a,b) -> a+b)
FROM input GROUP BY key;
-- T = ARRAY(BIGINT), S = BIGINT:
SELECT reduce_wasm(values, 0, (s,x) -> s+cardinality(x), (a,b) -> a+b)
FROM input;
-- T = BIGINT, S = ARRAY(BIGINT); initial contains an empty array per row:
SELECT reduce_wasm(value, initial,
    (s,x) -> concat(s,array[x]), (a,b) -> concat(a,b)) FROM input;
```

Partial/intermediate/final plans retain native-bound lambda expressions, and
spill uses typed intermediate values. `_partial` forwards lambda authority to
the source aggregate. Native companion merge/extract signatures carry only the
intermediate. Independent calls work when merge/finalize do not evaluate a lambda,
even if input-only generic lambda types were erased. Such slots retain their
symbolic position with explicit unbound-type metadata; `Lambda::has_bound_types()`
returns false, and `call`, `call_batch` and `max_batch_rows` return an error.
No UNKNOWN/concrete substitute is invented for the missing type. If those
stages need callback authority, use the ordinary staged plan, which retains the
expressions. An intermediate cannot reconstruct executable lambdas; supporting
callback-dependent independent calls needs an explicit API/planner contract.
This is not a Wasm expressiveness limit.

The example skips NULL input, requires a non-NULL initial state constant within
each group, rejects NULL lambda state, and returns NULL for empty/all-NULL
groups. As with native reduce, distributed/tree combination requires the
author's input/combine functions and initial state to satisfy the reduction
laws. Rust arguments/results support nullable and nested values; errors from
the native evaluator preserve their native category. A failed lifecycle call
invalidates Store.

Only the exact three typed lambda imports (`lambda_call`, `lambda_call_batch`,
`lambda_result`) are accepted. Calls are bound to this
query's declared lambdas; module start, scalar and legacy instances cannot use
them. Results use charged single-use tokens, byte/call limits and cancellation
checks. Immutable lambda bindings are reconstructed on checkpoint restore;
State stores positions only. `call` evaluates one row; `call_batch` evaluates
a bounded vector of rows using internal IPC. Query memory, per-call row limits
and cumulative per-export evaluation limits also apply. IPC and more native
optimizer paths remain performance work; this is not a claim of native throughput.
An older host that rejects all imports cannot load a module containing this
capability, even when selecting a scalar export from that same module. Existing
prebuilt modules without these imports remain compatible with the current host.

### Combining SQL lambdas, generics and variadic values

`MapVariadicWasm` in [the examples](examples/add/src/lib.rs) maps a fixed input
and every non-NULL tail value through the same SQL lambda, multiplies each mapped
BIGINT by a constant factor, and accumulates the result. Its declaration is:

```rust
#[velox_row_aggregate(
    arguments = ["T", "bigint"], variadic = "T", type_variables = ["T"],
    lambdas = ["function(T,bigint)"], constant_arguments = [1],
    returns = "bigint", intermediate = "row(total hugeint,count bigint)",
    default_null_behavior = false, checkpoint = true
)]
```

The native SQL signature is `(T, bigint, function(T,bigint), T...)`. The Rust
value tuple remains `(Option<Generic<'a>>, Option<i64>,
VariadicView<'a, Option<Generic<'a>>>)`. `create` retains `context.lambda(0)`
and the constant factor from value argument 1. The bulk hook flattens borrowed
head/tail values into bounded `call_batch` chunks; no input is retained in State
and no RecordBatch is exposed. NULL inputs are skipped, a NULL mapped result is
an error, and empty/all-NULL groups return NULL. A NULL/missing factor means 1;
missing factors in merge stages are neutral because intermediates already carry
scaled totals. Merge does not apply the factor again. Checkpoints preserve the
private factor and symbolic lambda position separately from SQL intermediates.

```sql
-- T = BIGINT; map a and every b/c value, then multiply by 2.
SELECT map_variadic_wasm(a, 2, x -> x, b, c) FROM input;
-- Zero variadic values: a still binds T and is mapped.
SELECT map_variadic_wasm(a, 2, x -> x) FROM input;
-- T = ARRAY(BIGINT): native cardinality evaluates the borrowed nested values.
SELECT key, map_variadic_wasm(arr1, 2, x -> cardinality(x), arr2)
FROM input GROUP BY key;
-- Bind T entirely from the arguments after FUNCTION:
SELECT tail_map_variadic_wasm(2, x -> cardinality(x), arr1, arr2) FROM input;
-- Every tail argument must be constant in this variant:
SELECT map_const_variadic_wasm(a, 2, x -> x, 10, 20) FROM input;
```

`TailMapVariadicWasm` declares only a fixed BIGINT factor and a generic T tail.
Its tuple is `(Option<i64>, VariadicView<'a, Option<Generic<'a>>>)`. At least one
typed tail value is needed to bind T; an empty tail has no source for the lambda
parameter type. This is generic type inference, not a Wasm/variadic restriction.
Use the fixed-T form when empty tails must be inferable. Repeated T values must
satisfy the native binder's type/coercion rules.

`MapConstVariadicWasm` uses `constant_arguments = [1,2]`: value argument 1 is
the factor; value argument 2 is the declared tail, so every supplied tail value
must be constant. These indices exclude FUNCTION, even though FUNCTION occupies
SQL argument 2. The host projects the constant flags before checking actual
value arguments and expands the last flag across the tail. Constant SQL NULL
is distinct from a nonconstant argument; zero tail needs no tail constant.

`MapNonNullVariadicWasm` combines a concrete lambda with a heterogeneous tail:

```rust
#[velox_row_aggregate(
    arguments = ["bigint"], variadic = "any",
    lambdas = ["function(bigint,bigint)"], constant_arguments = [0],
    returns = "bigint", intermediate = "row(total hugeint,count bigint)",
    default_null_behavior = false, checkpoint = true
)]
```

Its tuple is `(Option<i64>, VariadicView<'a, Option<Generic<'a>>>)`. Each non-NULL
tail value contributes `lambda(1) * factor`; the implementation checks validity
and leaves the payload unread, including arbitrary-byte VARCHAR. The tail can
mix integers, strings and nested values, or be empty:

```sql
SELECT map_non_null_variadic_wasm(2, x -> x, a, text, arr) FROM input;
-- Concrete lambda types remain inferable with an empty tail; result is NULL.
SELECT map_non_null_variadic_wasm(2, x -> x) FROM input;
WITH partials AS (
  SELECT map_non_null_variadic_wasm_partial(2, x -> x, a, text, arr) AS state
  FROM input
)
SELECT map_non_null_variadic_wasm_merge_extract(state) FROM partials;
```

This example's concrete FUNCTION type remains resolvable in independent merge
and extraction. Those methods use only encoded totals/counts and never invoke
the lambda, so `_partial` → `_merge_extract` and `_partial` → `_merge` → `_extract`
work without carrying an expression into those stages.

Grouped/single/staged and `_partial` calls use the same layout. Independent
companions that evaluate a lambda still need the expression-carrying API
described above. A generic lambda type erased from the intermediate cannot
be recovered, but can remain an unused symbolic slot. `MapVariadicWasm` works
with independent merge/extract for BIGINT and ARRAY inputs even though its
sum/count intermediate erases T. `ReduceWasm` can independently extract a
partial value without its unused input lambda type; merging several nonempty
partials still needs the combine expression. The native aggregate factory receives raw types
including FUNCTION while Rust initialization contains only value types plus
separate lambda bindings. This is additive to row ABI 3 and `ipc-v1`; existing
fixed-lambda modules keep their layout, while older fixed-only lambda hosts
reject modules containing the new variadic declarations.

### Batched lambda evaluation and optional aggregate hooks

`Lambda::call_batch(row_count, columns)` receives borrowed `Value` columns.
Each column has `row_count` values or one value to broadcast. Results are owned,
nullable/nested `Value`s. `max_batch_rows()` exposes this query's row limit
(default 65,536). Chunk larger work; an empty batch validates its shape and
returns no values without calling the host.

```rust
let initial = [Value::BigInt(0)];
let values = [Value::BigInt(1), Value::BigInt(2), Value::BigInt(3)];
let mapped = state.input.call_batch(3, &[&initial, &values])?;
```

The input lambda in this example has `function(bigint,bigint,bigint)` type.
It receives three `(0,value)` rows in one native evaluation. This API has no
RecordBatch parameter or native pointer.

Authors may override `VeloxRowAggregate::update_batch` and/or `merge_batch`.
The generated exports opt in only when the corresponding method is present;
ordinary implementations retain their existing per-row prepared-reader path.
An update hook consumes an iterator of `Result<Self::Input<'a>, String>`:
`Input` is still the tuple of declared scalar/borrowed values. A merge hook
consumes `Result<Generic<'a>, String>` intermediate row views. The adapter
validates all handles before mutation, groups noncontiguous rows stably, and
keeps each group's rows in their original order. Single-group batches use the
same hook. No eager conversion precedes the hook's iterator consumption.

Both methods return `Result<(), AggregateBatchError<Self::Error>>`. Use
`.map_err(AggregateBatchError::Input)?` for a row decoding error and `?` for an
author error. The distinction preserves typed `RowError` categories and avoids
interpreting a wire failure as an author's business error. Defaults call the
ordinary row method and stop at the first error.

`ReduceWasm` overrides both hooks: it maps a bounded chunk through the input
lambda once, then combines pairs with batched calls until one state remains.
It combines new mapped values with any existing accumulator, matching native
reduce's map/combine contract. It retains the initial-value validation flag
separately from `seen`, so a neutral merge-created state can later accept raw
input. Within a chunk the example combines adjacent pairs, preserving left-to-
right concatenation; general parallel/native reduce still requires reduction
laws rather than a promise of ordered results. Large global and interleaved
queries, all stages, host row limit 3, raw/merge mixing, spill and checkpoint
continuation have C++ regressions against native results where applicable.

Lambda formal parameters shadow input columns with the same name. For example,
`(key,value) -> key+value` uses the supplied lambda arguments even when the SQL
input contains `key` and `value`. The native aggregate factory passes lambdas
separately rather than appending those columns as apparent captures. Real outer
column captures need a separate evaluator input contract: both this bridge and
native `reduce_agg` currently evaluate bodies against the formal-parameter ROW.

`lambda_call_batch(i32,i32,i32,i32)->i64` passes an explicit row count to the
same bound evaluator. It shares the single-use charged response protocol with
`lambda_result`; neither import re-enters a guest allocator. Native decoding
requires the declared number of rows and validates every outer argument ROW.
`maxLambdaCalls` counts RPCs, `maxLambdaRowsPerCall` bounds one batch, and
`maxLambdaEvaluations` bounds total evaluated rows per export (default
1,000,000); batching does not bypass the evaluation budget. A host that only
supports the older two imports rejects modules containing the batch import.
Existing single-row modules still work with the new host.

### Legacy aggregate interface

An aggregate may receive
one-row nested Arrow values as `ArrayRef` by declaring the corresponding
`argument_types`, for example
`#[velox_aggregate(argument_types = ["array<bigint>"])]` with
`type Input<'a> = (Option<ArrayRef>,)`. An aggregate can return a one-row
`ArrayRef` for each group by setting `return_type`, for example
`#[velox_aggregate(return_type = "array<bigint>")]`. The SDK concatenates these
one-row arrays during finalization.

The SDK exports `velox_wasm_alloc` and `velox_wasm_free`. Each macro invocation
adds a `(i32, i32) -> v128` entry point and automatically writes one JSON
function declaration into the `velox.udf.v1` Wasm custom section. A module can
contain any number of scalar and aggregate UDFs. The user does not write a
manifest or call a registration import. The Wasm export defaults to the Rust
function name, without a `batch` suffix; `export = "other_name"` can override
it. The SQL name defaults to the Rust function name; use
`#[velox_scalar(name = "plus")]` to override it.
The SDK infers SQL types and nullability from the Rust signature. Nullable
scalar inputs disable Velox's default null filtering. Scalar declarations may
also specify `deterministic` and `default_null_behavior`. Aggregate declarations
may specify `ignore_duplicates`, `order_sensitive`, and
`default_null_behavior`. `#[velox_aggregate]` derives the SQL name and export
prefix from the Rust struct name in snake case: `SumI64` becomes `sum_i64`
and `sum_i64_create`, `sum_i64_update`, etc. Use `prefix = "avg_f64"` to override
the default when needed.

The host loads and registers every declaration from the single `.wasm` file:

```cpp
#include "velox/functions/wasm/Registration.h"

facebook::velox::functions::wasm::registerWasmModule("/path/to/udfs.wasm");
```

Velox reads every declaration from the custom section, compiles the module
once, validates all batch exports, and registers them using
`WasmVectorFunction` or `WasmAggregate`. Keep the custom section in the final
`.wasm` when applying post-build optimization or stripping.

Registration is startup-only: complete native registration first, then load
modules before queries start. A module publishes all declarations or fails
without mutation. Names are normalized; native scalar functions cannot be
shadowed, disjoint WASM overloads can be added, and overwrite replaces only
matching scalar or aggregate signatures, retaining other overloads. Native
aggregate/window names cannot be shadowed. Keep the custom section when
stripping modules.

The host accepts deployment policy through the public registration API:

```cpp
using namespace facebook::velox::functions::wasm;
WasmOptions options;
options.memoryLimitBytes = 64UL << 20;
options.tableElements = 10'000;
options.tables = 1;
options.fuelPerCall = 100'000'000;
options.maxCallMillis = 1'000;
registerWasmModule("/path/to/udfs.wasm", false, options);
```

Fuel is refreshed per export, including start/alloc/free and aggregate calls.
Each logical invocation also has an epoch deadline, and full exec builds check
the current task cancellation token. Only valid row error records are
recoverable under TRY. Runtime/resource/protocol failures are fatal and discard
the Store; aggregate cleanup never propagates guest failure to native destructors.
Query Stores charge accessible linear pages to MemoryPool before creation or
growth, through the host memory creator. Configured limits remain independent
caps. Owned IPC and Arrow decompression allocations are also charged; the IPC/decompression cap is 64 MiB. Table/stack/JIT overhead and
compilation require separate process/deployment budgets. See the
[host contract](../../../docs/designs/wasm-udf.md).

## Comparing against native Velox functions

With the Velox build dependencies, Wasmtime 39.x C API, and Rust's
`wasm32-unknown-unknown` target installed, build the Release benchmark from
the Velox repository root:

```shell
cmake -S . -B build/wasm-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DVELOX_BUILD_MINIMAL=ON \
  -DVELOX_BUILD_UDF_BENCHMARKS=ON -DVELOX_BUILD_WASM_UDF_TESTING=ON
cmake --build build/wasm-native --target \
  velox_wasm_native_udf_benchmark velox_wasm_udf_smoke_test velox_wasm_safety_test -j4
ctest --test-dir build/wasm-native --output-on-failure \
  -R 'velox_wasm_(udf_smoke_test|safety_test|native_udf_benchmark_check)'
build/wasm-native/velox/functions/wasm/tests/velox_wasm_native_udf_benchmark \
  --udf_batch_rows=1024 --udf_warmup_batches=100 \
  --bm_estimate_time=true --bm_max_trials=30 --bm_max_secs=2
```

Minimal mode does not build the full execution/GTest targets. Use a separate
full build to run scalar/runtime, aggregation/spill/cleanup and the native
per-signature metadata regressions:

```shell
cmake -S . -B build/wasm-full -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DVELOX_BUILD_MINIMAL=OFF \
  -DVELOX_BUILD_TESTING=ON -DVELOX_BUILD_TEST_UTILS=ON \
  -DVELOX_BUILD_WASM_UDF_TESTING=ON -DVELOX_ENABLE_RPC=OFF
cmake --build build/wasm-full --target \
  velox_wasm_udf_test velox_wasm_aggregate_test velox_wasm_safety_test \
  velox_vector_function_metadata_test -j4
ctest --test-dir build/wasm-full --output-on-failure \
  -R 'velox_wasm_(udf_test|aggregate_test|safety_test)|velox_vector_function_metadata_test'
```

`VELOX_ENABLE_RPC=OFF` omits the unrelated RPC operators and their FBThrift
dependency. Keep it enabled when testing those operators. Other optional
connectors/formats and dependency locations follow the normal Velox build
configuration for the deployment.

The scalar benchmark compares checked `add_i64` with native checked Presto
`plus`, `prefix` with a matching nullable native Simple Function, and
`array_sum_dispatch` with `ArraySumFunction` registered explicitly as a Simple
Function. Native built-in vector `concat` is labeled separately as
`nativeVectorConcat`. Aggregate cases compare `avg_f64`/`sum_i64` with native
`avg(DOUBLE)`/`sum(BIGINT)`. Each array contains the integers 1 through 16, without NULLs.
Both sides receive identical values: non-null flat vectors for primitive
inputs and ArrayVectors with flat child elements for ARRAY sum; every row is selected. Scalar cases evaluate an
`ExprSet`; aggregate cases update one group and extract its result on each
batch. Input creation, registration, module compilation, initialization,
warmup, result checks, and destruction are outside the measured interval.
WASM timing includes Arrow IPC serialization, guest execution with fuel
metering, and output conversion. Timings are per batch, rather than per row.
Use `--udf_module_path=/absolute/path/to/module.wasm` to select a compatible
module while keeping the benchmark binary and native controls identical. The
default is the example module produced by CMake. Alternate modules must provide
the example declarations expected by the benchmark correctness checks. Alternate
old/new module paths between processes for comparisons; retain the source,
module and executable hashes together with all raw samples.
Repeat with `--udf_batch_rows=128` and `4096` to measure batch-size effects.
The optimized 8192-row ARRAY case also passes the default 100-million fuel
budget; larger inputs can still require smaller batches or an explicitly larger budget.
Additional scalar cases cover 25% NULL rows, dictionary and constant encodings,
every-eighth-row selection, 2048-byte strings, a typed eight-argument variadic
sum, and generic hashing of ARRAYs with 32 BIGINT elements. Each has a matching
native Simple Function baseline and an untimed output check. Dictionary and
constant cases include the expression engine's peeling/memoization behavior;
the sparse case reports cost per input batch, selecting one eighth of its rows.
`--udf_generic_array_elements=N` sets the generic hash input length (default 32),
with positive-size and native vector-index range checks. The October 5 repeat
at 128 elements still exhausted the default 100-million fuel
budget at 1024 rows; the measured case uses 32 elements. Larger batches/workloads
may need chunking or an explicit `--udf_fuel_per_call` deployment policy.
Nested hashing adds more work, but is not a substitute for application-specific
compute-heavy UDFs. Startup/JIT, process peak memory and concurrency are separate
measurements, not inferred from these timings.

The benchmark checks output correctness before timing and checks accumulated
aggregate results afterward. These cases measure steady-state function calls,
rather than complete query execution or module startup cost.

The output-codec diagnostics are separate from native Simple Function
comparisons: `wasmOutputCodecEagerUnused`, `wasmOutputCodecLazyUnused` and
`wasmOutputCodecLazyUsed` run the same generic_identity guest with a ROW holding
a plain BIGINT and an eight-byte codec value. Input gathering/codec encoding are
prepared once outside timing; guest input serialization/invocation and host
output IPC parsing/materialization remain timed. Selecting only the plain field
causes 1024 eager decoder calls versus zero deferred calls per 1024-row batch;
selecting the codec field forces all 1024 rows. Outputs and decoder counts are
checked before timing. This trivial memcpy codec is a diagnostic fixture; these
cases do not measure a whole query or establish gains for arbitrary codecs.

## SQL generics, variadic arguments, constants, and initialization

`velox_scalar` accepts Rust generic functions. Put SQL variables in the Rust
signature with `SqlValue<'a>`, `SqlKnown<'a>`, `SqlComparable<'a>` or
`SqlOrderable<'a>`; inline bounds and `where` clauses both work. Velox binds the
SQL types when it resolves an expression. The generated export calls the
original Rust function with borrowed `Generic<'a>` values, including inside
ARRAY/MAP/ROW and a `VariadicView` tail. The author handles one row, without a
RecordBatch parameter. This does not add a per-row owned value tree or eagerly
read unused nested children/tail arguments.

```rust
use velox_wasm_sdk::{velox_scalar, SqlValue, VariadicView};

#[velox_scalar]
fn identity<'a, T: SqlValue<'a>>(value: Option<T>) -> Option<T> {
    value
}

#[velox_scalar]
fn first<'a, T: SqlValue<'a>>(
    value: Option<T>, _rest: VariadicView<'a, Option<T>>,
) -> Option<T> {
    value // Unused tail values are not read.
}

// The function remains Rust generic and is directly callable outside Wasm.
assert_eq!(identity::<i64>(Some(42)), Some(42));
assert_eq!(identity::<&str>(Some("hello")), Some("hello"));
```

`Option<T>` controls nullable arguments and results. `Result<_, String>` or the
existing row error types report per-row errors through SQL TRY. A nonnullable
input infers default NULL propagation; retain `default_null_behavior` to choose
explicit behavior. Name/export, initialization, constants, ASCII and null-free
options remain available. Alternate callbacks on this form use bare function
paths and the same Rust type parameters in the same order as the primary
function; the macro instantiates them automatically. For example,
`rust_ascii_dispatch<T>` and `rust_head_dispatch<T>` in the demo use generic
callbacks, with the latter receiving `NullFreeArrayView<'a, T>`.
Do not repeat `arguments`, `returns`, `variadic` or
`type_variables` on the Rust generic form: these are inferred from its signature.

| Rust bound | SQL binding constraint | Available operations |
| --- | --- | --- |
| `SqlValue<'a>` | Any type, including UNKNOWN and portable OPAQUE handles | Read/return a row value; `sql_owned()` |
| `SqlKnown<'a>` | The bound variable root is not UNKNOWN | Same value operations |
| `SqlComparable<'a>` | Velox comparable types | `sql_equals()`, `sql_equals_with()` and `sql_hash()` |
| `SqlOrderable<'a>` | Velox orderable types | Also `sql_compare(other, flags)` |

Orderable implies comparable. Known is independent: Velox UNKNOWN is
comparable/orderable. The known constraint checks the bound variable root, not
all descendants: `T: SqlKnown` accepts `T = array(unknown)`, whereas
`ArrayView<T>` with `T: SqlKnown` rejects an UNKNOWN element type. `SqlKnown + SqlComparable` and `SqlKnown + SqlOrderable`
preserve both constraints. Comparison uses the existing SDK Velox semantics
for NaNs, NULLs and complex values. `sql_equals()` uses NullAsValue;
`sql_equals_with(other, NullAsIndeterminate)` returns `Option<bool>` so SQL
nullable equality can propagate nested NULLs, including in comparable MAPs.
The `rust_equal` demo uses this form and is checked against native SQL `=`. MAP is comparable
when its children are comparable, and is excluded from orderable signatures.
OPAQUE is excluded from comparison signatures. A dynamic `Generic` Rust type
alone does not prove a SQL constraint: the host checks these traits' exported
constraints before calling the function. Direct Rust callers of runtime views
must use values satisfying the same constraints.

Nested inputs retain ordinary Rust type relationships:

```rust
use velox_wasm_sdk::{ArrayView, MapView, RowView, ArrayWriter,
    SqlComparable, SqlValue, velox_scalar};

#[velox_scalar]
fn head<'a, T: SqlValue<'a>>(array: ArrayView<'a, T>) -> Result<Option<T>, String> {
    if array.is_empty() { Ok(None) } else { array.at(0) }
}

#[velox_scalar]
fn lookup<'a, K, V>(map: MapView<'a, K, V>, key: K) -> Result<Option<V>, String>
where K: SqlComparable<'a>, V: SqlValue<'a> {
    Ok(map.find(key)?.flatten())
}

#[velox_scalar]
fn field<'a, T: SqlValue<'a>, U: SqlValue<'a>>(
    row: RowView<'a, (T, U)>,
) -> Result<Option<T>, String> {
    row.get::<0>()
}

#[velox_scalar]
fn pair<'a, T: SqlValue<'a>>(a: Option<T>, b: Option<T>) -> ArrayWriter<T> {
    ArrayWriter::from(vec![a, b])
}

#[velox_scalar]
fn own<'a, T: SqlValue<'a>>(value: T) -> Result<T::Owned, String> {
    value.sql_owned()
}
```

These infer `(array(T)) -> T`, `(map(K,V),K) -> V`, `(row(T,U)) -> T`,
`(T,T) -> array(T)` and `(T) -> T`. Generic tuple rows bind by position without
requiring field names. A whole-row variable preserves its bound ROW names.
Repeated occurrences of a variable require the same SQL type, including the
variadic tail. `sql_owned()` explicitly materializes accessed values: direct
Rust `i64` stays `i64`, `&str` becomes `String`, typed views become their existing
owned writer/value types, and an exported runtime `Generic` becomes
`Value<'static>`. Copying an OPAQUE token does not extend its invocation scope.

For arithmetic or another operation that needs concrete Rust implementations,
use build-time instantiations with ordinary Rust trait bounds:

```rust
#[velox_scalar(instantiations = [i64, f64])]
fn add<T: std::ops::Add<Output = T>>(left: T, right: T) -> T {
    left + right
}
```

This emits BIGINT and DOUBLE overloads under SQL name `add`, with distinct Wasm
exports `add__instance_0` and `add__instance_1`. With multiple type parameters,
use tuples, e.g. `instantiations = [(i64, f64), (f64, i64)]` for `<T, U>`.
Duplicate SQL input overloads and incomplete instantiations are rejected.
Operations keep their Rust implementation semantics; use checked arithmetic and
return Result when the SQL function requires overflow errors.

The runtime form instantiates `T` as a borrowed `Generic` view once at build
time, so arbitrary traits such as `Add` are usable only when that view implements
them. The concrete form compiles a finite set of Rust instances; it cannot add
new Rust monomorphizations when a query arrives. This is Rust's compilation
model, rather than a Wasm type limitation. Current macro restrictions include
const parameters, higher-ranked SQL bounds, associated signature types other
than `T::Owned`, and trait/type aliases that require Rust type resolution.
These are frontend restrictions, not intrinsic Wasm restrictions. Top-level
Rust generic `velox_row_aggregate` impls still require a separate type-family
adapter; generic aggregate State/helper types retain their existing support.
See the runnable `rust_*` functions and their direct tests in
[the example module](examples/add/src/lib.rs).

The explicit SQL-signature form remains available for dynamic/custom types,
DECIMAL integer variables, and heterogeneous tails:

```rust
use velox_wasm_sdk::{velox_scalar, Generic, VariadicView};

#[velox_scalar(
    arguments = ["T"], variadic = "T", returns = "T",
    type_variables = ["T"],
)]
fn first<'a>(value: Option<Generic<'a>>,
             _rest: VariadicView<'a, Option<Generic<'a>>>) -> Option<Generic<'a>> {
    value // Unused tail values are not read.
}

// Concrete SQL types can be inferred, including a typed variadic tail.
#[velox_scalar]
fn sum(values: VariadicView<'_, Option<i64>>) -> Result<i64, String> {
    values.skip_nulls().try_fold(0_i64, |sum, value| {
        sum.checked_add(value?).ok_or_else(|| "sum overflow".to_owned())
    })
}

#[velox_scalar(arguments = [], variadic = "any", returns = "bigint")]
fn count<'a>(values: VariadicView<'a, Option<Generic<'a>>>) -> i64 {
    values.len() as i64 // No tail value conversion or per-row Vec allocation.
}

#[velox_scalar(arguments = [], variadic = "varchar", returns = "varchar")]
fn first_text<'a>(values: VariadicView<'a, Option<&'a str>>)
    -> Result<Option<String>, String> {
    values.skip_nulls().next().transpose().map(|v| v.map(str::to_owned))
}
```

Each function invocation handles **one row**. `len()` is the number of actual
tail arguments. `at(index)` returns `Result<T, String>` and reports an invalid
index. `get(index)` returns `Result<Option<T>, String>`; its outer Option is
None only for an out-of-bounds index, whereas a SQL NULL in a nullable tail is
`Ok(Some(None))`. `iter()` and `into_iter()` yield `Result<T, String>`.
`skip_nulls()` on `VariadicView<Option<T>>` yields `Result<T, String>`, skips
SQL NULLs and **preserves errors**. Do not use `Iterator::flatten()` on these
Results: Rust would silently discard Err values. Propagate errors with `?`,
`try_fold`, `collect::<Result<Vec<_>, _>>()`, or `transpose()` as above.
Borrowed string/Generic parameters use a named lifetime shared with the view.
For nullable tails, `is_null(index)` inspects outer NULL metadata without
converting values (including non-UTF8 VARCHAR). `may_have_nulls()` conservatively
checks source batch metadata; nested child NULLs remain separate.

The generated adapter checks all tail column types and lengths once per batch,
even for an unused tail or an empty batch. Fixed row arguments and variadic
columns prepare readers once per batch; bool, i8/i16/i32/i64 and f32/f64 cache
their Arrow casts, and Option caches outer validity metadata. Other values use
an access-time fallback. Reader preparation does not validate string contents
or traverse nested values. Variadic readers allocate their shared metadata once
per batch; row views/iteration allocate no argument Vec. Views implement Clone
and share the reader metadata without copying values or allocating. This replaces
the earlier Copy implementation; code reusing a moved view should call clone().
Borrowed results still borrow the original batch, rather than reader metadata.
Existing prebuilt Wasm modules and the host ABI remain compatible. Creating a row view, checking its
length or skipping indices does not convert values. `at`, `next` and
`next_back` read only the accessed argument; `nth`/`nth_back` skip the preceding
arguments without reading them. Strings and nested views remain borrowed;
choosing an owned element type such as String copies that value on access.
`first_text` above stops after its first non-NULL value. Invalid UTF-8 in a later
unvisited argument is not an error; accessing it produces a row error. This is
guest-side demand decoding: host gathering and IPC parsing still process the
complete input batch.

`Option` exposes NULLs to the function; a nonnullable signature enables Velox's
default NULL filtering. `Result` errors are attached to the failing row, also
for generic and variadic functions, so `TRY(sum(...))` preserves successful
rows. Panics and Wasm traps still fail the batch.

`arguments` contains the fixed prefix. `variadic` declares a final tail with
zero or more arguments and requires a final Rust `VariadicView<'a, T>` or
`Variadic<T>` parameter. Repeated SQL `T` arguments must bind to the same type;
`variadic = "any"` accepts independently typed arguments exposed as Generic
values. A declaration with `arguments = []` supports a zero-argument call. The
SDK preserves its row count and builds empty/all-null results from the bound
return type. A return type using a variable still needs that variable to be
bound; an entirely empty `T... -> T` call cannot infer `T`.

Ordinary Rust tests can call these functions directly:

```rust
let values = [Some(20), None, Some(22)];
assert_eq!(sum(VariadicView::from_values(&values)), Ok(42));
assert_eq!(sum(VariadicView::from_values(&[])), Ok(0));
```

`from_values` borrows the slice and clones only accessed elements; borrowed
Generic/string/nested values remain borrowed. There is no input RecordBatch in
the author function or this unit test. The existing `Variadic<T>` API remains
available for owned convenience: `from_values(Vec<T>)`, `get -> Option<&T>` and
ordinary value iteration. Its generated adapter creates a Vec and reads all
tail arguments before calling user code; use VariadicView for lazy access.

Use Velox signature syntax for generic nesting: `array(T)`, `map(K,V)`, or
`row("name" T)`. Concrete Hive syntax such as `array<bigint>` and structured
legacy declarations remain supported. Variables accept `T` (unconstrained),
`T:known`, `T:comparable`, and `T:orderable`. The host uses Velox's native
`SignatureBinder` for constraints, repeated variables, and return type
inference. These constraints describe eligible SQL types. Use the SDK comparator/hash
for standard types, including NaN and nested NULL semantics.

`integer_variables = ["p", "s", "r=min(38,p+1)"]` supports decimal signatures
such as `decimal(p,s) -> decimal(r,s)`. The host preserves precision and scale;
short and long Velox decimals both use Arrow Decimal128 in the guest. Change
the returned `Decimal` value's precision/scale to match the declared resolved
return type.
For overlapping overloads, registration prefers concrete fixed signatures,
concrete variadic signatures, generic signatures, and finally variadic generic
signatures. Within one class, more concrete type nodes in the arguments **and return type**
take priority. Equal priority keeps declaration order. Determinism and default
NULL behavior are tracked per overload; the bound signature receives its own
metadata. Name-level metadata is conservative across all overloads.

Initialization can precompute state from constants and explicit query settings:

```rust
use velox_wasm_sdk::{velox_scalar, InitContext, VariadicView};

fn initialize(context: &InitContext<'_>) -> Result<(), String> {
    let constant = context.constant_value(0)?.ok_or("expected constant")?;
    let suffix = context.config("my_suffix").unwrap_or("");
    // Inspect constant, compile a pattern, or save state in guest memory.
    // Each expression owns an isolated WASM instance.
    let _ = (constant, suffix, context.return_sql_type());
    Ok(())
}

#[velox_scalar(
    arguments = ["varchar"], variadic = "varchar", returns = "varchar",
    constant_arguments = [0], initialize = "initialize",
    config_keys = ["my_suffix"],
)]
fn initialized<'a>(prefix: Option<&str>, values: VariadicView<'a, Option<&'a str>>)
    -> Result<String, String> {
    let mut result = prefix.unwrap_or("").to_owned();
    for value in values.skip_nulls() {
        result.push_str(value?);
    }
    Ok(result)
}
```

`constant_arguments` uses zero-based signature positions. If the last,
variadic position is marked constant, every supplied tail argument must be
constant. A field reference does not satisfy the requirement even if its vector
happens to contain identical values. A constant SQL NULL does satisfy it.

The initializer runs lazily, once before the first selected batch, using the
same instance as subsequent scalar calls. Unselected branches do not run it.
Failures are cached and propagated without retrying initialization. Separate
expressions have separate guest state. Initialization also works with variadic
arguments; it is not restricted to fixed arity.

The initialization IPC stream has exactly one row and one column per actual
argument. Constants carry their value; nonconstants carry null placeholders.
Schema metadata contains `velox.constant_arguments` (a `0`/`1` mask),
`velox.argument_type.N` (the bound SQL type), `velox.return_type`, and
`velox.config.KEY`. A custom return also carries `velox.return_wire_type`, a
recursive wire description with codec identities/versions or OPAQUE identity.
`InitContext` exposes these fields and argument types.
Only keys listed in `config_keys` are sent. Explicit values take precedence;
otherwise registered `QueryConfig` property defaults are sent. Unknown keys
without explicit values return `None`. Values use the native configuration
string format; custom accessor parsing or clamping must be reproduced in the
guest. The generated initialization export has the same `(i32,i32)->v128`
ABI and returns a one-row Boolean acknowledgment. Existing ABI v1 modules need
no changes. All new scalar row declarations set `row_api = true`; for
those calls the host includes the resolved `velox.return_type` in the input
schema. The generated adapter returns a value column and an optional error
per row using the existing result/error-column ABI. This is internal protocol
metadata; Rust scalar signatures remain row-oriented.

See [the SFI capability assessment](../../../docs/designs/wasm-scalar-sfi-parity.md)
for a source-linked matrix, test coverage, and the remaining differences.
