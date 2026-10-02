# WebAssembly UDF and UDAF Integration Proposal

*2026-08-06*

## Status

This document records the earlier manifest-based design. The current
self-contained module design and its in-process untrusted-code rationale are
described in [the Wasm UDF RFC](wasm-udf-rfc.md).

It describes the original proposal for loading WebAssembly scalar UDFs and
aggregate UDFs (UDAFs) into Velox, including the initial ABI, external manifest,
Arrow IPC protocol, execution lifecycle, and Rust SDK responsibilities. Some
packaging and implementation-status statements below are now outdated.

## Motivation

Velox functions are normally compiled into the host binary. This proposal adds
support for loading functions from `.wasm` files during process startup and
registering them in the existing scalar or aggregate function registry.

The intended user experience is:

1. Implement a row-oriented scalar function or aggregate in Rust.
2. Use the Rust SDK macros to generate Arrow IPC batch entry points.
3. Compile the crate to a `.wasm` module.
4. Write a manifest that describes the Velox signature, the `.wasm` path, and
   the exported entry points.
5. Configure the host application to load the manifest before query execution.

The WebAssembly boundary is batch-oriented even though the user-facing Rust
API is row-oriented. User arguments, intermediate values, and results cross
the boundary as Arrow IPC record batches. Aggregate creation is a specialized
control call that takes a scalar group count and returns a raw handle array.

## Goals

- Load UDF modules once during process startup. Hot reload is not required.
- Support scalar UDFs through Velox `VectorFunction` registration.
- Support UDAFs through Velox `Aggregate` registration.
- Make one WebAssembly call per Velox batch, rather than one call per row.
- Let Rust users implement row-oriented functions while SDK macros generate
  the batch-oriented adapter.
- Keep active UDAF states inside WebAssembly linear memory.
- Support partial/final aggregation and spilling through serializable
  intermediate states.
- Preserve Velox selectivity, null, conditional-result, and aggregate lifecycle
  semantics.
- Use a small, versioned ABI that can be validated when a manifest is loaded.

## Non-goals

- Updating or replacing a loaded module without restarting the process.
- Passing Velox vectors directly into WebAssembly linear memory.
- A zero-copy host-to-guest data path.
- WASI filesystem, network, clock, or process access.
- Accounting WebAssembly state memory in Velox memory arbitration in the first
  version.
- Complex Velox types, generic signatures, and variadic signatures in the first
  version.

## Architecture

The integration consists of five layers:

1. **Manifest loader**: reads a user-written manifest and validates the declared
   function and ABI.
2. **WebAssembly runtime**: owns the Wasmtime engine, compiled-module cache,
   stores, instances, exported functions, and linear-memory access.
3. **Arrow IPC bridge**: converts between selected Velox rows and Arrow IPC
   buffers.
4. **Velox adapters**: implement `VectorFunction` and `Aggregate` and register
   them in the existing registries.
5. **Rust SDK**: decodes Arrow IPC, invokes row-oriented user code, maintains
   aggregate state, and encodes Arrow IPC results.

```text
manifest
   |
   v
WasmUdfLoader -----> shared Wasmtime Engine
   |                         |
   |                         +---- compiled module cache
   |
   +---- scalar registry ----> WasmVectorFunction ----> Store + Instance
   |
   +---- aggregate registry --> WasmAggregate --------> Store + Instance

Velox selected rows
   -> compact RowVector
   -> Arrow IPC
   -> guest linear memory
   -> Rust batch adapter
   -> row-oriented function
   -> Arrow IPC result
   -> scatter into Velox result
```

The process owns one Wasmtime engine. Compiled modules are cached by canonical
Wasm path and file identity. Each scalar expression or aggregate adapter owns a
separate Store and Instance. Compiled code is shared, but mutable guest memory
and state are not shared between adapters.

The initial target is `wasm32-unknown-unknown`. Modules must not import WASI or
other host capabilities.

## Packaging and Manifest

The manifest is written by the user. The Rust SDK does not generate it. A
manifest describes one Velox function and is the unit passed to the loader.
Multiple manifests may reference different exports from the same `.wasm` file;
the compiled module can still be shared by the runtime cache.

Paths in the manifest are resolved relative to the manifest file. The loader
must canonicalize paths before using them as cache keys.

### Scalar manifest

