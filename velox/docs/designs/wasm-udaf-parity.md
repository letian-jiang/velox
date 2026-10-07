# WASM UDAF and native aggregate capability alignment

Assessment: 2026-10-05, using this checkout's `exec/Aggregate.h`,
`exec/SimpleAggregateAdapter.h`, `AggregateInfo.cpp`, `GroupingSet.cpp`,
`DistinctAggregations.cpp`, aggregate companion registration and
`window/AggregateWindow.cpp`. This assesses aggregate contracts separately from
scalar Simple Functions. Native aggregates can accept SQL lambdas through an
expression evaluator; the scalar SFI limitation must not be generalized to UDAFs.

The preferred Rust interface is `VeloxRowAggregate` with
`#[velox_row_aggregate]`. `Input<'a>` is a tuple of scalar values or borrowed
nested/Generic/VariadicView arguments; an author never receives a RecordBatch.
Each group has guest-owned State, updated across batches, with immutable
serialize/finalize methods. Input views cannot safely be retained in State;
`materialize()` produces owned values for storage. Extraction must preserve the
accumulator's SQL content because native window execution can extract repeatedly.

The older `VeloxAggregate`/`#[velox_aggregate]` ABI remains compatible. Its
VARBINARY intermediate and Arrow ArrayRef nested examples are legacy interfaces;
its i64-nanosecond TIMESTAMP wire does not have the full-range row protocol.
New row exports declare ABI 3 with state reports while preserving the v1
entrypoint calling convention, distinguished by explicit `row_api` metadata, with generated initialization and an optional
`to_intermediate` export. Old modules are not silently reinterpreted.

## Capability matrix

| Native aggregate capability | WASM UDAF status | Qualification |
| --- | --- | --- |
| Per-group state initialization/update/destruction | Implemented | Native rows hold UInt32 handles; guest State owns retained values |
| Single/grouped raw and intermediate input | Implemented | Separate single-group exports; selected rows compacted across IPC |
| Partial/intermediate/final/single stages | Implemented | Typed intermediate serialized as logical Arrow columns; VARBINARY remains available |
| SQL generics and nested variables | Implemented | Native SignatureBinder binds raw types or a typed intermediate for direct final-stage creation |
| Comparable/orderable/known type constraints | Implemented binding | Custom compare/hash still needs explicitly registered portable SDK semantics |
| Generic SQL comparison of retained State and current input | Implemented SDK operations | Standard owned/borrowed values compare directly; ARRAY/ROW visit reached children, MAP sorts entry indices. NULL/NaN/direction flags preserve native semantics. Mixed custom codecs retain payload materialization and registered semantics |
| Integer/DECIMAL constraints | Implemented signature bridge | Uses native variable constraints and scalar SDK Decimal/value validation; not every built-in decimal aggregate algorithm is reproduced |
| Typed/heterogeneous variadic tails | Implemented | Borrowed VariadicView reads accessed elements; a T tail is homogeneous, any may be heterogeneous; zero tail is valid |
| Nested ARRAY/MAP/ROW input/intermediate/output | Implemented transport/authoring | Row views, Generic and Value construction use the scalar SDK; non-NULL complex MAP keys preserve NULL children through owned State/stages; anonymous intermediate ROWs match positions and canonicalize descriptors without copying payloads; named signatures retain native constraints; State owns retained input |
| Full-range HUGEINT/TIMESTAMP and arbitrary-byte VARCHAR | Implemented row protocol | Legacy aggregate exports retain their narrower/UTF-8-oriented transport |
| Default NULL behavior | Implemented row adapter | Filter any outer NULL raw argument; empty/all-NULL groups extract NULL without invoking the author's extractor; non-NULL input clears the native group NULL flag |
| Non-default NULL behavior | Implemented | Option input and NULL intermediate Generic are passed to user code; author defines identity/empty output |
| Constants and configuration | Implemented declared values | InitContext exposes raw/intermediate type binding, separate raw/intermediate constants, bound input/return types and declared config values/default strings; custom native accessor transformations remain guest logic |
| Distinct, masks, aggregate ordering | Native operator integration | Current query tests verify DISTINCT, mask and ORDER BY; these are engine facilities, not SDK reimplementations |
| Window use and repeated extraction | Native AggregateWindow integration | Constant inputs now supplied to window aggregates; expanding, sliding, partition reset and empty frames covered |
| `toIntermediate` | Implemented for row UDAFs | Per-row temporary State → update → serialize → destroy; correctness fallback, not a native cost/fast-path claim; masked/default-NULL rows produce NULL |
| Spill intermediate extraction/merge | Implemented | Existing forced-spill operator test also exercises typed HUGEINT state; entire bridge operation is serialized for concurrent extraction |
| Same-name overloads across modules | Implemented | Concrete-before-generic dispatch, signature-level merge/overwrite; aggregate/window metadata uses conservative OR orderSensitive / AND ignoreDuplicates |
| Native typed Status/error category | Implemented in ABI 2/3 | RowError preserves StatusCode through the complete row aggregate lifecycle; UserError is a native user exception, other codes are native system exceptions; every failed export invalidates Store |
| Recoverable scalar row errors / TRY | Different aggregate contract | update/create/merge/extract Err fails the export; partially mutated aggregate state is discarded by invalidating Store; no per-row aggregate error column |
| Cleanup under failure | Implemented | Native destroy never leaks an exception; invalidates Store and clears host handles; SDK create rolls back inserted states on failure |
| Handle uniqueness and validation | Implemented | Nonzero, unique against all live create batches; SDK validates handle batches before mutation and rejects duplicate destruction |
| OPAQUE/custom state | Serialized codecs supported | Invocation-scoped OPAQUE handles cannot be retained across aggregate calls; row UDAFs reject OPAQUE without a serialization codec |
| Per-query memory charging | Accessible linear pages + IPC/Arrow | Host memory callbacks charge before initial commitment/growth; exact JIT/table/non-linear runtime accounting remains a gap |
| Clustered input, lazy value-hook pushdown, adaptive loops | Partially optimized | Default row path borrows grouped State per contiguous run; optional bulk hooks stably gather interleaved rows per group and decode tuples on iterator consumption. Fully selected nested input with Arrow-supported scalar leaf encodings retains plain siblings instead of whole-value copying. Sparse/encoded-complex gathering, native value-hook pushdown, transport and adaptive loops remain optimization gaps |
| Native per-group row-size accounting | Implemented in ABI 3 | Reports guest inline State and owned reserved capacities; updates native shared row-size counter by this aggregate’s delta; Store-wide overhead is covered by the accessible-page charge |
| State compaction | Logical shrink + opt-in Store rebuilding implemented | Full owned-State checkpoints preserve all active groups/handles/private state; actual accessible-page reduction returns native reclaim bytes. Whole-Store migration requires fragmentation/workspace thresholds; streaming/segmented zero-spare-capacity optimization remains |
| Automatic companion functions | Implemented with native rules | Generated `_partial`/`_merge`/`_merge_extract`/`_extract` family; return-type suffixes, native signature inference and transactional refresh. Callback-free independent companions allow erased input-only lambda types as explicit unbound slots; callbacks need an expression-carrying API |
| SQL lambda aggregate parameters | Implemented for row ABI 3 | Declared FUNCTION arguments after fixed values, typed `Lambda::call`, native evaluator/error propagation, grouped/staged execution, spill and checkpoint recovery; bounded batch callbacks and optional bulk update/merge hooks |
| Lambda plus variadic arguments in one signature | Implemented | FUNCTION lies between the fixed value prefix and the tail; Rust tuples/constant indices exclude functions. Native SQL resolution accepts expanded/empty tails and binds known types after unresolved lambdas |
| Lambda parameters sharing input-column names | Implemented | Formal parameters shadow input fields; AggregateInfo and aggregate test utilities pass lambdas separately without phantom capture columns |
| Actual outer-column captures in aggregate lambdas | Shared evaluator API gap | Native reduce and this bridge evaluate against formal-parameter ROWs; real captures require explicit values and a stage/merge contract |
| General host services and retained object methods | Remaining implementation gap | Only the three typed lambda imports are allowed; no WASI or arbitrary process APIs |
| Sanitizer/fuzz/concurrency/platform coverage | Additional validation | Selected deterministic tests are not full production hardening or portable exact-hash proof |

## Initialization and stage binding

The native AggregateInfo factory is called with raw input types and a
partial/single construction step even for a final/intermediate operator; the
operator subsequently invokes the intermediate-input methods. The WASM adapter
therefore preserves bound raw type information for that native path, tolerates
the shorter intermediate constant list, and enforces required raw constants
when the raw update path is used. Constants from the original expression are
not automatically available in downstream merge operators. They must influence
the serialized state or be represented in an intermediate field if needed
later. Direct factory creation with final/intermediate input binds against the
declared intermediate type. It cannot recover generic SQL variables that were
erased into an untyped VARBINARY payload; use a typed intermediate carrying T.

Direct merge/extract bindings clear the raw configuration constant mask;
constant intermediate data never impersonates a raw parameter.
`InitContext::aggregate_input_types()` reports Raw/Intermediate binding, not
the executing operator step. `intermediate_constant_value()` exposes a constant
provided to a merge factory separately, including constant SQL NULL. It is an
initialization hint; merge still consumes each selected row. The shared native
scalar extract adapter does not forward such constants, so its hint is absent.
Instances bound directly to intermediate input reject raw updates and do not
advertise raw `toIntermediate` support.

