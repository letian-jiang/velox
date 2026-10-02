# Java UDF and UDAF Integration Proposal

*2026-08-07*

## Status

This document proposes a design for loading Java scalar UDFs and aggregate
UDFs (UDAFs) into Velox. It defines the initial deployment model, manifest,
JNI boundary, Arrow IPC profile, Java type mapping, execution lifecycle, and
Java SDK responsibilities. It is a design document; the implementation has not
started.

The design is intentionally separate from the WebAssembly UDF design. The two
runtimes share Velox and Arrow IPC semantics, but they have different runtime
ABIs, isolation properties, packaging, and SDKs.

## Motivation

Velox functions are normally compiled into the host binary. The WebAssembly
UDF integration allows users to deploy Rust functions without linking their
business code into Velox. This proposal provides the same user-facing model to
Java users:

1. Implement a row-oriented scalar function or aggregate in Java.
2. Use Java annotations and an annotation processor to generate a
   batch-oriented adapter.
3. Package the implementation and generated adapter into a JAR.
4. Write a manifest that declares the Velox signature and implementation
   class.
5. Configure the host application to load the manifest before query
   execution.

The JNI boundary is batch-oriented even though the user-facing Java API is
row-oriented. Arguments, intermediate values, and results cross the boundary
as Arrow IPC record batches. The generated adapter performs the row loop
inside the JVM, so there is one JNI call per Velox batch rather than one JNI
call per row.

## Goals

- Support Java 8 bytecode and JDK 8 or later at runtime.
- Load UDF JARs once during process startup. Hot reload is not required.
- Support scalar UDFs through Velox `VectorFunction` registration.
- Support UDAFs through Velox `Aggregate` registration.
- Make one JNI call per Velox batch rather than one call per row.
- Keep the user-facing Java API row-oriented.
- Generate Arrow IPC adapters at Java compile time without reflection in the
  row loop.
- Preserve Velox selectivity, null, conditional-result, error, and aggregate
  lifecycle semantics.
- Keep live UDAF states in Java and store only opaque handles in Velox group
  accumulators.
- Support partial/final aggregation and spilling through opaque serializable
  intermediate states.
- Support primitive and nested `ARRAY`, `MAP`, and `ROW` types with recursive
  nullability.
- Permit Arrow C++ and Arrow Java library versions to differ while using a
  small, explicitly versioned common IPC profile.
- Validate manifests, generated adapter descriptors, IPC schemas, and all
  untrusted sizes at their respective boundaries.

## Non-goals

- Treating in-process Java as a security sandbox.
- Running untrusted Java code in the Velox process.
- Updating or replacing a loaded JAR without restarting the process.
- Making Java heap usage participate in Velox memory arbitration in version 1.
- Providing a hard per-invocation CPU limit for arbitrary Java code.
- Passing Velox vectors directly to Java through the Arrow C Data Interface in
  version 1.
- Zero-copy output from Java to Velox.
- Generic, variadic, union, extension, or run-end-encoded types in version 1.
- Supporting Java `record` types, which are unavailable on JDK 8.

## Design Summary

The integration has six layers:

1. **Manifest loader**: reads a user-written manifest and validates the Velox
   signature and deployment metadata.
2. **JVM runtime**: attaches to an existing JVM or creates the process JVM,
   manages thread attachment, class loaders, JNI references, and exception
   translation.
3. **Arrow IPC bridge**: converts selected Velox rows to a stable Arrow IPC
   profile and imports Java results.
4. **Velox adapters**: implement `VectorFunction` and `Aggregate` and register
   them in the existing registries.
5. **Java runtime SDK**: decodes and encodes Arrow IPC and provides row-value
   views, state registries, and the fixed batch adapter interfaces.
6. **Java annotation processor**: validates business methods and generates
   direct, type-specific batch adapters at Java compile time.

```text
manifest
   |
   v
JavaUdfLoader --------> process-wide JVM
   |                         |
   |                         +---- JAR / ClassLoader cache
   |
   +---- scalar registry ----> JavaVectorFunction ----> scalar adapter object
   |
   +---- aggregate registry --> JavaAggregate --------> aggregate adapter object
                                                        +--> handle -> State

Velox selected rows
   -> compact RowVector
   -> Arrow IPC
   -> one JNI call
   -> generated Java batch adapter
   -> row-oriented Java function
   -> Arrow IPC result
   -> scatter into Velox result
```

There is one JVM per process. A canonical JAR deployment is represented by an
isolated class loader. Each Velox scalar expression or aggregate adapter owns
a distinct generated Java adapter object. An aggregate adapter's state
registry is therefore not shared with another aggregate adapter.

## Trust Model

Java UDF JARs loaded by this design are trusted native-process extensions.
Java code can access the filesystem and network, create threads, load native
libraries, retain global state, exhaust the Java heap, loop indefinitely, or
terminate the process. The JVM does not provide isolation equivalent to a
WebAssembly instance.

If untrusted code or hard memory and CPU isolation is required, the Java SDK
and Arrow IPC protocol can be reused in a separate Java worker process. That
worker design is an alternative deployment mode, not part of the in-process
version 1 implementation.