```json
{
  "abi_version": 1,
  "name": "my_add",
  "kind": "scalar",
  "wasm": "./my_udfs.wasm",
  "entrypoint": "my_add_batch",
  "arguments": [
    {"type": "bigint", "nullable": false},
    {"type": "bigint", "nullable": false}
  ],
  "return": {"type": "bigint", "nullable": false},
  "deterministic": true,
  "default_null_behavior": true
}
```

### Aggregate manifest

```json
{
  "abi_version": 1,
  "name": "my_avg",
  "kind": "aggregate",
  "wasm": "./my_udafs.wasm",
  "entrypoints": {
    "create": "my_avg_create_batch",
    "destroy": "my_avg_destroy_batch",
    "update": "my_avg_update_batch",
    "update_single_group": "my_avg_update_single_group_batch",
    "serialize": "my_avg_serialize_batch",
    "merge": "my_avg_merge_batch",
    "merge_single_group": "my_avg_merge_single_group_batch",
    "finalize": "my_avg_finalize_batch"
  },
  "arguments": [
    {"type": "double", "nullable": true}
  ],
  "intermediate": {"type": "varbinary", "nullable": true},
  "return": {"type": "double", "nullable": true},
  "order_sensitive": false,
  "ignore_duplicates": false,
  "default_null_behavior": false
}
```

There is no numeric function ID. The manifest path identifies the deployment
unit, and the manifest maps the Velox function directly to named exports in the
Wasm module.

At load time, the host validates:

- the ABI version;
- required manifest fields;
- Velox type names and nullability declarations;
- scalar versus aggregate metadata;
- the presence and Wasm signatures of all declared exports;
- function-name collisions in the Velox registry; and
- that the module imports no disallowed capabilities.

The manifest is authoritative for the SQL signature. A mismatch between the
manifest and the Rust implementation is a configuration error. The host detects
all structural mismatches it can at startup and validates IPC schemas on every
call.

## Core WebAssembly ABI

### Value types

WebAssembly does not have an `i128` integer value type. It has the SIMD `v128`
value type, which Wasmtime can return directly. The ABI uses `v128` as four
little-endian `u32` lanes:

```text
lane 0: status
lane 1: data pointer
lane 2: data length
lane 3: reserved
```

Conceptually, this is:

```c
struct AbiResult {
  uint32_t status;
  uint32_t dataPtr;
  uint32_t dataLen;
  uint32_t reserved;
};
```

The Rust SDK must explicitly return `core::arch::wasm32::v128`. It must not
return a Rust `u128` or C-compatible struct and assume that the compiler will
lower it to a Wasm `v128` result. Building the SDK requires the `simd128` target
feature.

### Common memory exports

Every module exports guest allocation helpers with the following Wasm
signatures:

```text
velox_wasm_alloc: (i32 length) -> i32 pointer
velox_wasm_free:  (i32 pointer, i32 length) -> ()
```

Input and output byte buffers are immutable while owned by the other side.
An entry point must not return an output buffer that aliases its input buffer.

### Function entry points

Scalar entry points and the aggregate `destroy`, `update`, `serialize`,
`merge`, and `finalize` entry points use this Wasm signature:

```text
(i32 inputPointer, i32 inputLength) -> v128 AbiResult
```

Aggregate creation is a control operation with a specialized signature:

```text
(i32 groupCount) -> v128 AbiResult
```

It returns a raw little-endian `UInt32[groupCount]` state-handle array rather
than Arrow IPC. An operation with no result returns a successful result with
`(dataPtr, dataLen) == (0, 0)`.

Aggregate single-group update and merge entry points avoid a repeated handle
column and use this signature:

```text
(i32 stateHandle, i32 inputPointer, i32 inputLength) -> v128 AbiResult
```

### Status and errors

`status == 0` indicates success. If `status != 0`, `dataPtr` and `dataLen`
identify a UTF-8 error message in guest memory. A non-zero status is a batch
protocol, aggregate-business, or runtime error. Scalar row-business errors are
transported separately in the successful Arrow result so Velox can associate
them with individual input rows.

Wasm traps, invalid memory ranges, malformed IPC, schema mismatches, and
resource-limit violations are query errors. A UDAF update is not transactional:
if it mutates some states and then returns an error, the query fails and the
partially updated states are discarded with the query.

### Ownership and call sequence