Anonymous intermediate ROW signatures bind by position; explicit field names
are checked by native SignatureBinder. An ordinary final-stage factory can keep
raw type binding while receiving a positionally compatible intermediate ROW.
The row bridge canonicalizes these intermediate Arrow field descriptors to the
resolved intermediate type, including ROWs nested in ARRAY/MAP, before merge.
It preserves leaf/storage types, DECIMAL parameters and bridged type identity.
This descriptor-only operation shares value buffers and leaves caller/shared
vectors untouched; selection/export and IPC are still transport costs. The
initializer uses the same resolved type for intermediate constants/placeholders.
Raw/scalar signature matching is unchanged, and generic T keeps its bound names.

Config values initialize each group's State, rather than being global mutable
user data. The adapter initialization message is installed once per Store;
create runs once for each group. A final/merge state should be a neutral state:
constants that affect raw input must not be applied a second time when merging.

State extraction is serialized at the host bridge, including gather/decode and
failure handling, not merely the Wasmtime call. That meets Aggregate's
concurrent `extractAccumulators` contract for spill at the cost of serializing
those calls. This does not make a mutable native group update concurrent-safe
against unrelated engine mutations outside the Aggregate contract.

DISTINCT can supply a length-one constant vector alongside a longer selected
value vector. The transport expands only genuine constant encodings before
indexing; short nonconstant inputs remain fatal protocol errors.

## Fundamental boundary and additional work

WASM does not prevent aggregation stages, generics, variadic/nested values,
window execution, typed intermediates or spill. They use explicit values and
state handles here. A guest cannot dereference the native HashStringAllocator,
RowContainer, MemoryPool, C++ object/vtable or ExpressionEvaluator through a raw
host pointer. Codecs and mediated handles/imports can supply corresponding
operations. Retained object handles, general services, alternate transport,
zero-spare-capacity physical reclamation and query-wide budgets require additional design and
implementation work, rather than WASM SQL expressiveness impossibilities.

## SQL lambda callbacks

`ReduceWasm` declares value arguments `T,S` and trailing lambdas
`function(S,T,S)` / `function(S,S,S)`. The native binder retains both variables
for raw construction; merge signature construction includes only variables used
in intermediate/result types, and never validates an irrelevant merge signature
on the raw path. Native AggregateInfo constructs staged instances using raw
types and carries lambda expressions into intermediate/final nodes.

`InitContext::lambda(index)` returns a symbolic position. `Lambda::call` accepts
borrowed `Value` references and returns an owned nullable/nested value. The
author sees no RecordBatch or native pointer. Bound types are immutable per
Store initialization metadata; Store rebuilding reinstalls that metadata and
the native evaluator authority, while checkpoints retain only lambda positions.
The native `_partial` adapter now forwards lambda expressions before creating
groups. Direct `_merge`/`_extract` signatures inherit native companion rules:
an intermediate does not encode executable lambda expressions or recover an
erased input T. Independent companions work when merge/finalize do not invoke
callbacks; erased input-only FUNCTION types remain explicitly unbound symbolic
slots. Concrete-lambda heterogeneous and generic BIGINT/ARRAY variadic examples
verify both independent companion pipelines. Callback use
requires the ordinary staged plan, which carries expressions, or a future
expression-carrying companion protocol. `Lambda::has_bound_types()` exposes
unbound slots; evaluation fails before encoding or dispatching a callback.
Required result/intermediate types still obey native binding rules; this does
not permit an unbound return type. Expression authority is API work, not a Wasm
restriction or permission to serialize a native expression pointer.

Aggregate window execution currently converts every argument through
`Window::createWindowFunctions` / `exprToChannel` and has no lambda-expression
argument channel. This shared native window API gap also affects native
lambda-bearing aggregates; supporting lambda windows requires extending that
engine contract, rather than exposing native pointers to Wasm.

Only `velox_udf_v1.lambda_call(i32,i32,i32)->i64`,
`lambda_call_batch(i32,i32,i32,i32)->i64` and
`lambda_result(i32,i32,i32)->i32` imports are accepted. A call
validates the request, evaluates the bound expression exactly once, and holds
one query-pool-charged result; it returns a Store-local token and byte length.
The result import checks token, exact length and memory range before copying and
consuming the response. Replays, a second pending request, an unconsumed result,
unbound authority and calls during module start fail and invalidate the Store.
Native callbacks catch every C++ exception and rethrow it after Wasmtime returns;
native UserError remains a UserError. They never unwind through guest frames.
Request/result limits, a per-export call quota (default 1,000,000), cancellation
and invocation deadline checks apply. The batch row limit defaults to 65,536;
the cumulative evaluated-row limit defaults to 1,000,000 per export. Batching
does not bypass the latter. A synchronous native expression cannot be
forcibly stopped mid-body by guest fuel; its native execution/cancellation
contract still applies, with deadline checks at callback boundaries.

The initial API evaluated one row per callback. The current API adds batched
callbacks, with explicit row counts and host quotas, and optional row-iterator
`update_batch` / `merge_batch` hooks. The reducer maps once per chunk and uses
batched pairwise combination instead of one IPC round trip per input row.
Stable grouping supports interleaved groups while retaining order within each
State; defaults retain ordinary row semantics. IPC and more native fast paths
remain performance work, not fundamental Wasm SQL expressiveness limits.
The manifest puts lambdas after fixed value arguments and before a final
variadic value tail. Function arguments are omitted from Rust row tuples and
constant indices. SQL resolution uses the last declared type for every tail
position and accepts zero or more tail values. SignatureBinder continues binding
known arguments after unresolved lambda placeholders while returning false
until the missing lambda types are supplied. A fixed value can bind T for an
empty tail; a variable used only in the tail/lambda input still needs an actual
typed value to infer it. This uses normal native type inference.

Lambda signature names are formal parameters, not captures:
`parse/TypeResolver.cpp::resolveLambdaExpr` builds the signature from only those
parameters, then separately adds outer columns for body type inference. Native
AggregateInfo and the aggregate test utility no longer append matching input
columns as apparent captured arguments. Regressions compare native and Wasm
when both input and combine parameters shadow actual input fields, including
hash/streaming, all stages and the `_partial` companion. Actual outer-column
captures are a separate shared API obligation: native ReduceAgg and this bridge
evaluate lambda bodies against formal-parameter ROWs. A correct capture design
must specify which values survive raw input, combining, spill and merge.

Validation of the initial one-row stage is recorded in
`.deps/alignment-lambda-2026-10-05/`:
77 Rust tests, 78 full GTests (14 scalar, 22 runtime, 26 aggregate, 4 native
metadata, 12 native companions), six full CTest targets and three minimal CTest
targets. The benchmark correctness target now checks 33 cases, including native
and Wasm reduce. Three independent full-build processes with 1024 rows, 100
untimed warmup batches and 100 million fuel per export measured medians of
22.10 us native reduce and 17.48 ms Wasm reduce (**790.95x**), plus 1.77 us native
SUM and 55.74 us Wasm row SUM (**31.49x**). Each timed iteration updates one
existing group and extracts once; JIT, initialization, input allocation and
destruction are excluded. No builds/tests ran during these timings. These
measurements demonstrate the current optimization gap, not performance parity.
Raw runs and commands are in `benchmark_results/wasm-lambda-2026-10-05/`.

ABI 3 stores a four-byte host handle plus an eight-byte prior size report per
group. It declares variable-sized accumulation even when State is a fixed-width
Rust integer: the guest State occupies memory beyond the host handle. Reported
inline State and reserved owned capacities update the native row-size counter
used in row/spill sizing. Shared Store bookkeeping and allocator fragmentation
remain covered by the accessible-page charge, rather than assigned arbitrarily
to one group. Custom/shared allocators need an explicit AggregateState ownership
policy; serialized output length is not a memory footprint.

Accessible linear pages are charged before creation/growth through Wasmtime's
host memory callbacks, independently of the configured maximum. Shrinking
String/Vec capacity frees guest heap storage for reuse but cannot shrink live
standard Wasm memory. Pure logical compaction therefore returns zero reclaimed
bytes. The optional owned-State checkpoint contract now lets the host rebuild a
fragmented Store while groups stay active and report the actual accessible-page
reduction. SQL intermediate alone cannot preserve arbitrary private update state.
Successful last-group retirement also releases Store pages; native spill/flush
reconstructs from intermediates. Streaming/segmented reclamation with minimal
workspace remains an optimization obligation, rather than an inherent WASM SQL
limitation. See the current owned-State checkpoint section below for thresholds.

Current wasm32 offsets constrain contiguous guest memory to its address space.
Chunking/streaming or a revised ABI can change that constraint; Memory64 cannot
make arbitrary host C++ objects directly usable in the guest.

## Examples and evidence

`examples/add/src/lib.rs` contains `FirstValueWasm` (T → typed ROW(T,BOOLEAN) → T),
`VariadicSumWasm` (constant + borrowed bigint tail, configuration and typed
ROW(HUGEINT,BIGINT) intermediate), and `StrictSumWasm` (default NULL behavior and
full-width HUGEINT intermediate/output). `CheckedSumWasm` uses a HUGEINT state
with BIGINT input/result and reports overflow at extraction. See the SDK README
for authoring and companion examples.

`WasmAggregateTest` exercises nested and distinct T bindings, NULL-first
semantics, raw constants/config, zero variadic tails, all-NULL/empty groups,
full-width integers/timestamps and invalid-UTF-8 VARCHAR, masks/DISTINCT/ORDER
BY, expanding/sliding/empty window frames and typed stage binding. It also
checks `toIntermediate`, repeated/concurrent extraction and forced spill.
`WasmSafetyTest` checks overload replacement across modules, duplicate live
create handles, malformed aggregate IPC and destructor cleanup failure.
Rust adapter tests check type/handle/null policies and repeatable extraction.

