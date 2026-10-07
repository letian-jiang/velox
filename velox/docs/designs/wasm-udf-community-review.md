# WASM UDF community review

Review date: 2026-10-04, Asia/Singapore. This assessment covers the current
working tree, including uncommitted and untracked implementation files, on
`907acb13d55abe79093882f242dc19856578a6d3`. The local native baseline is its parent,
`568652c5f5aa2ba77228a09e8dc4c6c0bd0cdb40`; this is not a claim about today's
upstream main branch.

## Remediation after the review

The findings R1–R11 below preserve the **original audit**, before the fixes on
2026-10-04. They are historical evidence, not descriptions of the repaired
implementation. The [design contract](wasm-udf.md) is authoritative for current
behavior; the capability table later in this report reflects the fixes.

| Finding | Implemented change | Regression evidence |
| --- | --- | --- |
| R1 | Schema checks before array construction, full Arrow validation and Velox index checks before import; bounded decompression; safe MAP-as-LIST read before restoring MAP | `WasmSafetyTest`: malformed offsets/schema, DECIMAL precision, NULL MAP key roots/entries from a real guest with Store invalidation, trailing bytes, compressed allocation limit and retained ownership |
| R2 | Aggregate cleanup catches every failure, invalidates Store and clears handles | Safety cleanup fault injection; full `RowContainer` destructor regression |
| R3 | Finite table element/count limits in both Store constructors | Oversized initial table, table-count rejection and rejected `table.grow` |
| R4 | Fatal traps/protocol/bridge failures bypass TRY and invalidate Store; valid row errors remain recoverable | Forged OPAQUE under TRY; fuel/aggregate IPC invalidation; checked arithmetic preserves good rows |
| R5 | Prepare then publish registry snapshots at startup; normalized names; reject native/special-form shadowing; merge scalar overloads across modules | Partial-registration, aliases, concurrent registration, alpha-renamed generics, overwrite retaining other overloads |
| R6 | Repair fixture lifecycle/counts and Folly test initialization; add independent core metadata tests and standalone safety target | Full scalar/runtime/aggregate targets and `VectorFunctionMetadataTest`; minimal CTests |
| R7 | Parse and compile one bounded immutable snapshot; content-keyed weak cache; compile outside cache lock | Same-size/same-mtime changed contents are compiled separately |
| R8 | Public resource policy; query pool reserves full linear capacity before Store creation; IPC/decompression charging; epoch deadline and task cancellation | Pool charge/release, decompression rejection, callback cancellation and Driver task-token interruption |
| R9 | Consolidate current scope/wire/lifecycle/resource contracts; remove obsolete sidecar | Design, RFC, SDK README and parity assessment updated together |
| R10 | `checked_div` returns overflow as a row error | MIN/-1 is NULL under TRY while 10/2 remains 5 |
| R11 | Matching native Simple Function prefix baseline, separate built-in VectorFunction concat; checked-add overflow aligned | NULL/encoding/sparse/large-string/generic/variadic correctness checks plus fresh timing runs recorded below |

R8 has explicit scope limits: JIT/compilation, table/internal allocations and
host metadata are not exactly charged to query MemoryPool; fuel is per export,
not a query-wide CPU budget. Registration is privileged, startup-only work and
requires deployment limits. Linear capacity reservation is conservative rather
than measured RSS. Full native memory-arbitration/spill efficiency is not
claimed. These are documented obligations, not fundamental WASM SQL-type
limitations. Sanitizer/fuzzing and deployment load testing remain additional
release-hardening work.

## Validation after the fixes

- Rust workspace: **32 tests passed**.
- Minimal Release build: **3 CTest entries passed**, including smoke,
  benchmark output verification and the standalone safety regressions.
- Full Release build: **4 CTest entries passed**: **26 GTests** (11 scalar,
  7 runtime, 4 aggregate/operator and 4 independent native metadata tests),
  plus the standalone safety target. The aggregate checks include actual
  partial/final execution, forced spill and native RowContainer destruction.
- The Driver-context/Task-token cancellation regression completed in **25 ms**
  in this run. This is one observed test duration, not a latency distribution
  or real-time guarantee; the test cancels an executing infinite guest loop.
- `git diff --check` passed. C++ sources were formatted with clang-format 20.1.8;
  Rust formatting used the workspace formatter.

Commands and current-run logs are under `.deps/fixes-2026-10-04/`; source and
artifact hashes are recorded in `evidence.json`. The full build disables
unrelated RPC operators with `VELOX_ENABLE_RPC=OFF`, preserves normal exec
integration, and uses a separate build directory from minimal mode. Its test
entrypoints reuse native Folly initialization. The independent metadata target
has no WASM dependency. See the SDK README for portable target commands.
Sanitizers/fuzzing, every expression test, live-cursor cancellation stress,
process/JIT peak accounting and deployment load tests were not run. Those
remain release-hardening work; passing the selected targets is not proof of
production safety or complete native optimizer parity.

## Fresh performance evidence after the fixes

2026-10-04 Release, Wasmtime 39.0.0 / Arrow 18, usage tracking enabled;
1024 rows/batch, median of three independent processes, microseconds per batch:

| Matched Simple Function case | Native µs | WASM µs | WASM/native time |
| --- | ---: | ---: | ---: |
| Checked BIGINT add | 0.905 | 69.130 | 76.35x |
| Nullable prefix | 11.060 | 398.300 | 36.01x |
| Add: 25% NULL | 0.859 | 60.540 | 70.48x |
| Add: dictionary | 2.040 | 69.450 | 34.04x |
| Add: constant | 0.909 | 18.190 | 20.02x |
| Add: every eighth row selected | 0.224 | 24.480 | 109.37x |
| Prefix: 25% NULL | 10.030 | 353.510 | 35.25x |
| Prefix: 2048-byte strings | 231.120 | 1330.000 | 5.75x |
| Typed variadic sum: 8 arguments | 6.930 | 341.810 | 49.32x |
| Generic ARRAY hash: 32 BIGINT elements | 111.380 | 5100.000 | 45.79x |
| ARRAY sum: 16 BIGINT elements | 4.650 | 406.540 | 87.43x |

The native built-in VectorFunction concat is separate (**10.750 µs** at 1024
rows). It is not the prefix SFI baseline. All cases validate output before
timing. Registration/JIT, input creation, warmup, verification and final
teardown are excluded. Constant/dictionary cases include expression peeling
or memoization; the sparse case selects 128 rows in a 1024-row input batch.
The generic-hash workload is additional nested computation, not a proxy for
all compute-heavy applications. Its 128-element attempt exhausted the default
100-million fuel policy; successful timings use 32 elements, not a silently
increased fuel budget.

The **12 completed processes** cover 128/1024/4096/8192 rows with three runs per
size. Extension cases run at 1024; the other sizes measure the original scalar
and aggregate cases. 8192-row ARRAY sum succeeds under the unchanged default
fuel limit. All current raw logs, commands and medians are in
`benchmark_results/wasm-review-fixes-2026-10-04/`. The initial filter attempt
omitted Folly relative cases and was discarded; successful runs verify that
both native and WASM cases are present. Sparse/large-string results and these
steady-state batch times do not establish full-query, startup, concurrency,
peak-memory or cancellation performance.

## Alignment follow-up: 2026-10-05

Generic hash/compare/equals now traverse borrowed guest views; ARRAY/ROW
comparison stops at a decisive difference and hash visits all elements. Owned
Value operations avoid tree cloning. MAP comparison allocates sorted indices;
codec payloads are borrowed while their small headers/registry lookup may
allocate. At this stage mixed owned/borrowed comparisons materialized the borrowed
operand; the later mixed comparison stage removes standard-value materialization.
Owned SqlKey deliberately retains independent values. This is
SDK demand decoding, not host projection or guest-triggered host loading.

ASCII alternates declare `has_ascii`, request native cached input encoding,
and consume the transported `velox.scalar.ascii_inputs` flag once per batch.
NULL and nested strings follow native SFI rules; an older host without metadata
uses one batch scan. All VARCHAR scalar outputs get a host-computed encoding
cache based on actual returned bytes. Expression dictionary wrappers have
separate caches as in native vectors; no guest declaration can imply ASCII output.

The follow-up also fixes native error categorization outside TRY: validated
business errors propagate as VeloxUserError and permit healthy Store reuse.
Bridge/protocol errors and non-user statuses still invalidate the Store.
Every status payload is validated before reporting, preventing an earlier
business error from hiding a malformed later row. Regressions cover plain and
typed user errors without TRY, same-instance successful calls after those
errors, fatal system status under TRY, and malformed later-status invalidation.

Validation passes **37 Rust tests**, **27 full-build GTests** (12 scalar,
7 runtime, 4 aggregate/operator, 4 native metadata), and **7 CTest entries**
across minimal and full builds. New tests count allocations in borrowed
nested/string operations, prove comparison skips undecoded tails, exercise
all CompareFlags combinations on sliced nested data, check ASCII batch/cache
behavior, and verify a guest returning Unicode for ASCII input is classified
from its real output. Logs and source/artifact hashes are under
`.deps/alignment-2026-10-05/`.

Three fresh processes each measure all 27 cases at 1024 rows/batch, 100 untimed
warmup batches, unchanged default 100-million fuel/export, and usage tracking.
The actual output ASCII scan is included in measured batch execution:

| Matched Simple Function case | Native µs | WASM µs | WASM/native time |
| --- | ---: | ---: | ---: |
| Checked BIGINT add | 0.930 | 69.710 | 74.97x |
| Nullable prefix | 10.900 | 392.420 | 36.00x |
| Typed variadic sum: 8 arguments | 7.790 | 330.200 | 42.39x |
| Generic ARRAY hash: 32 BIGINT elements | 110.460 | 5170.000 | 46.80x |
| ARRAY sum: 16 BIGINT elements | 4.680 | 423.880 | 90.57x |