For an Arrow IPC call, the host:

1. Serializes the input batch into a host-owned Arrow IPC buffer.
2. Calls `velox_wasm_alloc`.
3. Validates the returned input range and copies the bytes into linear memory.
4. Calls the manifest-selected entry point.
5. Unpacks the `v128` result and validates the output range.
6. Decodes and consumes the result while the guest memory is valid.
7. Frees the output buffer, if present.
8. Frees the input buffer, including on error paths.

Host code must use guards so that input and output buffers are released when
decoding or scattering throws.

For aggregate `create`, the host passes the group count directly, validates
and copies the raw returned handle array, and frees that output buffer. There
is no input allocation or Arrow IPC encoding for this operation.

## Arrow IPC Contract

Version 1 uses the Arrow IPC streaming format with exactly one schema and one
record batch per non-empty payload. The first version disables IPC compression
and dictionary encoding. Repeating the schema has some overhead, but makes each
call self-describing and independently validatable.

Reserved runtime fields use the `__velox_wasm_` prefix. User argument names
must not use this prefix.

All arrays in a record batch have the same row count. The host and SDK validate
the row count and complete schema before consuming a batch.

The first version supports:

- boolean;
- signed 8-, 16-, 32-, and 64-bit integers;
- 32- and 64-bit floating point;
- UTF-8 strings;
- binary strings;
- dates; and
- timestamps.

Array, map, row, decimal, generic, and variadic types are deferred.

## Scalar Functions

### Input construction

`VectorFunction::apply` receives a `SelectivityVector` whose selected rows may
be non-contiguous. The host compacts only selected rows into an Arrow batch:

1. Collect the selected row indices in their original order.
2. Dictionary-wrap or gather every argument using those indices.
3. Build a compact `RowVector` with `N` rows.
4. Export the RowVector through the Velox Arrow bridge.
5. Serialize the resulting Arrow RecordBatch into Arrow IPC.

If all rows are selected and the arguments are already suitable for Arrow
export, the adapter may avoid the index vector and dictionary wrapping.

The active row indices remain on the host. They are not included in the IPC
batch.

### Guest execution

The generated Rust batch entry point:

1. decodes the Arrow IPC batch;
2. validates the declared argument types;
3. extracts one row at a time;
4. invokes the user's row-oriented Rust function; and
5. builds and serializes the output batch.

The WebAssembly boundary is crossed once per batch. The row loop runs entirely
inside the guest.

### Output and scatter

The output batch has exactly `N` rows. The host decodes it and scatters row `i`
to the original selected row index `selectedRows[i]`. It must preserve existing
result values at unselected positions, which is required for conditional Velox
expressions.

The host may decode directly from a non-owning view of linear memory if it:

- holds the Instance lock for the entire decode and scatter operation;
- performs no guest call that could grow or reuse memory;
- copies all output values into host-owned Velox buffers; and
- destroys all Arrow and temporary Velox views before freeing guest memory.

The initial implementation may instead copy the output IPC bytes to a
host-owned buffer before decoding. This is less subtle and should be the
default until the view-based path is proven correct.

### Null behavior

The scalar manifest declares `default_null_behavior`.

- When true, the host excludes rows with a null argument before constructing
  the IPC batch. The guest row function receives only non-null values.
- When false, validity information is preserved and the Rust function receives
  nullable values, typically as `Option<T>`.

The SDK supports nullable output values. A scalar row function may return
`Result<T, E>` where `E: ToString`. The generated result batch then contains a
nullable hidden `__velox_wasm_error: Utf8` column. An error row has a null value
and its error message in this column. The host maps compact rows back to the
original selected rows and records the errors in `EvalCtx`, which preserves
Velox `TRY` semantics. IPC decoding, schema, and other batch-level failures
still fail the whole invocation.

## Aggregate Functions

### Business row-oriented interface

The business UDAF implements exactly six row-oriented lifecycle methods. It
does not implement WebAssembly exports or Arrow IPC handling directly:

```rust
pub trait VeloxAggregate {
    type State;
    type Input<'a>
    where
        Self: 'a;
    type Output;
    type Error: std::fmt::Display;

    fn create() -> Result<Self::State, Self::Error>;
    fn destroy(state: Self::State);
    fn update(
        state: &mut Self::State,
        input: Self::Input<'_>,
    ) -> Result<(), Self::Error>;
    fn serialize(
        state: &Self::State,
    ) -> Result<Vec<u8>, Self::Error>;
    fn merge(
        state: &mut Self::State,
        intermediate: &[u8],
    ) -> Result<(), Self::Error>;
    fn finalize(
        state: &Self::State,
    ) -> Result<Self::Output, Self::Error>;
}
```

`update` is called once for each raw input row. `merge` is called once for
each non-null intermediate row and receives the business payload bytes; the
business implementation performs payload deserialization and state merging
inside `merge`. There is no separate business `deserialize` method.

`serialize` returns the complete intermediate byte sequence, and `merge`
receives exactly the same bytes. The SDK does not add a header, footer, magic,
or version field. `finalize` and `serialize` must not mutate state because Velox
may invoke extraction repeatedly. `destroy` consumes a state and is called at
most once for every successfully created state.

### State ownership

Active aggregate states live entirely inside the guest. The host stores only a
32-bit handle in each Velox group accumulator:

```text
Velox group accumulator                  WebAssembly instance
+----------------------+                 +------------------------+
| state handle: UInt32 | --------------> | handle -> Rust State   |
+----------------------+                 +------------------------+
```

Handle `0` is reserved as invalid. Guest-generated handles start at `1`. This
sentinel is a defensive check independent of Velox's group-row null and
initialized metadata: an uninitialized or cleared accumulator cannot
accidentally name a guest state. A handle is local to one Wasm Instance and
must never be passed to another Instance.

A handle is an opaque registry key, not a linear-memory address. The host never
dereferences it, and guest allocator activity or linear-memory growth cannot
change which state it identifies.

The host adapter reports:

```text
accumulatorFixedWidthSize() = sizeof(uint32_t)
accumulatorUsesExternalMemory() = true
isFixedSize() = true
```

Marking the accumulator as using external memory ensures that Velox invokes
the aggregate's destroy lifecycle for group state held by the guest.
`accumulatorUsesExternalMemory()` is a lifecycle signal; it does not make
Velox account for guest memory. `isFixedSize()` describes the host-visible
four-byte handle, not the guest State footprint.

The adapter does not override `accumulatorAlignmentSize()`, so the default
alignment is one byte. It must access the handle with unaligned-safe loads and
stores such as `folly::loadUnaligned<uint32_t>` and
`folly::storeUnaligned<uint32_t>`. It must never cast the accumulator address
to `uint32_t*` or use an accessor that assumes natural alignment.

### Guest state registry

Each generated UDAF owns a registry inside each Wasm Instance. The registry is
concrete state, not host metadata:

```rust
struct StateRegistry<S> {
    next_handle: Option<u32>,
    states: HashMap<u32, S>,
}
```

`next_handle` starts as `Some(1)`. Creating a state assigns the current value,
increments it with checked arithmetic, and inserts
`VeloxAggregate::create()` into `states`. Assigning `u32::MAX` changes
`next_handle` to `None`; a subsequent create fails the query with handle-space
exhaustion.

Handles are not reused during an Instance lifetime. This avoids an ABA problem
where a stale host handle could accidentally refer to a newly created state.
Removing a state releases the Rust state and its owned allocations for reuse by
the guest allocator. The number of live map entries follows the number of
active states, although hash-table capacity and WebAssembly linear-memory pages
may remain at their high-water marks after states are removed.

The generated Rust code uses a module-local, lazily initialized registry with
safe interior mutability, for example a `thread_local!` `RefCell` specialized
for the aggregate's concrete state type. Version 1 enables neither Wasm threads
nor reentrant host imports, and the host serializes calls to the Instance. A
future threaded or reentrant ABI must replace this storage strategy.

Registry operations are:

| Guest operation | Registry behavior |
|-----------------|-------------------|
| `create` | Allocate handles and insert newly created states. |
| `update` | Look up each handle mutably and call the row-oriented update method in input order. |
| `merge` | Look up the destination mutably and pass the opaque intermediate bytes to `merge`. |
| `finalize` | Look up each state immutably and call `finalize(&State)`. |
| `serialize` | Look up each state immutably and return the bytes from `serialize(&State)` unchanged. |
| `destroy` | Remove each state and pass ownership to the business `destroy` method. |