Logs, exact source/artifact hashes and measurement results are recorded under
`.deps/alignment-udaf-2026-10-05/` after validation. Aggregate benchmark controls
use native built-ins sum/average. `wasmRowSum` uses factor 1 and one bigint tail
with the same sum result as native sum; both timed paths update an existing
single group and extract once per batch. This is a batch measurement, not a
complete grouped/spill/window query performance claim.

Final validation: **49 Rust tests**, **37 GTests** (14 scalar, 7 runtime,
12 aggregate, 4 native metadata), **7 unique CTest entries** and **31 benchmark
correctness cases** pass. Three independent Release processes at 1,024 rows,
100 warmup batches and 100M fuel/export measured:

| Aggregate batch case | Native median | WASM median | WASM/native |
| --- | ---: | ---: | ---: |
| Legacy SUM | 2.020 us | 21.030 us | 10.41x |
| Row variadic SUM, factor 1, one tail | 2.020 us | 129.950 us | 64.33x |
| Legacy AVG | 0.438 us | 20.610 us | 47.10x |

The row case does additional argument/view/count/wide-state work relative to
the legacy example; these timings do not isolate adapter overhead or show a
speedup. Capability alignment is not execution-cost parity. Exact commands,
raw samples and unrounded medians are under
`benchmark_results/wasm-udaf-row-2026-10-05/`.

## State lookup optimization: 2026-10-05

The row adapter no longer looks up a single-group State in its handle registry
for each input row. Grouped updates preserve input order and borrow each state
once per contiguous handle run, after validating the complete handle column.
Unknown/NULL handles still fail before mutation; an author's error stops the
update immediately. This also applies to merge, which uses the update adapter.
It does not implement native lazy value hooks or change the authoring contract.

Fresh before/after medians from three independent processes per version use
1,024 rows, 100 warmups and 100M fuel/export. The row variadic SUM case decreases
from **108.870 us to 93.890 us/batch (13.76% less time)**. Unchanged native SUM
is 1.780 → 1.760 us; unchanged legacy WASM SUM is 16.760 → 17.250 us. The row
case still costs **53.35x native SUM** in this fixture. Input/configuration,
state creation, JIT and destruction remain outside timing; update plus extraction
of one retained group are timed. These measurements do not establish grouped,
window, spill or full-query performance parity.

After the optimization, **53 Rust tests**, the unchanged **37 full GTests**,
**7 CTest entries across two configurations**, and **31 benchmark correctness
cases** pass. Exact commands, source/module/binary hashes and logs are in
`.deps/alignment-hotpath-2026-10-05/`; raw before/after samples and the comparison
are in `benchmark_results/wasm-hotpath-2026-10-05-{before,after}/`. Companion
registration, exact guest-state memory accounting,
mediated callbacks and production hardening remain implementation obligations.

## Typed aggregate errors: 2026-10-05

Row UDAFs now emit ABI 2 metadata. ABI 2 keeps the ABI 1 call signatures and
logical transport, requires the row profile and defines tagged native Status
values in the result's status lane. The host also accepts ABI 1 declarations;
ABI 1-only hosts reject new row modules during manifest parsing rather than
silently changing their error semantics. Unknown versions, invalid ABI 2
profiles, unknown status tags and reserved bits fail validation.

Authors use `type Error = RowError` and return `RowError::new(StatusCode::...,
message)`. There is no adapter override or host callback to implement. Create,
update, merge, serialize/finalize and temporary toIntermediate calls all preserve
these errors. Ordinary String/ToString errors keep the existing message/export
failure behavior; marker-looking strings cannot impersonate a typed Status.

The host mirrors native EvalCtx::setStatus: UserError produces VeloxUserError,
while the other ten non-OK Status codes produce VeloxRuntimeError. These native
exceptions do not themselves store a StatusCode, just as with native EvalCtx;
this is not a promise of a separate native exception subclass for each code.
Every export failure discards Store, even UserError, because a batch update may
have already changed some groups. Aggregate errors do not create recoverable
row errors or allow continued accumulation on that Store. Scalar whole-export
errors retain fatal TRY behavior; scalar row error columns remain unchanged.

Regression tests cover every code through create/batch/single runtime calls,
invalid tags/reserved bits and legacy failures, schema ABI validation, scalar
TRY, SDK lifecycle/error escaping, native HUGEINT overflow in grouped/single
raw/merge updates, and refusal to extract or update failed state. Validation
evidence is recorded under `.deps/alignment-status-2026-10-05/`.

This stage passes **55 Rust tests**, **40 full GTests** (14 scalar, 9 runtime,
13 aggregate, 4 native metadata), **7 CTest entries across two configurations**
and **31 benchmark correctness cases**. Three-process diagnostic timings are
retained under `benchmark_results/wasm-status-2026-10-05-validated/`. Native and
legacy controls also changed substantially and the samples have large spread;
these runs do not establish a performance effect from error ABI changes.


## Native companion functions: 2026-10-05

Registration now prepares native companion entries without publishing them,
validates all generated names, then publishes them with the original scalar,
aggregate and window entries. A collision rolls back the entire module, even
with overwrite enabled. The declared-function return count excludes companions.
Companion aggregates are marked as companions and do not get window aliases.

The implementation reuses `AggregateCompanionAdapter` and
`AggregateCompanionSignatures`. `_partial` returns the declared intermediate
when used by a single-step aggregate operator; `_merge` merges and returns
intermediate; `_merge_extract` merges and finalizes; scalar `_extract` finalizes
one intermediate per selected row. Native adapters now forward constant inputs
so raw-input companions preserve constant/config initialization. Constants are
still not implicitly available in a downstream merge.

Adding or replacing overloads refreshes the entire owned family. When several
signatures share an intermediate type, native return-type suffixes distinguish
extract and merge-extract entries. Aggregate and scalar ownership are tracked
separately so refreshing an aggregate alias cannot delete an unrelated scalar
homonym. An existing native name or another module's generated alias cannot be
silently replaced. Generic variables must be inferable under the native companion
signature rules; erased VARBINARY state does not recover a SQL type variable.
Typed ROW(T,...) intermediates retain that information. Companion functions are
primarily planner adapters, with the same inference limits as native companions.

Native extract companions propagate whole-call exceptions, including UserError
under SQL TRY; they do not generate scalar row errors. On extraction failure,
the WASM wrapper releases the underlying aggregate and failed Store. A later
API retry creates a fresh instance; successful calls destroy their temporary
groups normally. Once the last temporary group is
retired, the healthy Store is released; the native function adapter remains
reusable and initializes a fresh Store on the next call. This preserves the
native error contract without retaining failed guest state.

Validation passes **55 Rust tests**, **54 full GTests** (14 scalar, 9 runtime,
15 aggregate, 4 native metadata, 12 native companion), **8 CTest entries across
two configurations**, and **31 benchmark correctness cases**. Query regressions
cover all companion stages, generic nested state and variadic constants.
Safety regressions cover transaction rollback, overload-family refresh, scalar
homonyms and overflow followed by healthy retries. The native companion tests
compare prepared entries with normal native registration. The example module
contains 58 scalar and 11 aggregate declarations. Logs and exact source/artifact
hashes are in `.deps/alignment-companion-2026-10-05/`. This stage adds no timing
claim; prior batch profiles do not establish companion/full-query cost parity.
Per-group memory/compaction, mediated callbacks, native optimization paths and
broader release validation remain implementation obligations.


## State accounting and capacity compaction: 2026-10-05

`VeloxRowAggregate::State` now implements `AggregateState`. SDK primitives,
String, Vec, Box, Option, arrays, tuples through arity 16, RefCell, CustomValue
and owned Value trees report their reserved allocations recursively. Structs
can derive the trait; custom allocator/shared ownership requires a deliberate
manual policy. Borrowed Value/OPAQUE invocation handles cannot impersonate
owned retained state. Inline State is added once by the adapter; vector element
inline sizes are counted through vector capacity, with child heaps added once.
Checked arithmetic rejects footprint overflow.

Generated row aggregates declare ABI 3, retaining typed Status and logical row
transport from ABI 2. Create and update/merge return little-endian
`(u32 handle,u64 retained bytes)` records for each affected unique group. This
piggybacks on existing exports without adding a second boundary call per batch.
Serialize/finalize prefix the same reports before the IPC result, including
interior-cache capacity growth after extraction. toIntermediate retains plain
IPC because its temporary states are destroyed before returning. Compact takes
one handle column, validates the entire set before mutation, recursively shrinks
capacity and returns the new reports. Initialization/destroy still return no
bytes. ABI 1/2 modules keep their original layouts and do not acquire these
reports; older hosts reject ABI 3 declarations during loading. ABI 3 is restricted
to row aggregates and requires a compact export.

The host validates report length, membership, uniqueness, per-state bounds and
total live reported bytes before changing any row counter. Growth/shrink deltas
preserve other variable keys/aggregates in the shared counter; saturated counters
stay saturated. Cleanup removes only this aggregate's contribution, even after
Store failure. A forged report is a fatal protocol error and releases Store.
Reported footprints describe author-owned data; the conservative Store charge
remains the security/resource bound even for an author that underreports.

Native companions forward compaction capability to the underlying aggregate.
Extract now initializes temporary flags and the row-size counter before use.
`CollectStringsWasm` demonstrates reserved Vec/String capacities, borrowed merge
reads, native stages, compaction, unchanged extraction and cleanup. Tests also
cover skipped rows, multiple groups, repeated handles, shared row counters,
mutable finalizer caches and malformed reports. No physical query-pool reclamation
was claimed at this earlier stage: guest compaction alone returns zero actual
reclaimed bytes. The current checkpoint section below supersedes the active-Store
rebuilding gap described here.

