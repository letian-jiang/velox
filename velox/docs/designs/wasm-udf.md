# WebAssembly scalar UDF and aggregate integration

Status: experimental implementation, updated 2026-10-05. This document defines
the current host contract; the [RFC](wasm-udf-rfc.md) explains the proposal and
[capability comparison](wasm-scalar-sfi-parity.md) describes native SFI differences.
The [community review](wasm-udf-community-review.md) retains the original findings
and their remediation status. This is not a production security certification.

## Authoring and execution

Rust scalar authors write ordinary row functions annotated with `velox_scalar`.
Arguments are scalars, borrowed byte/string views, `ArrayView`, `MapView`,
`RowView`, `Generic`, `VariadicView` (or owned `Variadic<T>`) and their
nullable/null-free forms. Scalar
user functions do not receive a RecordBatch. The generated adapter reads one
Arrow IPC batch, invokes the function for selected compact rows, and constructs
one result batch. Aggregate and legacy batch APIs have separate contracts.

A module embeds declarations as NUL-terminated JSON objects in `velox.udf.v1`.
No sidecar JSON is loaded. The host reads a bounded immutable byte snapshot,
parses its declarations and compiles exactly those bytes. The module cache
compares complete contents, independently of pathname, size or timestamp;
expired weak entries are removed and compilation occurs outside its mutex.
Compiled code can be shared, while each expression/aggregate owns its Store.

Standard scalar transport supports BOOLEAN, signed integers through HUGEINT,
floats, arbitrary-byte VARCHAR, VARBINARY, DATE, full-range TIMESTAMP,
DECIMAL, UNKNOWN and nested ARRAY/MAP/ROW. SQL type/integer variables and a
final variadic argument are bound by native SignatureBinder. The borrowed
variadic reader checks tail schemas once per batch, then converts only accessed
arguments without allocating a row Vec. NULL metadata can be checked without
value conversion. Nullable iteration preserves decoding
errors, including errors after a skipped NULL. The owned Variadic adapter remains
available and reads every tail argument before entering user code. Typed Rust ROW
views support tuples through arity 16; dynamic views handle larger rows.
Other logical types require explicit codecs. Custom and OPAQUE values use
codec ID/version/type identity or validated invocation-scoped object handles.
They do not expose a native pointer or arbitrary C++ object methods.

## Startup registration

```cpp
using namespace facebook::velox::functions::wasm;
WasmOptions policy;
policy.memoryLimitBytes = 64UL << 20;
policy.tableElements = 10'000;
policy.tables = 1;
policy.fuelPerCall = 100'000'000;
policy.maxCallMillis = 1'000;
registerWasmModule("/path/to/functions.wasm", false, policy);
```

Registration is **startup-only**: finish native registration first; do not run
queries or mutate native/window registries concurrently. Velox's window map is
not a concurrent hot-reload registry. Concurrent WASM registrations are
serialized. Export validation executes bounded module start functions before
publication. Every registry entry and replacement map is constructed first;
nonthrowing map swaps then publish scalar, aggregate and window entries. A
validation, collision or construction failure leaves the registries unchanged.
This does not promise a live transaction to queries resolving several registries.

SQL names are normalized before grouping/duplicate checks. A WASM scalar never
shadows a native Simple Function or native VectorFunction, including with
`overwrite=true`. Disjoint WASM scalar overloads from separate modules are
merged. Without overwrite, a matching dispatch signature rejects the entire
module; overwrite replaces only that signature, retaining other overloads.
Dispatch identity includes argument types, type-variable eligibility and
variadic shape, alpha-renames argument variables, and excludes return types,
constant flags and integer-variable constraints. Changing those properties of
the same dispatch slot requires overwrite. Aggregate overloads also merge/overwrite by dispatch slot; the corresponding
window entry receives all signatures, with conservative aggregate metadata.
Native aggregate/window names cannot be shadowed. The return count is the number
of distinct scalar/aggregate SQL names declared by the incoming module.