`update`, `merge`, `finalize`, and `serialize` reject handle `0` and unknown
handles. `destroy` is idempotent: handle `0` and already absent handles are
ignored so that error cleanup does not hide the original failure. Normal host
cleanup destroys all live handles through the business method. Dropping the
Wasmtime Store and Instance reclaims the entire linear memory even if a trap
prevents normal destruction, but code must not depend on Rust `Drop` side
effects during this abnormal teardown.

### Hidden state-handle column

For grouped aggregation, the state handle is a hidden `UInt32` Arrow column in
the same record batch as the user arguments:

```text
__velox_wasm_state_handle  arg0  arg1
-------------------------  ----  ----
12                         ...   ...
19                         ...   ...
12                         ...   ...
```

Rows zero and two update the same guest state. Handles are aligned one-to-one
with the compacted argument rows. A single `(inputPointer, inputLength)` pair is
therefore sufficient for both handles and arguments.

The SDK consumes the hidden column and does not expose it in the user's
row-oriented aggregate API.

The control column is constructed directly as an Arrow `UInt32` array. It does
not need a corresponding user-visible unsigned Velox SQL type because it is
created and consumed entirely inside the Wasm adapter.

The host first exports the compacted arguments as an in-memory Arrow
`RecordBatch`, inserts the handle array into that batch, and encodes the final
batch to IPC exactly once. It must not encode the arguments to IPC, decode them
again to add the handle column, and then encode a second time.

For single-group aggregation, the handle is passed as the first scalar Wasm
argument. Its Arrow batch contains only user arguments for raw input, or only
the Binary intermediate column for merge.

### Generated WebAssembly exports

The aggregate macro generates eight manifest-named exports. The host calls only
these batch exports; the adapter inside the guest invokes the business methods:

| Export | Wasm signature | Input | Output | Business call |
|--------|----------------|-------|--------|---------------|
| `create_batch` | `(i32 groupCount) -> v128` | scalar group count | raw little-endian `UInt32[]` handles | `create()` once per group |
| `destroy_batch` | `(i32 ptr, i32 len) -> v128` | Arrow handles | none | `destroy(State)` |
| `update_batch` | `(i32 ptr, i32 len) -> v128` | Arrow handles and arguments | none | `update(&mut State, Input)` once per row |
| `update_single_group_batch` | `(i32 handle, i32 ptr, i32 len) -> v128` | Arrow arguments | none | `update(&mut State, Input)` once per row for one state |
| `serialize_batch` | `(i32 ptr, i32 len) -> v128` | Arrow handles | Arrow Binary | `serialize(&State)` |
| `merge_batch` | `(i32 ptr, i32 len) -> v128` | Arrow handles and Binary | none | `merge(&mut State, payload)` once per non-null row |
| `merge_single_group_batch` | `(i32 handle, i32 ptr, i32 len) -> v128` | Arrow Binary | none | `merge(&mut State, payload)` once per non-null row for one state |
| `finalize_batch` | `(i32 ptr, i32 len) -> v128` | Arrow handles | one result column | `finalize(&State)` |

Except for `create_batch`, the aggregate entry points use these Arrow IPC
schemas:

| Operation | Input batch | Output batch |
|-----------|-------------|--------------|
| `update` | `__velox_wasm_state_handle: UInt32`, user arguments | none |
| `update_single_group` | user arguments | none |
| `merge` | `__velox_wasm_state_handle: UInt32`, `__velox_wasm_intermediate: Binary` | none |
| `merge_single_group` | `__velox_wasm_intermediate: Binary` | none |
| `finalize` | `__velox_wasm_state_handle: UInt32` | one user result column |
| `serialize` | `__velox_wasm_state_handle: UInt32` | `__velox_wasm_intermediate: Binary` |
| `destroy` | `__velox_wasm_state_handle: UInt32` | none |

`none` means that the operation returns a successful `AbiResult` with a null
pointer and zero length; it does not encode an empty Arrow IPC stream.

`create_batch` takes only the number of new groups. It calls the business
`create` method that many times and returns one handle per group in request
order. For example, handles `[12, 13, 14]` correspond positionally to the
host's new-group list `[groupA, groupB, groupC]`; no group ordinal crosses the
boundary. The output byte length must equal `groupCount * sizeof(uint32_t)`.
The SDK writes handles into a byte allocation using little-endian encoding so
the common byte-buffer deallocator can free it safely.