Validation: **60 Rust tests**, **56 full GTests** (14 scalar, 9 runtime,
17 aggregate, 4 native metadata, 12 native companion), **8 CTest entries across
two configurations**, and **31 benchmark correctness cases**. The example module
now has 58 scalar and 12 aggregate declarations. Logs and exact hashes are under
`.deps/alignment-state-2026-10-05/`. Remaining efficient memory reclamation,
mediated callbacks, native optimization and release-hardening obligations still
apply; this stage does not establish complete native performance/resource parity.

Three fresh Release processes at 1,024 rows, 100 warmups and 100M fuel/export
measure native SUM **1.770 us/batch** (1.740, 1.770, 2.600), legacy WASM SUM
**17.350 us** (18.450, 17.350, 16.620), and row WASM SUM **90.320 us**
(90.320, 90.140, 133.820), about **51.03x native** in this retained single-group
update/extract fixture. Output-codec diagnostic medians are 424.870/450.460/
421.790 us for eager-unused/lazy-unused/lazy-used. The slower third SUM process
also has a slower native control. These samples do not isolate the cost of
state reports or demonstrate a speedup; there is no fresh pre-change control
for this stage. No grouped/spill/window/full-query or peak-memory claim follows.
Raw commands and samples are in `benchmark_results/wasm-state-2026-10-05-validated/`.


## Linear page charging and Store retirement: 2026-10-05

The runtime replaces full-limit reservation with guarded host-managed linear
memory. It honors Wasmtime 39's reservation/guard requests, keeps the base
pointer stable and charges only accessible pages, before initial commitment and
each memory.grow. Inaccessible virtual space/guards are not allocated query bytes;
this charge is not RSS. The growth owner is captured per memory, independent of
which Driver thread executes the Store later. Native allocation failures cannot
be suppressed by a guest ignoring memory.grow's -1: callbacks retain the error,
return through Rust, then each call site raises it and invalidates Store.
Late pool attachment charges existing pages. Start/allocate/entrypoint/free and
failure/destruction paths are covered. Shared/threaded memory is disabled to
prevent using a different Wasmtime allocation path without query accounting.
Standard 64-KiB pages and guarded POSIX mappings are the current implementation.

Successful last-group destruction now releases the idle Store, including all
accessible page charges. Native spill/flush can therefore retire states, free
Store memory, then initialize/merge intermediates in a fresh Store. A direct
2-MiB variable-state regression proves more than 1 MiB of pool usage is released
and restores the identical ARRAY(VARCHAR) result. A failed Store stays poisoned;
cleanup cannot revive partially mutated state. At this earlier stage active-group
compaction only freed Rust capacity; the current checkpoint section below adds
Store rebuilding. Exact JIT/table/non-linear accounting and other release gates
remain open.

Validation: **62 full GTests** (14 scalar, 14 runtime, 18 aggregate, 4 native
metadata, 12 native companion), **8 CTest entries across two configurations**
and **31 benchmark correctness cases**, plus a fresh run of **60 Rust tests**.
The Rust implementation is unchanged from the state-accounting stage. New tests cover cross-thread page
charging, late attachment, native-cap rejection during start/allocator/export/
free, guard faults, zero-filled growth and fresh-Store intermediate restoration.
Logs and exact hashes are under `.deps/alignment-linear-memory-2026-10-05/`.

Three fresh Release processes at 1,024 rows, 100 warmups and 100M fuel/export
measure native SUM **1.720 us/batch** (1.720, 1.720, 1.720), legacy WASM SUM
**18.220 us** (18.220, 18.480, 16.570), and row WASM SUM **86.940 us**
(84.120, 86.940, 88.340), about **50.55x native** in the retained single-group
update/extract fixture. Output-codec diagnostic medians are 421.000/408.980/
422.750 us for eager-unused/lazy-unused/lazy-used. There is no fresh pre-change
control for this stage: these samples do not isolate the cost of memory
callbacks or establish a speedup. Initialization/JIT/group retirement are
outside timing; no grouped/spill/window/full-query or peak-memory claim follows.
Raw commands and samples are in
`benchmark_results/wasm-linear-memory-2026-10-05-validated/`.

## Prepared row readers: 2026-10-05

Generated scalar and row-aggregate fixed arguments now prepare readers before
the row loop. BOOL, TINYINT, SMALLINT, INTEGER, BIGINT, REAL and DOUBLE cache
the concrete Arrow array once; optional readers cache validity and skip the
inner primitive validity check after establishing non-NULL. Bounds and type
errors remain checked. Preparation does not eagerly convert values: a cast
mismatch is reported when the row is accessed, preserving scalar row errors.
Nested/custom/string values retain the access-time fallback. Scalar ASCII and
null-free callbacks retain their separate conversion path, including different
Rust parameter types such as Option<ArrayView> versus NullFreeArrayView.

Variadic columns prepare readers once per batch and share them with row views.
The batch metadata uses bounded guest heap allocations already covered by the
Store page charge; no per-row argument Vec is created. Returning borrowed
Generic/string results does not borrow metadata that may already be destroyed.
Views now implement Clone instead of Copy, sharing metadata without allocation;
Send/Sync remains available for compatible element types in native SDK usage.
This is a Rust SDK source change for code that relied on implicit copies; the
host ABI and prebuilt modules remain unchanged. Direct from_values/new views,
heterogeneous/zero tails, NULL metadata checks, short-circuit access and decoding
errors remain supported.

Validation: **64 Rust tests**, **62 full GTests**, **8 CTest entries across two
configurations** and **31 benchmark correctness cases** pass. Added regressions
prove one dynamic cast per prepared primitive column, allocation-free repeated
row reads, sliced validity, untyped NULLs, deferred wrong-type/UTF-8 errors,
bounds, floating-point values, reader destruction and borrowed lifetimes. The
Wasm target build exercises all generated scalar and aggregate wrappers;
existing native tests cover NULL/constant/dictionary selections, generic and
variadic/nested results, callback dispatch, stages, spill, windows and companions.
Logs, commands and source/module/binary hashes are under
`.deps/alignment-readers-2026-10-05/`. At that stage physical active-group
compaction was still open; the checkpoint section below updates it. Callbacks,
transport/native optimizer work and broader release validation remain open.

A controlled comparison restores the pre-change SDK sources (all Rust/Cargo
hashes match the saved baseline) and reproduces the exact original Wasm module
hash. The benchmark now accepts --udf_module_path, so both versions use the
same host executable. Six independent Release processes alternate old/new
modules in before/after/after/before/before/after order, with 1,024 rows, 100
warmups and 100M fuel/export. Both modules pass all 31 correctness cases first.

| Batch case | Before median (us) | After median (us) | Less time |
| --- | ---: | ---: | ---: |
| Row variadic SUM | 84.940 | 52.730 | 37.92% |
| Scalar checked ADD | 66.530 | 49.870 | 25.04% |
| Scalar ADD with NULLs | 57.990 | 45.390 | 21.73% |
| Native SUM control | 1.730 | 1.730 | 0.00% |
| Legacy WASM SUM control | 16.590 | 16.710 | -0.72% |

Native ADD and ADD-with-NULL medians change by +0.53%/-0.38%; their raw samples
are retained. Row SUM now costs **30.48x native SUM**, rather than 49.10x in
this fixture. These are batch measurements, not a grouped/spill/window or
full-query speedup claim. State creation/JIT/retirement are untimed; IPC, guest
fuel and update/extraction or scalar evaluation remain timed. No peak-memory
claim follows. Earlier sequential measurements had materially drifting controls
and are diagnostic only; the alternating comparison is the performance evidence.
Raw samples, exact commands, normalized ratios and executable/module hashes are
in `benchmark_results/wasm-readers-2026-10-05-paired/`.


## Full owned-State checkpoints and active Store reclamation: 2026-10-05

`AggregateCheckpoint`, separately derivable from `AggregateState`, reconstructs
all owned State rather than only its SQL intermediate. Opt in explicitly with
`#[velox_row_aggregate(..., checkpoint = true)]`. Named/tuple/unit/generic structs,
primitives, owned nested Value trees and container fields are supported; custom
ownership/enums can implement the codec manually. All five row examples now opt
in. The contract requires future behavior to be reconstructible from State:
mutable module globals, retained invocation handles/borrowed values and raw guest
addresses need a different ownership/reconstruction design. Reinitialization
installs the same context; restoration does not call create or merge.

The capability adds paired checkpoint/restore exports and `owned-v1` metadata to
ABI 3 without changing existing ABI-3 layouts. Older ABI-3 hosts can ignore the
feature and keep logical compaction. The private snapshot records full State,
identical handles and the registry's next high-water mark (including exhaustion).
It is scoped to the same module/context, not a durable module-upgrade format.

`GroupingSet::compact` supplies up to 1,000 rows per batch. The bridge compacts
that subset but checkpoints/restores **all** live handles before rebuilding its
Store. It first owns and charges the host snapshot, validates its full envelope,
releases old pages, initializes a fresh Store, validates every restored size, then
publishes all group-size deltas. Native NULL flags and contributions from other
keys/aggregates remain unchanged. Invalid framing, exports or reports poison the
whole aggregate; failed candidates are destroyed and cleanup cannot revive State.
The returned count compares original entry pages with final restored pages, so
allocations made during compact are not misreported as previously reclaimed bytes.

Rebuilding skips Stores unless pages exceed initialized pages plus twice live
reported State and one page. It also requires native free/reserved workspace of
an estimated `4 * live State + 32 * group count + 64 KiB`; the estimate is a
heuristic, and actual native allocation limits/fuel/cancellation/input-output
limits remain enforced. Below these thresholds compact returns zero so native
can spill. Whole-Store checkpoint/restore performs serialization and may need
additional allocations; streaming or segmented migration under zero spare
capacity remains an optimization gap. Accessible-page charging/reclaim is not
an RSS measurement or exact JIT/table/host-bookkeeping charge.