The generic hash median is **5.170 ms** versus **5.100 ms** in the October 4
runs. These separate-date measurements do not establish a speedup; the proven
benefit is removing value-tree copying and adding comparison short-circuiting.
Large remaining costs include IPC, dynamic access and fuel-metered execution.
A 128-element correctness attempt still exhausts the default fuel policy; it
has no successful timing and the budget was not increased. The configurable
`--udf_generic_array_elements` flag makes that workload reproducible without
editing source. All raw logs/commands/medians are in
`benchmark_results/wasm-alignment-2026-10-05/`, including the failed 128-element
attempt. The complete table is `report.txt` in that directory.

Intermediate October 5 timings before the row-error classification fix are
preserved separately in `benchmark_results/wasm-alignment-2026-10-05-before-row-error-fix/`.
The final table and primary artifact directory above use the final implementation.

## Borrowed variadic follow-up: 2026-10-05

The SDK now offers `VariadicView<'a, T>` alongside the existing owned
`Variadic<T>`. Native [VariadicView](../../expression/VariadicView.h) constructs
accessors instead of a per-row value vector. The borrowed Rust adapter follows
that model: validate every tail column type and length once per batch, then
construct a row view without allocations or value conversion. The schema check
runs even when the function ignores its tail or the batch is empty.

`len/is_empty`, checked `at/get`, forward/backward iteration and `nth/nth_back`
are available. `nth` skips argument values without reading them. Nullable tails
provide `skip_nulls`, `is_null(index)` and conservative `may_have_nulls` metadata.
NULL checks do not decode non-NULL UTF-8 strings, matching the useful native
OptionalAccessor behavior. Every value access yields Result, and skip_nulls
preserves conversion errors; flattening a Rust Result iterator would silently
drop errors and is deliberately absent from the examples. Generic and typed
nested arguments remain borrowed. An owned String element still copies its
bytes when accessed, and direct test-value slices clone only accessed elements.
The owned Variadic API and its eager Vec adapter remain compatible.

The examples use borrowed views for typed sum, heterogeneous count, generic
first and initialized variadic prefix, plus a new first-non-NULL VARCHAR
function. SQL regression proves an unvisited invalid UTF-8 tail is not decoded,
accessing that value yields a per-row UserError/TRY NULL, and sum overflow does
not discard successful rows. Rust regressions cover zero tails, SQL NULL versus
out-of-range, empty/mismatched schemas, row bounds, reverse iteration, skipped
invalid values, retained errors, sliced nested values and zero allocations for
borrowed access/NULL metadata. This is guest demand decoding; host gathering,
full IPC parsing and eager output codec decoding remain unchanged.

The final validation passes **42 Rust tests**, **28 full-build GTests**
(13 scalar, 7 runtime, 4 aggregate, 4 native metadata), and **7 CTest entries**.
The module now has 58 scalar declarations; the earlier fixture-count assertion
was updated accordingly. Minimal CTest also validates outputs of all 27
benchmark cases. Final logs and source/artifact hashes are under
`.deps/alignment-variadic-2026-10-05/`.

Three fresh independent processes time matched native/Wasm eight-argument
variadic sum and checked-add control at 1024 rows/batch, 100 untimed warmups and
unchanged 100M fuel/export. Registration/JIT, input generation and teardown are
excluded; schema checks and row/output processing remain in the timed path.

| Targeted Simple Function case | Native µs | WASM µs | WASM/native time |
| --- | ---: | ---: | ---: |
| Checked BIGINT add | 1.430 | 112.850 | 78.92x |
| Typed variadic sum: 8 arguments | 10.000 | 340.480 | 34.05x |

These are four timed cases, with the full 27-case correctness profile validated
separately. The variadic WASM median does not demonstrate an end-to-end speedup
against the earlier 330.200 µs full-suite result; process conditions and control
timings differ. The established benefit is no per-row argument Vec allocation
and no conversion of unvisited arguments. Final raw runs, exact commands and
medians are in `benchmark_results/wasm-variadic-view-2026-10-05/`. Intermediate
runs before NULL-metadata accessors are retained in its
`-before-null-metadata` sibling directory.

## Host output codec follow-up: 2026-10-05

Nested scalar codec outputs now use native LazyVectors over owned serialized
payloads. ROW fields, ARRAY elements and MAP values can remain serialized until
accessed; MAP keys and OPAQUE handles are resolved/validated eagerly. Top-level
scalar codec results are materialized before apply returns. Aggregate and
borrowed IPC decoders remain eager; owned IPC decoding exposes an explicit
`ArrowIpcDecodeOptions` policy for direct callers.

The first nonempty load decodes the whole codec column, because native
LazyVector loaders are single-use and cached UDF results may be consumed by
other row selections later. This implements column/field deferral, not selective
row decoding or host input projection. The native Project -> LazyDereference
query regression verifies zero decoder calls for an unused codec field and
three calls when that field is read. Ordinary ROW dereference can load all
children, as its paired regression shows; sparse scatter may force a copy.

The lazy vector owns IPC and retains MemoryPool until its complete destruction.
The lifetime test initially reproduced a pool leak/abort: LazyVector's base
NULL bitmap was freed after the loader's pool owner. A pool-retaining vector
deleter fixes that ordering, and the same test now loads/releases correctly
after the caller's pool reference disappears. Native materialized vectors still
follow the normal requirement that their pool outlive their buffers.

Deferred failure callbacks hold weak Store references, allowing a result to
outlive its producer without retaining guest memory or using a dangling
producer pointer. A decoder's UserError/std exception becomes a fatal runtime
bridge failure, invalidates a live Store and bypasses TRY. Failed partial vectors
are not published and retries do not expose their values. Wire shape/full Arrow
validation, codec identity/version, non-NULL payload presence, MAP-key structure,
OPAQUE handles and value/error exclusivity remain eager; codec byte contents
are interpreted on load.

Final validation passes **42 Rust tests**, **29 full-build GTests** (14 scalar,
7 runtime, 4 aggregate, 4 native metadata) and **7 unique CTest entries** across
the two builds. Safety regressions cover nonempty partial load followed by other
rows, NULL preservation, owned IPC/pool lifetime, ARRAY/MAP values, eager MAP
keys, malformed unused payload structure, expired nested handles, repeated
decode failure, live Store invalidation, producer destruction and sparse writes.
The benchmark correctness profile now contains **30 cases**: 27 existing cases
plus three output-codec diagnostics. Logs and hashes are under
`.deps/alignment-codec-2026-10-05/`.

Three independent Release processes measured 1,024 rows/batch with Wasmtime 39,
100M fuel per export and 100 untimed warmup batches. The following medians use
the same `generic_identity` guest and output IPC; only the decode policy and
consumed field differ. Registration/JIT, input gather/codec encoding and teardown
are excluded; invocation (including input serialization/copy), output IPC decode
and selected-field materialization are timed. The codec copies eight bytes.

| Output-codec diagnostic | Median per batch | Non-NULL codec decode calls per batch |
| --- | ---: | ---: |
| Eager, codec field unused | 1.160 ms | 1,024 |
| Lazy, codec field unused | 1.220 ms | 0 |
| Lazy, codec field used | 1.040 ms | 1,024 |

This establishes skipped decoder calls and native reconstruction for an unused
field, but does **not** establish lower latency: lazy-unused is 5.17% slower than
eager-unused in this fixture, while lazy-used is faster than both. These small
diagnostics do not measure complete queries or compare against native Simple
Functions. The separate matched checked-add control measures native **1.470 us**
and WASM **110.720 us** per batch (**75.32x**). Raw process results, commands and
medians are in `benchmark_results/wasm-output-codec-2026-10-05/`; the capability
change should not be presented as a general speedup.

## UDAF alignment follow-up: 2026-10-05

The aggregate authoring/transport assessment is now separate from scalar SFI:
[UDAF capability alignment](wasm-udaf-parity.md). New
`VeloxRowAggregate`/`#[velox_row_aggregate]` exports accept row values/borrowed
views, bind SQL generic/variadic/integer signatures and use typed logical
intermediates. Old aggregate modules preserve their VARBINARY/legacy transport.
Initialization supplies bound types, constants and declared config; create is
per group. Native empty/default-NULL semantics and non-default NULL merge are
explicit. Immutable extraction supports repeated expanding-window reads.

The host serializes complete aggregate bridge operations for concurrent spill
extraction, validates create handles against live earlier batches, and clears
handles under best-effort destruction. Same-name aggregate overloads merge and
replace by dispatch slot, with conservative aggregate/window metadata. Row
UDAFs expose toIntermediate using temporary per-row states. Window aggregates
now receive native constant inputs. DISTINCT's short constant vectors expand
before IPC gather; short nonconstant arguments remain errors. Native
AggregateInfo's shorter downstream constant lists do not masquerade as original
raw constants; required constants are enforced on raw update.

Regression evidence includes partial/intermediate/final/single, nested T and
NULL-first results, full-width HUGEINT, maximum native TIMESTAMP and arbitrary
VARCHAR bytes, configuration/constants/zero tails, DISTINCT/ORDER BY/masks,
expanding/sliding/empty windows, typed forced spill, direct final-stage binding,
toIntermediate, repeated/concurrent extraction, overload replacement and
cross-batch duplicate handles. Final validation passes **49 Rust tests**,
**37 GTests** (14 scalar, 7 runtime, 12 aggregate, 4 native metadata), **7 unique
CTest entries** and **31 benchmark correctness cases**. Logs and source/artifact
hashes are in `.deps/alignment-udaf-2026-10-05/`.