## Packaging and Manifest

A manifest describes one Velox function and is the unit passed to the loader.
Multiple manifests may reference different implementation classes in the same
JAR. Version 1 accepts one fat JAR per manifest; an explicit dependency
classpath can be added later.

Paths are resolved relative to the manifest file. The loader canonicalizes the
manifest and JAR paths before using them as cache keys.

### Scalar manifest

```json
{
  "abi_version": 1,
  "name": "java_add",
  "kind": "scalar",
  "jar": "./my-udfs.jar",
  "class": "com.example.udf.MyAdd",
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
  "name": "java_avg",
  "kind": "aggregate",
  "jar": "./my-udfs.jar",
  "class": "com.example.udf.MyAverage",
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

The manifest names the business implementation class, not the generated
adapter class. The annotation processor writes an internal JAR index that maps
the business class to the generated adapter. The manifest remains
authoritative for SQL registration and deployment metadata.

There is no numeric function ID and there are no manifest-selected Java method
names. The generated adapter implements a fixed SDK interface.

### Recursive type declarations

Primitive declarations retain the compact form shown above. Nested types use
recursive declarations so that every level has explicit nullability.

An array declaration is:

```json
{
  "type": "array",
  "nullable": true,
  "element": {"type": "bigint", "nullable": true}
}
```

A map declaration is:

```json
{
  "type": "map",
  "nullable": false,
  "key": {"type": "varchar", "nullable": false},
  "value": {
    "type": "decimal",
    "precision": 18,
    "scale": 2,
    "nullable": true
  }
}
```

A row declaration is:

```json
{
  "type": "row",
  "nullable": true,
  "fields": [
    {
      "name": "user_id",
      "type": {"type": "bigint", "nullable": false}
    },
    {
      "name": "tags",
      "type": {
        "type": "array",
        "nullable": true,
        "element": {"type": "varchar", "nullable": true}
      }
    }
  ]
}
```

The recursive grammar supports combinations such as:

```text
array(
  row(
    id bigint,
    attributes map(varchar, array(decimal(18, 2)))
  )
)
```

Version 1 limits nesting depth to a configurable value whose default is 32.
Map keys are always non-null. Duplicate row field names are rejected. Row
field order and names are part of the ABI.

### Startup validation

At load time, the host and Java bootstrap validate:

- the ABI version and function kind;
- required manifest fields and unknown fields;
- the canonical JAR path and file identity;
- the business implementation class and generated adapter index entry;
- that the generated adapter implements the expected fixed interface;
- that the adapter descriptor matches the manifest recursively;
- decimal precision and scale;
- map key non-nullability;
- row field names and ordering;
- maximum nesting depth;
- scalar versus aggregate metadata;
- function-name collisions in the Velox registry; and
- Java SDK and Arrow runtime compatibility.

A mismatch between the manifest and generated adapter descriptor is a startup
configuration error. IPC schemas are still validated on every call.

## JVM Runtime

### One JVM per process

The integration supports both embedding modes:

1. If Velox is already running inside a JVM, the embedding application passes
   its `JavaVM*` to the Java UDF runtime.
2. If the process is a native Velox application, the runtime creates the JVM
   through the JNI Invocation API.

The runtime must never attempt to create a second JVM in a process. JVM options
such as heap size, GC configuration, system properties, and the runtime SDK
classpath are process configuration. They are not accepted from individual
function manifests.

Version 1 targets Java 8 bytecode. The SDK, generated adapters, and runtime
must not use APIs or language constructs newer than Java 8.

### Native thread attachment

`JNIEnv*` is thread-local and must never be cached globally or passed between
threads. Each calling Velox thread obtains its own environment by attaching to
the JVM when necessary. A thread-local guard detaches threads that were
attached by the Java UDF runtime when those threads exit.

Every JNI invocation creates a bounded local-reference frame. Long-lived Java
objects are held with JNI global references managed by C++ RAII wrappers.

### Class loading

The runtime caches a module class loader by canonical JAR path and file
identity. File identity includes at least size and modification time; a content
digest may be used when stronger deployment identity is required.

The module loader is child-first for user packages and parent-first for:

- `java.*` and other platform packages;
- the Velox Java SDK packages; and
- Apache Arrow Java packages.

This rule prevents a user fat JAR from replacing the runtime SDK or the pinned
Arrow Java implementation. Calls from native-attached threads load classes
through the cached module class loader explicitly and do not rely on the
thread context class loader.

Class loaders are retained until process shutdown. Hot reload and class
unloading are not required.

### Adapter object lifetime

Each `JavaVectorFunction` or `JavaAggregate` owns one generated Java adapter
object. The object is created during Velox adapter construction and closed
explicitly during destruction. Separate adapter objects isolate scalar
instance fields and aggregate state registries.

Java static fields remain shared by all adapter objects loaded by the same
class loader. Business implementations must not use mutable static state for
per-expression or per-query data.

### JNI exceptions

Every JNI call checks for a pending exception. The bootstrap runtime converts
a Java exception and its stack trace into a UTF-8 diagnostic, clears the JNI
pending exception, and reports a Velox query error. It must not rely solely on
`ExceptionDescribe`, which writes to process stderr and does not produce a
structured error.

JNI local reference exhaustion, class loading failures, adapter linkage
errors, and Java `OutOfMemoryError` are query or startup errors according to
where they occur.

## Java SDK and Annotation Processor

The Java SDK is split into three artifacts:

```text
velox-java-annotations.jar
  @VeloxScalar
  @VeloxAggregate
  @VeloxRowType
  @VeloxField
  @Nullable