Tests cover private update factors omitted from SQL intermediates, generic ROW /
ARRAY state, first-seen NULL, continuation through raw and intermediate updates,
full migration from a subset, unchanged handle IDs and retired-handle high-water
marks, shared size counters, candidate cleanup, depth/length/budget validation,
forged envelopes/reports, traps and native pool failure before snapshot copying.
The zero-headroom regression makes checkpoint trap if called and verifies the
host skips rebuilding safely. Actual variable-state tests release more than 1 MiB
while retaining other groups. Validation logs/hashes are under
`.deps/alignment-checkpoint-2026-10-05/`. Callback/host-method mediation, native
optimization paths, exact non-linear budgets and production-hardening/portability
remain community release obligations.

Validation at this stage: **73 Rust tests** (57 SDK, 13 macros, 3 examples),
**68 full GTests** (14 scalar, 17 runtime, 21 aggregate, 4 native metadata,
12 native companion), **8 CTest entries across two configurations**, and
**31 benchmark correctness cases for each of the before/after modules** pass.
Both configurations produce the identical example module hash, with 58 scalar
and 12 aggregate declarations.

A controlled module comparison uses the same current host executable and six
independent processes in before/after/after/before/before/after order, with
1,024 rows, 100 warmups and 100M fuel/export. Median native SUM is **1.73 us**
for both modules; row WASM SUM changes **53.04 → 53.49 us/batch (+0.85%)**, about
**30.92x native** after the change. Legacy WASM SUM is **16.76 → 16.65 us**.
Scalar ADD is **49.39 → 48.82 us** and NULL ADD **45.47 → 45.14 us**;
native controls are **0.9068 → 0.89193 us** and **0.84409 → 0.8515 us**.
These repeated samples do not establish a material speedup. They compare guest
artifacts on the current bridge, not before/after C++ runtime implementations.
The retained-group update/extract fixture does not time Store rebuilding,
creation/JIT/destruction or full-query spill/window/peak-memory behavior. Raw
commands, module/executable hashes and samples are under
`benchmark_results/wasm-checkpoint-2026-10-05-paired/`.

## Batched lambdas and optional aggregate hooks: 2026-10-05

The initial single-row stage above is superseded by bounded
`Lambda::call_batch` and optional `update_batch` / `merge_batch` author hooks.
Arguments remain borrowed values/row tuples and intermediate views. Columns
can broadcast one value; zero-row SDK requests do not invoke native. Generated
exports retain the ordinary prepared-reader path unless the author overrides
the corresponding hook. Bulk grouping validates every handle before mutation,
collects interleaved rows stably, and decodes arguments on iterator consumption.
Input failures remain separate from author errors, including strings that forge
the typed-status prefix. Default hooks stop at the first error.

`ReduceWasm` maps each bounded chunk from the initial state and combines adjacent
pairs through batched native evaluations. It preserves existing State across
chunks/checkpoint recovery, and separately tracks initial-value validation when
raw input follows a merge-created state. Every result must be non-NULL; the
ordinary row fallback uses the same map/combine contract. General native/tree
reduce still requires the reduction laws. This does not promise ordered results
for arbitrary non-associative author functions.

The exact batch import type, unavailable authority, single-row-only providers,
zero/negative/oversized row counts, cumulative row quota/reset, large and
interleaved SQL queries, host chunk limit 3, raw/merge mixing and formal-parameter
shadowing have regressions. Native ReduceAgg's existing suite also passes after
the AggregateInfo correction.

Validation: **84 Rust tests** (66 SDK, 15 macros, 3 examples), **83 full GTests**
(14 scalar, 24 runtime, 29 aggregate, 4 metadata, 12 companions), **12 native
ReduceAgg tests**, and **nine CTest targets across full/minimal builds** pass.
Both configurations produce the identical module with 58 scalar and 13 aggregate
declarations. The exact source/artifact hashes, commands and final logs are in
`.deps/alignment-lambda-batch-2026-10-05/evidence.json`.

The final controlled comparison loads the original single-row guest and the
final batched guest in the **same final host executable**. Both pass all 33
benchmark correctness cases. Six independent Release processes run in
before/after/after/before/before/after order, with 1,024 rows, 100 untimed warmups
and 100M fuel per export. No builds or tests overlap timing.

| Batch case | Before median (us) | After median (us) | Change in time |
| --- | ---: | ---: | ---: |
| Native reduce control | 22.87 | 23.68 | +3.54% |
| WASM reduce | 16,480.00 | 746.73 | -95.47% |
| Native SUM control | 1.92 | 1.73 | -9.90% |
| Ordinary row WASM SUM | 55.70 | 60.97 | +9.46% |

Reduce is **22.07x faster**, but still **31.53x native** in this fixture.
Normalized against its native control, the gap decreases by 95.62%. The ordinary
row SUM path has no bulk-hook change; the recorded slowdown and SUM control
drift warrant further performance investigation, not a speedup claim. Its
after/native ratio is 35.24x. Exact commands, all individual samples and final
module/executable hashes are in
`benchmark_results/wasm-lambda-batch-2026-10-05-paired/`.

The timed operation updates an existing single group and extracts once. JIT,
creation/initialization, input allocation and destruction are excluded; IPC,
guest fuel and callback evaluation are included. This compares guest artifacts
on the current host, not old/new C++ runtimes or complete queries. It establishes
no grouped/spill/window/checkpoint or peak-memory speedup. Earlier preliminary
measurements used a different intermediate guest/executable; their relocated
logs and archived guest are retained for diagnosis, not the final result.

Performance parity and community readiness remain open. Further transport and
native fast paths, FUNCTION/variadic SQL resolution, expression-carrying
companions, real captures/window lambdas, exact non-linear accounting,
zero-spare-capacity reclamation and production/platform validation remain
implementation or shared engine API obligations, not fundamental Wasm limits.


## Lambda, generic and variadic alignment: 2026-10-05

This supersedes the FUNCTION/variadic resolution gap in the previous report.
Row ABI 3 now combines a fixed value prefix, one or more declared FUNCTIONs and
an expanded value tail. Rust receives only the value tuple, with borrowed
VariadicView access. Constant indices likewise refer only to values: the host
projects out FUNCTION flags and expands the declared tail flag across actual
arguments. The native SQL resolver recognizes variadic lambda signatures and
binds known suffix types after unresolved lambda placeholders. A T bound only
by an empty tail cannot infer the lambda parameter type and now produces a
UserError; a fixed T prefix or concrete FUNCTION type supports empty tails.
This is native inference, not a Wasm limitation.

Four complete examples cover generic fixed/tail mapping, constant tails,
generic inference exclusively after FUNCTION, and heterogeneous `any` tails.
The heterogeneous example inspects only NULL validity, so even arbitrary-byte
VARCHAR payloads remain unread. Other examples map borrowed nested inputs in
bounded batch callbacks. State checkpoints own private constants/callback
positions; typed intermediates contain already-scaled sums and counts, so merge
does not apply constants twice. Query regressions cover zero/multiple tails,
NULLs, constants at every tail position, nested ARRAYs, interleaved groups,
multiple batches, single/grouped and all aggregation stages. Independent
`_partial` → `_merge_extract` and `_partial` → `_merge` → `_extract` are also
verified for resolvable FUNCTION types whose merge/finalize does not call a
lambda. Callback-dependent companions or generic lambda types erased from the
intermediate retain the explicit binding/API obligation described above.

Final validation: **84 Rust tests**, **88 full alignment GTests** (14 scalar,
24 runtime, 34 aggregate, 4 metadata, 12 companions), **1,642 native expression
tests** and **12 native ReduceAgg tests** pass, as do six full and three minimal
CTest entries. The native expression suite retains three existing disabled
tests; these were not run. Metadata tests also occur in that native suite, so
the suite counts should not be added as distinct tests. Both configurations
produce the same guest containing 58 scalar and 17 aggregate declarations.
Sources, binaries, logs and passing/diagnostic classifications are recorded in
`.deps/alignment-lambda-variadic-2026-10-05/evidence.json`.

Six independent Release timing processes use CPU affinity 15 and alternate
before/after/after/before/before/after on one final host. The baseline guest is
the preceding batch-lambda implementation, not the original per-row version.
Each process uses 1024 rows, 100 untimed warmups and 100M fuel per export; both
guests also pass all 33 benchmark correctness cases. The final medians are:

| Operation | Previous guest | Current guest | Native control before → after | Current Wasm/native |
| --- | ---: | ---: | ---: | ---: |
| Row SUM | 54.01 us | 55.23 us | 1.73 → 1.73 us | 31.92x |
| Lambda reduce | 756.57 us | 731.69 us | 21.06 → 21.19 us | 34.53x |

The SUM change is +2.26%; reduce changes -3.29%, with overlapping samples.
These are regression/control measurements, not proof of an optimization from
adding variadic capability. They also do not compare old/new C++ hosts or time
the four new examples. Only an existing single group's update plus one
extraction is timed; IPC, fuel and callbacks are included, while JIT,
initialization, creation/input allocation/destruction are excluded. No builds
or tests overlap the timings. Samples, commands and hashes are under
`benchmark_results/wasm-lambda-variadic-2026-10-05-paired/`. These results do not
establish full-query/grouped/spill/window/checkpoint or peak-memory parity.
Transport/native fast paths and production validation remain required before
claiming community readiness.