Creation is batch-atomic. If the business `create` method fails after some
states have been inserted, the SDK removes those new states, invokes the
business `destroy` method for each one, and returns an error without exposing a
partial handle array.

`update` preserves the order of selected input rows. The Rust SDK processes the
batch in row order, which is required for order-sensitive aggregates. The same
handle may occur many times.

`destroy` removes states from the guest registry and passes ownership to the
business `destroy` method. Destroying handle `0` or an already absent handle is
a no-op to make cleanup paths idempotent. Other operations reject invalid or
unknown handles.

### Velox lifecycle mapping

The adapter maps Velox aggregate methods as follows:

| Velox method | Guest operation |
|--------------|-----------------|
| `initializeNewGroups` | `create` |
| `addRawInput` | `update` |
| `addSingleGroupRawInput` | `update_single_group` with a scalar handle |
| `addIntermediateResults` | `merge` |
| `addSingleGroupIntermediateResults` | `merge_single_group` with a scalar handle |
| `extractValues` | `finalize` |
| `extractAccumulators` | `serialize` |
| `destroy` | `destroy` |

The adapter does not initially implement the optional `toIntermediate` fast
path.

### Host call sequences

For `initializeNewGroups`, the host keeps the new group pointers in a stable
ordered list, calls `create_batch(newGroupCount)`, validates that the returned
buffer contains exactly that many non-zero, distinct handles, and writes
handle `i` into accumulator `i`. It then frees the guest output buffer. A zero
group count is handled locally without a guest call. The host rejects a count
larger than `INT32_MAX` and never passes a negative `i32` value.

For `addRawInput`, the host compacts the selected rows, reads the destination
handle for each row from its group accumulator, builds an Arrow batch with the
hidden handle column followed by the user arguments, copies it into guest
memory, and calls `update_batch`. `addSingleGroupRawInput` builds an arguments-
only batch and calls `update_single_group_batch` with the destination handle as
a scalar ABI argument.

For `extractAccumulators`, the host builds an Arrow handle batch, calls
`serialize_batch`, validates a one-column Binary result with the expected row
count, and writes those opaque values into the Velox `VARBINARY` intermediate
result vector. The host never examines the business payload.

For `addIntermediateResults`, the host builds an Arrow batch containing the
destination handles and upstream Binary intermediate values, then calls
`merge_batch`. The guest SDK passes each non-null Binary value unchanged to the
row-oriented `merge` method.
`addSingleGroupIntermediateResults` builds an intermediate-only batch and calls
`merge_single_group_batch` with one scalar destination handle.

For `extractValues`, the host sends one handle per output group to
`finalize_batch`, validates the manifest-declared result schema and row count,
and scatters the returned column into the Velox result vector.

For aggregate destruction, the host sends all still-live handles to
`destroy_batch`, clears the accumulator handle slots, and finally drops the
Wasmtime Store and Instance. The guest removes each known state before calling
the business `destroy` method, so repeated host cleanup cannot call business
destruction twice.

### Intermediate state

Velox registers the UDAF intermediate type as `VARBINARY`. During partial
aggregation or spilling, `serialize` produces one opaque byte string per
state. During intermediate or final aggregation, `merge` receives these byte
strings and combines them into states owned by the current guest Instance.

The host never interprets intermediate bytes. The business `serialize` method
defines the payload encoding, and the business `merge` method deserializes that
payload and combines it with the destination state. The SDK transports the
byte sequence unchanged and there is no separate business or Wasm
`deserialize` entry point.

The SDK adds no header, footer, magic, version, checksum, or payload length.
The Arrow Binary array already carries each value's length. If the business
format needs identification, compatibility checks, or corruption detection,
the business implementation includes those fields in the bytes returned by
`serialize` and validates them inside `merge`.

Distributed execution requires all stages to deploy business implementations
with compatible intermediate formats. Compatibility is a deployment contract;
the version 1 SDK does not enforce it with a generic wire envelope.

`finalize` and `serialize` must not mutate aggregate state. Velox may call
extraction repeatedly, and spilling may call accumulator extraction
concurrently with other activity.

### Null behavior

The aggregate manifest declares `default_null_behavior`.

- When true, rows with a null user argument are excluded before IPC encoding.
- When false, argument validity is preserved and nullable Rust values are
  passed to the row-oriented update function.