velox-java-processor.jar
  VeloxUdfProcessor

velox-java-runtime.jar
  fixed batch adapter interfaces
  Arrow IPC runtime
  row value views and builders
  aggregate state registry
  error and temporal value classes
```

The annotation processor uses the standard `javax.annotation.processing` API,
declares `SourceVersion.RELEASE_8`, and is discovered through:

```text
META-INF/services/javax.annotation.processing.Processor
```

The processor does not modify the user's source. It validates annotated
business methods and generates Java source, class files, descriptors, and an
internal adapter index during `javac` compilation.

For a business class `com.example.udf.MyAdd`, the processor may generate:

```text
com.example.udf.MyAdd__VeloxAdapter
META-INF/velox-udf/index.json
```

The index maps the business class name to the generated adapter and descriptor.
It is internal linkage metadata, not SQL registration metadata, and does not
replace the handwritten manifest.

The generated adapter reads Arrow vectors with direct, statically typed calls
and directly invokes the business method. Runtime reflection may be used for
class discovery and startup validation, but it is not used once per row.

## Fixed Java Batch Interfaces

Conceptually, generated scalar adapters implement:

```java
public interface BatchScalarAdapter extends AutoCloseable {
  UdfDescriptor descriptor();

  byte[] apply(byte[] inputIpc);
}
```

Generated aggregate adapters implement:

```java
public interface BatchAggregateAdapter extends AutoCloseable {
  UdfDescriptor descriptor();

  int[] create(int groupCount);

  void destroy(byte[] inputIpc);

  void update(byte[] inputIpc);

  void updateSingleGroup(int stateHandle, byte[] inputIpc);

  byte[] serialize(byte[] inputIpc);

  void merge(byte[] inputIpc);

  void mergeSingleGroup(int stateHandle, byte[] inputIpc);