## Independent companions with erased lambda input types: 2026-10-05

This supersedes the unused erased-lambda-type factory gap in the prior stage.
Direct merge/extract now binds only types needed by its result/intermediate.
Input-only generic variables absent from that binding leave a null FUNCTION type
slot with its original index. Initialization explicitly emits
`velox.lambda.types_bound.<index>=false` and omits that slot's input/output type
metadata. No UNKNOWN or concrete substitute is invented. Raw construction still
requires all FUNCTION types; return/intermediate types must resolve at every
stage. This change concerns unused input-only callback types, not relaxation of
native result inference.

Rust retains each declared lambda position, including unbound slots.
`Lambda::has_bound_types()` distinguishes them; scalar/batch evaluation and
row-limit access fail explicitly before request encoding. Checkpointing preserves
the symbolic index, and restored slots obey the same checks. Initialization
rejects malformed markers and contradictory type/wire descriptors. Bound types
alone never grant native expression/evaluator authority. New SDKs accept older
fully typed initialization messages without markers. Older typed-only SDKs
ignore true markers but reject an unbound slot's missing descriptors; previous
hosts already rejected those erased signatures before initialization.

Query tests verify independent merge/extract of multiple partials for BIGINT and
ARRAY(BIGINT) mapping, grouped/global execution, NULLs and erased T. Direct
`reduce_wasm_extract` succeeds without the unused input-lambda type. Independent
merge of multiple nonempty reduce partials still explicitly rejects missing
combine expressions; the change cannot invent callback authority. An expression-
carrying planner/API contract remains necessary for callback-dependent standalone
companions, as for the corresponding native evaluator contract.

Validation: **86 Rust tests** (68 SDK, 15 macro, 3 example) and **91 full alignment
GTests** (14 scalar, 24 runtime, 37 aggregate, 4 metadata, 12 companions) pass.
All six full and three minimal CTest targets pass; before/after guests each pass
33 benchmark correctness cases. Both configurations produce identical guests
with 58 scalar and 17 aggregate declarations. Native expression/ReduceAgg code
was not changed this stage; its previous 1,642/12 tests remain recorded in the
preceding evidence rather than being rerun. Detailed passing/diagnostic logs,
sources and artifact hashes are in
`.deps/alignment-unbound-lambda-2026-10-05/evidence.json`.

The first three processes per guest showed +8.24% reduce median change with
overlapping samples and a native-control outlier. Nine additional processes per
guest alternate order on CPU 15. All 12 per-guest samples from both cohorts are
retained. The combined medians and paired bootstrap percentile intervals are:

| Operation | Previous guest | Current guest | Native control before → after | Guest change, exploratory 95% interval | Current Wasm/native |
| --- | ---: | ---: | ---: | ---: | ---: |
| Row SUM | 54.105 us | 53.945 us | 1.735 → 1.765 us | -0.30%, [-5.64%, +8.21%] | 30.56x |
| Lambda reduce | 753.125 us | 750.835 us | 21.405 → 21.930 us | -0.30%, [-5.63%, +4.67%] | 34.24x |

The intervals resample 12 paired process indices 20,000 times with fixed seed
20261005 and keep every outlier. They are exploratory same-machine intervals,
not independent platform replication. No speedup or verified regression follows
from these data. Combined data/method are in the stage's `benchmark-combined.json`;
raw cohorts are `benchmark_results/wasm-unbound-lambda-2026-10-05-paired/` and
`benchmark_results/wasm-unbound-lambda-2026-10-05-confirmation/`.

One final Release host runs both guests with 1024 rows, 100 warmups and 100M fuel.
Only an existing single group's update plus extraction is timed, including IPC,
fuel and callbacks; JIT/init/creation/input allocation/destruction are excluded.
No builds/tests overlap timing. These are control/regression timings, not costs
for independent companions or old/new hosts. Transport/native fast paths,
callback-dependent independent companions, shared captures/window lambda APIs,
exact non-linear accounting, zero-spare-capacity reclamation and production/
platform validation remain obligations before community readiness.


## Independent merge initialization constants: 2026-10-05

A typed native constant ROW intermediate previously reached `constant_value(0)`
as if it were the original factor. `variadic_sum_wasm_merge_extract` then failed
in create with `expected i64, got Struct`. This is a bridge initialization bug,
not a Wasm limitation. Direct intermediate factory bindings now retain their
mode explicitly, clear the raw configuration constant mask and expose supplied
intermediate constants through a separate marker/accessor. Raw constants still
follow their declared value indices. The new metadata is additive; an older SDK
receives the cleared mask and cannot reinterpret intermediate data as raw config.

`InitContext::aggregate_input_types()` returns Raw/Intermediate type binding or
None for scalar/older-host messages. It does not report the operator step:
native staged factories can bind original raw types for a final operator.
`intermediate_constant_value()` distinguishes nonconstant/missing data from
constant SQL NULL and ordinary constants. It is a validation/capacity hint;
merge still contributes each selected row. The common native scalar extract
adapter does not forward constants to its aggregate factory, so its hint is
absent. New SDK validation rejects malformed modes/markers, ambiguous raw masks
and non-NULL placeholders marked as nonconstant. Direct intermediate-bound
instances reject raw updates, including empty batches, and do not advertise
raw toIntermediate support.

Native query regressions construct real typed HUGEINT/COUNT ROW constants and
check global/grouped `_merge_extract`, `_merge` plus `_extract`, and constant
scalar `_extract` for three UDAFs. A configured raw multiplier is not applied
again during merge. Typed constant plan nodes are used because the test SQL
parser maps HUGEINT casts to DOUBLE; that parser behavior is shared with native
functions. Earlier parser/setup failures are retained as diagnostics alongside
the actual pre-fix create failure. SDK tests verify raw/legacy/intermediate
binding, constant NULL and malformed/conflicting metadata; factory tests verify
the raw-update/toIntermediate boundary.

Final validation: **88 Rust tests** (70 SDK, 15 macro, 3 example), **92 full
alignment GTests** (14 scalar, 24 runtime, 38 aggregate, 4 metadata, 12 companions)
and six full/three minimal CTest targets pass. Both guests pass all 33 benchmark
correctness cases. Full/minimal builds produce identical modules with 58 scalar
and 17 aggregate declarations. Native expression/ReduceAgg sources and binaries
remain unchanged from their preceding 1,642/12-test validation. Sources, binaries,
commands and passing/diagnostic classifications are in
`.deps/alignment-merge-constants-2026-10-05/evidence.json`.

Nine paired Release processes per guest alternate order on CPU 15 with 1024
rows, 100 untimed warmups and 100M fuel. Both run on the same final host; old/new
hosts and independent companion costs are not compared. All samples, including
outliers, are retained. Medians and exploratory paired bootstrap intervals are:

| Operation | Previous guest | Current guest | Native control before → after | Guest change, 95% interval | Current Wasm/native |
| --- | ---: | ---: | ---: | ---: | ---: |
| Row SUM | 54.18 us | 54.15 us | 1.73 → 1.72 us | -0.06%, [-3.29%, +5.23%] | 31.48x |
| Lambda reduce | 754.42 us | 751.27 us | 21.07 → 21.12 us | -0.42%, [-5.81%, +3.59%] | 35.57x |

Intervals resample nine paired process indices 20,000 times with seed 20261005;
they are same-machine exploratory intervals, not platform replication. No
speedup or verified regression is established. Only an existing single group's
update plus extraction is timed. IPC/fuel/callbacks are included; JIT/init/
creation/input allocation/destruction are excluded. No builds/tests overlap
those timings. Raw evidence is in
`benchmark_results/wasm-merge-constants-2026-10-05-paired/`, with method/data in
the stage's `benchmark-combined.json`. Transport/native fast paths and production
hardening remain necessary before claiming community readiness.


## Anonymous intermediate ROW alignment: 2026-10-05

Native SignatureBinder distinguishes `row(bigint)` (positional) from
`row(value bigint)` (requires that name). Type::equivalent ignores ROW names
more broadly and is not a signature-binding rule. A reproducible bridge defect
accepted an aliased anonymous intermediate through the native binder but failed
in guest merge with `merge intermediate type mismatch`.

Grouped/single intermediate merge now canonicalizes only Arrow field descriptors
to the resolved native intermediate type, recursively through ARRAY/MAP/ROW.
It reuses value buffers, offsets, null counts and owners, preserves physical
VARCHAR/HUGEINT/TIMESTAMP markers and exact leaf/DECIMAL/codec/OPAQUE identity,
and never changes caller/shared vector types. Intermediate initialization uses
the same resolved type for constants/placeholders. Raw/scalar binding is
unchanged; explicitly named signatures still reject incompatible field names.
This is schema conversion without payload copying; selection/export and IPC
retain their transport costs.

The SDK adds full `PositionalSumWasm` anonymous-ROW and `named_row_length`
named-field examples. Query regressions exercise grouped/global companion
merge/extract and ordinary raw-type-bound final factories. Bridge regressions
cover nested ROW under ARRAY/MAP, NULLs, constant/dictionary encodings,
intermediate initialization and incompatible leaf/DECIMAL types. Caller aliases
remain unchanged. The scalar named-field example verifies native rejection of
aliases and uses VarcharBytes for VARCHAR bytes; plain &[u8] is VARBINARY.

Validation: **88 Rust tests**, **94 full alignment GTests** (15 scalar,
24 runtime, 39 aggregate, 4 metadata, 12 companion), **six full/three minimal
CTest entries**, and **33 benchmark correctness cases per old/new host-module
pair**. Full/minimal module hashes match, with 59 scalar/18 aggregate exports.
Unchanged native expression/ReduceAgg evidence is retained with unchanged source
and binary hashes, not rerun. Reproduction and final logs, sources/binaries and
methods are archived in `.deps/alignment-row-field-names-2026-10-05/evidence.json`.

