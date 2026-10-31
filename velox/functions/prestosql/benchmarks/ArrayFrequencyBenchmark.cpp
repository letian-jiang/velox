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
DEFINE_bool(indexed_first, false, "Measure the indexed implementation first");

namespace {

constexpr vector_size_t kBatchSize = 256;

template <typename TExec, typename T>
struct ArrayFrequencyBaseline : functions::ArrayFrequencyFunction<TExec, T> {};

template <typename TExec, typename T>
struct ArrayFrequencyIndexed {
  VELOX_DEFINE_FUNCTION_TYPES(TExec);

  struct Count {
    int32_t frequency;
    vector_size_t index;
  };

  FOLLY_ALWAYS_INLINE void call(
      out_type<Map<T, int32_t>>& out,
      arg_type<Array<T>> input) {
    counts_.clear();
    for (const auto& item : input.skipNulls()) {
      auto [iterator, inserted] = counts_.try_emplace(
          item, Count{0, static_cast<vector_size_t>(counts_.size())});
      ++iterator->second.frequency;
    }

    out.resize(counts_.size());
    for (const auto& [key, count] : counts_) {
      out[count.index] = std::make_tuple(key, count.frequency);
    }
  }

 private:
  folly::F14FastMap<arg_type<T>, Count> counts_;
};

template <typename T>
void registerFunctions() {
  registerFunction<
      ParameterBinder<ArrayFrequencyBaseline, T>,
      Map<T, int32_t>,
      Array<T>>({"array_frequency_baseline"});
  registerFunction<
      ParameterBinder<ArrayFrequencyIndexed, T>,
      Map<T, int32_t>,
      Array<T>>({"array_frequency_indexed"});
}

void verifyResults(
    functions::test::FunctionBenchmarkBase& benchmark,
    const RowVectorPtr& input) {
  const auto baseline =
      benchmark.evaluate("array_frequency_baseline(c0)", input);
  const auto indexed = benchmark.evaluate("array_frequency_indexed(c0)", input);
  const auto baselineMap = baseline->as<MapVector>();
  const auto indexedMap = indexed->as<MapVector>();
  for (vector_size_t row = 0; row < input->size(); ++row) {
    VELOX_CHECK(baseline->equalValueAt(indexed.get(), row, row));
    if (baseline->isNullAt(row)) {
      continue;
    }
    VELOX_CHECK_EQ(baselineMap->sizeAt(row), indexedMap->sizeAt(row));
    for (vector_size_t index = 0; index < baselineMap->sizeAt(row); ++index) {
      const auto baselineOffset = baselineMap->offsetAt(row) + index;
      const auto indexedOffset = indexedMap->offsetAt(row) + index;
      VELOX_CHECK(baselineMap->mapKeys()->equalValueAt(
          indexedMap->mapKeys().get(), baselineOffset, indexedOffset));
      VELOX_CHECK(baselineMap->mapValues()->equalValueAt(
          indexedMap->mapValues().get(), baselineOffset, indexedOffset));
    }
  }
}

void addCase(
    ExpressionBenchmarkBuilder& builder,
    const std::string& name,
    const ArrayVectorPtr& array) {
  auto input = builder.vectorMaker().rowVector({array});
  verifyResults(builder, input);
  auto& benchmark = builder.addBenchmarkSet(name, input).withIterations(1);
  if (FLAGS_indexed_first) {
    benchmark.addExpression("indexed", "array_frequency_indexed(c0)")
        .addExpression("baseline", "array_frequency_baseline(c0)");
  } else {
    benchmark.addExpression("baseline", "array_frequency_baseline(c0)")
        .addExpression("indexed", "array_frequency_indexed(c0)");
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
  memory::MemoryManager::initialize({});
  registerFunctions<int64_t>();
  registerFunctions<Varchar>();

  ExpressionBenchmarkBuilder builder;
  addCases(builder);
  if (!FLAGS_validate_only) {
    builder.registerBenchmarks();
    folly::runBenchmarks();
  }
  return 0;
}
