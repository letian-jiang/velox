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

#include <gtest/gtest.h>

#include "velox/common/file/FileSystems.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/Spill.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"
#include "velox/functions/wasm/Registration.h"

namespace facebook::velox::functions::wasm::test {
namespace {

using exec::TestScopedSpillInjection;
using exec::test::AssertQueryBuilder;
using exec::test::OperatorTestBase;
using exec::test::PlanBuilder;
using exec::test::TempDirectoryPath;

class WasmAggregateTest : public OperatorTestBase {
 protected:
  static void SetUpTestCase() {
    OperatorTestBase::SetUpTestCase();
    ASSERT_EQ(registerWasmModule(WASM_MODULE_PATH), 23);
  }

  void SetUp() override {
    OperatorTestBase::SetUp();
    filesystems::registerLocalFileSystem();
  }
};

TEST_F(WasmAggregateTest, partialFinal) {
  std::vector<RowVectorPtr> input{
      makeRowVector(
          {"key", "value", "number", "label"},
          {makeFlatVector<int64_t>({0, 0, 1, 2, 3, 4}),
           makeNullableFlatVector<double>(
               {1.0, 3.0, 2.0, std::nullopt, 10.0, std::nullopt}),
           makeNullableFlatVector<int64_t>({1, 2, 3, 4, 5, std::nullopt}),
           makeNullableFlatVector<StringView>(
               {StringView("a"),
                StringView("long"),
                StringView("x"),
                std::nullopt,
                StringView("abc"),
                std::nullopt})}),
      makeRowVector(
          {"key", "value", "number", "label"},
          {makeFlatVector<int64_t>({0, 1, 1, 2, 3, 4}),
           makeNullableFlatVector<double>(
               {5.0, 4.0, 6.0, 8.0, 14.0, std::nullopt}),
           makeNullableFlatVector<int64_t>({6, 7, 8, 9, 10, std::nullopt}),
           makeNullableFlatVector<StringView>(
               {StringView("mid"),
                StringView("longer"),
                StringView("yy"),
                StringView("z"),
                StringView("abcdef"),
                std::nullopt})}),
  };

  core::PlanNodeId partialNodeId;
  core::PlanNodeId finalNodeId;
  auto plan = PlanBuilder()
                  .values(input)
                  .partialAggregation(
                      {"key"},
                      {"avg_f64(value)",
                       "sum_i64(number)",
                       "longest_string(label)"})
                  .capturePlanNodeId(partialNodeId)
                  .localPartition({})
                  .finalAggregation()
                  .capturePlanNodeId(finalNodeId)
                  .planNode();

  auto expected = makeRowVector(
      {"key", "average", "sum", "longest"},
      {makeFlatVector<int64_t>({0, 1, 2, 3, 4}),
       makeNullableFlatVector<double>({3.0, 4.0, 8.0, 12.0, std::nullopt}),
       makeNullableFlatVector<int64_t>({9, 18, 13, 15, std::nullopt}),
       makeNullableFlatVector<StringView>(
           {StringView("long"),
            StringView("longer"),
            StringView("z"),
            StringView("abcdef"),
            std::nullopt})});
  auto task = AssertQueryBuilder(plan).maxDrivers(1).assertResults(expected);

  const auto stats = exec::toPlanStats(task->taskStats());
  EXPECT_EQ(stats.at(partialNodeId).inputRows, 12);
  EXPECT_EQ(stats.at(partialNodeId).outputRows, 5);
  EXPECT_EQ(stats.at(finalNodeId).inputRows, 5);
  EXPECT_EQ(stats.at(finalNodeId).outputRows, 5);
}

TEST_F(WasmAggregateTest, singleGroupPartialFinal) {
  auto input = makeRowVector(
      {makeNullableFlatVector<int64_t>({5, std::nullopt, 7, -2, 32})});
  auto plan = PlanBuilder()
                  .values({input})
                  .partialAggregation({}, {"sum_i64(c0)"})
                  .localPartition({})
                  .finalAggregation()
                  .planNode();

  AssertQueryBuilder(plan).maxDrivers(1).assertResults(
      makeRowVector({makeFlatVector<int64_t>({42})}));
}

TEST_F(WasmAggregateTest, partialFinalWithSpill) {
  constexpr vector_size_t kNumGroups = 1'024;
  constexpr int32_t kNumBatches = 8;
  std::vector<RowVectorPtr> input;
  input.reserve(kNumBatches);
  for (int32_t batch = 0; batch < kNumBatches; ++batch) {
    input.push_back(makeRowVector(
        {"key", "value"},
        {makeFlatVector<int64_t>(
             kNumGroups, [](vector_size_t row) { return row; }),
         makeFlatVector<double>(kNumGroups, [batch](vector_size_t row) {
           return static_cast<double>(row * 2 + batch);
         })}));
  }

  core::PlanNodeId partialNodeId;
  core::PlanNodeId finalNodeId;
  auto plan = PlanBuilder()
                  .values(input)
                  .partialAggregation({"key"}, {"avg_f64(value)"})
                  .capturePlanNodeId(partialNodeId)
                  .localPartition({})
                  .finalAggregation()
                  .capturePlanNodeId(finalNodeId)
                  .planNode();

  auto expected = makeRowVector(
      {"key", "average"},
      {makeFlatVector<int64_t>(
           kNumGroups, [](vector_size_t row) { return row; }),
       makeFlatVector<double>(kNumGroups, [](vector_size_t row) {
         return static_cast<double>(row * 2) + 3.5;
       })});

  const auto spillDirectory = TempDirectoryPath::create();
  TestScopedSpillInjection scopedSpillInjection(100, ".*", /*maxInjections=*/1);
  auto task = AssertQueryBuilder(plan)
                  .spillDirectory(spillDirectory->getPath())
                  .config(core::QueryConfig::kSpillEnabled, true)
                  .config(core::QueryConfig::kAggregationSpillEnabled, true)
                  .config(core::QueryConfig::kMaxOutputBatchRows, 64)
                  .maxDrivers(1)
                  .assertResults(expected);

  const auto stats = exec::toPlanStats(task->taskStats());
  EXPECT_EQ(stats.at(partialNodeId).spilledBytes, 0);
  EXPECT_GT(stats.at(finalNodeId).spilledBytes, 0);
  EXPECT_GT(stats.at(finalNodeId).spilledRows, 0);
  EXPECT_GT(stats.at(finalNodeId).spilledFiles, 0);
  EXPECT_GT(stats.at(finalNodeId).spilledPartitions, 0);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

} // namespace
} // namespace facebook::velox::functions::wasm::test