  byte[] finish(byte[] inputIpc);
}
```

These interfaces describe the logical ABI. The C++ runtime caches the
corresponding classes and method IDs and invokes the generated object through
JNI.

`create` returns a JNI `int[]`; each Java `int` carries the raw bits of an
opaque unsigned 32-bit handle. The other data-bearing operations use Arrow IPC
in `byte[]`. Operations with no output return `void`. A thrown Java exception
is a batch-level failure.

JNI arrays use signed `jsize` lengths. Input and output IPC payloads must
therefore be no larger than `INT32_MAX`, and the runtime enforces a lower
configurable batch byte limit. Velox inputs larger than the configured limit
are chunked before invoking Java.

Version 1 uses `byte[]` because it has simple and explicit ownership. A future
version may pass input through a direct `ByteBuffer`, but it must define buffer
lifetime, mutation, and asynchronous-retention rules before doing so.

## Arrow IPC Compatibility Profile

Arrow library releases and Arrow format versions are different concepts.
Arrow C++ and Arrow Java library versions do not need to match. Both sides
must support the following version 1 IPC profile:

| Property | Version 1 requirement |
|----------|-----------------------|
| Container | Arrow IPC streaming format |
| Metadata version | V5 |
| Schema messages | Exactly one |
| Record batches | Exactly one for every non-empty payload |
| Compression | Disabled |
| Dictionary encoding | Disabled |
| Endianness | Little-endian |
| UTF-8 and Binary | 32-bit offsets |
| Array | Arrow `List`, 32-bit offsets |
| Map | Standard Arrow `Map` |
| Row | Standard Arrow `Struct` |
| Decimal | `Decimal128` only |
| Timestamp | Nanoseconds, no timezone |
| Extension types | Disabled |
| Large offset types | Disabled |
| View types | Disabled |
| Union and run-end encoding | Disabled |

Writers set these options explicitly instead of relying on Arrow library
defaults. An older reader can read output from a newer writer only when the
writer does not use newer format features. The restricted profile is therefore
the compatibility contract, not the Arrow library release number.

The build pins and tests one Arrow Java release that supports Java 8. It does
not need to match the Arrow C++ version linked by Velox. A release combination
is supported only after bidirectional compatibility tests pass:

```text
Arrow C++ encode -> Arrow Java decode -> Arrow Java encode -> Arrow C++ decode
```

Reserved runtime fields use the `__velox_java_` prefix. User argument and row
field names must not use this prefix.

Top-level fields use stable names:

- scalar arguments: `arg0`, `arg1`, and so on;
- scalar or aggregate value result: `result`;
- scalar row error: `__velox_java_error`;
- aggregate state handle: `__velox_java_state_handle`;
- aggregate intermediate value: `__velox_java_intermediate`.

All arrays in one record batch have the same row count. Both sides validate
the complete recursive schema, including names, field ordering, child
nullability, decimal parameters, timestamp unit, and the absence of
dictionary encoding.

## Type Mapping

### Primitive types

| Velox type | Arrow type | Non-null Java type | Nullable Java type |
|------------|------------|--------------------|--------------------|
| `BOOLEAN` | `Bool` | `boolean` | `Boolean` |
| `TINYINT` | `Int8` | `byte` | `Byte` |
| `SMALLINT` | `Int16` | `short` | `Short` |
| `INTEGER` | `Int32` | `int` | `Integer` |
| `BIGINT` | `Int64` | `long` | `Long` |
| `REAL` | `Float32` | `float` | `Float` |
| `DOUBLE` | `Float64` | `double` | `Double` |
| `VARCHAR` | `Utf8` | `String` | `@Nullable String` |
| `VARBINARY` | `Binary` | `byte[]` | `@Nullable byte[]` |
| `DATE` | `Date32` | `VeloxDate32` | `@Nullable VeloxDate32` |
| `TIMESTAMP` | `Timestamp(ns, no tz)` | `VeloxTimestamp` | `@Nullable VeloxTimestamp` |
| `DECIMAL(p,s)` | `Decimal128(p,s)` | `BigDecimal` | `@Nullable BigDecimal` |

Java primitives are used for non-null numeric and boolean values. Nullable
numeric and boolean values use boxed types. Reference types are non-null unless
annotated with the SDK's `TYPE_USE` `@Nullable` annotation.

`UNKNOWN`, intervals, timestamp with time zone, and unsigned user-visible SQL
types are not supported in version 1.

### Date and timestamp

`VeloxDate32` stores a signed 32-bit number of days since 1970-01-01.
`VeloxTimestamp` stores a signed 64-bit number of nanoseconds since 1970-01-01.
These SDK value classes preserve the exact wire representation and avoid Java
calendar, default-time-zone, range, and precision ambiguities.

Version 1 does not attach a timezone string to Arrow timestamps. A future
timestamp-with-time-zone mapping requires a separate logical type contract and
must not silently reuse this type.

### Decimal

Both Velox short and long decimal values cross the boundary as Arrow
`Decimal128(precision, scale)`. Using one width avoids a dependency on newer
Arrow Java `Decimal64` support.

The Java row type is `java.math.BigDecimal`. The adapter validates:

- `1 <= precision <= 38`;
- the exact manifest scale;
- that output does not require rounding to reach the declared scale; and
- that output precision does not exceed the declaration.

The adapter performs the equivalent of `setScale(scale,
RoundingMode.UNNECESSARY)` before writing output. It never silently truncates,
rounds, or changes scale. An invalid scalar row output becomes a row error; an
invalid aggregate output fails the aggregate batch.

### Array

The mapping is:

```text
Velox ARRAY(T) <-> Arrow List<T> <-> Java List<T>
```

Inputs are read-only, Arrow-backed implementations of `List<T>`. They lazily
read elements from the child vector and are valid only during the current
business-method call. Mutation and retention beyond the call are unsupported.

Outputs may be any `List<T>` implementation. The generated adapter consumes
the returned list synchronously and writes it to an Arrow `ListVector`.

The SDK `@Nullable` annotation can appear on type arguments in Java 8:

```java
public List<@Nullable Long> evaluate(
    List<@Nullable Long> input) {
  // ...
}
```

A null array and a non-null array containing null elements are distinct.

### Map

The physical mapping is:

```text
Velox MAP(K,V)
  <-> Arrow Map<K,V>
  <-> List<Struct<key: K non-null, value: V>>
```

The Arrow map's entries struct and key field are non-null. Values may be null,
and key and value types may themselves be nested types.

The primary Java mapping is an SDK type rather than `java.util.Map`:

```java
public interface VeloxMap<K, V>
    extends Iterable<VeloxMap.Entry<K, V>> {
  int size();

  K keyAt(int index);

  V valueAt(int index);
}
```

Using `java.util.Map` as the ABI type would introduce observable mismatches:

- `byte[]` keys use identity equality in Java;
- Java equality for floating point, decimal, and nested keys may differ from
  Velox SQL equality;
- duplicate keys may be silently overwritten by `put`;
- an implementation may lose the physical entry order.

Input `VeloxMap` values are read-only Arrow-backed views. Output values are
built with `VeloxMaps.builder()`, which rejects null keys and obvious duplicate
keys and preserves entry order. After importing a Java map result, the host
performs final duplicate-key validation using Velox key equality. SQL semantics
must not depend on physical map entry order even though transport preserves it.

Convenience conversion to `java.util.Map` may be provided for supported simple
key types, but it is not the ABI type.

### Row

The mapping is:

```text
Velox ROW <-> Arrow Struct <-> Java @VeloxRowType POJO
```

JDK 8 has no Java records. Version 1 uses annotated plain Java classes:

```java
@VeloxRowType
public final class UserInfo {
  @VeloxField(index = 0, name = "id")
  public long id;

  @VeloxField(index = 1, name = "name")
  @Nullable
  public String name;