Three independent Release processes, 1,024 rows and 100M fuel/export, measured
medians of **2.020 us native SUM**, **21.030 us legacy WASM SUM**,
**129.950 us row WASM SUM** (64.33x native), **0.438 us native AVG** and
**20.610 us legacy WASM AVG**. Warmup is 100 untimed batches. The timed path
updates one existing group and extracts once; JIT, initialization, input
allocation and destruction are excluded. Row SUM uses factor 1 and one bigint
tail, matching output while doing extra argument/count/wide-state work.
It does not isolate adapter overhead or demonstrate a speedup. Raw runs and
unrounded medians are in `benchmark_results/wasm-udaf-row-2026-10-05/`.

Per-group guest memory/compaction, exact runtime/JIT charging, automatic
companion functions, typed native aggregate Status categories, lambda callback
imports, native pushdown/clustered/adaptive optimization and broader production
validation remain gaps. OPAQUE retained state requires a serialization codec;
invocation handles are rejected. Native aggregates can have SQL lambda/evaluator
hooks; the scalar SFI restriction does not apply to the whole aggregate API.
These mediated capabilities need implementation; direct host C++ pointer/object
compatibility is the isolated ABI boundary.

## Scope and evidence of the original audit

The review examines ABI definitions, module loading/cache, metadata parsing,
registration, scalar initialization and execution, aggregate lifecycle, Arrow
transport/ownership, custom codecs and OPAQUE handles, Rust views/writers and
comparison/hash helpers, macro-generated adapters, build integration, tests,
benchmarks and all three existing WASM design documents. Native references
include `SimpleFunctionAdapter`, `SimpleFunctionMetadata`,
`SimpleFunctionRegistry`, `SignatureBinder`, `VectorReaders`, view/writer
types, `ExprCompiler`, `Expr`, `EvalCtx`, `RowContainer` and the Arrow bridge.

Evidence levels used below:

- **Reproduced:** a probe exercised the current release library and/or guest
  module and recorded the stated behavior.
- **Source confirmed:** the current code establishes the behavior; a full query
  or failure-injection test was not run for that finding.
- **Design gap:** an intended contract needs implementation or an explicit
  scope decision. This does not mean WebAssembly cannot support it.

On this review date, Rust workspace tests passed **32 tests** and the existing
minimal build passed **2 CTest entries**. The additional probe passed its
execution and recorded the failure behaviors below. Full expression tests,
the full WASM GTest targets, HashAggregation/spill/cancellation integration,
sanitizers and fuzzing were not executed. Minimal mode overrides the normal
`VELOX_BUILD_TESTING` and `VELOX_ENABLE_EXEC` variables to OFF; cache BOOL values
alone do not describe the effective build.

Local probe source, commands and logs are under
`.deps/review-2026-10-04/`. They are ignored diagnostic artifacts, not committed
regression tests. `run_probe.py` rebuilds/replays the local diagnostic with the
saved compile/link arguments and modified modules; it requires the existing
pre-fix Release build and is not a portable clean-checkout test. Do not replay
its original bad-behavior expectations on the repaired build; use the new
`velox_wasm_safety_test` regression target instead. `evidence.json` records
the source/artifact hashes and commands. `ctest-current.log` contains only this
review's CTest run; the longer `ctest.log` also contains historical runs.
Existing 2026-10-02 benchmark results are reused as historical
performance evidence; this review did not rerun the twelve timing processes.

## Original findings that blocked the implementation PR

### R1 P1 Validate untrusted Arrow arrays before native import

**Reproduced.** [ArrowIpc.cpp](../../functions/wasm/ArrowIpc.cpp) reads a batch,
checks its top-level dimensions, and calls `importFromArrowAsOwner` without
`RecordBatch::ValidateFull()`. Logical wire schema checks also occur after
native import. Arrow IPC parsing is not equivalent to validating array buffer
invariants: the bundled Arrow 18 reader does not automatically perform full
record-batch validation.

The probe starts with a valid one-row VARBINARY result containing `abc`, changes
the serialized final offset from 3 to 64, and observes:

```text
ValidateFull: Offset invariant failure ... 64 > 3
host accepted invalid binary length=64 for actual buffer=3
```

The decoder constructs a native StringView describing bytes beyond the actual
Arrow data buffer. The probe deliberately does not read those bytes, and does
not demonstrate a crash or data disclosure; acceptance of the invalid range is
confirmed. Downstream consumers can read the range, creating a host memory
safety risk. Rust SDK validation cannot defend this boundary because the guest
can supply its own exports and bytes.

Validate the complete batch and expected schema **before native conversion**.
Check nested lengths/offsets, dictionary indices, validity buffers, wire scalar
component invariants, Velox index-size limits and SQL-specific MAP/DECIMAL
constraints. Full Arrow validation alone does not establish every Velox SQL
invariant. Add hostile-output tests and ASan/UBSan/fuzz coverage. Bound
decompression allocations before they occur, independently of validation.

### R2 P1 Prevent guest cleanup failures escaping native destructors

**Reproduced exception; source-confirmed destructor risk.**
[WasmAggregate.cpp](../../functions/wasm/WasmAggregate.cpp),
`destroyInternal`, invokes guest code without catching traps, fuel failures or
ABI errors. The probe substitutes another valid `(i32,i32)->v128` export as
destroy, initializes a real aggregate state, and observes an exception escaping
`Aggregate::destroy`.

The native chain is `RowContainer::~RowContainer -> clear ->
freeRowsExtraMemory -> freeAggregates -> Accumulator::destroy ->
Aggregate::destroy -> WasmAggregate::destroyInternal`. An exception escaping the
ordinary C++ destructor can terminate the host process, including during
unwinding after an earlier query failure. A full RowContainer crash test was
not run, so process termination is a consequence of the source call chain,
not an observed full-query result.

Make teardown nonthrowing, invalidate host handles even when guest cleanup
fails, and reclaim the Store independently of guest cooperation. Preserve the
original query error and report cleanup failures through an appropriate
diagnostic path. Add death/fault-injection tests using the actual RowContainer
and HashAggregation cleanup paths, including guest destroy loops and traps.

### R3 P1 Bound tables independently of linear memory

**Reproduced.** Both [Runtime.cpp](../../functions/wasm/Runtime.cpp) constructors
call `wasmtime_store_limiter` with `table_elements=-1` and `tables=-1`. A module
with a 200,000-element funcref table instantiates and executes with a 65,536-byte
linear-memory cap in the probe. The cap governs linear memory, not table storage.

This is a missing resource control for the RFC's untrusted-code goal, rather
than failure of the documented linear-memory cap. Define finite table element
and table count limits; test initial table declarations and `table.grow`.
Module-size, compilation and instance-count limits also require deployment
policy. Fuel is not a substitute for allocation limits.

### R4 P1 Separate protocol and execution failures from recoverable row errors

**Reproduced.** [WasmVectorFunction.cpp](../../functions/wasm/WasmVectorFunction.cpp)
catches every `std::exception` except `VeloxRuntimeError` and calls
`context.setErrors(rows, ...)`. Runtime and bridge checks use
`VELOX_USER_FAIL/CHECK`, so TRY can suppress them. The probe observes
`TRY(opaque_forge(...))` returning NULL after host handle validation fails.
A guest panic from the checked-divide example likewise makes both rows NULL
under TRY, including the otherwise successful `10 / 2` row.

The RFC currently says traps, resource exhaustion and malformed protocols fail
the query, while business errors preserve row-level TRY behavior. The code
does not implement that distinction. It also leaves the guest instance usable
after suppressed invocation failures, without defining whether its state is
safe to reuse.

Define an error taxonomy: valid row error records may be recoverable; invalid
ABI/IPC/handles and resource/runtime failures should follow an explicit fatal
policy and instance invalidation rule. Initialization business failures deserve
a separate decision, since native SFI caches initialization errors for selected
rows. Test TRY, IF/CASE selection, subsequent batches and cleanup for each class.

### R5 P1 Make registration atomic and define collisions across registries

**Partial registration reproduced; other collision behavior source confirmed.**
[Registration.cpp](../../functions/wasm/Registration.cpp) validates exports
before mutation, then registers SQL names sequentially and merely adds boolean
registration results. A module with one new SQL name and the remaining names
already registered returns 1 and leaves the new name installed. The actual
module has 64 declarations and 60 distinct registered SQL names. Export
validation does not provide a transaction across the registries.

Further source issues:

- Grouping and duplicate detection use raw names; the registry sanitizes names.
  Case aliases can therefore collide after grouping.
- Scalar registration only checks the vector-function registry. `ExprCompiler`
  checks vector functions before Simple Functions, so `overwrite=false` does
  not protect a matching native Simple Function from being shadowed.
- A later module cannot add overloads to an existing vector name with
  `overwrite=false`; `overwrite=true` replaces the name's existing overload
  list. This differs from native Simple Function registration by signature.

Normalize names before validation; specify namespaces and signature-level
collision rules. Provide a commit/rollback mechanism or explicitly coordinated
startup registration that cannot expose partial state. Preflight checks alone
are insufficient when concurrent registrations are allowed. Test failures at
each mutation point, native/vector collisions, case aliases and module overload
extension. A weaker best-effort API would require changing the RFC's atomicity
promise explicitly.

### R6 P1 Repair and execute the full test targets

**Source confirmed.** [WasmAggregateTest.cpp](../../functions/wasm/tests/WasmAggregateTest.cpp)
still asserts `registerWasmModule(...) == 23`, while the current module registers
60 SQL names in a clean registry. Its suite setup cannot pass as written.
Minimal mode omits this target, the scalar GTest target and `RuntimeTest.cpp`;
the two passing CTest entries do not execute their full integration coverage.

