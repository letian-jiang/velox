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
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <folly/lang/Bits.h>

#include "velox/common/base/BitUtil.h"
#include "velox/common/memory/Memory.h"
#include "velox/core/Expressions.h"
#include "velox/core/QueryCtx.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/RowContainer.h"
#include "velox/expression/Expr.h"
#include "velox/functions/java/ArrowIpc.h"
#include "velox/functions/java/Manifest.h"
#include "velox/functions/java/Registration.h"
#include "velox/functions/java/Runtime.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::functions::java::test {
namespace {

void check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string readFile(const std::string& path) {
  std::ifstream input(path);
  check(input.good(), "cannot read " + path);
  return std::string(
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string javaClasspath() {
  auto dependencies = readFile(JAVA_DEPENDENCY_CLASSPATH);
  while (!dependencies.empty() &&
         (dependencies.back() == '\n' || dependencies.back() == '\r')) {
    dependencies.pop_back();
  }
#ifdef _WIN32
  constexpr char kClasspathSeparator = ';';
#else
  constexpr char kClasspathSeparator = ':';
#endif
  return std::string(JAVA_RUNTIME_JAR) + kClasspathSeparator + dependencies;
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

void testDefaultNulls(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto input = makeRow(
      {makeFlat<int64_t>(BIGINT(), {1, std::nullopt, 3, 100}, pool),
       makeFlat<int64_t>(BIGINT(), {2, 20, std::nullopt, -1}, pool)},
      pool);
  auto result = evaluateCall("java_add", BIGINT(), input, execCtx);
  auto values = result->as<SimpleVector<int64_t>>();
  check(values != nullptr, "java_add returned a non-bigint vector");
  check(values->valueAt(0) == 3, "java_add returned the wrong first row");
  check(values->isNullAt(1), "java_add did not preserve a null argument");
  check(values->isNullAt(2), "java_add did not preserve a null argument");
  check(values->valueAt(3) == 99, "java_add returned the wrong last row");
}

void testNullableStrings(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto first = makeFlat<StringView>(
      VARCHAR(), {StringView("a"), StringView("b"), StringView("c")}, pool);
  auto second = makeFlat<StringView>(
      VARCHAR(), {StringView("x"), std::nullopt, StringView("z")}, pool);
  auto result = evaluateCall(
      "java_prefix", VARCHAR(), makeRow({first, second}, pool), execCtx);
  auto values = result->as<SimpleVector<StringView>>();
  check(values != nullptr, "java_prefix returned a non-varchar vector");
  check(values->valueAt(0).str() == "ax", "java_prefix returned wrong row 0");
  check(values->isNullAt(1), "java_prefix did not return a nullable row");
  check(values->valueAt(2).str() == "cz", "java_prefix returned wrong row 2");
}

void testBusinessErrors(memory::MemoryPool* pool) {
  auto dividends = makeFlat<int64_t>(BIGINT(), {84, 20, 21}, pool);
  auto divisors = makeFlat<int64_t>(BIGINT(), {2, 0, 3}, pool);
  SelectivityVector rows(3);
  auto gathered = gatherToArrowIpc(rows, {dividends, divisors}, pool);
  auto manifest = loadScalarManifest(JAVA_CHECKED_DIVIDE_MANIFEST);
  JavaInstance instance(manifest.jarPath, manifest.implementationClass, false);
  std::vector<std::optional<std::string>> errors;
  auto result = decodeArrowIpcResult(
      instance.invokeScalar(gathered.ipc), BIGINT(), 3, pool, &errors);
  auto values = result->as<SimpleVector<int64_t>>();
  check(
      values->valueAt(0) == 42 && values->isNullAt(1) &&
          values->valueAt(2) == 7,
      "fallible Java scalar returned wrong values");
  check(
      !errors[0].has_value() && errors[1] == "division by zero" &&
          !errors[2].has_value(),
      "fallible Java scalar returned wrong per-row errors");
}

void testNestedComplexTypes(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto scoreOffsets = allocateOffsets(2, pool);
  auto scoreSizes = allocateSizes(2, pool);
  scoreOffsets->asMutable<vector_size_t>()[0] = 0;
  scoreOffsets->asMutable<vector_size_t>()[1] = 3;
  scoreSizes->asMutable<vector_size_t>()[0] = 3;
  scoreSizes->asMutable<vector_size_t>()[1] = 0;
  auto scores = std::make_shared<ArrayVector>(
      pool,
      ARRAY(BIGINT()),
      nullptr,
      2,
      scoreOffsets,
      scoreSizes,
      makeFlat<int64_t>(BIGINT(), {1, std::nullopt, 3}, pool));

  auto mapOffsets = allocateOffsets(2, pool);
  auto mapSizes = allocateSizes(2, pool);
  mapOffsets->asMutable<vector_size_t>()[0] = 0;
  mapOffsets->asMutable<vector_size_t>()[1] = 2;
  mapSizes->asMutable<vector_size_t>()[0] = 2;
  mapSizes->asMutable<vector_size_t>()[1] = 0;
  auto labels = std::make_shared<MapVector>(
      pool,
      MAP(VARCHAR(), BIGINT()),
      nullptr,
      2,
      mapOffsets,
      mapSizes,
      makeFlat<StringView>(VARCHAR(), {StringView("x"), StringView("y")}, pool),
      makeFlat<int64_t>(BIGINT(), {10, std::nullopt}, pool));

  auto profileNulls = allocateNulls(2, pool, bits::kNotNull);
  bits::setNull(profileNulls->asMutable<uint64_t>(), 1, true);
  auto profileType =
      ROW({"name", "scores", "labels"},
          {VARCHAR(), ARRAY(BIGINT()), MAP(VARCHAR(), BIGINT())});
  auto profiles = std::make_shared<RowVector>(
      pool,
      profileType,
      profileNulls,
      2,
      std::vector<VectorPtr>{
          makeFlat<StringView>(
              VARCHAR(), {StringView("alice"), StringView("ignored")}, pool),
          scores,
          labels});

  auto result = evaluateCall(
      "java_profile_transform",
      profileType,
      makeRow({profiles}, pool),
      execCtx);
  auto rows = result->as<RowVector>();
  check(rows != nullptr, "complex Java scalar returned a non-row vector");
  check(!rows->isNullAt(0) && rows->isNullAt(1), "row nullability was lost");
  auto names = rows->childAt(0)->as<SimpleVector<StringView>>();
  check(names->valueAt(0).str() == "ALICE", "row varchar field was corrupted");

  auto outputScores = rows->childAt(1)->as<ArrayVector>();
  auto outputScoreValues =
      outputScores->elements()->as<SimpleVector<int64_t>>();
  const auto scoreStart = outputScores->offsetAt(0);
  check(
      outputScores->sizeAt(0) == 3 &&
          outputScoreValues->valueAt(scoreStart) == 3 &&
          outputScoreValues->isNullAt(scoreStart + 1) &&
          outputScoreValues->valueAt(scoreStart + 2) == 1,
      "nested array values or element nullability were corrupted");

  auto outputLabels = rows->childAt(2)->as<MapVector>();
  auto outputKeys = outputLabels->mapKeys()->as<SimpleVector<StringView>>();
  auto outputValues = outputLabels->mapValues()->as<SimpleVector<int64_t>>();
  const auto mapStart = outputLabels->offsetAt(0);
  check(
      outputLabels->sizeAt(0) == 2 &&
          outputKeys->valueAt(mapStart).str() == "x" &&
          outputValues->valueAt(mapStart) == 11 &&
          outputKeys->valueAt(mapStart + 1).str() == "y" &&
          outputValues->isNullAt(mapStart + 1),
      "nested map values or value nullability were corrupted");
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
  constexpr int32_t kOffset = 1;
  auto input =
      makeFlat<double>(DOUBLE(), {1.0, std::nullopt, 3.0, 10.0, 20.0}, pool);
  SelectivityVector rows(input->size());

  HashStringAllocator partialAllocator(pool);
  auto partial = makeAggregate(
      "java_avg",
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
  check(serialized != nullptr, "Java average intermediate is not varbinary");
  check(
      serialized->valueAt(0).size() == 16 &&
          serialized->valueAt(1).size() == 16 &&
          serialized->valueAt(2).size() == 16,
      "Java average serialization has the wrong size");
  check(
      folly::loadUnaligned<uint64_t>(serialized->valueAt(0).data() + 8) == 2 &&
          folly::loadUnaligned<uint64_t>(serialized->valueAt(1).data() + 8) ==
              2,
      "Java average update produced wrong counts");
  destroyGroups(*partial, partialGroups);

  HashStringAllocator finalAllocator(pool);
  auto final = makeAggregate(
      "java_avg",
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
      "Java average update/serialize/merge/finish returned wrong values");
  VectorPtr repeated;
  final->extractValues(finalGroups.pointers.data(), 3, &repeated);
  check(
      repeated->equalValueAt(result.get(), 0, 0) &&
          repeated->equalValueAt(result.get(), 1, 1) && repeated->isNullAt(2),
      "Java average finish mutated state");
  destroyGroups(*final, finalGroups);
}

void testGroupedSum(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1;
  HashStringAllocator allocator(pool);
  auto aggregate = makeAggregate(
      "java_sum_long",
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
      "Java sum returned wrong grouped values");
  destroyGroups(*aggregate, groups);
}

void testSingleGroupPartialFinal(memory::MemoryPool* pool) {
  constexpr int32_t kOffset = 1;
  HashStringAllocator partialAllocator(pool);
  auto partial = makeAggregate(
      "java_sum_long",
      core::AggregationNode::Step::kPartial,
      BIGINT(),
      VARBINARY(),
      partialAllocator,
      kOffset);
  AggregateGroups partialGroups(1, kOffset);
  initializeGroups(*partial, partialGroups);
  auto input = makeFlat<int64_t>(BIGINT(), {5, std::nullopt, 7, -2, 32}, pool);
  SelectivityVector rows(input->size());
  partial->addSingleGroupRawInput(
      partialGroups.pointers[0], rows, {input}, false);
  VectorPtr intermediate;
  partial->extractAccumulators(partialGroups.pointers.data(), 1, &intermediate);
  destroyGroups(*partial, partialGroups);

  HashStringAllocator finalAllocator(pool);
  auto final = makeAggregate(
      "java_sum_long",
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
      "Java single-group partial/final returned the wrong value");
  destroyGroups(*final, finalGroups);
}

} // namespace
} // namespace facebook::velox::functions::java::test

int main() {
  using namespace facebook::velox;
  using namespace facebook::velox::functions::java;
  using namespace facebook::velox::functions::java::test;
  try {
    initializeJavaUdfRuntime({"-Djava.class.path=" + javaClasspath()});
    memory::MemoryManager::initialize(memory::MemoryManager::Options{});
    auto pool = memory::memoryManager()->addLeafPool("java-udf-smoke");
    auto queryCtx = core::QueryCtx::create();
    core::ExecCtx execCtx(pool.get(), queryCtx.get());

    check(
        registerJavaScalarFunction(JAVA_ADD_MANIFEST),
        "cannot register java_add");
    check(
        registerJavaScalarFunction(JAVA_PREFIX_MANIFEST),
        "cannot register java_prefix");
    check(
        registerJavaScalarFunction(JAVA_CHECKED_DIVIDE_MANIFEST),
        "cannot register java_checked_divide");
    check(
        registerJavaScalarFunction(JAVA_PROFILE_TRANSFORM_MANIFEST),
        "cannot register java_profile_transform");
    check(
        registerJavaAggregateFunction(JAVA_AVERAGE_MANIFEST),
        "cannot register java_avg");
    check(
        registerJavaAggregateFunction(JAVA_SUM_LONG_MANIFEST),
        "cannot register java_sum_long");

    testDefaultNulls(pool.get(), execCtx);
    testNullableStrings(pool.get(), execCtx);
    testBusinessErrors(pool.get());
    testNestedComplexTypes(pool.get(), execCtx);
    testAveragePartialMerge(pool.get());
    testGroupedSum(pool.get());
    testSingleGroupPartialFinal(pool.get());
    std::cout << "Java UDF smoke test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Java UDF smoke test failed: " << error.what() << '\n';
    return 1;
  }
}