  @VeloxField(index = 2, name = "scores")
  public List<@Nullable Double> scores;
}
```

Version 1 row classes have an accessible no-argument constructor and annotated
non-static, non-final fields. Explicit field indexes avoid relying on Java
reflection or source declaration order. Indexes must be contiguous from zero,
and index, name, Java type, and recursive nullability must match the manifest.

The annotation processor generates a type-specific reader and writer. The
reader constructs a POJO for each non-null input row. A future `VeloxRowView`
API may avoid these objects for performance-sensitive functions, but it is not
required for version 1.

An Arrow Struct has a parent validity bitmap independent of child validity
bitmaps. If the parent row is null, the Java argument is null and child slots
are ignored even if their physical buffers contain values.

### Nested example

The following declaration:

```json
{
  "type": "array",
  "nullable": false,
  "element": {
    "type": "row",
    "nullable": true,
    "fields": [
      {
        "name": "name",
        "type": {"type": "varchar", "nullable": false}
      },
      {
        "name": "prices",
        "type": {
          "type": "map",
          "nullable": false,
          "key": {"type": "varchar", "nullable": false},
          "value": {
            "type": "decimal",
            "precision": 18,
            "scale": 2,
            "nullable": true
          }
        }
      }
    ]
  }
}
```

maps to:

```java
@VeloxRowType
public final class Product {
  @VeloxField(index = 0, name = "name")
  public String name;

  @VeloxField(index = 1, name = "prices")
  public VeloxMap<String, @Nullable BigDecimal> prices;
}

@VeloxScalar
public final class NormalizeProducts {
  public List<@Nullable Product> evaluate(
      List<@Nullable Product> products) {
    return products;
  }
}
```

### Recursive nullability

Top-level argument nullability, array element nullability, map value
nullability, row nullability, and row-field nullability are independent.

`default_null_behavior` applies only when an entire scalar or aggregate raw
argument is null. It does not filter a row because an array contains a null
element, a map contains a null value, or a nullable row field is null.

The generated adapter validates non-null declarations while reading inputs and
writing outputs. The host repeats recursive schema and output validation before
importing Java results.

## Scalar Functions

### Business API

A scalar implementation is a class annotated with `@VeloxScalar` and exactly
one supported `evaluate` method:

```java
@VeloxScalar
public final class MyAdd {
  public long evaluate(long left, long right) {
    return left + right;
  }
}
```

The business object is instantiated once per generated scalar adapter object.
The method cannot be generic, asynchronous, or variadic in version 1.

A scalar row-business error is reported explicitly:

```java
@VeloxScalar
public final class CheckedDivide {
  public long evaluate(long left, long right) throws VeloxRowException {
    if (right == 0) {
      throw new VeloxRowException("division by zero");
    }
    return left / right;
  }
}
```

Only `VeloxRowException` is converted to a per-row error. Other Java
exceptions and errors fail the whole batch.

### Input construction

`VectorFunction::apply` receives a possibly sparse `SelectivityVector`. The
host compacts only selected rows, preserving their original order, exports the
compact `RowVector`, and writes one Arrow IPC stream.

Top-level fields are `arg0`, `arg1`, and so on. Active Velox row indexes remain
on the host and do not cross JNI.

### Java execution

The generated adapter:

1. opens the Arrow IPC stream;
2. validates the complete input schema;
3. obtains the type-specific Arrow vectors;
4. loops over compact rows in order;
5. creates primitive values, nested views, or row POJOs as required;
6. invokes the business `evaluate` method directly;
7. appends results or row errors; and
8. writes exactly one output record batch.

All Arrow resources are closed through Java 8 `AutoCloseable` and
try-with-resources. The generated code does not depend on `Cleaner`.

### Output and scatter

The output contains a `result` field and exactly the compact input row count.
The host recursively validates and imports it, then scatters compact row `i`
to original selected row `selectedRows[i]`. Existing result values at
unselected positions are preserved for conditional expressions.

All imported output values must be owned by Velox before the Java `byte[]` and
temporary Arrow objects are released. This includes strings nested inside
arrays, maps, and rows; output copying cannot special-case only top-level
strings.

### Null behavior and row errors

When `default_null_behavior` is true, Velox excludes rows having a null
top-level argument before IPC encoding. The Java method receives non-null
top-level arguments. When false, top-level validity is preserved and nullable
Java types are passed to the business method.

Nullable result values use the Arrow result validity bitmap. When a business
method throws `VeloxRowException`, the generated result contains a null result
at that row and its message in nullable UTF-8 field
`__velox_java_error`. The host maps compact errors back to original rows and
records them in `EvalCtx`, preserving Velox `TRY` semantics.

IPC decoding errors, schema mismatches, unexpected Java exceptions, and
resource-limit failures are batch-level query errors.

## Aggregate Functions

### Business API

An aggregate class is annotated with `@VeloxAggregate` and implements six
row-oriented lifecycle methods:

```java
@VeloxAggregate
public final class MyAverage {
  public AverageState create() throws VeloxUdfException {
    return new AverageState();
  }

  public void destroy(AverageState state) {}

  public void update(
      AverageState state,
      @Nullable Double value) throws VeloxUdfException {
    // Update state.
  }