[WasmScalarTest.cpp](../../functions/wasm/tests/WasmScalarTest.cpp) defines
`SetUpTestSuite`, while its `FunctionBaseTest` base defines `SetUpTestCase`.
The fixture must harmonize these hooks and preserve the base setup, which
registers the type resolver, native functions and test memory manager. The
selected GTest version's SuiteApiResolver rejects simultaneous nondefault
legacy and modern setup hooks. This was checked against
[GTest 1.13 SuiteApiResolver](https://github.com/google/googletest/blob/v1.13.0/googletest/include/gtest/internal/gtest-internal.h#L477-L489),
but the full fixture was not compiled/run here.

Replace brittle counts with checks derived from validated declarations and
explicit expected names. Run scalar/runtime/aggregate GTests with exec enabled,
including partial/final/spill, before marking the PR ready. Add dedicated
expression-registry regression tests for the signature-specific metadata
changes in [VectorFunction.cpp](../../expression/VectorFunction.cpp) and
[VectorFunction.h](../../expression/VectorFunction.h); WASM smoke coverage is
not a replacement for testing this shared API with other functions.

## Additional design and code findings

### R7 P2 Load and identify one immutable module artifact

**Source confirmed.** Metadata parsing and compilation read the path separately.
The compilation cache key uses path, size and modification time, rather than
content identity. If the file changes between reads, the declarations and code
need not describe the same artifact; preserving size and timestamp can also
reuse a stale compiled module. The global compilation mutex serializes cache
miss compilation, and expired weak entries remain in the map.

Read one bounded byte buffer, parse and compile it, and use its content hash for
identity. Define module lifetime, replacement and cache eviction. Hot reload
can remain out of scope while registration still uses one immutable snapshot.

### R8 Resource accounting and cancellation need an explicit contract

**Design gap, source confirmed.** Fuel is enabled and refreshed for start,
alloc/free and scalar/aggregate entrypoints. It is not a query-wide CPU budget,
wall-clock timeout or query cancellation hook. Each exported call receives a
fresh budget, so one logical invocation can consume several such budgets.

The 64 MiB default does not account for module compilation/JIT artifacts,
std::string result copies, Arrow default-pool allocations, decompression,
tables, total concurrent instances, or retained guest state through Velox's
MemoryPool. Aggregate `accumulatorUsesExternalMemory=true` ensures lifecycle
cleanup; it does not account for guest memory or trigger arbitration based on
guest state growth. Serialization makes spill representation possible, but
does not prove arbitration/spill integration.

Expose resource policy through the registration/wrapper API, not only the
low-level WasmInstance constructors. Decide how to charge guest and Arrow
memory, cap compressed outputs or disable compression, bound total instances,
and connect cancellation/deadlines through epoch interruption or another
defined mechanism. Measure cancellation latency. These are implementation
tasks; the RFC must state which controls exist and which are deployment duties.

### R9 P2 Reconcile conflicting design documents and specify wire evolution

**Source confirmed.** [wasm-udf-rfc.md](wasm-udf-rfc.md) still says there is no
fuel. [wasm-udf.md](wasm-udf.md) says ARRAY/MAP/ROW/DECIMAL/generic/variadic types
are deferred. The SDK and [capability assessment](wasm-scalar-sfi-parity.md)
describe those implementations. The RFC's atomic registration and fatal
protocol-error contracts also exceed observed behavior.

Publish one authoritative scope and compatibility model. ABI version 1 currently
contains legacy batch/aggregate transport and `row_api=true` scalar transport
with additional logical markers and custom codec descriptions. Document
required markers, signedness, endian/ownership, NULL placement, error encoding,
accepted Arrow IPC versions/features, feature negotiation and which extensions
require rejection or a new version. Do not imply that all v1 exports have the
new scalar type range. Remove or update the obsolete sidecar JSON examples if
the public loading API supports embedded metadata only.

### R10 P2 Correct the checked-divide example's overflow behavior

**Reproduced.** The example checks division by zero but performs native Rust
signed division for other inputs. `i64::MIN / -1` panics/traps, rather than
producing a recoverable row error. With `[MIN, 10] / [-1, 2]`, TRY yields NULL
for both rows in the probe. Use `checked_div` and return a row error for overflow;
test that the successful second row remains 5.

### R11 P2 Describe benchmark baselines and semantics precisely

**Source confirmed.** The native add case is a checked native Simple Function,
and ARRAY sum explicitly registers `ArraySumFunction` as a Simple Function.
The native VARCHAR concat registration uses `VELOX_REGISTER_VECTOR_FUNCTION`,
so this case is a native built-in VectorFunction comparison, not an SFI-only
comparison. WASM `add_i64` uses unchecked release Rust addition, unlike the
native checked add implementation; the measured small values avoid the
difference, but the two authoring contracts differ.

Original recommendation: keep the useful end-to-end native built-in baselines, label each correctly,
and add matching Simple Function implementations and overflow contracts when
making SFI performance claims. Include NULL distributions, dictionary/constant
encodings, sparse selections, large strings, nested fields, generic/variadic
cases and at least one function with substantial computation. Separately measure
startup/JIT, host/guest peak memory, concurrency and cancellation. Do not infer
these quantities from steady-state simple arithmetic timings.

## Simple Function capability comparison

“Covered” means the standard value-domain paths have implementation and
selected tests. It is not proof over arbitrary inputs, every native type or all
engine optimizations. This table describes the repaired implementation; the
original R1–R11 observations above describe its pre-fix state.

| Capability | Native Simple Function | Current WASM scalar SDK | Difference and disposition |
| --- | --- | --- | --- |
| Row authoring | `call` variants; adapter handles batches | `velox_scalar`; adapter handles batches | Covered; host integration is a VectorFunction, not the native templated adapter |
| BOOLEAN and signed integers through HUGEINT, floats | Native physical values | Rust scalars; HUGEINT marked high/low wire | Covered; explicit transport conversions |
| VARCHAR arbitrary bytes | StringView byte view | VarcharBytes; str/String validate UTF-8 | Covered through byte API; str conversion errors are per row |
| VARBINARY, DATE | Native scalar views | Byte slices/vectors, Date32 | Covered |
| Full-range TIMESTAMP | Native seconds/nanos | i128 guest representation and marked struct | Covered for row scalars and row UDAFs; legacy batch/aggregate timestamps retain i64-nanosecond limit |
| DECIMAL and precision expressions | Native short/long storage and signature constraints | Decimal128 transport; integer variables; checked writers | Covered for scalar values; native representation differs |
| TIME, INTERVAL, timezone/IP/JSON and other logical types | Native registered types | Require explicit custom codecs; no general built-in mapping | Partial; enumerate supported codecs rather than saying every Velox type works automatically |
| UNKNOWN | Native unknown/NULL | Arrow Null and Generic/Option | Covered; entirely unbound return variables cannot infer a result type |
| Nested ARRAY/MAP/ROW | Native typed views | ArrayView/MapView/RowView, nested combinations | Covered; borrowed guest views follow IPC materialization |
| Large ROW arity | Template or generic access | Typed tuple limit 16; DynamicRow/Generic arbitrary arity | SDK ergonomics limit, not WASM limitation |
| Nested result writers | Native vector-backed writers | Guest writers/Value, then Arrow construction | Covered construction; no equivalent native buffer-writing cost |
| SQL generics and nested variables | Template registrations and SignatureBinder | Same host binder, runtime Generic | Rust generic scalar authoring now infers SQL variables from SDK bounds or emits concrete build-time instances; generic UDAF impl inference remains pending |
| Known/comparable/orderable constraints | Native type properties | Host binder checks native properties | Binding covered; a custom type additionally needs registered guest semantics for compare/hash |
| Runtime generic cast/access | Native GenericView | Generic get/try_cast/as_array/map/row | Covered standard domain; runtime checks/dispatch remain |
| Typed/heterogeneous variadic tail | Native VariadicView readers | Borrowed VariadicView, owned Variadic<T>, Generic with any | Covered; final tail only, zero tail allowed; borrowed view validates schema once and reads only accessed arguments |
| Fixed/generic overload precedence | Native metadata priority | Reimplemented rank/concrete count | Canonical dispatch-slot collisions and shared metadata have independent regressions; broader ambiguous-overload testing remains useful |
| Adding overloads across modules | Native signature-level registration | Disjoint overloads merge; overwrite replaces matching dispatch slot | Startup-only transaction; return/constant-only differences do not create a separate slot |
| Determinism metadata | Declared native property | Per-overload property plus conservative name metadata | Covered selected resolution/coercion paths; author must honor declaration despite mutable guest state |
| callNullable | Native nullable argument pointers | Option inputs/outputs | Covered common semantics; no separate ordinary-call fast path for nullable functions |
| only callNullFree | Recursive NULL skips call | NullFree views and generated skip | Covered standard nested values |
| call plus callNullFree | Native loop selection | Macro alternate with batch metadata/per-row fallback | Covered dispatch; different execution cost |
| callAscii | Native batch encoding metadata | Host cached batch flag transported through IPC; SDK selects once per batch | Aligned batch dispatch, including NULL/nested-input rules; older-host fallback scans once |
| ASCII result propagation | Native default ASCII behavior hooks | Host validates actual output bytes and caches encoding | Implemented on apply result; no trust in guest preservation claims, and expression wrappers retain independent caches |
| One-time initialization and constants | Native object state and typed constants | InitContext and per-expression guest state | Covered; lifetime/error policy differs for opaque constants |
| Variadic plus initialization | This native checkout explicitly rejects it | Supported by WASM SDK | WASM exceeds this native authoring restriction; do not claim native supports it here |
| QueryConfig | Full typed accessors/default transformations | Declared config keys as strings/default strings | Partial; guest must reproduce parsing/clamping; no automatic accessor equivalence |
| User row errors and Status | EvalCtx and native exceptions/status | Result/error column/RowError | Valid row errors are recoverable; traps/protocol/bridge failures are fatal and invalidate Store |
| Generic compare/equality/hash | Native vector/type semantics | Borrowed recursive traversal and codec callbacks | Exact selected native semantics; no eager value trees for Generic, MAP sorting/header allocations remain; algorithms still require source-version maintenance |
| Custom comparison/hash | Native custom type implementation | Explicit CodecSemantics by codec ID/version | Partial bridge; no automatic delegation/translation of arbitrary native comparator code |
| OPAQUE value serialization | Native serde registry/object access | CustomValue codec reconstructs object | Covered transport; pointer identity not promised |
| OPAQUE identity | Native shared_ptr | Validated invocation-scoped handles | Covered pass-through; no methods or retained query-lifetime guest handles |
| Selected rows and conditional writes | Native adapter plus expression engine | Host gather/scatter | Selected tests pass; guest outputs validated before import |
| Dictionary/constant encodings | Expr peeling/memoization plus encoding-specialized adapter | Expr can still peel/memoize; adapter flattens remaining encodings | Partial optimization; do not say WASM loses all engine-level dictionary optimization |
| Adaptive/flat-null-free loops and SIMD | Native adapter specialization/compiler | Guest Rust loops, limited macro dispatch, simd128-enabled ABI | No native adapter fast-path parity; enabling SIMD does not prove user loops vectorize |
| Demand-driven nested access | Native reader/view behavior varies; generic readers can delay access | Guest view get is on access; whole IPC input already read | Partial; no guest-triggered host field/row loading or projection protocol |
| Lazy output codec decode | Native values/readers and LazyDereference | Nested scalar codec columns retain owned payloads until loaded | Implemented per column; first load decodes the whole column, native readers may force it; MAP keys/handles/top-level scalar/aggregate stay eager |
| String input-buffer reuse | Native reuse_strings_from_arg and writer support | Some host-owned buffer sharing; bytes cross IPC | No equivalent cross-boundary reuse of arbitrary existing buffers |
| MemoryPool access/accounting | Native pool and initialization overload | Guest allocator, no pool pointer/import | Full configured linear capacity reserved plus IPC/Arrow charging; no exact JIT/table/internal-allocation accounting |
| Host services/callbacks | Native code can call process APIs | Typed aggregate lambda imports only; no WASI | Query-bound evaluator implemented; general service/object methods remain work |
| Cancellation while executing | Query/engine mechanisms around native work | Fuel plus logical deadline and Driver task-token epoch checks | Implemented interruption; no real-time guarantee or query-wide CPU fuel budget |
| Function owner/canonical name/help metadata | Native metadata facilities | Canonical names and internal owner stamp; no author help text | Help/user ownership metadata remains an authoring extension |
| Lambda parameters | Native SFI does not provide general SQL lambda support | Not provided | Shared SFI limitation, not a unique WASM regression |
| Author controls vector encoding or zero-copy map_keys | Native VectorFunction can; SFI author cannot generally do so | Row SDK cannot do so | Compare with SFI and VectorFunction separately |

Native lazy-vector support does not mean every typed native reader loads only
the fields a function uses. A future demand-decoding benchmark must measure the
actual native reader and loader calls for the same function. Guest view access,
Arrow batch parsing, host loading and output materialization are four separate
layers; current view laziness does not make the whole path lazy.

The standard comparison/hash implementation is checked against this x86-64
checkout. It must remain synchronized with native semantics, and needs broader
property-based testing and supported-platform validation before any portable
exact-hash promise. Porting a custom comparator is the codec author's explicit
responsibility, even if the native signature binder classifies that type as
comparable.

## Fundamental boundary and implementable gaps

The guest cannot directly dereference arbitrary host pointers, StringView
addresses, MemoryPool objects, shared_ptr objects, C++ vtables or exception
objects. wasm32 pointers name guest linear memory. These are fundamental
boundaries of the selected isolated ABI, not reasons to drop SQL generics,
variadic arguments, nested values, initialization or custom semantics.

Handles/imports/codecs can provide corresponding operations without exposing
the native object ABI. Retained query handles, object methods, memory charging,
field projection, demand loading, alternate transport and
query-wide CPU budgeting are additional implementation work. Memory64 or another
return ABI can change address-size/encoding constraints, but cannot make a raw
host C++ pointer a valid guest object. The mandatory v128/SIMD return and
current 32-bit offsets are design choices; other WASM ABIs are possible.

SFI itself hides encodings and does not support arbitrary vector-encoding
control or general SQL lambdas. The paper's section 5 discusses these native
limitations. They should not be presented as deficiencies unique to WASM.
[Simple (yet Efficient) Function Authoring for Vectorized Engines](https://www.vldb.org/pvldb/vol17/p4187-pedreira.pdf)
provides the authoring model, while concrete compatibility judgments here use
the local native source. [Native scalar documentation](https://facebookincubator.github.io/velox/develop/scalar-functions.html)
provides the public interface overview.

## Performance evidence and limits

Historical Release measurements from 2026-10-02, 1024 rows/batch, median of
three independent processes:

| Case | Native microseconds/batch | WASM microseconds/batch | WASM/native time | Native API |
| --- | ---: | ---: | ---: | --- |
| BIGINT add | 0.901 | 66.108 | 73.41x | Checked Simple Function; guest example has a different overflow contract |
| VARCHAR concat | 10.628 | 511.132 | 48.09x | Built-in VectorFunction |
| ARRAY sum, 16 BIGINT elements/row | 4.466 | 366.242 | 82.01x | Explicit native Simple Function registration |

The saved twelve-process experiment covers 128/1024/4096/8192 rows and includes
IPC, guest fuel-metered execution and conversion. The 8192-row ARRAY case
completes under the default 100-million fuel policy. It excludes registration,
compilation, initialization, input creation, warmup, correctness checks and
final destruction. Per-eval allocations/replacement remain timed. These are
steady-state per-batch costs, not complete-query, startup, concurrency, memory
or cancellation measurements. The results support substantial overhead for
simple functions; they do not predict the relative cost for compute-heavy UDFs.

## Original proposed RFC scope and PR sequence

The initial RFC can propose a scalar-only experimental facility, explicitly
listing supported logical types and the costs and isolation obligations.
Aggregation can be a separate proposal because teardown, memory arbitration,
spill, intermediate compatibility and legacy type transport introduce another
set of contracts. If aggregate execution remains in the first PR, R2 and its
full operator tests are release gates.

Recommended review sequence:

1. Correct and consolidate the RFC/design contracts. Decide threat model,
   registration transaction/collision policy, resource ownership, fatal error
   classes, wire evolution and aggregate scope.
2. Fix native shared registry metadata behavior with independent expression
   tests, or document how to avoid that shared API change in the initial PR.
3. Submit runtime/ABI/registration and scalar standard-value transport with
   hostile guest/output tests. Include R1/R3/R4/R5 fixes and resource policy.
4. Add nested views/writers, generic/variadic/initialization coverage and the
   capability matrix; add custom/OPAQUE bridges with their explicit contracts.
5. Add aggregate lifecycle/spill/accounting as separately reviewable work,
   including R2 and full operator tests.
6. Add accurately labeled benchmark baselines and reproducible measurement
   instructions after semantics and ABI are stable.

Compare the eventual PR against an agreed upstream base, not only `git diff
HEAD`: the prototype commit already contains the original feature. The Java
files removed in the working tree were introduced by that prototype and are
absent from its native parent, so they should disappear from the final feature
diff rather than appear as unrelated upstream deletions. Ensure all new bridge
and Rust source files are included; ignored local dependencies and measurement
artifacts are not a portable dependency/build story.

Before calling the implementation ready, run the full targets and shared
expression regressions, malformed ABI/IPC fuzz and sanitizer tests, cleanup
fault injection, and full query cancellation/aggregation/spill tests for the
claimed scope. The existing normal tests and timing runs remain useful evidence
but do not satisfy those gates.

Runtime background: [Wasmtime security](https://docs.wasmtime.dev/security.html)
and [execution interruption](https://docs.wasmtime.dev/examples-interrupting-wasm.html).
The implementation checks use the installed Wasmtime 39 C API; current online
documentation is background, not a claim that a newer API is used here.

## Guest output and UDAF hot paths: 2026-10-05

The SDK now reuses buffers for contiguous borrowed generic outputs with exact
wire types and gathers other borrowed outputs by source ranges. NULL/error
rows, nested child NULLs and representation conversion retain their contracts.
Row UDAF single-group updates/merges borrow State once per batch; grouped updates
borrow once per contiguous run after validating all handles. Neither change
removes host/guest IPC copies or implements native allocator-backed State.

Validation passes **53 Rust tests**, **37 unchanged full GTests**, **7 CTest
entries across two configurations**, and **31 benchmark correctness cases**.
Fresh before/after Release timing profiles each use three independent processes,
1,024 rows, 100 warmups and 100M fuel/export. Row variadic SUM is
108.870 → 93.890 us/batch (13.76% less time); native SUM is 1.780 → 1.760 us and
legacy WASM SUM is 16.760 → 17.250 us. The output-codec generic ROW diagnostics
decrease by 48.98–50.85%. Their prepared input gather remains outside timing;
the SUM fixture times one retained group's update and extraction. No full-query,
grouped/spill/window, peak-memory or general cost-parity claim follows.

See the [scalar parity follow-up](wasm-scalar-sfi-parity.md) and
[UDAF parity follow-up](wasm-udaf-parity.md),
`.deps/alignment-hotpath-2026-10-05/` and the raw
`benchmark_results/wasm-hotpath-2026-10-05-{before,after}/` profiles. The remaining
implementation and hardening gates above still apply before community readiness.

## Native aggregate Status: 2026-10-05

The row aggregate SDK now declares ABI 2 and preserves RowError through create,
update, merge, serialize/finalize and temporary toIntermediate operations. ABI 2
retains the call signatures and logical row profile, adding tagged Status lanes;
ABI 1 declarations remain supported and ABI 1-only hosts reject new modules.
Native UserError becomes VeloxUserError; the other ten non-OK Status codes become
VeloxRuntimeError, matching EvalCtx::setStatus. Every failed export invalidates
Store. Legacy error messages remain fatal and marker-like ordinary errors cannot
forge a typed Status. Scalar whole-export failures retain fatal TRY behavior.

Validation passes **55 Rust tests**, **40 full GTests**, **7 CTest entries across
two build configurations**, and **31 benchmark correctness cases**. Tests include
all codes through every runtime call shape, invalid tags/reserved bits, ABI
version/profile validation, scalar TRY and grouped/single raw/merge HUGEINT
overflow followed by rejected state extraction/update. Exact records are in
`.deps/alignment-status-2026-10-05/`. Diagnostic timings under
`benchmark_results/wasm-status-2026-10-05-validated/` have wide sample spread and
substantial movement in unchanged native/legacy controls, so they do not prove
an error-ABI performance effect. Companion registration and remaining resource,
callback, optimizer and production validation obligations remain open.


## Native companion registration: 2026-10-05

Aggregate registration now includes native partial, merge, merge-extract and
extract families in the module transaction. Native signature generation,
return-type suffixes and adapters are reused; constant inputs are forwarded by
the aggregate adapter. Refreshing source overloads removes obsolete owned
aliases without deleting scalar homonyms. Generated-name collisions reject the
whole module. Declared registration counts and original window aliases retain
their contracts; companion aggregates have no window aliases.

Native extract propagates whole-call UserError even under TRY. The WASM wrapper
preserves that behavior and releases an extraction instance after any failure,
so an API retry cannot reuse failed guest state. Tests exercise overflow then
successful retries through the same expression, generic nested intermediates,
variadic constants, all companion stages, collisions and overload refresh.
Prepared native entries are also compared against normal native registration.

Validation passes **55 Rust tests**, **54 full GTests** (14 scalar, 9 runtime,
15 aggregate, 4 native metadata, 12 native companion), **8 CTest entries across
two configurations**, and **31 benchmark correctness cases**. The example module
now has 58 scalar and 11 aggregate declarations. Exact logs and hashes are under
`.deps/alignment-companion-2026-10-05/`; intermediate failed attempts are retained
with their corrected causes in the evidence record. No additional performance
claim follows from this registration stage. Earlier sections describe their
historical stage; companion registration and typed aggregate Status are now
implemented. Remaining memory/compaction, callback, optimizer and release
hardening obligations still prevent an overall community-readiness claim.


## Retained aggregate State accounting: 2026-10-05

Generated row UDAFs now declare ABI 3. AggregateState reports reserved owned
capacity recursively, with a derive for structs and explicit policy for
custom/shared allocators. Reports piggyback on create/update/merge and prefix
serialize/finalize IPC, including interior-cache growth. The host validates the
entire report before updating native per-group row-size counters and preserves
other keys/aggregates. Cleanup removes this aggregate's contribution even after
Store failure. ABI 1/2 payloads remain compatible; ABI 3 requires row aggregates
and compact. Legacy prebuilt modules do not acquire new size semantics.

Compaction shrinks guest containers and updates row sizes without changing SQL
state. It does not shrink linear pages or release the capacity reservation:
native compact returns zero actual freed bytes so reclaim can continue to spill.
This earlier stage provided only logical compaction. The current owned-State
checkpoint section below adds physical Store rebuilding. The Store capacity charge
bounds guest memory even if an author's footprint policy underreports; State
bookkeeping and fragmentation are not falsely assigned to a particular group.
Native companions forward compaction, and extract initializes temporary flags
and row-size storage before use.

Validation passes **60 Rust tests**, **56 full GTests**, **8 CTest entries across
two configurations**, and **31 benchmark correctness cases**. Tests cover
reserved nested capacity, growth/shrink, shared counters, multiple/skipped groups,
merge, unchanged extraction/cleanup, mutable finalizer caches, repeated handles,
malformed/duplicate/excess reports and failed-Store disposal. The example module
has 58 scalar and 12 aggregate declarations. Logs and hashes are under
`.deps/alignment-state-2026-10-05/`. These selected checks do not complete the
remaining callback, optimizer, resource-reclamation and production-hardening gates.

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


## Accessible linear memory and retirement: 2026-10-05

The runtime now uses Wasmtime 39 host-memory callbacks with guarded POSIX mappings.
It honors the virtual reservation/guard contract, commits only current pages,
keeps the pointer stable and charges native MemoryPool before initial commitment
or growth. Growth keeps its original owner across Driver threads. Callbacks never
unwind C++ through Rust; native allocation failures are retained and raised on
return even if the guest handles memory.grow's -1. Constructor/allocator/export/
free paths check them. Shared/threaded memory is disabled so it cannot bypass
accounting through a separate runtime path. Current platform verification is Linux.

Successful last-group retirement unmaps Store memory and frees its query charge;
native spill/flush can then initialize a fresh Store from saved intermediates.
The native function adapter is reusable, but no longer retains the empty Store.
Failed Stores remain invalidated. At this earlier stage compact with active
groups returned zero actual freed bytes. The current checkpoint section below
adds owned-State rebuilding; streaming/segmented optimization and exact non-linear
resource accounting remain implementation obligations. The charge covers accessible pages, not measured RSS.

Validation passes **62 full GTests**, **8 CTest entries across two configurations**
and **31 benchmark correctness cases**, plus a fresh run of **60 Rust tests**.
The Rust implementation is unchanged from the state-accounting stage. New checks cover growth and pool attribution on another
thread, late attachment, ignored native-cap failures during start/allocate/export/
free, zero pages, guard faults, idle release and exact intermediate restoration.
Logs, source/library/binary hashes and commands are under
`.deps/alignment-linear-memory-2026-10-05/`. This is progress toward the original
resource goal, not completion of production portability/fuzz/sanitizer/callback/
optimizer or active-group compaction gates.

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


## Owned-State checkpoints and active Store rebuilding: 2026-10-05

Active row UDAFs can now opt into complete owned-State checkpointing with
`checkpoint = true` and derive/implement `AggregateCheckpoint`. This is an
additive ABI-3 capability: paired exports plus `state_checkpoint = "owned-v1"`.
It leaves existing exports/payloads intact. Full State, private update factors,
caches, handle IDs and the registry high-water mark survive; SQL intermediate
is a separate distributed-stage contract. All five row examples enable it.

The bridge migrates every live group even when native compact receives a subset.
A query-owned checkpoint is charged before copying and validated before discarding
old pages; restore initializes the same context without recreating group State.
All size reports are validated before publishing native counters. NULL flags and
other key/aggregate size contributions survive. Malformed snapshots/reports or
failed exports abort/poison State and leave cleanup safe. Actual reclaimed-page
bytes, measured against compact entry, are returned instead of logical sizes.

This implementation uses fragmentation/workspace heuristics and whole-Store
serialization. Insufficient spare capacity returns zero, leaving native free to
spill. Actual native allocation, Store, fuel and cancellation limits still apply.
Streaming/segmented migration, exact non-linear charging and native optimization
paths remain work; the feature does not claim zero-workspace reclaim or measured
RSS reductions. Authors must explicitly promise that future behavior can be
reconstructed from owned State; mutable globals, shared graphs, raw guest pointers
and invocation handles require an appropriate ownership/reconstruction design.

New deterministic tests cover full migration from one selected group, generic
nested and first-NULL states, variadic configuration omitted from intermediate,
continued updates/merge, handle high-water marks, shared counters, decode rollback,
malformed framing/reports, traps, native snapshot allocation failure and a
zero-headroom skip. Real Rust variable-state regressions reclaim more than 1 MiB
while other groups remain live. Logs and exact hashes are under
`.deps/alignment-checkpoint-2026-10-05/`; details and release gaps are in
[UDAF alignment](wasm-udaf-parity.md#full-owned-state-checkpoints-and-active-store-reclamation-2026-10-05).

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

## Current SQL UDAF lambda stage (2026-10-05)

This supersedes the callback/import gap in the earlier stage reports. Row ABI 3
now accepts declared trailing FUNCTION signatures through a fixed typed import
allowlist. `InitContext::lambda` / `Lambda::call` pass row values, preserve native
evaluator exception categories, and retain only symbolic positions in State.
Query-bound callback authority is reinstated on active Store rebuilding. The
native partial companion adapter forwards lambda expressions. Generics T/S,
nested input/state, grouped/single/all native stages, forced spill and active
checkpoint continuation have query tests. Earlier all-import-rejecting hosts
cannot load a module containing this new capability.

The protocol holds one charged response with a single-use Store-local token.
Runtime tests cover replay, missing consumption, malformed import types, module
start authority, forged ranges/length, quotas, cancellation and query pool OOM.
Callbacks never unwind a C++ exception through Wasmtime. Synchronous native
execution retains native cancellation semantics, with deadline checks before
and after the callback.

Validation: 77 Rust tests and 78 full GTests; all six full and three minimal
CTest targets passed. Benchmark correctness covers 33 cases. Exact sources,
artifacts and logs are in `.deps/alignment-lambda-2026-10-05/evidence.json`.
Three independent full-build timing processes (1024 rows, 100 untimed warmups,
100 million fuel) measured medians of 22.10 us native reduce and 17.48 ms Wasm
reduce, a **790.95x** gap; row SUM measured 55.74 us against 1.77 us native SUM.
The timed operation updates an existing group and extracts once. Builds/tests,
JIT, initialization, input allocation and destruction are excluded. Raw commands
and runs are in `benchmark_results/wasm-lambda-2026-10-05/`.

This establishes callback functionality, not throughput parity or community
readiness. Per-row IPC/callback evaluation must be replaced or supplemented by
batched callbacks and adaptive updates. Standalone lambda-bearing merge/extract
companions need an explicit expression-carrying planner/API contract; a typed
intermediate alone cannot reconstruct lambdas. Native aggregate window argument
conversion also has no lambda channel. These are implementation/engine API
obligations, not fundamental Wasm expressiveness limits. General services,
retained host-object methods, exact non-linear runtime charging, broader native
optimization and production hardening remain open.

## Current batched UDAF lambda stage (2026-10-05)

This supersedes the per-row callback obligation immediately above. The row SDK
now supports bounded `Lambda::call_batch` with Value columns/broadcast and
optional `update_batch` / `merge_batch` hooks consuming row/view iterators.
Existing authors retain the default prepared-reader path. All handles are
validated before mutation; interleaved rows are grouped stably and only consumed
arguments are decoded. Bulk input failures cannot forge author typed statuses.
`ReduceWasm` maps from the initial state and combines pairs/chunks with existing
State; raw input after merge tracks initial validation separately from `seen`.

The native factory's phantom capture columns are also fixed: lambda signatures
contain formal parameters, which shadow identically named SQL input columns.
Native/Wasm regressions cover hash, streaming, stages and `_partial`; the native
ReduceAgg suite and its argument-extraction utility are validated. Real outer
column captures still require a value/stage contract, and native ReduceAgg also
evaluates against formal-parameter ROWs. FUNCTION plus a variadic tail needs
manifest layout and fixed-count SQL lambda resolver changes. These are shared
API/implementation obligations, not fundamental Wasm restrictions.

Validation: **84 Rust tests, 83 full GTests, 12 native ReduceAgg tests**, all six
full and three minimal CTest targets, and 33 benchmark correctness cases per
before/after guest pass. Both configurations produce the same guest hash.
Final sources, artifacts, commands and passing/diagnostic log classification are
in `.deps/alignment-lambda-batch-2026-10-05/evidence.json`.

Six independent Release processes alternate before/after/after/before/before/
after on the same final host, with 1024 rows, 100 untimed warmups and 100M fuel.
Reduce medians change **16.48 ms → 746.73 us (22.07x faster)**; native controls
change 22.87 → 23.68 us. The final guest remains **31.53x native**; normalized
gap reduction is 95.62%. Ordinary row SUM changes 55.70 → 60.97 us (+9.46%),
while native SUM changes 1.92 → 1.73 us (-9.90%); this requires further performance
investigation rather than a speedup claim. Raw samples/commands/final hashes are
in `benchmark_results/wasm-lambda-batch-2026-10-05-paired/`. Preliminary logs use
a separately archived intermediate guest and are not the final evidence.

Only update of an existing single group and one extraction are timed; JIT,
initialization, creation/input allocation/destruction are excluded. IPC, fuel and
callback evaluation are included. This establishes no complete-query,
grouped/spill/window/checkpoint or peak-memory improvement. Transport/fast paths,
expression-carrying companions, captures/window lambdas, exact non-linear
accounting, zero-spare-capacity reclaim and production/platform validation remain
open before claiming community readiness.


## Current lambda/variadic UDAF stage (2026-10-05)

This supersedes the FUNCTION/variadic gap in the preceding stage. The manifest
places FUNCTIONs between fixed values and the final variadic value tail. Rust
row tuples and constant indices omit functions. Native signature binding keeps
processing known suffix arguments after an unresolved lambda, while the SQL
resolver recognizes zero/expanded tails and resolves T from values after
FUNCTION. Uninferable empty generic tails produce a user type-inference error;
fixed T prefixes and concrete lambda types support empty tails.

Four authoring examples cover fixed plus variadic T, constant T tails, T bound
only after FUNCTION, and heterogeneous `any`. Query tests compare actual native
SUM projections across single/grouped/staged execution, NULL/constant/nested
inputs and bounded callback chunks. A validity-only heterogeneous tail does
not decode payloads. Independent lambda-bearing companions are not universally
blocked: concrete/resolvable FUNCTION types with no callbacks during merge or
finalize pass both `_partial` → `_merge_extract` and `_partial` → `_merge` →
`_extract`. Callback-dependent companions need expression authority; erased
lambda generic types also remain a direct-factory binding gap.

Final validation passes 84 Rust tests, 88 full alignment GTests, 1,642 native
expression tests and 12 native ReduceAgg tests, plus all six full/three minimal
CTest targets. Three existing native expression tests remain disabled; metadata
coverage overlaps the native suite. The final guest contains 58 scalar and 17
aggregate declarations and is identical in the two configurations. Evidence
and passing/diagnostic log classifications are in
`.deps/alignment-lambda-variadic-2026-10-05/evidence.json`.

Six CPU-15-pinned Release processes alternate baseline/current guests on the
same final host, with 1024 rows, 100 untimed warmups and 100M fuel. The baseline
already contains batch lambdas. All 33 correctness cases pass for both guests.
Final row SUM is **55.23 us versus 1.73 us native (31.92x)**; final lambda reduce
is **731.69 us versus 21.19 us native (34.53x)**. Guest changes are +2.26% and
-3.29%, respectively, with overlapping samples; no capability-related speedup
claim follows. Raw evidence is in
`benchmark_results/wasm-lambda-variadic-2026-10-05-paired/`.

Only update of an existing single group plus one extraction is timed. IPC/fuel/
callbacks are included; JIT, initialization, creation/input allocation and
destruction are excluded. No builds/tests overlap timing. This does not measure
old/new hosts, the new examples or whole-query costs. Performance transport,
callback-dependent standalone companions, real captures/window lambda channels,
exact non-linear accounting, zero-spare-capacity reclaim and broader hardening
remain implementation/API work before community readiness. The capability
matrix and SDK examples describe the current contracts; historical stage claims
above are retained with their original evidence.


## Current erased-lambda-type companion stage (2026-10-05)

This supersedes the callback-free erased generic type gap above. Independent
merge/extract preserves input-only unresolvable FUNCTION positions with explicit
`types_bound=false` metadata and no invented type descriptor. Required result/
intermediate types remain bound under native rules, and raw construction still
requires complete FUNCTION types. Rust `Lambda::has_bound_types()` exposes the
slot; evaluation/row-limit access fails before request encoding. Checkpointed
symbolic positions retain those checks. Malformed/contradictory metadata is
rejected. Typed metadata is compatible with older SDKs; unbound descriptors fail
closed there. A typed slot still requires native expression/evaluator authority.

Query regressions cover grouped/global merge of multiple BIGINT/ARRAY partials
with erased input T and both independent companion pipelines. Direct reduce
extraction now works with an unused erased input lambda. Combining multiple
nonempty reduce partials without a combine expression still fails explicitly.
The remaining standalone callback gap concerns expression authority, not merely
irrelevant input-type binding.

86 Rust tests and 91 full alignment GTests pass, along with all six full/three
minimal CTest entries and 33 benchmark correctness cases for each guest. Both
build configurations emit identical modules with 58 scalar/17 aggregate
entries. Unchanged native expression/ReduceAgg validation is retained from the
previous stage. Evidence is in
`.deps/alignment-unbound-lambda-2026-10-05/evidence.json`.

Three initial process pairs suggested a reduce regression, but included broad
jitter and a native-control outlier. Nine additional pairs alternate order on
CPU 15. All 12 samples per guest are retained: row SUM medians are
54.105 → 53.945 us and reduce medians 753.125 → 750.835 us, both about -0.30%.
Exploratory paired bootstrap 95% intervals are [-5.64%, +8.21%] and
[-5.63%, +4.67%], respectively, so no speedup/verified regression is established.
Current native medians are 1.765 us SUM and 21.930 us reduce; Wasm gaps remain
30.56x/34.24x. Raw cohorts, methods and hashes accompany the evidence. Only an
existing single group's update/extraction is timed, with IPC/fuel/callbacks
included and JIT/init/creation/input allocation/destruction excluded. No builds/
tests overlap timings. Independent companion and full-query costs are not
measured here; performance/hardening obligations remain open.


## Current independent merge constant stage (2026-10-05)

A real typed ROW intermediate constant was incorrectly exposed as raw argument
0, causing a variadic aggregate's create to read ROW as its BIGINT factor and
fail. Direct intermediate factory bindings now clear raw configuration constants
and carry a separate intermediate-constant marker. Rust exposes factory input
type binding through `aggregate_input_types()` and the hint through
`intermediate_constant_value()`, including constant SQL NULL. This is not the
operator step; ordinary native staged factories can retain raw type binding.
Merge still consumes each selected row, so initialization must not seed its
accumulated total from the hint. Shared native scalar extract does not forward
constants to the aggregate factory. Intermediate-only instances reject raw
updates and do not advertise raw toIntermediate. Raw constant behavior and
older-SDK mask compatibility are preserved; malformed/conflicting new metadata
is rejected.

Typed native query tests cover three UDAFs across global/grouped constant
`_merge_extract`, `_merge` plus `_extract`, and scalar `_extract`, with raw
configuration not applied twice. The test parser cannot represent HUGEINT
constants through SQL correctly, so typed native plan nodes exercise the actual
engine contract. SDK NULL/mode/metadata cases and direct factory boundary tests
also pass. Validation totals are 88 Rust tests, 92 full alignment GTests, six
full/three minimal CTest targets and 33 correctness cases per benchmark guest.
Full/minimal guest hashes match; unchanged native expression/ReduceAgg evidence
is retained. Exact sources/binaries/log classifications are in
`.deps/alignment-merge-constants-2026-10-05/evidence.json`.

Nine paired processes per guest alternate order on CPU 15, one final Release
host, 1024 rows, 100 warmups and 100M fuel. Row SUM changes 54.18 → 54.15 us;
reduce changes 754.42 → 751.27 us. Exploratory paired-bootstrap 95% intervals
are [-3.29%, +5.23%] and [-5.81%, +3.59%], so no speedup/verified regression is
established. Native controls after are 1.72/21.12 us; gaps remain 31.48x/35.57x.
All samples are retained in
`benchmark_results/wasm-merge-constants-2026-10-05-paired/`. Only an existing
single group's update/extraction is timed, including IPC/fuel/callbacks;
JIT/init/creation/input allocation/destruction are excluded. Builds/tests do not
overlap timings. Whole-query, companion and platform costs remain unmeasured;
performance/hardening obligations remain open.


## Current anonymous intermediate ROW stage (2026-10-05)

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

## Current complex MAP key stage (2026-10-05)

The bridge/SDK now preserves non-NULL complex MAP keys containing NULL children,
matching native `map_from_entries` and Simple Function MapView comparisons.
The Presto `map()` constructor's stricter indeterminate-key policy is separate.
Scalar identity/materialization/equality/hash/lookup and owned UDAF states through
partial/intermediate/final and merge/extract companions have native regressions.
The SDK README provides a generic `map_lookup` view example.

Untrusted MAP IPC is checked using its equivalent LIST<STRUCT> layout before
Arrow MAP array construction can abort on NULL key/entry data. Schema validation
precedes constructors; restoration shares buffers, and trailing bytes fail
closed. Real guest regressions verify protocol-fatal failure and Store
invalidation. The retained fixture-export abort log is diagnostic, not a
demonstrated pre-fix guest exploit or a claim of exhaustive Arrow fuzz safety.

Validation: **89 Rust tests**, **96 full alignment GTests**, **six full/three
minimal CTests** and **35 current benchmark correctness cases** pass. Matching
full/minimal guests contain 61 scalar/18 aggregate exports. Retained native
expression/Reduce validation has unchanged source/binary hashes, not new runs.

With identical host, Release/SIMD compiler flags and all other Rust/Cargo files,
nine alternating paired processes isolate borrowed `MapView::find` versus
legacy materialization. Complex-key last lookup drops **35.87 → 15.88 ms** per
1024-row batch (-55.73%, exploratory interval [-64.32%, -44.61%]); current cost
remains **48.55x native**. Both timing sides use 1B fuel because legacy find
exhausts 100M even with matching SIMD; current default correctness passes at
100M. The initial non-SIMD control cohort is preserved as diagnostic only.
Ordinary UDAF Row SUM is 53.78 → 53.71 us and reduce 734.37 → 760.48 us at
100M fuel; their intervals cross zero, establishing neither improvement nor
regression. These are separate single-machine workloads, not whole-query or
complete native parity evidence.

[The UDAF assessment](wasm-udaf-parity.md) and
[scalar assessment](wasm-scalar-sfi-parity.md) contain the contracts, fixture,
controls, intervals and remaining obligations. Raw commands/samples are in
`benchmark_results/wasm-map-keys-2026-10-05-paired/` and
`benchmark_results/wasm-map-lookup-2026-10-05-paired/`; source/artifact hashes and
compiler controls are in `.deps/alignment-map-keys-2026-10-05/evidence.json`.
Transport/native fast paths, callback-dependent companions/captures/window
channels, exact runtime accounting, retained host services, streaming reclamation
and production hardening remain implementation work. Community readiness is
still unestablished.

## Current encoded-leaf gather stage (2026-10-05)

Fully selected ordinary nested inputs now export directly when only scalar
leaves have Arrow-supported dictionary/constant encodings. Arrow flattens those
leaves while retaining plain sibling buffers, avoiding the old whole-value copy.
Selection indices are generated on demand. Encoded complex/UNKNOWN values,
sparse inputs and unloaded lazy bases retain native gathering; the capability
check does not load a lazy base. There is no ABI or Arrow core change.

The pre-change ownership assertion reproduces unnecessary sibling copying.
Current ownership/lifetime/value regressions cover nested ARRAY/MAP and NULLs,
unchanged caller children, root dictionary/constant, full/sparse/empty selection,
UNKNOWN and selective LazyVector loading of only the two referenced base rows.
All six full/three minimal CTests and 96 full alignment GTests pass; Rust's
89-test result is retained with unchanged Rust/Cargo hashes, not rerun. Guest
bytes remain unchanged in both builds. Both old/current hosts pass 38 benchmark
correctness cases, including three new host-only transport diagnostics.

Nine paired processes show mixed dictionary/constant-leaf ROW gather plus IPC
serialization drops about **56%** for the 1024-row, 2 MiB payload fixture.
This diagnostic executes no guest and is not a native SFI/query speedup claim.
Ordinary UDAF timing uses a separate existing-group fixture and unchanged 100M
fuel. Its reduce raw positive difference prompted nine additional pairs.
Combined eighteen-pair medians are SUM **54.055 → 54.405 us** and reduce
**737.235 → 752.205 us**. Reduce raw change is +2.03%, interval
[+0.16%, +3.28%]; native-normalized interval [-1.20%, +2.99%] crosses zero.
The raw difference is retained and cause attribution/profiling remains open;
normalization establishes neither code regression nor its absence. No ordinary
UDAF speedup or complete performance parity is claimed.

[The scalar assessment](wasm-scalar-sfi-parity.md) and
[UDAF assessment](wasm-udaf-parity.md) contain method, controls, uncertainty
and remaining obligations. Final evidence is under
`.deps/alignment-gather-leaves-2026-10-05/`, with both paired measurement cohorts
in `benchmark_results/wasm-gather-leaves-2026-10-05-{paired,ordinary-paired}/`.
Community readiness and the complete thread objective remain open.

## Current primitive column stage (2026-10-05)

Profiling the earlier small reduce raw difference did not establish causal
attribution: two whole-process counter pairs include JIT/setup and frequency /
cache variation. Correctly filtered perfmap sampling identified
Generic::materialize and Vec<Value> construction as significant hot paths.
An initial filter error measured only initialization; those artifacts are
explicit diagnostics. Private perfmap instrumentation changes no production
source and its samples are not steady-state speedup measurements.

The SDK now builds seven primitive SQL column types directly from borrowed
leaves or owned scalars, removing intermediate owned Value vectors. It retains
NULL/type checks and float bit patterns, with no ABI/API/dependency change.
All **90 Rust tests**, **96 full alignment GTests**, **six full/three minimal
CTests** pass. Guest hashes match across builds with 61 scalar/18 aggregate
exports; all three variants pass 38 benchmark correctness cases.

Nine rotating/reversed triplets separate SDK optimization from the total
old-host/SDK comparison. On one unchanged current host, reduce drops
**759.81 → 652.27 us** (-14.15%, exploratory interval [-16.48%, -8.62%]).
Against the archived pre-gather host/old SDK, total reduce drops
**756.80 → 652.27 us** (-13.81%, [-17.34%, -9.77%]). Current reduce still costs
30.78x native. SUM and isolated SDK scalar ADD intervals cross zero. These
selected 1024-row, existing-group / flat-ADD fixtures retain 100M fuel and
native controls; they do not establish grouped/spill/window/whole-query parity
or explain the earlier small raw difference retrospectively.

[The UDAF assessment](wasm-udaf-parity.md) and
[scalar assessment](wasm-scalar-sfi-parity.md) contain the method/controls and
remaining obligations. Profiling is archived in
`.deps/alignment-reduce-profile-2026-10-05/`, final evidence in
`.deps/alignment-primitive-build-2026-10-05/`, and all triplets in
`benchmark_results/wasm-primitive-build-2026-10-05-paired/`. Full alignment and
community readiness remain unestablished.

## Current owned/borrowed comparison stage (2026-10-05)

Standard `Value::compare/equals` now compares owned aggregate State with borrowed
update/merge values directly. Scalars/bytes avoid materialization, ARRAY/ROW
short-circuit and MAP sorts indices without copying payload trees. Both operand
directions preserve flags, NULL/NaN semantics and DECIMAL constraints. Custom
codec identity/registered semantics and OPAQUE errors remain intact; mixed custom
payload materialization remains an SDK optimization opportunity. State still
owns values retained across invocations.

All **92 Rust tests**, **96 full alignment GTests** and **six full/three minimal
CTests** pass. Allocation instrumentation verifies zero allocations for mixed
standard nested comparisons, and malformed unread timestamp tails prove demand
decoding. Host/native binaries and ABI/API/dependencies are unchanged.

Ten alternating process pairs retain all samples and native controls. Reduce
medians are 731.155 → 698.055 us, but the exploratory interval crosses zero;
SUM's raw positive difference is also retained. Native samples vary sharply,
and a post-run snapshot shows unrelated high-load workers predating the cohort,
with the benchmark CPU in their affinity. This stage claims neither speedup nor
absence of regression; isolated performance validation remains open. The
[UDAF assessment](wasm-udaf-parity.md) records every control, interval and scope.
Evidence is in `.deps/alignment-mixed-compare-2026-10-05/` and raw logs in
`benchmark_results/wasm-mixed-compare-2026-10-05-paired/`. Complete alignment and
upstream readiness remain open.