Aggregate registration also prepares the native `_partial`, `_merge`,
`_merge_extract` and scalar `_extract` companions. Their signatures and
return-type suffixes follow native AggregateCompanionSignatures. Generated names
are validated before publication and refreshed when the source overload set
changes; a collision rejects the whole module, including with overwrite enabled.
Companions do not receive window aliases and are excluded from the return count.
The native companion adapters forward raw constants. Return types must remain
inferable from their intermediate types under native companion rules; typed
intermediates preserve generic variables. These aliases primarily support
planner aggregate modes. Native extract propagates whole-call UserError even
under TRY; the WASM wrapper releases a failed extraction instance before any
later API retry. See [UDAF alignment](wasm-udaf-parity.md) for examples and tests.

## ABI and ownership

The host supports ABI 1, 2 and 3. ABI 2 requires `row_api=true` and adds typed
invocation errors. ABI 3 is restricted to row aggregates and adds retained-state
size reports and a required compact entrypoint; generated row UDAFs declare ABI 3.
Existing scalar and legacy aggregate declarations retain ABI 1. Export call
signatures are identical, but ABI 3 success payloads have the layouts below;
ABI 1/2 modules retain their original payload contracts.
The allocator is
`(i32)->i32`, free is `(i32,i32)->()`, scalar/batch exports are
`(i32,i32)->v128`, aggregate create is `(i32)->v128` and single-group update is
`(i32,i32,i32)->v128`. The v128 result contains four little-endian u32 lanes:
status, output pointer, output length, reserved. Reserved must be zero;
nonzero status is an invocation failure, not a recoverable scalar row error.
Status 0 succeeds, 1 carries a legacy error message, and `0x100 + code` carries
a native non-OK StatusCode (1–11) with its message as the output bytes. Code 1
becomes VeloxUserError; codes 2–11 become VeloxRuntimeError, matching native
EvalCtx::setStatus. Unknown nonzero statuses remain fatal protocol failures.
Every failed invocation invalidates the Store, including typed UserError.
Inputs and output allocations must be disjoint. The host copies output before
freeing guest buffers. wasm32 addressing and the SIMD return are ABI choices.

The transport profiles remain compatible across these versions. Legacy
batch/aggregate exports retain
their original Arrow physical types and i64-nanosecond timestamp range.
`row_api=true` scalar declarations use logical markers for full TIMESTAMP
(seconds/nanos), HUGEINT (high/low), arbitrary-byte VARCHAR and custom/OPAQUE
bridges, recursively inside nested types. The host verifies the expected
schema and required marker identities; unrecognized ABI versions or required
markers are rejected. Optional metadata is not feature negotiation. An
incompatible required extension needs a new ABI version or an explicitly
specified capability negotiation protocol before adoption.

Results contain exactly one record batch and one value column, optionally
followed by nullable UTF-8 `__velox_wasm_error`. Row counts must match selected
inputs, an error row must have a NULL value, and extra batches are rejected.
Arrow 18 IPC is the current transport implementation. The host checks the
expected logical/physical schema before constructing arrays, then checks one
record batch, exact row counts, `ValidateFull`, Velox index bounds and no bytes
after end-of-stream. Unsupported/dictionary schemas are rejected before data
construction. Full validation includes offsets, child lengths, validity and
DECIMAL precision. MAP bodies are first read with the identical LIST/STRUCT
layout: Arrow's MapArray constructor would otherwise abort on NULL entries or
keys before returning a validation Status. After validating those invariants,
the host restores MAP descriptors while sharing the same value buffers.
A key itself must be non-NULL, but nested key children may be NULL, as with
native Simple Function views/writers and map_from_entries. The stricter
indeterminate-key restriction belongs to the SQL map() constructor.
Decoding depth is limited to 64.

The host can retain IPC storage through imported vector ownership and charges
it to the query MemoryPool until its last owner releases it. Arrow reader
allocations, including decompression, use a bounded pool; its lifetime follows
outstanding buffers. The default and current absolute IPC/decompression cap
is 64 MiB, independently of guest linear memory. Borrowed decoder results
copy variable-width leaves before the borrowed input is released.