Nine process pairs alternate order on CPU 15, using an archived old host with
its old guest and the rebuilt host with its new guest. At 1024 rows, 100 warmups
and 100M fuel/export, all samples are retained:

| Operation | Before host/guest | After host/guest | Native control before → after | Change, exploratory 95% interval | Current Wasm/native |
| --- | ---: | ---: | ---: | ---: | ---: |
| Row SUM | 57.51 us | 55.62 us | 1.73 → 1.73 us | -3.29%, [-20.62%, +15.08%] | 32.15x |
| Lambda reduce | 839.63 us | 822.88 us | 21.85 → 22.18 us | -1.99%, [-26.91%, +35.23%] | 37.10x |

Intervals resample nine paired process indices 20,000 times with seed 20261005.
Wide intervals cross zero; no speedup or verified regression is established.
Only update of an existing single group plus extraction is timed, including
IPC/fuel/callbacks and excluding JIT/init/create/input allocation/destruction.
No builds/tests overlap timings. This measures ordinary SUM/reduce, not the
aliased schema conversion workload, companion/whole-query costs or platform
replication. Raw measurements and commands are under
`benchmark_results/wasm-row-field-names-2026-10-05-paired/`.

Native transport/pushdown/fast paths, callback-dependent standalone companion
authority, actual lambda captures/window argument channels, retained host
services, exact non-linear runtime accounting, streaming/segmented reclamation
and sanitizer/fuzz/platform hardening remain obligations. Those are API or
implementation work, not fundamental Wasm SQL capability limitations. This
stage does not establish full native capability/performance parity or community
readiness.

## Complex MAP keys and safe IPC alignment: 2026-10-05

Native `map_from_entries` rejects a NULL key root, but accepts a non-NULL
ROW/ARRAY key containing NULL children. Native Simple Function MapView equality
treats those children as comparable values. The Presto `map()` constructor's
stricter indeterminate-key rule is a function-specific policy, not a restriction
on every MAP value. The old bridge/SDK incorrectly applied that stricter rule
universally; the saved reproduction accepts the native value and then fails in
guest `generic_identity` with `Wasm MAP key contains NULL`.

MAP readers and owned writers now reject NULL entries/key roots while preserving
NULL children. Explicit `contains_nulls`/null-free views still detect nested
NULLs. `first_value_wasm` retains these values in owned State and passes actual
partial/intermediate/final queries and merge/extract companions. Scalar tests
compare native construction, lookup, equality/hash, borrowed identity and owned
identity. The SDK README includes the generic `map_lookup` example; author
arguments remain scalar/borrowed views, never a RecordBatch.

Arrow's MAP constructor can abort on NULL entries/keys before `ValidateFull`
returns a Status. The IPC reader therefore checks the declared schema first,
reads MAP using its identical LIST<STRUCT<key,value>> layout, validates those
arrays and MAP invariants, then restores the original descriptors sharing value
buffers. Unsupported/dictionary output schemas and trailing bytes fail closed.
Real guest regression modules return hostile MAP key/entry IPC; both paths must
produce a protocol-fatal exception and invalidate Store before retry. The saved
`safety-fixed.log` abort was in the old trusted test-fixture exporter; it is not
evidence of a demonstrated pre-fix guest exploit. Arrow itself is unmodified.

Validation: **89 Rust tests**, **96 full alignment GTests** (16 scalar,
24 runtime, 40 aggregate, 4 metadata, 12 companion), and **six full/three minimal
CTest entries** pass. Full/minimal guest hashes match, with 61 scalar/18 aggregate
exports. Native expression/Reduce evidence is retained with unchanged source and
binary hashes, not rerun. Final logs, diagnostic failures, source/artifact hashes
and commands are in `.deps/alignment-map-keys-2026-10-05/evidence.json`.

Ordinary UDAF timing compares the archived old host/guest with current host/guest
in nine alternating process pairs on CPU 15, at 1024 rows, 100 warmups and
100M fuel/export. Both pass correctness first (33 old / 35 current cases).

| Operation | Before | After | Native control before → after | Change, exploratory 95% interval | Current Wasm/native |
| --- | ---: | ---: | ---: | ---: | ---: |
| Row SUM | 53.78 us | 53.71 us | 1.72 → 1.73 us | -0.13%, [-3.17%, +2.52%] | 31.05x |
| Lambda reduce | 734.37 us | 760.48 us | 21.04 → 21.00 us | +3.56%, [-0.88%, +7.64%] | 36.21x |

Intervals cross zero; neither a speedup nor a regression is established. These
time update/extraction on an existing single group, including IPC/fuel/callbacks,
excluding JIT/init/create/input allocation/destruction. They do not measure the
complex-key state workload, companion or whole-query speedup.

Separately, `MapView::find` now compares borrowed values directly instead of
materializing the lookup key and every candidate. A control SDK restores only
the old `find` implementation; all other Rust sources and Cargo files match.
Both guests use the same current host, Release compiler/profile/features and
`-C target-feature=+simd128`. Nine alternating pairs use 1024 rows, 16 MAP entries
per row, ROW(BIGINT,ARRAY(BIGINT)) keys with eight payload elements, and last-key
lookup. All keys/values are non-NULL in this performance fixture, so both find
implementations have the same observable behavior. Both pass all 35 correctness
cases. Old find exhausts 100M fuel even with matching SIMD flags; both timing
sides explicitly use **1B fuel/export**, unlike the ordinary UDAF experiment.

| Scalar MAP lookup | Legacy find | Borrowed find | Change, exploratory 95% interval |
| --- | ---: | ---: | ---: |
| Wasm, ms per 1024-row batch | 35.87 | 15.88 | -55.73%, [-64.32%, -44.61%] |
| Native Simple Function, ms per batch | 0.318 | 0.327 | +2.86%, [-41.80%, +8.07%] |

Current lookup still costs **48.55x native**, versus 112.80x for legacy find.
Native-normalized change is -56.96%, with interval [-58.33%, -19.17%]. Controls
and Wasm samples vary substantially; all are retained. Bootstrap intervals
resample nine paired process indices 20,000 times with seed 20261005 and are
single-machine exploratory evidence. Scalar evaluation includes gather/IPC/fuel
but excludes JIT/init/input allocation/destruction. There is no UDAF or general
query speedup claim. No builds/tests overlap either experiment. The initial
control accidentally lacked SIMD; that cohort and module are preserved as
diagnostics and excluded from conclusions. Raw final measurements are in
`benchmark_results/wasm-map-keys-2026-10-05-paired/` and
`benchmark_results/wasm-map-lookup-2026-10-05-paired/`.

Transport/native optimizer paths, callback-dependent standalone companion
authority, actual captures/window lambda channels, exact runtime accounting,
retained host services, streaming reclamation and production hardening remain
implementation obligations. This stage does not establish full parity or
community readiness; MAP semantics and borrowed lookup are implementable Wasm
capabilities, not fundamental isolation limitations.

## Encoded scalar leaf gathering: 2026-10-05

The host's full-selection direct export now accepts ordinary ROW/ARRAY/MAP with
Arrow-supported scalar dictionary/constant leaves over flat storage. It lets
the native Arrow bridge flatten those leaves while retaining plain siblings.
Previously one encoded BIGINT tag caused a whole ROW copy, including a large
sibling ARRAY. Selection indices are allocated only for gather fallbacks.
Complex encodings, encoded UNKNOWN, unloaded lazy bases and sparse selection
keep native gathering. Wrapper inspection never loads a lazy base. Guest ABI,
SDK arguments and typed/staged intermediate rules are unchanged.

The safety regression demonstrates the old whole-value copy through failure of
an original-buffer ownership assertion. Current tests cover retain/release,
detached caller lifetime, unchanged source children, nested ARRAY/MAP, outer
and leaf NULLs, full/sparse/empty selection, complex root dictionary/constant,
UNKNOWN and an actual lazy loader that accepts only two referenced base rows.
All six full/three minimal CTests and 96 full alignment GTests pass. Rust's
89-test evidence is retained with unchanged Rust/Cargo sources, not rerun.
Full/minimal guest hashes are unchanged, with 61 scalar/18 aggregate exports.

Three host-only gather/serialization diagnostics have the same logical
ROW(payload ARRAY(BIGINT),tag BIGINT): 1024 rows, 256 payload elements per row
(2 MiB BIGINT payload), and a flat/dictionary/constant tag. Nine alternating
process pairs on CPU 15 compare the archived pre-optimization host with current
host, using matching fixtures/build settings and identical guest bytes. Both
pass all 38 correctness cases (including the three host-only diagnostics).
At 100 warmups, dictionary-leaf gather drops **181.76 → 79.48 us** (-56.27%,
exploratory interval [-56.37%, -55.68%]); constant-leaf gather drops
**180.19 → 79.88 us** (-55.67%, [-55.92%, -54.97%]). Plain control is
79.85 → 78.35 us. These time preparation/serialization and temporary payload
teardown into a reused destination, excluding source/destination allocation
and correctness decode. They execute no guest/native Simple Function and do
not establish UDAF or whole-query speedup. The
[scalar assessment](wasm-scalar-sfi-parity.md) records normalized controls.

Ordinary UDAF SUM/reduce are timed separately on an existing single group,
including IPC/fuel/callbacks and excluding JIT/init/create/input allocation and
destruction, at 1024 rows, 100 warmups and unchanged 100M fuel. The first nine
pairs show reduce raw +2.38% (interval [+0.08%, +4.10%]), with native-normalized
interval crossing zero. Nine further pairs focus on SUM/reduce; all samples
from both cohorts are retained and combined in recorded order.