  public byte[] serialize(
      AverageState state) throws VeloxUdfException {
    // Encode the complete state.
    throw new UnsupportedOperationException();
  }

  public void merge(
      AverageState state,
      byte[] intermediate) throws VeloxUdfException {
    // Decode and merge the intermediate state.
  }

  @Nullable
  public Double finish(
      AverageState state) throws VeloxUdfException {
    // Produce the final value without mutating state.
    throw new UnsupportedOperationException();
  }
}
```

`update` has one business argument per manifest raw argument after the state
argument. `merge` receives exactly the business bytes produced by `serialize`.
There is no separate deserialize method.

`serialize` and `finish` must not mutate state because Velox may invoke
extraction repeatedly. Java cannot enforce immutable receiver access, so this
is a documented business contract verified by tests where possible.

Aggregate lifecycle errors are batch-level query errors. There is no per-row
UDAF error channel because a failed update may already have mutated earlier
states in the batch.

### State ownership

Live aggregate states are owned by the generated Java adapter. Velox stores
only an opaque 32-bit handle in each group accumulator:

```text
Velox group accumulator                 Java aggregate adapter
+----------------------+                +------------------------+
| state handle: UInt32 | -------------> | handle -> Java State   |
+----------------------+                +------------------------+
```

Handle zero is invalid. The generated registry conceptually contains:

```java
final class StateRegistry<S> {
  long nextHandle = 1;
  final Map<Integer, S> states = new HashMap<Integer, S>();
}
```

`nextHandle` is a `long` so it can allocate every non-zero unsigned 32-bit bit
pattern. The raw bits are cast to Java `int` for storage and JNI transport.
Handle values are never reused during the adapter object's lifetime, avoiding
an ABA problem. Creation after `0xffffffff` is exhausted fails the query.

Handles are local to one Java aggregate adapter and must never be passed to
another adapter object.

The C++ adapter reports:

```text
accumulatorFixedWidthSize() = sizeof(uint32_t)
accumulatorUsesExternalMemory() = true
isFixedSize() = true
```

It uses unaligned-safe loads and stores for the accumulator handle, matching
the aggregate lifecycle requirements used by the WebAssembly adapter.

### Registry behavior

| Operation | Registry behavior |
|-----------|-------------------|
| `create` | Allocate handles and insert newly created states. |
| `update` | Look up each handle mutably and invoke `update` in row order. |
| `merge` | Look up each destination and pass opaque intermediate bytes to `merge`. |
| `finish` | Look up each state and invoke `finish`. |
| `serialize` | Look up each state and return bytes from `serialize` unchanged. |
| `destroy` | Remove each state and pass it to the business `destroy` method. |

`update`, `merge`, `finish`, and `serialize` reject zero and unknown handles.
`destroy` is idempotent: zero and absent handles are ignored so cleanup does
not hide an earlier failure.

Batch creation is atomic. If the Nth business `create` fails, the adapter
removes and destroys all states created by the current call before propagating
the error. It never returns a partial handle array.

### Aggregate Arrow schemas

The generated adapter uses the following IPC schemas:

| Operation | Input batch | Output |
|-----------|-------------|--------|
| `create` | Scalar `groupCount` through JNI | JNI `int[]` handles |
| `update` | `__velox_java_state_handle: UInt32`, raw arguments | None |
| `updateSingleGroup` | Raw arguments; handle is a JNI scalar | None |
| `merge` | `__velox_java_state_handle: UInt32`, `__velox_java_intermediate: Binary` | None |
| `mergeSingleGroup` | `__velox_java_intermediate: Binary`; handle is a JNI scalar | None |
| `serialize` | `__velox_java_state_handle: UInt32` | `__velox_java_intermediate: Binary` |
| `finish` | `__velox_java_state_handle: UInt32` | `result` with manifest return type |
| `destroy` | `__velox_java_state_handle: UInt32` | None |

None means the Java method returns `void`; it does not create an empty Arrow
stream.

For grouped raw input, handles are aligned one-to-one with compact argument
rows. The SDK consumes the hidden handle field and does not expose it to the
business method. For a single group, the handle is a scalar JNI argument and
is not repeated in the Arrow batch.

### Velox lifecycle mapping

| Velox method | Java adapter operation |
|--------------|------------------------|
| `initializeNewGroups` | `create` |
| `addRawInput` | `update` |
| `addSingleGroupRawInput` | `updateSingleGroup` |
| `addIntermediateResults` | `merge` |
| `addSingleGroupIntermediateResults` | `mergeSingleGroup` |
| `extractValues` | `finish` |
| `extractAccumulators` | `serialize` |
| `destroy` | `destroy` |

The initial adapter does not implement the optional `toIntermediate` fast
path.

### Intermediate state

The registered aggregate intermediate type is always nullable `VARBINARY`.
`serialize` returns one non-null opaque byte sequence for each live state.
`merge` receives those bytes unchanged. The SDK adds no magic, version,
checksum, or generic envelope.

A null incoming intermediate value represents no contribution and is skipped
without invoking the business `merge` method. If a business format needs
versioning or corruption detection, `serialize` includes it and `merge`
validates it.

Distributed stages must deploy compatible business implementations and
intermediate encodings. This is a deployment contract.

### Aggregate null behavior

When `default_null_behavior` is true, rows with a null top-level raw argument
are excluded before IPC encoding. When false, validity is preserved and the
generated adapter passes nullable Java values to `update`.

Nested null values are governed by the recursive type declaration and do not
trigger default top-level row filtering.

The final result may be null according to the manifest. A successfully
serialized live state always produces a non-null intermediate payload.

### Destruction and abnormal failure

Normal cleanup sends all live handles to `destroy`, clears the Velox
accumulator slots, closes the Java adapter, and releases its global reference.
Each successfully created state receives business destruction at most once.

JNI failure, JVM termination, or a fatal Java error may prevent normal
business destruction. Correctness must not depend on Java finalizers or object
finalization. Releasing a global reference allows ordinary GC reclamation but
does not substitute for the business `destroy` callback.

## Concurrency

Calls to one aggregate adapter are serialized with a mutex. This is required
because the Java state registry and business states are mutable and because
Velox may request accumulator extraction concurrently during spilling.

The same conservative locking rule applies to a scalar adapter object because
the generated adapter owns Arrow allocators and a business object that may
contain instance fields. Different adapter objects may execute concurrently on
different JVM-attached Velox threads.

Generated Java code does not call back into Velox while holding registry
mutation state. Version 1 does not support reentrant invocation of the same
adapter.

## Memory, Cancellation, and Resource Limits

Java Arrow temporary allocations use an allocator owned by the adapter or a
bounded child allocator. Every vector, reader, writer, and allocator has an
explicit close path.

The first version does not account ordinary Java objects in a Velox memory
pool. Consequently:

- Velox sees four bytes of accumulator data per aggregate group;
- Java `HashMap`, state objects, arrays, strings, and collections are absent
  from Velox group-size accounting;
- Java state growth does not independently cause Velox to spill; and
- Java heap pressure is limited primarily by process JVM configuration such as
  `-Xmx` and can fail the query or process instead of triggering cooperative
  spilling.

Batch byte and row limits reduce temporary Arrow memory and bound JNI array
sizes. Chunking preserves selected-row order. Sequential aggregate chunks
update the same handles and must not change group semantics.

The host checks query cancellation before and after every Java batch call. The
generated row loop may also perform a cooperative cancellation check at a
configurable interval. Java thread interruption is cooperative and cannot
provide a hard guarantee for arbitrary business code. Version 1 has no
equivalent of WebAssembly fuel or epoch interruption.

## Shared Velox Arrow IPC Components

The Java adapter requires the same gather, IPC encode, recursive result import,
and scatter semantics as the WebAssembly adapter. These runtime-neutral pieces
should live in a shared component rather than be copied into the Java
directory.

The shared component includes:

- selected-row compaction;
- recursive Velox-to-Arrow schema conversion;
- explicit IPC profile writer options;
- insertion of configurable hidden columns;
- one-schema/one-batch validation;
- recursive output schema validation;
- deep ownership of nested variable-width output values; and
- scatter into an existing Velox result.

Runtime-specific code supplies hidden field names, runtime diagnostics, and
the actual Wasmtime or JNI invocation.

## Validation and Failure Handling

All values crossing the Java boundary are validated:

- IPC payload length is within configured and JNI limits;
- the payload contains exactly one schema and one record batch;
- compression, dictionary encoding, extension types, and unsupported layouts
  are absent;
- the complete schema matches the manifest recursively;
- all arrays have the expected row count;
- row names and field order match;
- decimal precision and scale match;
- timestamps use nanoseconds without a timezone;
- non-null fields do not contain semantic nulls;
- list and binary offsets are valid and use the required 32-bit layouts;
- map entries and keys are non-null;
- map keys are unique under Velox equality;
- nesting depth and total child values are within configured limits;
- state handles are non-zero and known;
- `create` returns the requested number of non-zero distinct handles; and
- no additional record batch or trailing protocol data is accepted.

A scalar `VeloxRowException` is a row error. All other validation, JNI,
business aggregate, Java exception, allocation, and protocol failures are
query errors.

An aggregate update is not transactional. If it mutates some states and then
fails, the query fails and normal error cleanup destroys the remaining states.

## Build Integration

The host integration is optional behind a CMake option such as:

```text
VELOX_ENABLE_JAVA_UDF
```

Enabling it also enables Arrow support and locates JNI headers and the JVM
library. The build pins the Java runtime SDK and Arrow Java dependencies. JNI
types are not exposed through public Velox APIs.

The proposed source layout is:

```text
velox/functions/udf/
  ArrowIpc.*