Scalar nested custom-codec outputs can retain their serialized payloads in
native LazyVectors. Unused ROW fields, ARRAY elements and MAP values do not
invoke the codec decoder until a consumer loads them. The first nonempty load
materializes the entire codec column: native LazyVector loaders are single-use,
and UDF results may be reused by consumers of other rows. This is column-level
output deferral, not selective row decoding or guest-triggered host loading.
MAP keys and invocation-scoped OPAQUE handles remain eager; top-level scalar
codec results are materialized before returning from apply. Aggregate and
borrowed-IPC decoding retain their eager contract. Owned-IPC callers can opt in
with `ArrowIpcDecodeOptions::lazyCodecs`.

The lazy vector retains IPC and MemoryPool ownership through full vector/base
destruction. Its failure callback holds a weak Store reference, so results can
outlive their producer without retaining guest memory or a dangling pointer.
Decoder failures are fatal system errors, bypass TRY, discard partial decoded
values and invalidate a live Store; repeated attempts cannot expose a partial
vector. Schema, complete Arrow data, codec identity/version, payload presence,
MAP key structure, handles and value/error exclusivity are checked before any
lazy codec result is published. Serialized codec contents are interpreted on
load. Ordinary ROW readers, sparse copying or downstream operators may force
columns; the native LazyDereference path can preserve unused fields.

## Errors and lifecycle

Only a valid scalar error column creates recoverable row errors. SDK `Result`
and `RowError` preserve TRY behavior; typed system Status remains fatal.
A trap, deadline/fuel exhaustion, invalid IPC/ABI/handle or host bridge failure
is a system failure, bypasses TRY and permanently invalidates the Store.
The checked arithmetic examples return row errors for integer overflow;
one bad row does not invalidate otherwise successful rows.

Guest aggregate destroy is best effort and nonthrowing to native callers.
A trap, bad acknowledgment or host allocation failure causes Store disposal;
host group handles are cleared regardless. Store disposal does not depend on
guest cleanup succeeding. This protects RowContainer destruction/unwinding.
Aggregation serialize/merge offers an intermediate representation, but is not
a claim of complete native memory-arbitration or spill efficiency parity.

## Row aggregate contract

`VeloxRowAggregate`/`#[velox_row_aggregate]` expose row inputs and owned group
State, immutable serialize/finalize, and typed logical intermediate results.
Native SignatureBinder supports variables/constraints, nested and variadic
signatures across stages. Generated initialization supplies bound types,
constant masks and declared configuration; State creation is per group.
Original raw constants are not implicitly available in downstream merges.
Default NULL behavior filters raw outer NULLs and keeps empty groups NULL;
non-default merge also receives outer-NULL intermediate values. The legacy
VARBINARY/ArrayRef aggregate ABI is retained without changing its transport.

Whole aggregate bridge operations are serialized, including concurrent spill
extraction. Create handles are nonzero/unique against live batches. Native
window execution supplies constant inputs, and repeated extraction must not
change accumulator contents. Row UDAFs implement toIntermediate through
per-row temporary state creation/update/serialization/destruction. Typed
intermediates work through partial/intermediate/final and spill operators.
OPAQUE state requires serialization codecs; invocation handles cannot be kept
across calls. Aggregate Err is an export failure, not a scalar row error column.
`type Error = RowError` preserves native Status categories through create,
update, serialize, merge, finalize and toIntermediate. Ordinary ToString errors
retain the legacy fatal export-error contract. Error types must be `'static`;
this does not impose a lifetime on borrowed Input views. A scalar whole-export
typed UserError is still a fatal bridge failure under TRY; only its validated
row-error column can produce a recoverable scalar error.
Row aggregate State implements AggregateState. Primitives and owned standard
values have recursive reserved-capacity accounting/compaction; structs can derive
it and custom/shared allocators implement an explicit ownership policy. Guest
inline State and its owned heap capacity are reported; Store bookkeeping and
fragmentation remain covered by the accessible-page charge. Borrowed/OPAQUE invocation
values cannot be retained as owned State.