| Ordinary UDAF, 18 paired processes/host | Before us | After us | Change, exploratory 95% interval | Current Wasm/native |
| --- | ---: | ---: | ---: | ---: |
| Row SUM | 54.055 | 54.405 | +0.65%, [-1.79%, +2.88%] | 31.27x |
| Lambda reduce | 737.235 | 752.205 | +2.03%, [+0.16%, +3.28%] | 35.16x |

Native SUM is 1.74 → 1.74 us; native reduce is 21.280 → 21.395 us.
Native-normalized reduce change has interval [-1.20%, +2.99%]. The raw positive
difference remains visible; normalization does not prove the code caused it,
nor establish absence of a regression. Profiling/cause attribution remains
open; this stage makes no ordinary UDAF improvement claim. The transport
diagnostics are not a substitute for those ordinary aggregate measurements.
Bootstrap intervals resample 18 paired indices 20,000 times with seed 20261005;
gather diagnostics use their nine pairs separately. These are single-machine
exploratory intervals, not platform replication. No builds/tests overlap timing.

Source/module/binary hashes, the pre-change failure, final tests and both
measurement cohorts are recorded in
`.deps/alignment-gather-leaves-2026-10-05/evidence.json`; raw measurements are
in `benchmark_results/wasm-gather-leaves-2026-10-05-paired/` and
`benchmark_results/wasm-gather-leaves-2026-10-05-ordinary-paired/`.
IPC transfer remains eager and native value-hook/pushdown/encoding/adaptive
paths, captures/companion authority, exact runtime accounting, retained host
services, streaming reclaim and production hardening remain implementation
obligations. Full parity and community readiness remain unestablished.

## Direct primitive column construction: 2026-10-05

Profiling targeted the previous stage's small positive raw reduce difference.
Two long-loop process pairs (20,000 batches each) use the archived old host and
current host with identical old guest bytes. Whole-process retired instructions
change -0.31%, cycles +0.63% and task-clock -1.54%, with CPU frequency/cache
variation. These counters include registration/JIT/warmup/teardown and cannot
attribute the earlier steady-state 2% difference or prove its absence. An
initial incorrect relative-benchmark filter measured only initialization; those
files are explicitly preserved as diagnostics and excluded from attribution.

A private LD_PRELOAD profiling shim enables Wasmtime perfmap symbols without
changing production sources. Sampling the current host/old guest at 99 Hz for
40,000 reduce batches records 3,453 samples, none lost, including startup/JIT.
Generic::materialize has 9.8% self samples, and Vec<Value> construction is
prominent. JIT symbols can alias identical machine code; Date32 labels in this
BIGINT fixture do not imply DATE arguments. Follow-up sampling of the new SDK
is also retained (3,127 samples). Those percentages diagnose hot paths rather
than establish steady-state speedup.

BOOLEAN/TINYINT/SMALLINT/INTEGER/BIGINT/REAL/DOUBLE column construction now
copies owned scalars or reads borrowed Generic leaves directly into typed Arrow
input, removing the intermediate owned Vec<Value> materialization. This also
applies to nested primitive children and internal lambda argument columns.
Other logical types keep their existing schema validation/construction; public
writer validation still checks bound types before recording row outputs. ABI,
author arguments, manifests, dependencies and state ownership are unchanged.

New tests cover all seven types, signed limits, sliced mixed owned/borrowed
values, detached source lifetime, NULL/empty outputs, type rejection, NaN
payload/sign and negative-zero bits. Validation: **90 Rust tests**, **96 full
alignment GTests**, **six full/three minimal CTests**, and **38 benchmark
correctness cases per variant**. Full/minimal guest hashes match, with unchanged
61 scalar/18 aggregate exports. The native host executable/source is unchanged
in the isolated SDK comparison; native expression/Reduce evidence is retained
with unchanged source/binary hashes, not rerun.

Nine balanced process triplets on CPU 15 compare (A) archived pre-gather host /
old SDK, (B) current host / old SDK, and (C) current host / new SDK. Variant
positions rotate and reverse so each occupies each position three times.
At 1024 rows, 100 warmups and unchanged 100M fuel, all samples/controls remain.

| Case | B old SDK → C new SDK, us/batch | SDK change, exploratory 95% interval | A old host/SDK → C current, us/batch | Total change, exploratory interval | Current Wasm/native |
| --- | ---: | ---: | ---: | ---: | ---: |
| Checked scalar ADD | 48.59 → 47.91 | -1.40%, [-4.39%, +0.89%] | 49.32 → 47.91 | -2.86%, [-4.40%, -1.24%] | 52.91x |
| Row SUM | 53.69 → 53.36 | -0.61%, [-7.71%, +2.75%] | 53.64 → 53.36 | -0.52%, [-1.66%, +2.74%] | 30.84x |
| Lambda reduce | 759.81 → 652.27 | -14.15%, [-16.48%, -8.62%] | 756.80 → 652.27 | -13.81%, [-17.34%, -9.77%] | 30.78x |

Native reduce controls are A 21.17 / B 21.11 / C 21.19 us; ADD controls
0.90434 / 0.90577 / 0.90547 us; SUM controls 1.73 / 1.74 / 1.73 us.
SDK reduce native-normalized interval is [-16.91%, -9.35%]; total reduce
interval is [-17.26%, -9.76%]. SUM and isolated SDK ADD intervals cross zero,
so no improvement is established for them. The total comparison shows the
current fixture improves relative to the older complete implementation; it
does not retrospectively identify the cause of the earlier small raw difference.

Intervals bootstrap nine paired triplet indices 20,000 times with seed 20261005.
Only scalar evaluation or update/extraction of an existing single group is
timed, including IPC/fuel/callbacks; JIT/init/create/input allocation/destruction
are excluded. No builds/tests/profiling overlap comparative timings. These are
single-machine selected workloads, not grouped/spill/window/whole-query parity.
Profiling artifacts are in `.deps/alignment-reduce-profile-2026-10-05/`, final
source/module/binary hashes and methods in
`.deps/alignment-primitive-build-2026-10-05/evidence.json`, and raw triplets in
`benchmark_results/wasm-primitive-build-2026-10-05-paired/`.

Generic materialization in remaining comparisons, callback transport, native
pushdown/encoding/adaptive paths, captures/companion authority, exact runtime
accounting, retained host services, streaming reclaim and production hardening
remain implementation work. Full capability/performance alignment and community
readiness remain open.

## Owned State / borrowed input comparison: 2026-10-05

This extends demand decoding to `Value::compare/equals` with one owned and one
borrowed operand. Reduce's per-row initial-value check previously materialized
its borrowed argument before comparison. A generic aggregate comparing retained
nested State with an update/merge input also copied the whole borrowed tree.
Standard scalar and byte values now compare directly; ARRAY/ROW visit only
reached children, and MAP allocates sorted entry indices without copying value
trees. Operand direction is preserved, including equals-only length mismatch
sentinels and NULL ordering independently of ascending. DECIMAL precision/scale,
NaN/signed-zero behavior, type errors, custom codec identity/semantics and OPAQUE
errors retain their contract. Mixed custom codec payloads still materialize;
that remaining cost is an SDK optimization, not an isolation restriction.

State still owns retained values. This change does not allow a borrowed input
to survive an invocation. The SDK README shows a min-like update/merge helper
that compares a borrowed candidate first and materializes only a retained
replacement. Standard nested ARRAY/ROW/string mixed comparisons verify zero
allocations in both directions; a malformed timestamp in an unreached tail
proves demand decoding. The all-flags corpus covers sliced nested values and
MAP insertion order, and scalar tests cover all standard domains, type
mismatches, custom identities and opaque errors. **92 Rust tests**, **96 full
alignment GTests**, and **six full/three minimal CTests** pass, including existing
grouped/staged, nested-lambda and forced-spill UDAFs. The same native host binary
is retained; only the guest and SDK documentation change. Full/minimal guest
bytes match, with 61 scalar and 18 aggregate declarations.

Ten alternating process pairs retain native controls, unchanged 1024 rows,
100 warmups and 100M fuel. These time update/extract of an existing single group,
including IPC/callbacks, and exclude JIT/init/create/input allocation/destruction.
All 38 correctness cases pass with both guest modules.

| Workload | Before us | After us | Raw change, exploratory paired 95% interval |
| --- | ---: | ---: | ---: |
| Lambda reduce | 731.155 | 698.055 | -4.53%, [-24.37%, +23.35%] |
| Row SUM | 53.770 | 57.900 | +7.68%, [+1.16%, +68.63%] |
| Scalar ADD control | 52.375 | 54.140 | +3.37%, [-41.78%, +100.57%] |

Native reduce is 21.135 → 21.565 us, SUM 1.785 → 1.785 us and ADD
1.00740 → 0.93261 us. Individual native samples vary substantially (for example,
native reduce reaches 49.64 us). Native-normalized intervals cross zero for all
three Wasm cases; SUM's raw positive difference is retained, not dismissed as
proof of no regression. A post-run process snapshot shows CPU-intensive workers
from another benchmark predating this cohort, with CPU 15 in their affinity.
It records start times/CPU usage, not per-case causal attribution; no unrelated
process was modified. These results establish neither speedup nor absence of
regression. Isolated follow-up timing remains necessary. All paired samples,
commands and hashes are archived in
`benchmark_results/wasm-mixed-compare-2026-10-05-paired/`, with validation/source
evidence and the load snapshot under `.deps/alignment-mixed-compare-2026-10-05/`.
No grouped/spill/window/whole-query performance or complete alignment is claimed.