The aggregate result may be null according to the manifest output type. A live
state's business `serialize` method returns a non-null payload. A null incoming
intermediate value represents no state contribution; `merge_batch` skips it
without invoking the business `merge` method. The adapter maps guest Arrow
validity bits to Velox nulls.

### Concurrency

A Wasmtime Store cannot be used concurrently. Each aggregate adapter therefore
serializes access to its Store, Instance, memory, allocator exports, and state
registry with a mutex. This is required in particular because Velox may call
`extractAccumulators` concurrently during spilling.

The same locking rule applies to a scalar adapter unless its execution context
guarantees strict thread confinement.

## Memory and Resource Limits

The first version stores only the 32-bit state handle in the Velox accumulator.
It does not report guest state sizes to Velox. Consequently:

- Velox sees four bytes of accumulator data per group;
- guest `HashMap`, `Vec`, `String`, and other allocations are absent from Velox
  group-size and query-memory accounting;
- guest state growth does not independently cause Velox to spill; and
- memory pressure can reach the Wasmtime limit and fail the query instead of
  triggering cooperative spilling.

This is an explicit first-version limitation. `isFixedSize()` describes the
host-visible accumulator as fixed size, not the actual guest state footprint.

Every Store must have a hard linear-memory limit. The runtime also configures a
maximum Wasm stack. Fuel or epoch interruption can be enabled to prevent
unbounded computation and integrate with query cancellation.

A future design may report per-state estimates or reserve total linear-memory
growth in a Velox memory pool. That work is separate from the initial ABI and
must not add a tracked-size field to the group accumulator until the accounting
semantics are defined.

## Rust SDK

The SDK consists of a runtime crate and a proc-macro crate.

A scalar user function is row-oriented:

```rust
#[velox_scalar(export = "my_add_batch")]
fn my_add(left: i64, right: i64) -> i64 {
    left + right
}
```

The macro generates the `(i32, i32) -> v128` export, Arrow IPC decoding, row
loop, output-array construction, IPC encoding, and ABI error conversion.

An aggregate exposes row-oriented state transitions:

```rust
#[velox_aggregate(prefix = "my_avg")]
impl VeloxAggregate for MyAverage {
    type State = AverageState;
    type Input<'a> = (Option<f64>,);
    type Output = Option<f64>;
    type Error = String;

    fn create() -> Result<Self::State, Self::Error>;
    fn destroy(state: Self::State);
    fn update(
        state: &mut Self::State,
        input: Self::Input<'_>,
    ) -> Result<(), Self::Error>;
    fn serialize(
        state: &Self::State,
    ) -> Result<Vec<u8>, Self::Error>;
    fn merge(
        state: &mut Self::State,
        intermediate: &[u8],
    ) -> Result<(), Self::Error>;
    fn finalize(
        state: &Self::State,
    ) -> Result<Self::Output, Self::Error>;
}
```

The aggregate macro generates the eight manifest-named batch exports and a
module-local `UInt32 -> State` registry. It does not generate or require a
`deserialize` method: business payload decoding is part of the business
`merge` implementation. `finalize` and `serialize` take immutable state
references to enforce side-effect-free extraction.

The SDK and handwritten manifest are intentionally separate. The SDK may offer
a validation CLI in the future, but the `.wasm` file does not need to contain
registration metadata.

## Build Integration

The host integration should be optional behind a CMake option such as:

```text
VELOX_ENABLE_WASM_UDF
```

Enabling it also enables Arrow support. Wasmtime and Arrow versions must be
pinned by the build. Wasmtime is linked through its C or header-only C++ API;
the integration must not expose Wasmtime types through public Velox APIs.

The proposed source layout is:

```text
velox/functions/wasm/
  Abi.*
  Manifest.*
  Runtime.*
  ArrowIpc.*
  WasmVectorFunction.*
  WasmAggregate.*
  Registration.*
  tests/

velox/functions/wasm/sdk/
  velox-wasm-sdk/
  velox-wasm-macros/
  examples/
```

The application embedding Velox is responsible for calling the manifest loader
before queries are planned or executed.

## Validation and Failure Handling

The host must validate all untrusted values crossing the boundary:

- allocation results are in current linear memory;
- `pointer + length` does not overflow and is in bounds;
- output row count matches the expected compacted row count;
- output fields match the manifest schema;
- group accumulators are initialized before their state handles are read;
- `create_batch` returns exactly `groupCount * sizeof(uint32_t)` bytes without
  multiplication overflow;
- state handles returned by `create_batch` are non-zero and distinct within
  the returned batch;
- state handles consumed by other operations are non-zero;
- aggregate operation schemas contain the reserved fields in the required
  order and types; and
- IPC payloads contain exactly one record batch.

Modules have no host imports in version 1. Wasmtime memory, stack, and execution
limits remain mandatory even when modules are trusted, because accidental
infinite loops and excessive allocation must not destabilize the host process.

## Performance Considerations

Arrow IPC necessarily introduces serialization and memory copies. The design
optimizes the call count, not zero-copy transport:

```text
Velox vectors
  -> Arrow IPC encode
  -> copy into guest
  -> guest IPC decode
  -> row-oriented user loop
  -> guest IPC encode
  -> host IPC decode
  -> Velox scatter
```

The adapter may split a Velox input into multiple Wasm calls when a configurable
maximum row count or IPC byte size is exceeded. It must not split individual
aggregate groups semantically; sequential chunks update the same handle in
input order.

Benchmarks should report at least:

- gather and Velox-to-Arrow conversion time;
- host IPC encoding time;
- host-to-guest copy time;
- guest IPC decoding time;
- user function time;
- guest IPC encoding time;
- host IPC decoding time; and
- scatter time.

Possible later optimizations include schema caching, direct output-memory
views, and dictionary encoding for the grouped state-handle column. These
optimizations must preserve the version 1 semantics.

## Testing Strategy

Tests are required at four levels:

1. **ABI tests** validate `v128` lane layout, allocation ownership, bounds
   checking, traps, and error cleanup.
2. **IPC tests** cover every supported type, nulls, empty batches, malformed
   streams, and schema mismatches across C++ Arrow and Rust Arrow.
3. **Scalar tests** cover sparse selectivity, conditional expressions,
   constant and dictionary inputs, null behavior, chunking, and result scatter.
4. **Aggregate tests** cover create counts and raw handle arrays, batch-create
   rollback, repeated handles, multiple groups, global aggregation, nulls,
   partial/final execution, spilling, repeated finalization and serialization,
   business state destruction, and concurrent accumulator extraction.

End-to-end tests build example Rust modules with the SDK and load them through
handwritten manifests.

## Implementation Phases

1. Add the Wasmtime runtime wrapper, ABI types, manifest parser, and startup
   validation.
2. Implement Arrow IPC conversion and scalar functions for primitive types.
3. Implement guest state handles, scalar-count `create_batch`, and the
   aggregate create, update, finalize, and destroy lifecycle.
4. Add business-defined intermediate serialization and merge, then validate
   partial/final and spill execution.
5. Add resource limits, cancellation, diagnostics, negative tests, and
   benchmarks.
6. Evaluate complex types and transport optimizations based on measurements.

## Alternatives Considered

### Function IDs and a shared dispatcher

A single `call(functionId, operation, pointer, length)` export can support many
functions, but duplicates information already present in the manifest. Named
exports are easier to inspect and validate and let the manifest directly select
the function. The design therefore has no function ID.

### Generated manifest embedded in the Wasm module

Embedding generated metadata would reduce handwritten configuration, but it
couples deployment naming and SQL registration to the Rust build. A handwritten
manifest is more explicit and allows multiple Velox registrations to reference
one module. The SDK therefore generates code only, not registration metadata.

### Separate handle and argument buffers

An aggregate update could accept a raw handle array and a second Arrow IPC
buffer. This requires two independent allocations and lengths and introduces a
new alignment invariant. A hidden `UInt32` column keeps handles and arguments
in one self-describing batch, so one pointer-length pair is sufficient.

### Host-owned aggregate state

Moving serialized state back to the host after every update would make memory
accounting easier, but adds serialization and copies to every batch and violates
the requirement that active state remain in the guest. The host stores only a
handle; state crosses the boundary only for intermediate aggregation or
spilling.

### Returning a pointer to `AbiResult`

Returning a result-structure pointer requires another guest-memory read and a
separate ownership rule for the structure. Wasmtime supports `v128`, which can
carry the four `u32` fields directly. The design therefore returns a packed
`v128`.
