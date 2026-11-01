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

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include <set>

#include "velox/benchmarks/ExpressionBenchmarkBuilder.h"
#include "velox/functions/prestosql/ArrayFunctions.h"

using namespace facebook::velox;

DEFINE_bool(validate_only, false, "Check results and order without timing");
DEFINE_int32(variant_order, 0, "Choose which implementation is measured first");

namespace {

constexpr vector_size_t kBatchSize = 256;

// Original two-pass implementation used as a performance baseline.
template <typename TExecParams, typename T>
struct ArrayFrequencyBaseline {
  VELOX_DEFINE_FUNCTION_TYPES(TExecParams);

  FOLLY_ALWAYS_INLINE void call(
      out_type<Map<T, int>>& out,
      arg_type<Array<T>> inputArray) {
    frequencyCount_.clear();

    for (const auto& item : inputArray.skipNulls()) {
      frequencyCount_[item]++;
    }

    // To make the output order of key value pairs deterministic (since F14
    // does not provide ordering guarantees), we do another iteration in the
    // input and look up element frequencies in the F14 map. To prevent
    // duplicates in the output, we remove the keys we already added.
    for (const auto& item : inputArray.skipNulls()) {
      auto it = frequencyCount_.find(item);
      if (it != frequencyCount_.end()) {
        auto [keyWriter, valueWriter] = out.add_item();
        keyWriter = item;
        valueWriter = it->second;
        frequencyCount_.erase(it);
      }
    }
  }

 private:
  folly::F14FastMap<arg_type<T>, int> frequencyCount_;
};

template <typename T>
void registerFunctions() {
  registerFunction<
      ParameterBinder<ArrayFrequencyBaseline, T>,
      Map<T, int32_t>,
      Array<T>>({"array_frequency_baseline"});
  registerFunction<
      ParameterBinder<functions::ArrayFrequencyFunction, T>,
      Map<T, int32_t>,
      Array<T>>({"array_frequency"});
}

void verifyResults(
    functions::test::FunctionBenchmarkBase& benchmark,
    const RowVectorPtr& input) {
  const auto baseline =
      benchmark.evaluate("array_frequency_baseline(c0)", input);
  const std::vector<std::string> functions = {"array_frequency"};
  for (const auto& function : functions) {
    auto actual = benchmark.evaluate(function + "(c0)", input);
    // Verify physical key order as well as map equality, before and after
    // recursively flattening the output.
    for (bool flatten : {false, true}) {
      if (flatten) {
        BaseVector::flattenVector(actual);
      }
      DecodedVector expectedMaps(*baseline);
      DecodedVector actualMaps(*actual);
      const auto* expectedMap = expectedMaps.base()->as<MapVector>();
      const auto* actualMap = actualMaps.base()->as<MapVector>();
      for (vector_size_t row = 0; row < input->size(); ++row) {
        VELOX_CHECK(baseline->equalValueAt(actual.get(), row, row));
        if (baseline->isNullAt(row)) {
          continue;
        }
        const auto expectedRow = expectedMaps.index(row);
        const auto actualRow = actualMaps.index(row);
        VELOX_CHECK_EQ(
            expectedMap->sizeAt(expectedRow), actualMap->sizeAt(actualRow));
        for (vector_size_t index = 0; index < expectedMap->sizeAt(expectedRow);
             ++index) {
          const auto expectedOffset =
              expectedMap->offsetAt(expectedRow) + index;
          const auto actualOffset = actualMap->offsetAt(actualRow) + index;
          VELOX_CHECK(expectedMap->mapKeys()->equalValueAt(
              actualMap->mapKeys().get(), expectedOffset, actualOffset));
          VELOX_CHECK(expectedMap->mapValues()->equalValueAt(
              actualMap->mapValues().get(), expectedOffset, actualOffset));
        }
      }
    }
  }
}

void addCase(
    ExpressionBenchmarkBuilder& builder,
    const std::string& name,
    const ArrayVectorPtr& array) {
  auto input = builder.vectorMaker().rowVector({array});
  verifyResults(builder, input);
  std::vector<std::pair<std::string, std::string>> variants = {
      {"baseline", "array_frequency_baseline"}, {"indexed", "array_frequency"}};
  if (FLAGS_variant_order != 0) {
    std::reverse(variants.begin(), variants.end());
  }
  // Keep each timed invocation short enough for repeated measurements even
  // for the largest arrays. Each iteration evaluates a 256-row batch.
  auto& benchmarkSet = builder.addBenchmarkSet(name, input).withIterations(10);
  for (const auto& [label, function] : variants) {
    benchmarkSet.addExpression(label, function + "(c0)");
  }
}

void addCases(ExpressionBenchmarkBuilder& builder) {
  auto& maker = builder.vectorMaker();
  for (vector_size_t length : {8, 32, 128, 1024}) {
    const std::set<vector_size_t> cardinalities = {1, 8, length / 2, length};
    for (auto cardinality : cardinalities) {
      std::vector<std::string> shortKeys;
      std::vector<std::string> longKeys;
      for (vector_size_t key = 0; key < cardinality; ++key) {
        shortKeys.push_back(fmt::format("{:08x}", key));
        longKeys.push_back(std::string(56, 'x') + shortKeys.back());
      }
      for (bool withNulls : {false, true}) {
        const auto suffix = fmt::format(
            "_n{}_k{}_null{}", length, cardinality, withNulls ? 20 : 0);
        auto keyAt = [=](vector_size_t index) {
          const auto row = index / length;
          const auto position = index % length;
          return (position * 13 + row * 17) % cardinality;
        };
        auto isNullAt = [=](vector_size_t index) {
          return withNulls &&
              (static_cast<uint32_t>(index) * 1103515245u + 12345u) % 5 == 0;
        };
        auto sizeAt = [=](vector_size_t) { return length; };

        addCase(
            builder,
            "bigint" + suffix,
            maker.arrayVector<int64_t>(
                kBatchSize,
                sizeAt,
                [=](vector_size_t index) { return int64_t(keyAt(index)); },
                nullptr,
                isNullAt));
        addCase(
            builder,
            "varchar8" + suffix,
            maker.arrayVector<StringView>(
                kBatchSize,
                sizeAt,
                [&](vector_size_t index) {
                  return StringView(shortKeys[keyAt(index)]);
                },
                nullptr,
                isNullAt));
        addCase(
            builder,
            "varchar64" + suffix,
            maker.arrayVector<StringView>(
                kBatchSize,
                sizeAt,
                [&](vector_size_t index) {
                  return StringView(longKeys[keyAt(index)]);
                },
                nullptr,
                isNullAt));
      }
    }
  }

  auto empty = maker.arrayVector<int64_t>({{}, {}, {}});
  verifyResults(builder, maker.rowVector({empty}));
  auto allNull = maker.arrayVector<int64_t>(
      3,
      [](vector_size_t) { return 8; },
      [](vector_size_t index) { return int64_t(index); },
      nullptr,
      [](vector_size_t) { return true; });
  allNull->setNull(1, true);
  verifyResults(builder, maker.rowVector({allNull}));
}

} // namespace

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  VELOX_CHECK_GE(FLAGS_variant_order, 0);
  VELOX_CHECK_LT(FLAGS_variant_order, 2);
  memory::MemoryManager::initialize({});
  registerFunctions<int64_t>();
  registerFunctions<Varchar>();

  ExpressionBenchmarkBuilder builder;
  addCases(builder);
  builder.testBenchmarks();
  if (!FLAGS_validate_only) {
    // Use the standard evaluation, flattening and result reuse timing.
    builder.registerBenchmarks();
    folly::runBenchmarks();
  }
  return 0;
}