ABI 3 success payloads are little-endian 12-byte records `(u32 handle,u64 bytes)`:
create reports all created groups; grouped update/merge reports every affected
unique group once; single update/merge reports its one group. Serialize/finalize
prefix unique-group records before their normal IPC result, so interior-cache
growth is accounted after extraction. The host knows the affected handles and
therefore the prefix length. Compact accepts one handle column and returns the
same records after recursive capacity shrinking. Initialization/destroy return
empty payloads; toIntermediate returns plain IPC after destroying temporary State.
Unknown/duplicate/missing handles, invalid lengths, per-state bounds and total
reported live-size overflow fail the entire call and invalidate Store. All reports
are validated before row-size changes are published.

Native row-size counters include this aggregate's State bytes and preserve other
keys/aggregates; the prior report is stored beside its host handle. Row size is
not another query-pool charge. Guest compact makes capacity reusable. With
`state_checkpoint = "owned-v1"` and paired checkpoint/restore exports, the host
can also rebuild a fragmented Store while groups remain active. It saves all
owned State and the handle high-water mark to a query-owned buffer, validates
membership/framing, releases old pages, initializes the same context and restores
without calling create/merge. Every live group's size is updated, even when native
compact receives a subset; native NULL flags and shared counters are preserved.
Only the actual reduction from pages held at compact entry is returned as reclaimed
bytes. Insufficient workspace or insufficient fragmentation returns zero, letting
native continue to spill. No physical reduction is inferred from logical sizes.

When the last group is successfully retired, the Store is released. Native
spill/flush continues to use SQL intermediates, which need not preserve all private
update state. Failed migrations invalidate the whole aggregate, including failure
while constructing its replacement Store; cleanup safely removes native handles
and only its own row-size contribution. Companions forward compact capability.
The checkpoint feature is optional/additive to ABI 3 and an explicit owned-State
reconstruction contract. Whole-Store snapshots need temporary capacity; streaming/
segmented reclamation and exact non-linear runtime charging remain obligations in
[UDAF alignment](wasm-udaf-parity.md).

### Optional owned-State checkpoint protocol (ABI 3)

Declarations specify `"state_checkpoint": "owned-v1"` and nonempty `checkpoint`
and `restore` entrypoints. Both are required; unmarked entrypoints or an unknown
protocol are rejected. Older ABI-3 hosts can ignore this capability. ABI-1/2
modules keep their payloads and cannot declare it.

- checkpoint: `(i32 maxOutputBytes) -> v128`; success returns owned raw bytes.
- restore: `(i32 stateAllocationBudget, i32 pointer, i32 length) -> v128`;
  success returns the ordinary `(u32 handle,u64 State bytes)` report for every
  restored group. The target adapter must be initialized and empty.
- Snapshot envelope: four ASCII bytes `VWS1`, little-endian `u32 nextHandle`,
  `u32 stateCount`, followed by records `(u32 handle,u32 State length,State bytes)`.
  Zero nextHandle preserves exhausted handle space. Handles are nonzero/unique,
  match the host's entire live set, and are below nextHandle unless exhausted.
  The SDK encodes records in handle order and rejects trailing/truncated bytes.
- State encoding is private to the same compiled module: it includes owned fields,
  float bits and full-width integers, preserving update configuration/caches that
  SQL intermediate can omit. Lengths, allocations and recursive nesting are
  checked. The standard codecs never retain invocation OPAQUE/borrowed values.
  This is not a cross-version module upgrade or durable spill format.

## Resource and cancellation contract

