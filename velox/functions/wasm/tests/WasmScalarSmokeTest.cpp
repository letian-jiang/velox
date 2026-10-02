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

#include <cmath>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include "velox/common/memory/Memory.h"
#include "velox/core/Expressions.h"
#include "velox/core/QueryCtx.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/RowContainer.h"
#include "velox/expression/Expr.h"
#include "velox/functions/wasm/ArrowIpc.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Registration.h"
#include "velox/functions/wasm/Runtime.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::functions::wasm::test {
namespace {

void check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

const ScalarManifest& scalarManifest(const std::string& name) {
  static const auto manifests = loadEmbeddedManifests(WASM_MODULE_PATH);
  for (const auto& manifest : manifests.scalars) {
    if (manifest.name == name) {
      return manifest;
    }
  }
  throw std::runtime_error("Missing embedded Wasm scalar function: " + name);
}

std::string materialize(const ArrowIpcInput& input) {
  std::string ipc(input.size(), '\0');
  input.write(reinterpret_cast<uint8_t*>(ipc.data()), input.size());
  return ipc;
}

template <typename T>
std::shared_ptr<FlatVector<T>> makeFlat(
    const TypePtr& type,
    const std::vector<std::optional<T>>& values,
    memory::MemoryPool* pool) {
  auto vector = BaseVector::create<FlatVector<T>>(type, values.size(), pool);
  for (vector_size_t row = 0; row < values.size(); ++row) {
    if (values[row].has_value()) {
      vector->set(row, values[row].value());
    } else {
      vector->setNull(row, true);
    }
  }
  return vector;
}

RowVectorPtr makeRow(
    std::vector<VectorPtr> children,
    memory::MemoryPool* pool) {
  std::vector<std::string> names;
  std::vector<TypePtr> types;
  names.reserve(children.size());
  types.reserve(children.size());
  for (size_t index = 0; index < children.size(); ++index) {
    names.push_back("c" + std::to_string(index));
    types.push_back(children[index]->type());
  }
  return std::make_shared<RowVector>(
      pool,
      ROW(std::move(names), std::move(types)),
      nullptr,
      children.empty() ? 0 : children.front()->size(),
      std::move(children));
}

VectorPtr evaluateCall(
    const std::string& name,
    const TypePtr& returnType,
    const RowVectorPtr& input,
    core::ExecCtx& execCtx) {
  std::vector<core::TypedExprPtr> inputs;
  for (size_t index = 0; index < input->childrenSize(); ++index) {
    inputs.push_back(
        std::make_shared<core::FieldAccessTypedExpr>(
            input->childAt(index)->type(), "c" + std::to_string(index)));
  }
  auto call = std::make_shared<core::CallTypedExpr>(
      returnType, std::move(inputs), name);
  exec::ExprSet expression({call}, &execCtx);
  exec::EvalCtx context(&execCtx, &expression, input.get());
  SelectivityVector rows(input->size());
  std::vector<VectorPtr> result(1);
  expression.eval(rows, context, result);
  return result.front();
}

void testRegistryAndDefaultNulls(
    memory::MemoryPool* pool,
    core::ExecCtx& execCtx) {
  auto input = makeRow(
      {makeFlat<int64_t>(BIGINT(), {1, std::nullopt, 3, 100}, pool),
       makeFlat<int64_t>(BIGINT(), {2, 20, std::nullopt, -1}, pool)},
      pool);
  auto result = evaluateCall("add_i64", BIGINT(), input, execCtx);
  auto values = result->as<SimpleVector<int64_t>>();
  check(values != nullptr, "add_i64 returned a non-bigint vector");
  check(values->valueAt(0) == 3, "add_i64 returned the wrong first row");
  check(values->isNullAt(1), "add_i64 did not preserve a null argument");
  check(values->isNullAt(2), "add_i64 did not preserve a null argument");
  check(values->valueAt(3) == 99, "add_i64 returned the wrong last row");
}

void testScalarOverloads(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto integers = evaluateCall(
      "overloaded_add",
      BIGINT(),
      makeRow(
          {makeFlat<int64_t>(BIGINT(), {1, 20}, pool),
           makeFlat<int64_t>(BIGINT(), {2, 22}, pool)},
          pool),
      execCtx);
  auto integerValues = integers->as<SimpleVector<int64_t>>();
  check(
      integerValues != nullptr && integerValues->valueAt(0) == 3 &&
          integerValues->valueAt(1) == 42,
      "bigint overload returned wrong values");

  auto doubles = evaluateCall(
      "overloaded_add",
      DOUBLE(),
      makeRow(
          {makeFlat<double>(DOUBLE(), {1.5, 20.25}, pool),
           makeFlat<double>(DOUBLE(), {2.25, 22.5}, pool)},
          pool),
      execCtx);
  auto doubleValues = doubles->as<SimpleVector<double>>();
  check(
      doubleValues != nullptr && doubleValues->valueAt(0) == 3.75 &&
          doubleValues->valueAt(1) == 42.75,
      "double overload returned wrong values");
}

void testNullableStrings(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto first = makeFlat<StringView>(
      VARCHAR(), {StringView("a"), StringView("b"), StringView("c")}, pool);
  auto second = makeFlat<StringView>(
      VARCHAR(), {StringView("x"), std::nullopt, StringView("z")}, pool);
  auto input = makeRow({first, second}, pool);
  auto result = evaluateCall("prefix", VARCHAR(), input, execCtx);
  auto values = result->as<SimpleVector<StringView>>();
  check(values != nullptr, "prefix returned a non-varchar vector");
  check(values->valueAt(0).str() == "ax", "prefix returned the wrong row");
  check(values->isNullAt(1), "prefix did not return a nullable row");
  check(values->valueAt(2).str() == "cz", "prefix returned the wrong row");
}

void testComplexTypes(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto offsets = AlignedBuffer::allocate<vector_size_t>(3, pool);
  auto sizes = AlignedBuffer::allocate<vector_size_t>(3, pool);
  auto* rawOffsets = offsets->asMutable<vector_size_t>();
  auto* rawSizes = sizes->asMutable<vector_size_t>();
  rawOffsets[0] = 0;
  rawOffsets[1] = 2;
  rawOffsets[2] = 2;
  rawSizes[0] = 2;
  rawSizes[1] = 0;
  rawSizes[2] = 1;
  auto array = std::make_shared<ArrayVector>(
      pool,
      ARRAY(BIGINT()),
      nullptr,
      3,
      offsets,
      sizes,
      makeFlat<int64_t>(BIGINT(), {7, 8, 9}, pool));
  array->setNull(1, true);
  auto arrayResult = evaluateCall(
      "array_identity", ARRAY(BIGINT()), makeRow({array}, pool), execCtx);
  for (vector_size_t row = 0; row < 3; ++row) {
    check(arrayResult->equalValueAt(array.get(), row, row),
          "array_identity changed a complex row");
  }
  auto dictionaryIndices = AlignedBuffer::allocate<vector_size_t>(3, pool);
  dictionaryIndices->asMutable<vector_size_t>()[0] = 2;
  dictionaryIndices->asMutable<vector_size_t>()[1] = 0;
  dictionaryIndices->asMutable<vector_size_t>()[2] = 1;
  auto dictionary = BaseVector::wrapInDictionary(
      nullptr, dictionaryIndices, 3, array);
  auto dictionaryResult = evaluateCall(
      "array_identity", ARRAY(BIGINT()), makeRow({dictionary}, pool), execCtx);
  for (vector_size_t row = 0; row < 3; ++row) {
    check(dictionaryResult->equalValueAt(dictionary.get(), row, row),
          "array_identity changed a dictionary-encoded complex row");
  }

  auto mapOffsets = AlignedBuffer::allocate<vector_size_t>(2, pool);
  auto mapSizes = AlignedBuffer::allocate<vector_size_t>(2, pool);
  mapOffsets->asMutable<vector_size_t>()[0] = 0;
  mapOffsets->asMutable<vector_size_t>()[1] = 2;
  mapSizes->asMutable<vector_size_t>()[0] = 2;
  mapSizes->asMutable<vector_size_t>()[1] = 0;
  auto map = std::make_shared<MapVector>(
      pool,
      MAP(VARCHAR(), BIGINT()),
      nullptr,
      2,
      mapOffsets,
      mapSizes,
      makeFlat<StringView>(VARCHAR(), {StringView("a"), StringView("b")}, pool),
      makeFlat<int64_t>(BIGINT(), {10, 20}, pool));
  auto mapResult = evaluateCall(
      "map_identity", MAP(VARCHAR(), BIGINT()), makeRow({map}, pool), execCtx);
  for (vector_size_t row = 0; row < 2; ++row) {
    check(mapResult->equalValueAt(map.get(), row, row),
          "map_identity changed a complex row");
  }

  auto rowType = ROW({"a", "b"}, {BIGINT(), ARRAY(VARCHAR())});
  auto nestedOffsets = AlignedBuffer::allocate<vector_size_t>(2, pool);
  auto nestedSizes = AlignedBuffer::allocate<vector_size_t>(2, pool);
  nestedOffsets->asMutable<vector_size_t>()[0] = 0;
  nestedOffsets->asMutable<vector_size_t>()[1] = 2;
  nestedSizes->asMutable<vector_size_t>()[0] = 2;
  nestedSizes->asMutable<vector_size_t>()[1] = 1;
  auto strings = std::make_shared<ArrayVector>(
      pool,
      ARRAY(VARCHAR()),
      nullptr,
      2,
      nestedOffsets,
      nestedSizes,
      makeFlat<StringView>(
          VARCHAR(),
          {StringView("first"), StringView("second"), StringView("third")},
          pool));
  auto nestedRow = std::make_shared<RowVector>(
      pool,
      rowType,
      nullptr,
      2,
      std::vector<VectorPtr>{makeFlat<int64_t>(BIGINT(), {1, 2}, pool), strings});
  auto rowResult = evaluateCall(
      "row_identity", rowType, makeRow({nestedRow}, pool), execCtx);
  for (vector_size_t row = 0; row < 2; ++row) {
    check(rowResult->equalValueAt(nestedRow.get(), row, row),
          "row_identity changed a nested row");
  }
}

void testSparseGatherAndScatter(memory::MemoryPool* pool) {
  auto left = makeFlat<int64_t>(BIGINT(), {1, 20, 3, 40}, pool);
  auto right = makeFlat<int64_t>(BIGINT(), {10, 20, 30, 40}, pool);
  SelectivityVector rows(4, false);
  rows.setValid(0, true);
  rows.setValid(2, true);
  rows.updateBounds();

  auto gathered = gatherToArrowIpc(rows, {left, right}, pool);
  auto inputIpc = materialize(gathered.input);
  auto inputBuffer = std::make_shared<arrow::Buffer>(
      reinterpret_cast<const uint8_t*>(inputIpc.data()), inputIpc.size());
  auto inputStream = std::make_shared<arrow::io::BufferReader>(inputBuffer);
  auto reader =
      arrow::ipc::RecordBatchStreamReader::Open(inputStream).ValueOrDie();
  auto batch = reader->Next().ValueOrDie();
  auto gatheredLeft =
      std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
  auto gatheredRight =
      std::static_pointer_cast<arrow::Int64Array>(batch->column(1));
  check(
      !gatheredLeft->IsNull(0) && !gatheredLeft->IsNull(1) &&
          !gatheredRight->IsNull(0) && !gatheredRight->IsNull(1) &&
          gatheredLeft->Value(0) == 1 && gatheredLeft->Value(1) == 3 &&
          gatheredRight->Value(0) == 10 && gatheredRight->Value(1) == 30,
      "gather produced wrong input values");
  const auto& manifest = scalarManifest("add_i64");
  WasmInstance instance(
      WasmModule::compile(manifest.wasmPath), manifest.entrypoint);
  auto compact = decodeArrowIpcResult(
      instance.invoke(gathered.input), BIGINT(), gathered.rowCount, pool);
  auto compactValues = compact->as<SimpleVector<int64_t>>();
  check(
      !compactValues->isNullAt(0) && !compactValues->isNullAt(1) &&
          compactValues->valueAt(0) == 11 && compactValues->valueAt(1) == 33,
      "Wasm returned " + std::to_string(compactValues->valueAt(0)) + ", " +
          std::to_string(compactValues->valueAt(1)) + " before scatter");
  VectorPtr result = makeFlat<int64_t>(BIGINT(), {100, 200, 300, 400}, pool);
  scatterArrowResult(rows, compact, BIGINT(), pool, result);

  auto values = result->as<SimpleVector<int64_t>>();
  check(
      values->valueAt(0) == 11,
      "scatter returned " + std::to_string(values->valueAt(0)) +
          " for selected row 0");
  check(values->valueAt(1) == 200, "scatter overwrote an unselected row");
  check(
      values->valueAt(2) == 33,
      "scatter returned " + std::to_string(values->valueAt(2)) +
          " for selected row 2");
  check(values->valueAt(3) == 400, "scatter overwrote an unselected row");
}

void testLargeInputDirectWrite(memory::MemoryPool* pool) {
  constexpr int32_t kInputSize = 8 << 20;
  std::string bytes(kInputSize, 'x');
  auto input = makeFlat<StringView>(
      VARBINARY(), {StringView(bytes.data(), bytes.size())}, pool);
  SelectivityVector rows(1);
  auto gathered = gatherToArrowIpc(rows, {input}, pool);
  const auto& manifest = scalarManifest("binary_length");
  WasmInstance instance(
      WasmModule::compile(manifest.wasmPath), manifest.entrypoint);
  auto result =
      decodeArrowIpcResult(instance.invoke(gathered.input), INTEGER(), 1, pool);
  check(
      result->as<SimpleVector<int32_t>>()->valueAt(0) == kInputSize,
      "large Arrow IPC input was corrupted while writing linear memory");
}

void testCompiledModuleCache() {
  const auto& manifest = scalarManifest("add_i64");
  check(
      WasmModule::compile(manifest.wasmPath) ==
          WasmModule::compile(manifest.wasmPath),
      "compiled Wasm module was not reused");
}

void testScalarBusinessErrors(memory::MemoryPool* pool) {
  auto dividends = makeFlat<int64_t>(BIGINT(), {84, 20, 21}, pool);
  auto divisors = makeFlat<int64_t>(BIGINT(), {2, 0, 3}, pool);
  SelectivityVector rows(3);
  auto gathered = gatherToArrowIpc(rows, {dividends, divisors}, pool);
  const auto& manifest = scalarManifest("checked_divide");
  WasmInstance instance(
      WasmModule::compile(manifest.wasmPath), manifest.entrypoint);
  std::vector<std::optional<std::string>> errors;
  auto result = decodeArrowIpcResult(
      instance.invoke(gathered.input), BIGINT(), 3, pool, &errors);
  auto values = result->as<SimpleVector<int64_t>>();
  check(
      values->valueAt(0) == 42 && values->isNullAt(1) &&
          values->valueAt(2) == 7,
      "fallible scalar returned wrong values");
  check(
      !errors[0].has_value() && errors[1] == "division by zero" &&
          !errors[2].has_value(),
      "fallible scalar returned wrong per-row errors");
}

void testRegisteredScalarFunctions(
    memory::MemoryPool* pool,
    core::ExecCtx& execCtx) {
  auto boolean = evaluateCall(
      "not_bool",
      BOOLEAN(),
      makeRow({makeFlat<bool>(BOOLEAN(), {true, false}, pool)}, pool),
      execCtx);
  check(
      !boolean->as<SimpleVector<bool>>()->valueAt(0) &&
          boolean->as<SimpleVector<bool>>()->valueAt(1),
      "boolean batch returned wrong values");

  auto integers = evaluateCall(
      "widen",
      BIGINT(),
      makeRow(
          {makeFlat<int8_t>(TINYINT(), {1, -2}, pool),
           makeFlat<int16_t>(SMALLINT(), {20, 30}, pool),
           makeFlat<int32_t>(INTEGER(), {300, -400}, pool)},
          pool),
      execCtx);
  check(
      integers->as<SimpleVector<int64_t>>()->valueAt(0) == 321 &&
          integers->as<SimpleVector<int64_t>>()->valueAt(1) == -372,
      "integer batch returned wrong values");

  auto floating = evaluateCall(
      "hypotenuse",
      DOUBLE(),
      makeRow(
          {makeFlat<float>(REAL(), {3.0f, 5.0f}, pool),
           makeFlat<double>(DOUBLE(), {4.0, 12.0}, pool)},
          pool),
      execCtx);
  check(
      std::abs(floating->as<SimpleVector<double>>()->valueAt(0) - 5.0) <
              1e-12 &&
          std::abs(floating->as<SimpleVector<double>>()->valueAt(1) - 13.0) <
              1e-12,
      "floating-point batch returned wrong values");

  auto binary = evaluateCall(
      "binary_length",
      INTEGER(),
      makeRow(
          {makeFlat<StringView>(
              VARBINARY(), {StringView("abc"), StringView("de")}, pool)},
          pool),
      execCtx);
  check(
      binary->as<SimpleVector<int32_t>>()->valueAt(0) == 3 &&
          binary->as<SimpleVector<int32_t>>()->valueAt(1) == 2,
      "binary batch returned wrong values");

  auto dates = evaluateCall(
      "next_date",
      DATE(),
      makeRow({makeFlat<int32_t>(DATE(), {0, 20'000}, pool)}, pool),
      execCtx);
  check(
      dates->as<SimpleVector<int32_t>>()->valueAt(0) == 1 &&
          dates->as<SimpleVector<int32_t>>()->valueAt(1) == 20'001,
      "date batch returned wrong values");

  auto timestampValue = Timestamp::fromNanos(1'234'567'890);
  auto timestamps = evaluateCall(
      "timestamp_identity",
      TIMESTAMP(),
      makeRow({makeFlat<Timestamp>(TIMESTAMP(), {timestampValue}, pool)}, pool),
      execCtx);
  check(
      timestamps->as<SimpleVector<Timestamp>>()->valueAt(0) == timestampValue,
      "timestamp batch returned wrong values");
}

struct AggregateGroups {
  explicit AggregateGroups(int32_t count, int32_t offset)
      : storage(count, std::vector<char>(offset + sizeof(uint32_t), 0)),
        pointers(count) {
    for (int32_t i = 0; i < count; ++i) {
      pointers[i] = storage[i].data();
    }
  }

  std::vector<std::vector<char>> storage;
  std::vector<char*> pointers;
};

std::unique_ptr<exec::Aggregate> makeAggregate(
    const std::string& name,
    core::AggregationNode::Step step,
    const TypePtr& argumentType,
    const TypePtr& resultType,
    HashStringAllocator& allocator,
    int32_t offset) {
  core::QueryConfig config({});
  auto aggregate =
      exec::Aggregate::create(name, step, {argumentType}, resultType, config);
  aggregate->setAllocator(&allocator);
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      0);
  return aggregate;
}

void initializeGroups(exec::Aggregate& aggregate, AggregateGroups& groups) {
  std::vector<vector_size_t> indices(groups.pointers.size());
  std::iota(indices.begin(), indices.end(), 0);
  aggregate.initializeNewGroups(groups.pointers.data(), indices);
}

void destroyGroups(exec::Aggregate& aggregate, AggregateGroups& groups) {
  aggregate.destroy(
      folly::Range<char**>(
          groups.pointers.data(),
          groups.pointers.data() + groups.pointers.size()));
}

void testAveragePartialMerge(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1; // Deliberately exercises unaligned handles.
  auto input =
      makeFlat<double>(DOUBLE(), {1.0, std::nullopt, 3.0, 10.0, 20.0}, pool);
  SelectivityVector rows(input->size());

  HashStringAllocator partialAllocator(pool);
  auto partial = makeAggregate(
      "avg_f64",
      core::AggregationNode::Step::kPartial,
      DOUBLE(),
      VARBINARY(),
      partialAllocator,
      kOffset);
  AggregateGroups partialGroups(3, kOffset);
  initializeGroups(*partial, partialGroups);
  std::vector<char*> inputGroups{
      partialGroups.pointers[0],
      partialGroups.pointers[0],
      partialGroups.pointers[0],
      partialGroups.pointers[1],
      partialGroups.pointers[1]};
  partial->addRawInput(inputGroups.data(), rows, {input}, false);
  VectorPtr intermediate;
  partial->extractAccumulators(
      partialGroups.pointers.data(),
      partialGroups.pointers.size(),
      &intermediate);
  auto serialized = intermediate->as<SimpleVector<StringView>>();
  check(serialized != nullptr, "average intermediate is not varbinary");
  check(
      serialized->valueAt(0).size() == 16 &&
          serialized->valueAt(1).size() == 16 &&
          serialized->valueAt(2).size() == 16,
      "average business serialization was not preserved");
  const auto firstIntermediate = serialized->valueAt(0);
  const auto secondIntermediate = serialized->valueAt(1);
  check(
      folly::loadUnaligned<uint64_t>(firstIntermediate.data() + 8) == 2 &&
          folly::loadUnaligned<uint64_t>(secondIntermediate.data() + 8) == 2,
      "average update produced counts " +
          std::to_string(
              folly::loadUnaligned<uint64_t>(firstIntermediate.data() + 8)) +
          ", " +
          std::to_string(
              folly::loadUnaligned<uint64_t>(secondIntermediate.data() + 8)));
  destroyGroups(*partial, partialGroups);

  HashStringAllocator finalAllocator(pool);
  auto final = makeAggregate(
      "avg_f64",
      core::AggregationNode::Step::kSingle,
      DOUBLE(),
      DOUBLE(),
      finalAllocator,
      kOffset);
  AggregateGroups finalGroups(3, kOffset);
  initializeGroups(*final, finalGroups);
  SelectivityVector intermediateRows(3);
  final->addIntermediateResults(
      finalGroups.pointers.data(), intermediateRows, {intermediate}, false);
  VectorPtr result;
  final->extractValues(finalGroups.pointers.data(), 3, &result);
  auto values = result->as<SimpleVector<double>>();
  check(
      std::abs(values->valueAt(0) - 2.0) < 1e-12 &&
          std::abs(values->valueAt(1) - 15.0) < 1e-12 && values->isNullAt(2),
      "average update/serialize/merge/finalize returned " +
          std::to_string(values->valueAt(0)) + ", " +
          std::to_string(values->valueAt(1)) +
          ", null=" + std::to_string(values->isNullAt(2)));
  VectorPtr repeated;
  final->extractValues(finalGroups.pointers.data(), 3, &repeated);
  check(
      repeated->equalValueAt(result.get(), 0, 0) &&
          repeated->equalValueAt(result.get(), 1, 1) && repeated->isNullAt(2),
      "average finalize mutated guest state");
  destroyGroups(*final, finalGroups);
}

void testSumGroups(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1;
  HashStringAllocator allocator(pool);
  auto aggregate = makeAggregate(
      "sum_i64",
      core::AggregationNode::Step::kSingle,
      BIGINT(),
      BIGINT(),
      allocator,
      kOffset);
  AggregateGroups groups(3, kOffset);
  initializeGroups(*aggregate, groups);
  auto input = makeFlat<int64_t>(BIGINT(), {5, std::nullopt, 7, -2, 40}, pool);
  std::vector<char*> inputGroups{
      groups.pointers[0],
      groups.pointers[0],
      groups.pointers[0],
      groups.pointers[1],
      groups.pointers[1]};
  SelectivityVector rows(input->size());
  aggregate->addRawInput(inputGroups.data(), rows, {input}, false);
  VectorPtr result;
  aggregate->extractValues(groups.pointers.data(), 3, &result);
  auto values = result->as<SimpleVector<int64_t>>();
  check(
      values->valueAt(0) == 12 && values->valueAt(1) == 38 &&
          values->isNullAt(2),
      "sum UDAF returned wrong grouped values");
  destroyGroups(*aggregate, groups);
}

void testArraySum(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1;
  HashStringAllocator allocator(pool);
  auto aggregate = makeAggregate(
      "array_sum",
      core::AggregationNode::Step::kSingle,
      ARRAY(BIGINT()),
      BIGINT(),
      allocator,
      kOffset);
  AggregateGroups groups(1, kOffset);
  initializeGroups(*aggregate, groups);
  auto offsets = AlignedBuffer::allocate<vector_size_t>(3, pool);
  auto sizes = AlignedBuffer::allocate<vector_size_t>(3, pool);
  offsets->asMutable<vector_size_t>()[0] = 0;
  offsets->asMutable<vector_size_t>()[1] = 2;
  offsets->asMutable<vector_size_t>()[2] = 2;
  sizes->asMutable<vector_size_t>()[0] = 2;
  sizes->asMutable<vector_size_t>()[1] = 0;
  sizes->asMutable<vector_size_t>()[2] = 1;
  auto input = std::make_shared<ArrayVector>(
      pool,
      ARRAY(BIGINT()),
      nullptr,
      3,
      offsets,
      sizes,
      makeFlat<int64_t>(BIGINT(), {7, 8, 9}, pool));
  input->setNull(1, true);
  SelectivityVector rows(3);
  aggregate->addSingleGroupRawInput(
      groups.pointers[0], rows, {input}, false);
  VectorPtr result;
  aggregate->extractValues(groups.pointers.data(), 1, &result);
  check(
      result->as<SimpleVector<int64_t>>()->valueAt(0) == 24,
      "array_sum returned the wrong result");
  destroyGroups(*aggregate, groups);
}

void testArrayCollect(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1;
  HashStringAllocator allocator(pool);
  auto aggregate = makeAggregate(
      "array_collect",
      core::AggregationNode::Step::kSingle,
      BIGINT(),
      ARRAY(BIGINT()),
      allocator,
      kOffset);
  AggregateGroups groups(2, kOffset);
  initializeGroups(*aggregate, groups);
  auto input = makeFlat<int64_t>(BIGINT(), {3, std::nullopt, 5}, pool);
  std::vector<char*> inputGroups{
      groups.pointers[0], groups.pointers[0], groups.pointers[0]};
  SelectivityVector rows(input->size());
  aggregate->addRawInput(inputGroups.data(), rows, {input}, false);
  VectorPtr result;
  aggregate->extractValues(groups.pointers.data(), 2, &result);
  auto* lists = result->as<ArrayVector>();
  check(lists != nullptr, "array_collect returned a non-array vector");
  check(lists->sizeAt(0) == 2 && lists->sizeAt(1) == 0,
        "array_collect returned wrong list lengths");
  auto* elements = lists->elements()->as<SimpleVector<int64_t>>();
  check(elements != nullptr &&
            elements->valueAt(lists->offsetAt(0)) == 3 &&
            elements->valueAt(lists->offsetAt(0) + 1) == 5,
        "array_collect returned wrong list elements");
  destroyGroups(*aggregate, groups);

  HashStringAllocator partialAllocator(pool);
  auto partial = makeAggregate(
      "array_collect",
      core::AggregationNode::Step::kPartial,
      BIGINT(),
      VARBINARY(),
      partialAllocator,
      kOffset);
  AggregateGroups partialGroups(1, kOffset);
  initializeGroups(*partial, partialGroups);
  partial->addSingleGroupRawInput(
      partialGroups.pointers[0], rows, {input}, false);
  VectorPtr intermediate;
  partial->extractAccumulators(
      partialGroups.pointers.data(), 1, &intermediate);
  destroyGroups(*partial, partialGroups);

  HashStringAllocator finalAllocator(pool);
  auto final = makeAggregate(
      "array_collect",
      core::AggregationNode::Step::kFinal,
      VARBINARY(),
      ARRAY(BIGINT()),
      finalAllocator,
      kOffset);
  AggregateGroups finalGroups(1, kOffset);
  initializeGroups(*final, finalGroups);
  SelectivityVector intermediateRows(1);
  final->addSingleGroupIntermediateResults(
      finalGroups.pointers[0], intermediateRows, {intermediate}, false);
  VectorPtr mergedResult;
  final->extractValues(finalGroups.pointers.data(), 1, &mergedResult);
  check(mergedResult->equalValueAt(result.get(), 0, 0),
        "array_collect partial/final changed the complex result");
  destroyGroups(*final, finalGroups);
}

void testSingleGroupAggregate(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1;
  HashStringAllocator partialAllocator(pool);
  auto partial = makeAggregate(
      "sum_i64",
      core::AggregationNode::Step::kPartial,
      BIGINT(),
      VARBINARY(),
      partialAllocator,
      kOffset);
  AggregateGroups partialGroups(1, kOffset);
  initializeGroups(*partial, partialGroups);

  auto input = makeFlat<int64_t>(BIGINT(), {5, std::nullopt, 7, -2}, pool);
  SelectivityVector rows(input->size());
  partial->addSingleGroupRawInput(
      partialGroups.pointers[0], rows, {input}, false);
  VectorPtr firstIntermediate;
  VectorPtr repeatedIntermediate;
  partial->extractAccumulators(
      partialGroups.pointers.data(), 1, &firstIntermediate);
  partial->extractAccumulators(
      partialGroups.pointers.data(), 1, &repeatedIntermediate);
  check(
      repeatedIntermediate->equalValueAt(firstIntermediate.get(), 0, 0),
      "repeated accumulator extraction changed serialized state");

  auto moreInput = makeFlat<int64_t>(BIGINT(), {32}, pool);
  SelectivityVector moreRows(1);
  partial->addSingleGroupRawInput(
      partialGroups.pointers[0], moreRows, {moreInput}, false);
  VectorPtr intermediate;
  partial->extractAccumulators(partialGroups.pointers.data(), 1, &intermediate);
  destroyGroups(*partial, partialGroups);

  HashStringAllocator finalAllocator(pool);
  auto final = makeAggregate(
      "sum_i64",
      core::AggregationNode::Step::kFinal,
      VARBINARY(),
      BIGINT(),
      finalAllocator,
      kOffset);
  AggregateGroups finalGroups(1, kOffset);
  initializeGroups(*final, finalGroups);
  SelectivityVector intermediateRows(1);
  final->addSingleGroupIntermediateResults(
      finalGroups.pointers[0], intermediateRows, {intermediate}, false);
  VectorPtr result;
  final->extractValues(finalGroups.pointers.data(), 1, &result);
  check(
      result->as<SimpleVector<int64_t>>()->valueAt(0) == 42,
      "single-group update/merge returned the wrong result");
  destroyGroups(*final, finalGroups);
}

void testLongestStringGroups(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1;
  HashStringAllocator allocator(pool);
  auto aggregate = makeAggregate(
      "longest_string",
      core::AggregationNode::Step::kSingle,
      VARCHAR(),
      VARCHAR(),
      allocator,
      kOffset);
  AggregateGroups groups(3, kOffset);
  initializeGroups(*aggregate, groups);
  auto input = makeFlat<StringView>(
      VARCHAR(),
      {StringView("a"),
       StringView("this is the longest string"),
       std::nullopt,
       StringView("xy"),
       StringView("hello")},
      pool);
  std::vector<char*> inputGroups{
      groups.pointers[0],
      groups.pointers[0],
      groups.pointers[0],
      groups.pointers[1],
      groups.pointers[1]};
  SelectivityVector rows(input->size());
  aggregate->addRawInput(inputGroups.data(), rows, {input}, false);
  VectorPtr result;
  aggregate->extractValues(groups.pointers.data(), 3, &result);
  auto values = result->as<SimpleVector<StringView>>();
  check(
      values->valueAt(0).str() == "this is the longest string" &&
          values->valueAt(1).str() == "hello" && values->isNullAt(2),
      "longest-string UDAF returned wrong grouped values");
  destroyGroups(*aggregate, groups);
}

} // namespace
} // namespace facebook::velox::functions::wasm::test

int main() {
  using namespace facebook::velox;
  using namespace facebook::velox::functions::wasm;
  using namespace facebook::velox::functions::wasm::test;
  try {
    memory::MemoryManager::initialize(memory::MemoryManager::Options{});
    auto pool = memory::memoryManager()->addLeafPool("wasm-scalar-smoke");
    auto queryCtx = core::QueryCtx::create();
    core::ExecCtx execCtx(pool.get(), queryCtx.get());

    check(
        registerWasmModule(WASM_MODULE_PATH) == 23,
        "cannot register Wasm module");
    testRegistryAndDefaultNulls(pool.get(), execCtx);
    testScalarOverloads(pool.get(), execCtx);
    testNullableStrings(pool.get(), execCtx);
    testComplexTypes(pool.get(), execCtx);
    testSparseGatherAndScatter(pool.get());
    testLargeInputDirectWrite(pool.get());
    testCompiledModuleCache();
    testScalarBusinessErrors(pool.get());
    testRegisteredScalarFunctions(pool.get(), execCtx);
    testAveragePartialMerge(pool.get());
    testSumGroups(pool.get());
    testArraySum(pool.get());
    testArrayCollect(pool.get());
    testSingleGroupAggregate(pool.get());
    testLongestStringGroups(pool.get());
    std::cout << "WebAssembly scalar UDF and UDAF smoke test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "WebAssembly scalar UDF smoke test failed: " << error.what()
              << '\n';
    return 1;
  }
}