velox/functions/java/
  Manifest.*
  JvmRuntime.*
  JavaInstance.*
  JavaVectorFunction.*
  JavaAggregate.*
  Registration.*
  tests/

velox/functions/java/sdk/
  velox-java-annotations/
  velox-java-processor/
  velox-java-runtime/
  examples/
```

The application embedding Velox initializes the JVM configuration and calls
the manifest loader before queries are planned or executed.

## Performance Considerations

The version 1 data path is:

```text
Velox vectors
  -> selected-row gather
  -> Arrow IPC encode
  -> copy into Java byte[]
  -> Java IPC decode
  -> generated row loop
  -> Java IPC encode
  -> copy out of Java byte[]
  -> C++ IPC decode
  -> recursive deep copy and scatter
```

The design optimizes boundary-call count and developer usability, not
zero-copy transport. Nested Java values add additional costs:

- array and map inputs use lazy views to avoid eager collection materialization;
- row POJO inputs allocate one object per non-null row;
- boxed nullable primitive elements allocate or use cached wrappers; and
- complex outputs require recursive traversal and validation.

Benchmarks report at least:

- gather and Velox-to-Arrow conversion time;
- C++ IPC encoding time;
- JNI input-copy time;
- Java IPC decoding time;
- nested view or POJO construction time;
- business function time;
- Java IPC encoding time;
- JNI output-copy time;
- C++ IPC decoding and recursive validation time; and
- deep copy and scatter time.

Possible future optimizations include direct input `ByteBuffer`, cached Arrow
schemas, generated row views, vector-oriented user APIs, and an Arrow C Data
Interface path. Each optimization requires a separate lifetime and ownership
design and must preserve version 1 semantics.

## Testing Strategy

Tests are required at five levels:

1. **JNI runtime tests** cover existing and created JVMs, native-thread
   attachment, local/global reference cleanup, class loader isolation,
   exception stack traces, and shutdown behavior.
2. **Annotation processor tests** compile valid and invalid scalar, aggregate,
   row, nullable, and recursively nested Java sources under Java 8.
3. **IPC compatibility tests** perform bidirectional C++/Java round trips for
   every supported Arrow library-version pair and reject every feature outside
   the fixed profile.
4. **Scalar tests** cover sparse selectivity, conditional results, constants,
   dictionaries flattened at the boundary, top-level and nested nulls, row
   errors, complex outputs, chunking, and deep ownership after Java buffers are
   released.
5. **Aggregate tests** cover batch-create rollback, repeated handles, grouped
   and global aggregation, nested raw inputs and final outputs, partial/final
   execution, spilling, repeated extraction, idempotent destruction,
   intermediate compatibility, and concurrent accumulator extraction.

Complex-type tests include:

- null array versus array containing null elements;
- nested arrays and empty arrays;
- maps with null values, null keys, duplicate primitive keys, binary keys, and
  nested keys;
- empty rows, named rows, null rows, and independently null row fields;
- decimal boundary precision, scale mismatch, overflow, and forbidden
  rounding; and
- combinations such as `array(row(map(varchar, array(decimal))))`.

End-to-end tests build example Java 8 JARs with the annotation processor, load
handwritten manifests, and execute the functions through Velox plans.

## Implementation Phases

1. Extract or generalize the runtime-neutral Arrow IPC bridge and define the
   fixed IPC profile.
2. Add JVM bootstrap, JNI RAII wrappers, class loader cache, primitive manifest
   parsing, and scalar registration.
3. Add Java annotations, processor, runtime SDK, primitive scalar functions,
   null behavior, and row errors.
4. Add Decimal, Array, Map, Row, recursive manifest declarations, and complex
   scalar tests.
5. Add aggregate state handles and create, update, finish, and destroy.
6. Add serialize, merge, partial/final execution, spilling, and concurrent
   extraction.
7. Add resource limits, cooperative cancellation, diagnostics, negative tests,
   cross-version Arrow test matrices, and benchmarks.

## Alternatives Considered

### Require identical Arrow C++ and Java versions

Library release numbers do not define the cross-language wire contract, and
keeping C++ and Java dependencies at identical versions can be impossible when
Java 8 support is required. The design instead pins an explicit stable IPC
profile and verifies each supported library combination bidirectionally.

### Runtime reflection for every row

Reflection would reduce SDK build tooling, but adds row-level invocation and
boxing overhead and defers type errors until query execution. The annotation
processor generates direct calls and reports unsupported signatures during the
user's Java build.

### Expose Apache Arrow vectors directly to users

A vector-oriented API can be faster, but exposes Arrow Java versions and
allocator ownership to every business implementation. Version 1 keeps the
user-facing API row-oriented. A separate expert vector API can be added later.

### Use `java.util.Map` for MAP

Java `Map` equality and duplicate behavior do not faithfully represent all
Velox map keys, especially binary and nested keys. The SDK uses an ordered
`VeloxMap` entry view and validates output keys under Velox semantics.

### Use Java serialization for aggregate intermediate state

Java object serialization is language- and class-version-sensitive, unsafe for
untrusted bytes, and difficult to evolve. The business implementation owns an
explicit byte encoding through `serialize` and `merge`.

### Use direct ByteBuffer in version 1

A direct input buffer can remove one copy, but requires strict rules for native
buffer lifetime, mutation, Java retention, and exception cleanup. `byte[]`
provides a simpler ownership contract for the initial ABI.

### Run Java in a worker process

A worker process provides stronger crash, memory, and capability isolation but
adds process management, RPC framing, failure recovery, and latency. The
in-process design is appropriate for trusted extensions. A worker deployment
can reuse the manifest, generated adapters, and Arrow IPC profile in the
future.