| Resource | Current control |
| --- | --- |
| Module bytes | Bounded snapshot, default 64 MiB; metadata 1 MiB / 256 declarations |
| Linear memory | Per-Store limiter, default 64 MiB; accessible pages charged to native MemoryPool before initial commitment and each growth |
| Table storage | Finite per-table element limit (10,000) and table count (1) |
| Stack | Wasmtime maximum 2 MiB |
| Export execution | 100-million fuel units per exported call; finite positive policy |
| Logical invocation deadline | 1 second by default, includes allocator, entrypoint and deallocator |
| Cancellation | Epoch ticker every 10 ms; current Driver task token checked inside guest execution, including start |
| Host output | Pointer/range/alias checks and configured output byte limit, default 64 MiB |
| Arrow decoding | 64 MiB allocation budget, native MemoryPool charge, no decompression worker threads |

Fuel is refreshed per export; it is not a query-wide CPU budget. Epoch polling
is a scheduling mechanism, not a guaranteed real-time latency bound. Query
Stores are created lazily after a pool and current task token are available;
standalone callers can provide their own cancellation check and pool.
The Wasmtime 39 host-memory creator charges accessible linear pages before
making them readable/writable. It reserves the virtual address range and guard
requested by Wasmtime, keeps the base pointer stable and commits only current
pages. Those inaccessible reservations/guards are not charged as allocated
query bytes. Committed address space is not a measurement of RSS. Newly grown
pages are zero-filled. Growth callbacks capture their original Store/pool owner,
including execution on another Driver thread. Pool allocation failures are
remembered across the C/Rust callback and propagated as fatal native errors even
if guest code handles memory.grow's -1. Constructor, allocator, entrypoint and
free paths all check them; invalidation unmaps memory and releases the charge.

