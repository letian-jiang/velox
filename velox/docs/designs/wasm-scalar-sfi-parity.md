# WASM scalar UDF and Velox Simple Function capability alignment

This branch implements row-oriented Rust scalar authoring over the WASM Arrow
IPC adapter. User code receives a scalar value or a borrowed nested view; batch
layout and encoding conversion remain adapter responsibilities. This follows
the authoring model described by [Simple (yet Efficient) Function Authoring for
Vectorized Engines](https://www.vldb.org/pvldb/vol17/p4187-pedreira.pdf),
PVLDB 17(12), 4187–4199, 2024, DOI 10.14778/3685800.3685836. The paper's abstract
and introduction motivate hiding vectorization while supporting nested types,
type variables, and variadic parameters. The concrete contracts below were
checked against this checkout's source, rather than inferred from the paper.

This is a capability assessment, not a claim that IPC matches native execution
cost or that arbitrary C++ objects can be exposed through the value ABI.

Aggregate capabilities have a separate [UDAF assessment](wasm-udaf-parity.md).
In particular, native aggregate lambda/evaluator hooks and intermediate/state
contracts differ from scalar SFI; scalar limitations do not describe UDAFs.

## Implementation and source map

| Native implementation | Rust SDK / WASM adapter |
| --- | --- |
| [UdfTypeResolver.h](../../expression/UdfTypeResolver.h), [ComplexViewTypes.h](../../expression/ComplexViewTypes.h) | [views.rs](../../functions/wasm/sdk/velox-wasm-sdk/src/views.rs): typed and dynamic views, checked casts, nullable children, iterators, materialization |
| [ComplexWriterTypes.h](../../expression/ComplexWriterTypes.h), [GenericWriter.cpp](../../expression/GenericWriter.cpp) | [writers.rs](../../functions/wasm/sdk/velox-wasm-sdk/src/writers.rs), [value.rs](../../functions/wasm/sdk/velox-wasm-sdk/src/value.rs): arbitrary nested construction and mutable child writers |
| [SimpleFunctionAdapter.h](../../expression/SimpleFunctionAdapter.h), [SimpleFunctionMetadata.h](../../core/SimpleFunctionMetadata.h) | [scalar.rs](../../functions/wasm/sdk/velox-wasm-sdk/src/scalar.rs), [macros](../../functions/wasm/sdk/velox-wasm-macros/src/lib.rs): row calls, null-free and ASCII alternatives, return values and row errors |
| [SimpleFunctionRegistry.cpp](../../expression/SimpleFunctionRegistry.cpp), [SignatureBinder.cpp](../../expression/SignatureBinder.cpp) | [Registration.cpp](../../functions/wasm/Registration.cpp), [Manifest.cpp](../../functions/wasm/Manifest.cpp): signature binding, constraints, constants and overload priority |
| [ComplexVector.cpp](../../vector/ComplexVector.cpp), [FloatingPointUtil.h](../../type/FloatingPointUtil.h), [BitUtil.cpp](../../common/base/BitUtil.cpp) | [comparison.rs](../../functions/wasm/sdk/velox-wasm-sdk/src/comparison.rs): standard value comparison and 64-bit hashing |
| [Timestamp.h](../../type/Timestamp.h), [Type.h](../../type/Type.h) | [ArrowIpc.cpp](../../functions/wasm/ArrowIpc.cpp): recursive full-range TIMESTAMP and full-width HUGEINT transport |

The SDK [README](../../functions/wasm/sdk/README.md) contains complete authoring
examples. The [native view/writer documentation](https://facebookincubator.github.io/velox/develop/view-and-writer-types.html)
is useful for the corresponding native interfaces.

## Capability matrix

“Implemented” describes the standard value domain: BOOLEAN, signed integers
through HUGEINT, REAL, DOUBLE, arbitrary-byte VARCHAR, VARBINARY, DATE, TIMESTAMP, DECIMAL,
UNKNOWN, ARRAY, MAP, ROW, and their recursive combinations. Custom logical values use explicit versioned codecs and optional portable
comparison/hash callbacks. OPAQUE values use codecs or invocation-scoped
object handles; guest access to arbitrary native object methods needs imports.

| Native Simple Function capability | Current scalar SDK | Alignment or qualification |
| --- | --- | --- |
| One-row authoring; vectorization hidden | `#[velox_scalar]` | Implemented; the adapter handles Arrow batches |
| Primitive values, borrowed strings/binary, nullable outputs | Rust scalars, `VarcharBytes`, `Option<T>`, `Result<T,E>` | Implemented; `str` validates UTF-8 per row, byte views preserve arbitrary VARCHAR; `Option` corresponds to native NULL output |
| Recursive typed ARRAY / MAP / ROW arguments | `ArrayView`, `MapView`, tuple `RowView` | Implemented, including empty/all-null values, offsets, nullable children and field access |
| Runtime generic casts and value access | `Generic::get`, `try_cast`, `as_array/map/row` | Implemented with errors for invalid casts instead of unchecked access |
| Iteration, lookup and materialization | Typed iterators; MAP `find/at/contains`; `materialize` | Implemented; MAP lookup distinguishes missing from NULL and is linear |
| Recursive result construction | `ArrayWriter`, `MapWriter`, `RowWriter`, `GenericWriter`, `Value` | Implemented; nested children can be owned values or borrowed Generic children |
| Writer capacity, growth and mutable child slots | `reserve`, `resize`, `add_item`, `get_mut`, ROW `get_writer_at` | Implemented; the writer builds guest-owned values and then Arrow arrays |
| Arbitrary ROW arity | `DynamicRow`, `Generic`, `Value::Row` | Implemented dynamically; typed Rust tuples have an SDK limit of 16 fields |
| Generics, Any, nested generic signatures | SQL variables bound by native `SignatureBinder` | Implemented; runtime `Generic` replaces C++ template authoring at the exported ABI |
| Comparable, Orderable, known-type constraints | Signature variables + comparator/hash helpers | Implemented for standard types and explicitly registered portable codec semantics |
| Concrete and generic overload priority | Native rank classes and concrete-node count | Implemented; count includes argument and result types |
| Per-overload determinism and NULL behavior | Signature-specific vector metadata | Implemented; bound resolution, coercion and execution select the same properties |
| Typed / heterogeneous variadic tails | Borrowed `VariadicView<T>`, `Generic` with `any`; owned `Variadic<T>` retained | Implemented, including zero tails, checked indexing, bidirectional iteration, NULL metadata checks and NULL skipping; borrowed tails convert only accessed arguments, with schema checks once per batch |
| Constant arguments and one-time initialization | `constant_arguments`, `initialize`, `InitContext` | Implemented, including nested constants, NULL constants and per-expression guest state |
| Query configuration | Explicit declared `config_keys` | Declared explicit values and registered property defaults are sent; custom accessor parsing/clamping remains guest logic |
| Short / long DECIMAL; integer precision expressions | `Decimal`, native precision constraints | Implemented through Decimal128; writer precision/scale and overflow checked per row |
| Complete native TIMESTAMP range and nanosecond precision | `Timestamp(i128)` plus marked seconds/nanos wire struct | Implemented in the scalar row adapter, including nested values and initialization |
| Complete signed HUGEINT range | Rust `i128` plus marked high/low wire struct | Implemented; this is distinct from DECIMAL(38,0) |
| NULL filtering and nullable calls | Host default-null metadata, Rust `Option` | Implemented; nested child NULL is separate from outer NULL |
| Only `callNullFree` | `NullFreeArray/Map/RowView` parameters | Implemented: recursive NULL inputs yield NULL without calling user code |
| Regular call plus `callNullFree` alternative | `call_null_free="path"` | Implemented; batch validity metadata avoids recursive scans for null-free batches, with per-row fallback for mixed inputs |
| Regular call plus `callAscii` alternative | `call_ascii="path"`, `has_ascii` metadata | Native cached input ASCII flags cross IPC; batch dispatch considers top-level VARCHAR, ignores NULL and nested strings, and falls back for non-ASCII/unknown flags |
| Generic equality / ordering / hashing | `CompareFlags`, `NullHandling`, `Generic::compare/equals/hash` | Implemented for standard types, directly checked against native vectors |
| Generic values as hash keys | Owned `SqlKey` for Rust HashMap/HashSet | Implemented with NULL/NaN SQL value equality and cached full 64-bit value hash |
| Row errors, structured Status and SQL TRY | `Result<T,E>`, `Result<T,RowError>` and result error column | Implemented; typed codes go through native `EvalCtx::setStatus`, including fatal system-status behavior |
| Selected rows, dictionaries and constants | Host gather/scatter and expression evaluation | Implemented semantically; guest sees flattened columns |
| Deferred output codecs | Owned payloads in native LazyVectors for nested scalar ROW/ARRAY/MAP values | Implemented per column; first load decodes all rows, native LazyDereference can skip unused fields; MAP keys/handles/top-level scalar/aggregate remain eager |
| Native buffer reuse | Host-owned IPC buffers, VARCHAR wire views and compact-result reuse | Implemented within the host; IPC still copies across host/guest address spaces |
| Custom and extension logical types | `WasmTypeCodec`, `CustomValue`, `CodecSemantics` | Implemented through explicit ID/version/type identity; portable compare/hash must reproduce native semantics |
| OPAQUE | Native serde codec or `OpaqueHandle` | Implemented value reconstruction or original-object roundtrip; handles are invocation-scoped and C++ type-checked |
| Host MemoryPool object and arbitrary callbacks | Isolated guest allocator; no general imports (typed UDAF lambda imports are separate) | C++ object ABI cannot be passed directly; mediated allocation/callbacks could be added |
| ASCII result metadata | Host scans actual VARCHAR output and caches ASCII state | Implemented on the result of apply; expression dictionary wrappers have their own cache, as with native vectors |
| Native adaptive loops | Batch NULL metadata checks, per-row fallback and IPC | Remaining optimizer differences, not a SQL expressiveness limitation |

## Nested values and SQL semantic details

Borrowed views are cheap handles into guest Arrow arrays. Reading a child
creates another view rather than eagerly copying a nested value. `materialize`
recursively owns the content; writer output validation and array construction
retain the resolved schema even for no rows, NULL-only rows, or empty children.
Dynamic ROW values have no artificial field-count limit beyond the selected ABI
and runtime resource bounds.

Array elements, map values, and row fields can be NULL. MAP keys must satisfy
the no-NULL/nested-NULL invariant used by Velox's map comparator; output
validation rejects violations as errors on their originating row. Key
uniqueness remains an author responsibility, as with native `MapWriter`.

The comparator implements both null-as-value and three-valued SQL equality.
A definite mismatching non-null child takes precedence over an earlier
indeterminate child; unequal ARRAY lengths are definitely unequal. NaN payloads
compare equal, NaN sorts above non-NaN numbers, and +0/-0 have equal hashes.
MAP keys are sorted before comparison and MAP hashing is insensitive to entry
order. SQL MAP ordering is rejected under null-as-indeterminate ordering, while
the native internal default comparison can still order maps. Hash algorithms
follow the current native source, including the byte-string hash; the test
compares exact hash bits on this x86-64 host. Generic comparison/hash traverse
borrowed Arrow views directly. ARRAY/ROW comparison stops at the first decisive
difference; hash necessarily visits every element. Standard ARRAY/ROW/string
operations with identical bound schemas allocate no value trees. MAP comparison
allocates sorted entry indices, not copied keys or values. Custom callbacks borrow
payload bytes but currently allocate small codec headers/registry lookup keys.
Owned Value operations avoid cloning their trees. Mixed borrowed/owned comparison
reads standard values directly and visits only reached nested children; mixed
custom codec comparison still materializes its payload. `sql_key()` deliberately owns its value
so it can outlive an invocation. These changes do not remove host gather/IPC work.

The TIMESTAMP protocol uses explicit field metadata, not just field names.
An ordinary SQL ROW named `(seconds,nanos)` remains a ROW. The same separation
applies to HUGEINT `(high,low)`. Full-width integer halves retain the entire
signed bit pattern, including values outside DECIMAL(38,0). Existing batch and
aggregate ABI v1 timestamp exports still have their old `i64` nanosecond range;
this change extends the scalar row protocol rather than silently changing old
modules. All newly generated scalar exports use the row protocol, including
primitive signatures. VARCHAR uses a separate marked one-field binary struct;
`VarcharBytes<&[u8]>` preserves every byte and `str` returns a row error for
invalid UTF-8. Generic hash/comparison operate on raw bytes. Ordinary ROWs with
matching field names are kept distinct from all three logical scalar markers.

## Fundamental boundary versus additional implementation

WASM does not prevent generics, variadic parameters, recursive values, decimal
precision binding, SQL comparison, initialization, or nullable calls. These
capabilities are implemented here. Rust exports cannot expose unconstrained
Rust type parameters in a fixed WASM function ABI; the host instead binds SQL
variables and passes runtime types. This is an authoring/ABI distinction, not
an inability to express a generic SQL function.

A sandboxed guest cannot dereference an arbitrary host `StringView`,
`MemoryPool*`, `std::shared_ptr<T>`, C++ vtable, or host function address. A pointer
inside guest linear memory denotes a different address space. Passing the raw
64-bit host address, enabling Memory64, or compiling Rust does not make those
objects usable. Native C++ exceptions and C++ object layout also cannot be used
as a cross-boundary ABI. **Transparent native object/pointer compatibility is
the fundamental limitation of the isolated ABI.**

Equivalent operations remain possible with explicit design:

- A custom logical value uses a versioned codec and preserves logical type
  identity; this implementation exposes host codec registration and portable
  guest comparison/hash registration. Arbitrary native comparator code is not
  automatically translated into WASM.
- An OPAQUE object stays in the host behind a validated invocation-scoped
  handle, or uses the native serialization registry and a guest codec. Result
  vectors retain shared ownership after the temporary scope disappears.
  Query-lifetime retained guest handles and host object-method imports remain
  additional work; a handle does not give the guest pointer access.
- Host services and SQL callbacks can use imports with explicit argument and
  result contracts. Imports are disabled by this runtime policy, not prohibited
  by WebAssembly. Native Simple Functions are not a general SQL-lambda callback
  interface either.
- A live `MemoryPool` object cannot be shared through a pointer. Query Stores
  use host memory callbacks to charge accessible linear pages before creation
  and growth; owned IPC and Arrow reader allocations are also charged to the
  native pool. Virtual reservations/guards are inaccessible and excluded from
  allocation bytes. Native pool failures cannot be hidden by memory.grow's -1.
  JIT/table/metadata overhead still needs separate accounting/deployment controls.
- Zero-copy designs can use memory arranged for both parties or validated
  host-owned result descriptors. This implementation shares owned result
  buffers within the host but still serializes across the WASM boundary; arbitrary pre-existing host buffers cannot simply be referenced
  by a guest pointer.

The current wasm32 module also has a 32-bit linear-memory address space. One
contiguous guest object outside that address space cannot be represented;
chunking, streaming or a different module ABI is required. Runtime fuel and
memory caps are resource policies, not restrictions on SQL type expressiveness.
Native ISA-specific intrinsics require WASM equivalents or emulation; the hash
port's software CRC illustrates semantic portability without equal cost.

Additional differences are implementation work, and must not be described as
WASM impossibilities: custom QueryConfig accessor transformations, automatic
arbitrary native comparator delegation, retained query-lifetime handles and
host-method imports, and remaining native encoding optimizations. Rust
`str`/Arrow UTF-8 cannot represent invalid UTF-8 even though native
StringView is a byte view; this scalar adapter resolves that mismatch with
logical binary transport and `VarcharBytes` without changing SQL VARCHAR into
VARBINARY.
Structured `RowError` preserves the code and message of native Status; ordinary
`ToString` errors remain recoverable user errors. Native non-user statuses use
the host's fatal behavior instead of being converted into recoverable NULLs.
A trap has no row-error record, so it fails the query, bypasses TRY and
invalidates the Store; use `Result` for recoverable per-row errors. Full exec
builds connect task cancellation through epoch polling and a finite deadline. The SDK does not claim full
compatibility with every existing native C++ Simple Function object ABI.

## Validation and measurement

The Rust workspace tests cover typed nested views, sliced offsets, empty and
all-null values, recursive ownership, complex and NaN MAP lookup, mutable
writers, row contract errors, null-free views, exact iteration, SQL hash keys,
full-range TIMESTAMP/HUGEINT, arbitrary-byte VARCHAR and row-local UTF-8
decoding errors, generic/variadic invocation and signature macros. Codec tests
cover metadata/version identity, nested schema distinction and explicit semantics.

The release WASM module is compiled with SIMD and executed under Wasmtime by
[WasmScalarSmokeTest.cpp](../../functions/wasm/tests/WasmScalarSmokeTest.cpp).
Its differential checks compare every selected result with native
`BaseVector::compare`, `hashValueAt` and `equalValueAt`, including primitive
values, string lengths across hash chunk boundaries, different NaN payloads,
signed zeros, invalid UTF-8 and embedded NUL, decimals, dates, timestamp extrema, HUGEINT extrema, dictionaries,
ARRAY, MAP and ROW. Other checks exercise per-overload metadata/coercion,
ASCII and null-free fallback, typed Status behavior, nested writers, TRY, selected rows, constants and
initialization. The minimal build's CTest also runs benchmark output checks;
this is not a substitute for running the entire Velox test suite. Further
checks cover native custom comparison/hash in ARRAY/MAP/ROW, OPAQUE serde
transformation, original-object identity, forged handles, foreign scope/type
rejection and owned IPC result lifetime/copy-on-write.

[WasmNativeUdfBenchmark.cpp](../../functions/wasm/tests/WasmNativeUdfBenchmark.cpp)
measures checked native/WASM add, matching Simple Function prefix, separately
labeled native vector concat, ARRAY sum, NULL/encoding/sparse/large-string
cases, generic ARRAY hash, typed variadic sum, and the existing average
and sum aggregate cases. ARRAY sum explicitly registers the existing native
`ArraySumFunction` as a Simple Function, with 16 non-null elements per row on
both sides. Registration, compilation, input construction and correctness
checks are outside timing; WASM timing includes IPC and fuel-metered execution.
Results are per batch, not per row. Functional alignment does not eliminate
serialization, memory ownership, dynamic typing, or boundary costs.

The initial 2026-10-02 Release validation passed 28 Rust workspace tests and both
minimal-build CTest entries. Nine independent benchmark processes completed:
128, 1024 and 4096 rows, with three repetitions at each size. At 1024 rows,
the median process measurements were:

| Scalar case | Native microseconds/batch | WASM microseconds/batch | WASM/native time |
| --- | ---: | ---: | ---: |
| BIGINT addition | 0.893 | 62.545 | 70.02x |
| VARCHAR concatenation | 10.547 | 676.208 | 64.11x |
| ARRAY sum, 16 BIGINT elements/row | 4.629 | 2334.549 | 504.36x |

These are end-to-end adapter timings, not a profile attributing time to each
component. The initial 8192-row ARRAY case exhausted the default 100-million
fuel budget; the final runs retained that budget and used 4096 rows instead.
This is a resource-budget failure, not a missing ARRAY capability. Raw results,
environment, hashes, scripts and test logs are saved locally under
`benchmark_results/wasm-sfi-parity-2026-10-02`; the initial failed attempt is
preserved separately in its sibling `-fuel-limit-attempt` directory. This
ignored results directory is a local measurement artifact, not a repository
dependency.

After the custom/OPAQUE bridge and adapter optimizations, the latest 2026-10-02
Release validation passed 32 Rust workspace tests and both minimal-build CTest
entries. Twelve independent benchmark processes completed: 128, 1024, 4096 and
8192 rows, with three repetitions at each size. Native code, guest module and
measured source hashes match the saved measurement metadata. At 1024 rows, the
median process measurements were:

| Scalar case | Native microseconds/batch | WASM microseconds/batch | WASM/native time |
| --- | ---: | ---: | ---: |
| BIGINT addition | 0.901 | 66.108 | 73.41x |
| VARCHAR concatenation | 10.628 | 511.132 | 48.09x |
| ARRAY sum, 16 BIGINT elements/row | 4.466 | 366.242 | 82.01x |

Compared with the initial measurements, WASM concatenation takes 24.4% less
time (1.32x speedup), and ARRAY sum takes 84.3% less time (6.37x speedup).
Addition's median time increased by 5.7%; its three process measurements range
from 60.004 to 81.166 microseconds, so these results do not establish an
addition speedup or a uniform improvement across functions.

The optimizations reuse owned host IPC buffers, avoid copies of plain vectors,
cache array view offsets, check recursive NULL metadata once per batch, and
build VARCHAR output directly. They still serialize across the guest boundary;
these timings do not isolate each optimization's contribution. The 8192-row
ARRAY case now completes under the unchanged 100-million fuel budget, with a
median WASM time of 2752.714 microseconds/batch versus 35.103 for native
(78.42x). The native performance gap remains substantial for these simple
functions.

Raw results, process ranges, hashes, environment, reproduction scripts and test
logs are saved locally under
`benchmark_results/wasm-bridge-optimized-2026-10-02`. An intermediate measurement
before the final VARCHAR/value-layout adjustment is preserved in the sibling
`-pre-layout-fix` directory. All directories are ignored local measurement
artifacts. This validation does not cover the full Velox test suite, complete
queries, startup costs or concurrent workloads.


## Validation and timing after the 2026-10-04 review fixes

The 2026-10-04 Rust workspace passed 32 tests. Minimal Release passes 3 CTest
entries; full Release passes 4 entries, including 26 scalar/runtime/operator/
native-metadata GTests and standalone hostile-output regressions. Full
aggregation executes partial/final and forced spill, and verifies nonthrowing
RowContainer teardown. Driver task-token interruption is tested inside an
infinite guest loop. Broader sanitizer/fuzzing and deployment stress remain
additional checks.

Twelve fresh timing processes cover 128/1024/4096/8192 rows, three runs per size.
At 1024 rows the median WASM/native time is 76.35x for checked add, 36.01x for
matching prefix Simple Functions, 87.43x for ARRAY sum, 45.79x for generic ARRAY
hash (32 elements), and 49.32x for eight-argument variadic sum. 2048-byte prefix
is 5.75x; constant add is 20.02x; sparse add selects 128 rows out of 1024 and is
109.37x per input batch. Usage tracking is enabled on both sides. These selected
steady-state cases do not establish native performance parity.

The October 4 medians, raw logs and commands are in
`benchmark_results/wasm-review-fixes-2026-10-04/`; the
[remediation report](wasm-udf-community-review.md) records current test evidence,
resource scope and the complete 1024-row table. Earlier tables in this document
are historical measurements before the fixes, rather than current results.

## Borrowed traversal and ASCII alignment: 2026-10-05

Generic operations no longer materialize whole input values. Tests demonstrate
zero allocations for standard nested ARRAY/ROW/string hash/compare with identical
schemas, and comparison stops before unread tails. MAP sort indices and custom
headers can allocate. At this stage mixed owned/borrowed comparison and SqlKey
retained explicit ownership costs; the later mixed comparison stage removes
standard-value materialization. Input ASCII flags now cross IPC, dispatch is once per
batch, and VARCHAR output metadata is computed from actual output bytes.
Expression dictionary wrappers maintain independent encoding caches.

Validated user errors also keep their native exception category outside TRY;
same-instance recovery is tested. Malformed later status payloads are fatal
before any user error can stop reporting, and non-user statuses still bypass TRY.

The Rust workspace passes 37 tests; full build covers 27 GTests, plus the safety
target; minimal and full builds together pass 7 CTest entries. Three fresh
1024-row processes complete all 27 benchmark cases. Generic ARRAY hash remains
5.170 ms/batch (46.80x native); removing copies did not establish a throughput
improvement over the separately measured October 4 results. The 128-element
attempt still exhausts default fuel, with no successful timing. Host projection,
lazy output codec decoding, native adaptive loops and exact JIT/internal memory
accounting remain gaps. Full results and reproduction commands are in
`benchmark_results/wasm-alignment-2026-10-05/` and the
[community review follow-up](wasm-udf-community-review.md).


## Borrowed variadic alignment: 2026-10-05

`VariadicView<'a,T>` now matches the useful native variadic reader operations:
zero-tail calls, typed and heterogeneous Generic tails, checked indexing,
forward/backward iteration, skipped indices, outer NULL metadata, conservative
batch NULL flags and NULL-skipping iteration that preserves decoding errors.
Schemas are checked once per batch and only accessed values are converted.
The owned `Variadic<T>` adapter remains supported and keeps its original Vec
and value-iteration contract. Examples and SQL execution use the borrowed view.

Allocation tests prove standard borrowed nested/string access and NULL metadata
checks allocate no per-row value vector; invalid unvisited UTF-8 is skipped,
while visited decoding errors and arithmetic overflow remain per-row UserErrors
under SQL TRY. Validation passes 42 Rust tests, 28 full-build GTests and 7 CTest
entries. Evidence is under `.deps/alignment-variadic-2026-10-05/`.

Three fresh targeted runs at 1024 rows, eight arguments, 100 warmups and 100M
fuel/export measure native variadic sum at 10.000 µs and WASM at 340.480 µs per batch
(34.05x). The checked-add control is also retained. These four timed cases do
not replace the historical 27-case tables or establish an end-to-end speedup.
Full-profile correctness is validated separately. See
`benchmark_results/wasm-variadic-view-2026-10-05/` and the community review for
raw runs, scope and method. Host input gather/IPC parsing, eager output codec
decode and exact JIT/internal-memory accounting remain unresolved.


## Host output codec alignment: 2026-10-05

Owned nested scalar codec results can defer native reconstruction in ROW fields,
ARRAY elements and MAP values. First nonempty access materializes the whole
codec column to respect single-load LazyVector semantics and shared-result row
reuse. MAP keys, invocation-scoped OPAQUE handles and top-level scalar codec
outputs stay eager; aggregate and borrowed IPC paths keep their original eager
contract. Native Project -> LazyDereference skips unused codec fields; normal
ROW readers or copying can still force them. This is a host output improvement,
while full input gather/IPC parsing and selective row decoding remain gaps.

Pending vectors retain IPC/pool through complete destruction and use weak Store
failure callbacks. Decoder failures remain fatal runtime bridge failures under
TRY, invalidate a live Store and never expose partial decoded vectors. Protocol
structure and payload presence are checked eagerly; codec bytes are interpreted
when loaded. Tests exercise projection, reuse, lifetime, NULLs, ROW/ARRAY/MAP,
malformed wire fields, expired handles, errors and sparse writes. The current
validation is 42 Rust tests, 29 GTests and 7 unique CTest entries, with 30-case
benchmark correctness. See the community review and
`.deps/alignment-codec-2026-10-05/` for the exact evidence.

The output-codec diagnostics validate 0 decoder calls for an unused lazy column
versus 1,024 with eager decoding. Three-process timing medians at 1,024 rows are
1.160 ms eager-unused, 1.220 ms lazy-unused and 1.040 ms lazy-used. This fixture
does not demonstrate a latency improvement. The separate matched checked-add
control is 1.470 us native versus 110.720 us WASM (75.32x). Exact methods and raw
results are recorded in the community review and
`benchmark_results/wasm-output-codec-2026-10-05/`.

## Borrowed output buffer reuse: 2026-10-05

Generic identity/projection results with an exact matching wire type now return
one slice for a contiguous source range and retain its buffers. Noncontiguous
borrowed results gather by source ranges, including error/NULL runs, rather
than allocating one sliced array per output row and concatenating them. This
works for nested and marked logical types; exact field metadata is required.
Mixed owned results and representation conversions keep their existing builder.
Arrow offset overflow returns an adapter error rather than panicking. Host IPC
serialization/copying remains; this is guest buffer reuse, not host/guest shared
memory. Rust tests check retained buffer addresses, sliced inputs, nested NULLs,
reordering/multiple sources, error masks and legacy-to-wire conversion.

The same output-codec fixture, with input gather outside timing and guest export
plus host output decode/access inside timing, was measured before and after
rebuilding. Three-process medians at 1,024 rows, 100 warmups and 100M fuel/export:

| Output-codec diagnostic | Before | After | Less batch time |
| --- | ---: | ---: | ---: |
| Eager, unused codec field | 821.790 us | 407.840 us | 50.37% |
| Lazy, unused codec field | 812.020 us | 399.100 us | 50.85% |
| Lazy, used codec field | 820.930 us | 418.860 us | 48.98% |

All three cases benefit from the borrowed output change; this comparison does
not isolate lazy codec decoding, prove a full-query speedup or cover all output
shapes. Native and legacy aggregate controls are retained in the same profiles.
Validation passes 53 Rust tests, 37 full GTests, 7 CTest entries across two build
configurations and 31 benchmark correctness cases. Exact evidence is in
`.deps/alignment-hotpath-2026-10-05/` and
`benchmark_results/wasm-hotpath-2026-10-05-{before,after}/`.

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
`.deps/alignment-readers-2026-10-05/`. Physical active-group compaction, callbacks,
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

## Complex MAP keys and borrowed lookup: 2026-10-05

Native MAP values can contain non-NULL ROW/ARRAY keys with NULL children:
`map_from_entries` checks the key root, and Simple Function MapView.find uses
NULL-as-value equality. The stricter `map()` indeterminate-key rule belongs to
that constructor. The SDK/IPC bridge now preserves those native values in both
borrowed and owned paths while rejecting NULL key roots/entries. `null_free`
and `contains_nulls` retain their explicit recursive checks. Native construction,
generic equality/hash, missing/present lookup, borrowed identity and owned
identity are covered; retained UDAF states/staged companions use the same values.
The SDK README demonstrates generic `map_lookup` with view arguments.

The untrusted IPC reader validates schemas before constructing arrays. Because
Arrow MAP construction can abort on NULL key/entry data before ValidateFull,
MAP is read as the identical LIST<STRUCT> layout, validated, then restored with
shared value buffers. Real guest hostile key/entry results fail fatally and
invalidate Store; wrong schemas/trailing bytes fail closed. This is a bridge
fix, not a change to Arrow or the SQL type system.

`MapView::find` also avoids unconditional materialization of its lookup key and
each candidate. Nine alternating paired processes compare the old find against
borrowed find, with one current host, identical Release/SIMD compiler parameters,
lockfiles and all other Rust sources. Both guests pass 35 correctness cases.
The fixture has 1024 rows, 16 entries/row, a last-key lookup with
ROW(BIGINT,ARRAY(BIGINT)) keys and eight payload elements, all non-NULL. Timings
include scalar evaluation/gather/IPC/fuel, excluding JIT/init/input allocation
and destruction; 100 warmups precede each process on CPU 15.

Wasm medians are **35.87 → 15.88 ms/batch**, a **55.73%** time reduction;
the paired exploratory 95% interval for change is [-64.32%, -44.61%]. Native
Simple Function MapView.find control is 0.318 → 0.327 ms, leaving current Wasm
**48.55x native**. Native-normalized change is -56.96%, interval
[-58.33%, -19.17%]. All samples, including drifting controls, are retained.
Intervals bootstrap nine paired indices 20,000 times with seed 20261005;
this is single-machine evidence for this workload, not general query parity.

Old find exhausts 100M fuel with matching SIMD flags. Both comparison sides
explicitly use **1B fuel/export**; current default benchmark correctness passes
at 100M. An initial control missing SIMD was discarded from conclusions and
preserved as a diagnostic cohort. Final commands/modules/compiler fingerprints
and samples are in `benchmark_results/wasm-map-lookup-2026-10-05-paired/` and
`.deps/alignment-map-keys-2026-10-05/evidence.json`. Full/minimal CTests pass
(six/three entries), with 89 Rust tests and 96 full alignment GTests; modules
match with 61 scalar/18 aggregate exports. Ordinary UDAF timing and remaining
obligations are recorded in [the UDAF assessment](wasm-udaf-parity.md).

## Host gather for encoded scalar leaves: 2026-10-05

Fully selected nested inputs can now retain plain sibling buffers when only
scalar leaves have Arrow-supported dictionary/constant encodings over flat
storage. The old bridge accepted only entirely plain vector trees, so one
encoded BIGINT tag forced a deep copy of a large sibling ARRAY payload.
The native Arrow bridge already knows how to flatten such scalar leaves.
Complex dictionary/constant values, encoded UNKNOWN, unloaded lazy bases and
sparse selection retain gathering. The capability check inspects wrappers
without loading a lazy base, and selection indices are created only on demand.
No Arrow core code or guest ABI changes.

The pre-change safety regression fails the original-buffer ownership assertion.
Current tests verify retain/release, unchanged caller children, detached input
lifetime and values through nested ARRAY/MAP, outer/leaf NULLs, full/sparse/empty
selection, root dictionary/constant and UNKNOWN fallback. An actual LazyVector
loader rejects every unreferenced base row and observes only the two needed
indices. Full/minimal builds pass all six/three CTests and 96 full alignment
GTests; Rust's 89-test evidence is retained with unchanged Rust/Cargo hashes,
not rerun. Full/minimal guest bytes are unchanged, with 61 scalar/18 aggregates.

Host-only gather/IPC serialization diagnostics use identical logical
ROW(payload ARRAY(BIGINT),tag BIGINT) data: 1024 rows, 256 payload elements/row
(2 MiB BIGINT payload), a flat/dictionary/constant tag, 100 warmups and a reused
output destination. They include temporary input preparation and teardown,
exclude source/destination allocation and correctness decode, and execute no
guest or native Simple Function. Nine alternating process pairs on CPU 15 use
the archived pre-optimization host and current host with matching fixtures,
compiler/build settings and guest bytes. All 38 correctness cases pass for
each host, including these three diagnostic cases.

| Host gather + serialization | Before us/batch | After us/batch | Change, exploratory 95% interval |
| --- | ---: | ---: | ---: |
| Plain ROW control | 79.85 | 78.35 | -1.88%, [-2.16%, -0.38%] |
| Dictionary scalar leaf | 181.76 | 79.48 | -56.27%, [-56.37%, -55.68%] |
| Constant scalar leaf | 180.19 | 79.88 | -55.67%, [-55.92%, -54.97%] |

Plain-control-normalized change is -55.43% (dictionary; interval
[-56.08%, -54.90%]) and -54.82% (constant; [-55.50%, -54.12%]). All samples
are retained. Intervals resample nine paired process indices 20,000 times with
seed 20261005. No builds/tests overlap timing. These single-machine diagnostics
establish cheaper preparation for this fixture, not a general Wasm/native or
whole-query speedup. IPC serialization/transfer is still eager; selective guest
views do not become end-to-end demand loading, and sparse/encoded complex
transport and native pushdown remain optimization work.

Raw measurements/commands are in
`benchmark_results/wasm-gather-leaves-2026-10-05-paired/`. Evidence and the
ordinary SUM/reduce follow-up are recorded in
`.deps/alignment-gather-leaves-2026-10-05/evidence.json` and
[the UDAF assessment](wasm-udaf-parity.md).

## Primitive output construction follow-up: 2026-10-05

SDK BOOLEAN/TINYINT/SMALLINT/INTEGER/BIGINT/REAL/DOUBLE columns now read borrowed
leaves or copy owned scalars directly to typed Arrow input, without an
intermediate owned Vec<Value>. This applies to nested primitive children and
lambda arguments. NULL/type checks and floating-point bits are preserved;
author APIs, guest ABI and dependencies are unchanged. New coverage includes
all seven types, signed limits, slices, mixed ownership, detached source
lifetime, NULL/empty values, type rejection and NaN/negative-zero bits.
90 Rust tests, 96 full alignment GTests and six full/three minimal CTests pass.
Full/minimal guest hashes match, with 61 scalar/18 aggregate exports.

Nine balanced process triplets isolate old/new SDK on one unchanged current
host and also compare the archived pre-gather host/old SDK. All three pass 38
correctness cases. Scalar checked ADD is 48.59 → 47.91 us for the SDK comparison
(-1.40%, interval [-4.39%, +0.89%]), so no isolated ADD speedup is established.
The total old-host/SDK comparison is 49.32 → 47.91 us (-2.86%,
[-4.40%, -1.24%]), with native controls 0.90434 → 0.90547 us. Current ADD still
costs 52.91x native. This is one flat checked-ADD fixture, not scalar optimizer
or full-query parity.

The main measured gain is lambda reduce: **759.81 → 652.27 us**, -14.15%
on the same host (interval [-16.48%, -8.62%]). The
[UDAF assessment](wasm-udaf-parity.md) records the profiling rationale, complete
controls, total comparison and remaining obligations. Evidence is in
`.deps/alignment-primitive-build-2026-10-05/evidence.json` and
`benchmark_results/wasm-primitive-build-2026-10-05-paired/`.

## Mixed owned/borrowed comparison follow-up: 2026-10-05

The standard-value demand-decoded comparison path now also covers mixed owned
and borrowed operands. ARRAY/ROW stop before unreached children, MAP allocates
sorted indices and standard byte strings are borrowed. Custom payload
materialization remains for mixed codec comparisons; OPAQUE comparisons still
fail. This principally benefits retained UDAF State/input checks and changes no
scalar API or ABI. All 92 Rust tests, 96 full alignment GTests and nine CTests
pass. Both-direction/flags tests and allocation instrumentation cover nested
and scalar domains.

Ten paired processes retain every sample. Scalar ADD is 52.375 → 54.140 us,
with interval [-41.78%, +100.57%]; native controls also vary substantially.
A post-run snapshot shows unrelated CPU-intensive workers predating this
cohort with the benchmark CPU in their affinity. This establishes neither
speedup nor absence of regression. The [UDAF assessment](wasm-udaf-parity.md)
records the complete observations, controls and unresolved performance follow-up.
Evidence is in `.deps/alignment-mixed-compare-2026-10-05/` and
`benchmark_results/wasm-mixed-compare-2026-10-05-paired/`.
