# Velox WebAssembly scalar UDF SDK

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

The macro supports `bool`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`, owned or
borrowed UTF-8 strings, owned or borrowed byte strings, `Date32`, and
`Timestamp`. Wrap argument and result types in `Option` for nullable values.
Return `Result<T, E>` with `E: ToString` for per-row business errors; the SDK
maps these errors to Velox rows so `TRY` returns null only for failing rows.
`Date32` stores days since the Unix epoch; `Timestamp` stores nanoseconds since
the Unix epoch.

For `ARRAY`, `MAP`, `ROW`, and nested combinations, use the lower-level
`#[velox_arrow_scalar]` macro. It passes one compact Arrow `RecordBatch` to the
Rust function and expects an `ArrayRef` with the same number of rows. Declare
the Velox SQL types using Hive-style type strings; the SDK embeds these
signatures in the same custom section as row-oriented scalar functions:

```rust
use velox_wasm_sdk::{arrow_array::{ArrayRef, RecordBatch}, velox_arrow_scalar};

#[velox_arrow_scalar(
    arguments = ["array<bigint>"],
    returns = "array<bigint>"
)]
fn array_identity(batch: &RecordBatch) -> Result<ArrayRef, String> {
    Ok(batch.column(0).clone())
}
```

`map<varchar,bigint>` and `row<id:bigint,values:array<varchar>>` are also
supported. The batch API exposes Arrow's nested arrays directly; Rust code
must check child validity and return a correctly typed array. The host validates
the result against the declared Velox type. The row-oriented
`#[velox_scalar]` still infers only primitive types. An aggregate may receive
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