Store attachment after standalone creation charges all currently accessible
pages before subsequent query calls. The implementation uses guarded POSIX
mappings, disables data-image CoW and shared/threaded guest memory, and accepts
standard 64-KiB Wasm pages. Shared memory must not bypass the accounting callback.
The current validation is on Linux; other supported host platforms still require
build and resource tests. See the version-pinned [MemoryCreator contract](https://docs.rs/wasmtime/39.0.0/wasmtime/trait.MemoryCreator.html)
and [C API implementation](https://github.com/bytecodealliance/wasmtime/blob/v39.0.0/crates/c-api/src/config.rs).

Tables, JIT artifacts, compilation CPU, metadata objects and Wasmtime internal
allocators are not exactly charged to the query pool. Deployment must bound
active modules/instances, registration concurrency and process RSS, and should
isolate compilation of adversarial modules. The registration path is privileged
startup work. Neither linear limits nor epoch interruption control JIT work.
There is no arbitrary guest access to MemoryPool, files, network or services.
Only the exact typed `velox_udf_v1.lambda_call`, `lambda_call_batch` and
`lambda_result` imports are allowed. Authority is bound exclusively to declared row UDAF lambdas in a query;
scalar/legacy instances and module start have no callback authority. WASI and
all other imports are rejected.

### Aggregate lambda capability

Row ABI 3 manifests can declare `lambda_callback = "ipc-v1"` and separate
`lambdas` FUNCTION signatures. The Rust macro emits these fields from
`#[velox_row_aggregate(..., lambdas = ["function(S,T,S)", ...])]`. Value argument
tuples exclude the functions; `InitContext::lambda(index)` supplies a symbolic
`Lambda`, whose `call(&[&Value])` returns an owned value. Generics/nested types
bind through native SignatureBinder. IPC is internal to this callback.

The call import returns a Store-local single-use result token and length. The
read import validates exact token/length and guest range, copies, then consumes
the charged result. Reentry into the guest allocator is unnecessary. Pending
results cannot cross an invocation; malformed requests or response reads poison
the Store. A `noexcept` native bridge captures evaluator exceptions and rethrows
them after Wasmtime returns, preserving native error categories. Existing
input/output byte limits apply, with per-export RPC and cumulative evaluation
quotas, per-call row limits and task cancellation/deadline checks. The new
`lambda_call_batch(i32,i32,i32,i32)->i64` additionally declares the exact row
count, which native decoding validates before vector evaluation. Rust
`Lambda::call_batch` takes borrowed Value columns with length-one broadcast.
Optional `update_batch` / `merge_batch` methods consume iterators of ordinary
row arguments/intermediate views; generated exports use those methods only
when the author overrides them. Grouping keeps each group's input order and
validates every handle before mutation. Native reduce-style mapping/tree
combination replaces the per-row callback path in `ReduceWasm`. Guest fuel
does not interrupt a synchronous native function mid-body. Further IPC and
native optimization work remains. See [UDAF alignment](wasm-udaf-parity.md).
The imports are an explicit additional ABI-3 capability. Earlier hosts that
reject imports cannot load such a mixed module, including its scalar exports;
this host continues to load older modules without these imports.

Lambda formal parameters shadow identically named input fields. AggregateInfo
passes lambda expressions separately without treating formal names as capture
columns. Actual outer-column captures need an explicit evaluator/stage contract:
both native ReduceAgg and the Wasm bridge currently provide only formal-parameter
ROWs for evaluation. FUNCTION plus a final variadic tail is supported: functions
follow the fixed value prefix and precede the tail. Rust row tuples and constant
argument indices exclude functions. The native SQL resolver accepts expanded or
empty tails, and SignatureBinder binds known value types after unresolved
lambda placeholders. Variables used only by a generic tail still need a typed
tail value for inference; a fixed-T value can bind an empty tail. Existing
fixed-lambda manifests preserve their layout; older fixed-only hosts reject
new variadic-lambda declarations.

Independent merge/extract signatures may erase input-only generic lambda types.
Those slots retain their declared positions with explicit
`velox.lambda.types_bound.<index>=false` initialization metadata, omit input/output
type descriptors, and cannot be evaluated. Rust `Lambda::has_bound_types()`
reports that state; `call`, `call_batch` and row-limit access fail explicitly.
Types required by the intermediate or result must still resolve. This allows
callback-free companion stages without inventing a replacement for erased T.
Knowing a lambda's types does not grant expression authority: any actual callback
also requires the query's bound native expression/evaluator. Older typed-only
SDKs ignore the new true marker and retain their ordinary behavior; they reject
an unbound slot's missing type descriptors during initialization. New SDKs accept
older fully typed messages without a marker. Callback-dependent independent
companions still need an explicit expression-carrying planner/API contract.

Native named ROW signatures check field names, while anonymous ROW signatures
bind by position. The bridge resolves a compatible intermediate's Arrow field
names before grouped/single merge, including nested ROWs inside ARRAY/MAP,
without changing caller vectors or copying value buffers. It preserves wire
markers and exact leaf/DECIMAL/custom/OPAQUE identity. Initial intermediate
constants/placeholders use the same resolved type. This is an intermediate
transport operation; raw/scalar binder rules remain native.

Aggregate initialization also records whether its factory bound raw or
intermediate input types. This differs from the operator step: native staged
plans may construct a final operator using raw types. Direct merge constants
use `velox.aggregate.intermediate_constant`; the raw configuration constant mask
is cleared so intermediate data cannot be mistaken for an original parameter.
`InitContext::aggregate_input_types()` and `intermediate_constant_value()` expose
this contract. An intermediate constant is an initialization hint, not an initial
contribution: merge still processes every selected row. Constant SQL NULL is
distinguished from a missing/nonconstant hint. The common native scalar extract
adapter does not supply constants to its aggregate factory. Typed direct merge
instances reject raw update and do not advertise raw `toIntermediate`.

## Validation and performance

Build/run commands are in the [SDK README](../../functions/wasm/sdk/README.md).
The standalone safety target covers malformed IPC, decompression limits,
resource controls, cancellation, Store invalidation, collisions/overloads and
cleanup. Full scalar/runtime/aggregate GTests and shared expression metadata
regressions exercise execution integration. Passing these tests does not
replace broader sanitizer/fuzzing and deployment load testing.

The benchmark separately identifies checked native Simple Function add,
matching nullable Simple Function prefix, built-in vector concat, native Simple
Function ARRAY sum, and native aggregates. Registration, compilation, warmup,
initialization and final destruction are outside steady-state batch timing.
The 2026-10-02 results predate validation/epoch/accounting changes and must not
be labeled current measurements. The benchmark now includes NULL, dictionary/constant, sparse-selection, large
string, generic nested hash and variadic cases. These remain selected workloads,
not a claim of broad performance parity. Guest view
access is lazy, but the IPC batch is serialized eagerly; this is not end-to-end
demand decoding. For fully selected inputs of matching length, the host can
export ordinary ROW/ARRAY/MAP descriptors directly when their leaves are flat
or scalar dictionary/constant vectors over supported flat storage. Arrow
flattens only the encoded leaves, retaining plain sibling buffers. Selection
indices are allocated only when a gather fallback needs them. Encoded complex
values, UNKNOWN encodings and unloaded lazy bases keep the native gather
fallback; the capability check does not load a lazy base. Sparse selection
continues to copy only selected complex rows. IPC still copies payloads into
guest memory, and this optimization does not provide native vector/value-hook
pushdown or remove the transport boundary.

The `arrowGatherPlainRow`/`arrowGatherDictionaryLeafRow`/
`arrowGatherConstantLeafRow` benchmark cases diagnose host gather plus IPC
serialization. They do not execute a guest or compare native Simple Functions.

Guest BOOLEAN/TINYINT/SMALLINT/INTEGER/BIGINT/REAL/DOUBLE column construction
reads borrowed scalar leaves or owned scalars directly into typed Arrow input,
avoiding an intermediate owned Value vector. This is an SDK optimization for
primitive outputs, nested primitive children and lambda arguments, preserving
NULL/type checks and floating-point bit patterns. It does not remove IPC or
change the row author interface, state ownership contract or guest ABI.

A validated scalar business error keeps its native `VeloxUserError` category
outside TRY and does not invalidate a healthy Store. Only exceptions from the
validated error-reporting step receive this treatment; a UserError originating
from a bridge/protocol failure remains fatal. All structured row-status payloads
are validated before any business error can stop reporting, so a malformed
later row cannot hide behind an earlier valid user error. Non-user statuses
remain fatal and invalidate the Store, including inside TRY.

## Borrowed generic operations and ASCII metadata

`Generic::hash/compare/equals` traverse borrowed guest Arrow arrays instead of
materializing whole SQL values. ARRAY/ROW comparisons short-circuit, while hash
must read every element. MAP comparison sorts index vectors and borrows keys
and values. Codec semantics borrow payload bytes; their headers and registry
lookup may allocate. Owned `Value` operations do not clone trees. Mixed
owned/borrowed comparison reads standard scalars, strings and reached nested
children directly; MAP allocates sorted entry indices, while mixed custom codec
comparison retains payload materialization and registered semantics. `sql_key`
remains an explicit owned-value operation. Host input loading, IPC parsing and
custom output codec decoding remain eager; this is guest-side demand decoding.

The SDK emits optional scalar manifest `has_ascii=true` when a `call_ascii`
alternate exists. That implementation requests native Expr input ASCII caching
and passes its conservative batch flag as IPC schema metadata
`velox.scalar.ascii_inputs=true|false`. NULL and nested strings do not affect
the top-level VARCHAR decision. A missing key supports older hosts by scanning
the guest batch once. The adapter selects the ASCII alternative once per batch.

For all VARCHAR scalar results, the host computes ASCII state from actual
returned bytes after scatter. No guest declaration can mark non-ASCII output
as ASCII. This caches validated metadata on the vector returned by apply;
expression-created dictionary wrappers have independent caches, as with native
vectors. The native input-to-output preservation hook is intentionally unused
because the guest's result is untrusted. The output scan is part of measured
batch execution, not an untimed benchmark optimization.
