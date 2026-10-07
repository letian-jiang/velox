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

#include <limits>
#include <thread>
#include <unordered_set>
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/file/FileSystems.h"
#include "velox/core/QueryCtx.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/RowContainer.h"
#include "velox/exec/Spill.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"
#include "velox/expression/Expr.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Registration.h"
#include "velox/functions/wasm/WasmAggregate.h"

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
    const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
    std::unordered_set<std::string> names;
    for (const auto& scalar : declarations.scalars)
      names.insert(scalar.name);
    ASSERT_EQ(
        registerWasmModule(WASM_MODULE_PATH),
        names.size() + declarations.aggregates.size());
  }

  void SetUp() override {
    OperatorTestBase::SetUp();
    filesystems::registerLocalFileSystem();
  }
};

TEST_F(WasmAggregateTest, cleanupFailureDoesNotEscapeRowContainer) {
  const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  auto manifest = *std::find_if(
      declarations.aggregates.begin(),
      declarations.aggregates.end(),
      [](const auto& declaration) { return declaration.name == "sum_i64"; });
  manifest.entrypoints.destroy = "checked_divide";
  WasmAggregate aggregate(
      BIGINT(), manifest, WasmModule::compile(WASM_MODULE_PATH));
  const auto before = pool()->usedBytes();
  {
    exec::RowContainer rows(
        {BIGINT()},
        true,
        {exec::Accumulator(&aggregate, VARBINARY())},
        {},
        false,
        false,
        false,
        false,
        false,
        false,
        pool());
    const auto column = rows.columnAt(1);
    aggregate.setAllocator(&rows.stringAllocator());
    aggregate.setOffsets(
        column.offset(),
        column.nullByte(),
        column.nullMask(),
        column.initializedByte(),
        column.initializedMask(),
        rows.rowSizeOffset());
    auto* group = rows.newRow();
    const vector_size_t index = 0;
    aggregate.initializeNewGroups(
        &group, folly::Range<const vector_size_t*>(&index, 1));
    // The destructor calls the malformed guest destroy. Escaping exceptions
    // would terminate this test process, including during query unwinding.
  }
  EXPECT_EQ(pool()->usedBytes(), before);
}

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
  auto plan =
      PlanBuilder()
          .values(input)
          .partialAggregation(
              {"key"},
              {"avg_f64(value)", "sum_i64(number)", "longest_string(label)"})
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
        {"key", "value", "wide_value"},
        {makeFlatVector<int64_t>(
             kNumGroups, [](vector_size_t row) { return row; }),
         makeFlatVector<double>(
             kNumGroups,
             [batch](vector_size_t row) {
               return static_cast<double>(row * 2 + batch);
             }),
         makeFlatVector<int128_t>(kNumGroups, [batch](vector_size_t row) {
           return static_cast<int128_t>(row) * 2 + batch;
         })}));
  }

  core::PlanNodeId partialNodeId;
  core::PlanNodeId finalNodeId;
  auto plan =
      PlanBuilder()
          .values(input)
          .partialAggregation(
              {"key"}, {"avg_f64(value)", "strict_sum_wasm(wide_value)"})
          .capturePlanNodeId(partialNodeId)
          .localPartition({})
          .finalAggregation()
          .capturePlanNodeId(finalNodeId)
          .planNode();

  auto expected = makeRowVector(
      {"key", "average", "sum"},
      {makeFlatVector<int64_t>(
           kNumGroups, [](vector_size_t row) { return row; }),
       makeFlatVector<double>(
           kNumGroups,
           [](vector_size_t row) {
             return static_cast<double>(row * 2) + 3.5;
           }),
       makeFlatVector<int128_t>(kNumGroups, [](vector_size_t row) {
         return static_cast<int128_t>(row) * 16 + 28;
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

// Row UDAFs use bound logical types for both intermediate and final results.
namespace facebook::velox::functions::wasm::test {
namespace {

TEST_F(WasmAggregateTest, rowGenericNestedPartialIntermediateFinal) {
  auto input = makeRowVector(
      {"key", "items"},
      {makeFlatVector<int64_t>({0, 0, 1, 1}),
       makeArrayVector<int64_t>({{1, 2}, {99}, {}, {3}})});
  auto expected = makeRowVector(
      {"key", "first"},
      {makeFlatVector<int64_t>({0, 1}),
       makeArrayVector<int64_t>({{1, 2}, {}})});
  auto plan = PlanBuilder()
                  .values({input})
                  .partialAggregation({"key"}, {"first_value_wasm(items)"})
                  .localPartition({})
                  .intermediateAggregation()
                  .localPartition({})
                  .finalAggregation()
                  .planNode();
  AssertQueryBuilder(plan).maxDrivers(1).assertResults(expected);
  auto single = PlanBuilder()
                    .values({input})
                    .singleAggregation({"key"}, {"first_value_wasm(items)"})
                    .planNode();
  AssertQueryBuilder(single).assertResults(expected);

  // Bind a separate SQL T and preserve a first NULL, rather than silently
  // applying default-null filtering to a nullable row API.
  auto strings = makeRowVector({makeNullableFlatVector<StringView>(
      {std::nullopt, StringView("later")})});
  auto nullFirst = PlanBuilder()
                       .values({strings})
                       .partialAggregation({}, {"first_value_wasm(c0)"})
                       .finalAggregation()
                       .planNode();
  AssertQueryBuilder(nullFirst).assertResults(
      makeRowVector({makeNullableFlatVector<StringView>({std::nullopt})}));
}

TEST_F(WasmAggregateTest, genericMapStatesPreserveNestedNullKeys) {
  auto mapKeys = makeRowVector(
      {"id", "tags"},
      {makeNullableFlatVector<int64_t>({std::nullopt, 7, 8}),
       makeNullableArrayVector<int64_t>(
           {{std::nullopt, 1}, {2}, {std::nullopt}})});
  auto maps =
      makeMapVector({0, 1, 2}, mapKeys, makeFlatVector<int64_t>({10, 20, 30}));
  auto input = makeRowVector(
      {"key", "value"}, {makeFlatVector<int64_t>({0, 0, 1}), maps});
  auto expectedMaps =
      BaseVector::wrapInDictionary(nullptr, makeIndices({0, 2}), 2, maps);
  auto expected =
      makeRowVector({makeFlatVector<int64_t>({0, 1}), expectedMaps});
  auto staged = PlanBuilder()
                    .values({input})
                    .partialAggregation({"key"}, {"first_value_wasm(value)"})
                    .localPartition({})
                    .intermediateAggregation()
                    .localPartition({})
                    .finalAggregation()
                    .planNode();
  AssertQueryBuilder(staged).maxDrivers(1).assertResults(expected);
  auto companions =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {"key"}, {"first_value_wasm_partial(value) as state"})
          .singleAggregation(
              {"key"}, {"first_value_wasm_merge(state) as state"})
          .project({"key", "first_value_wasm_extract(state)"})
          .planNode();
  AssertQueryBuilder(companions).assertResults(expected);
}

TEST_F(WasmAggregateTest, rowVariadicConstantsConfigurationAndZeroTail) {
  auto input = makeRowVector(
      {makeNullableFlatVector<int64_t>({1, std::nullopt, 3}),
       makeNullableFlatVector<int64_t>({4, 5, std::nullopt})});
  auto plan = PlanBuilder()
                  .values({input})
                  .partialAggregation({}, {"variadic_sum_wasm(2, c0, c1)"})
                  .localPartition({})
                  .intermediateAggregation()
                  .localPartition({})
                  .finalAggregation()
                  .planNode();
  AssertQueryBuilder(plan)
      .config("wasm_aggregate_multiplier", "3")
      .assertResults(makeRowVector({makeFlatVector<int64_t>({78})}));

  auto zero = PlanBuilder()
                  .values({input})
                  .singleAggregation({}, {"variadic_sum_wasm(2)"})
                  .planNode();
  AssertQueryBuilder(zero).assertResults(
      makeRowVector({makeNullableFlatVector<int64_t>({std::nullopt})}));
  auto nullConstant =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {}, {"variadic_sum_wasm(cast(null as bigint), c0)"})
          .planNode();
  AssertQueryBuilder(nullConstant)
      .assertResults(makeRowVector({makeFlatVector<int64_t>({4})}));

  auto invalid = PlanBuilder()
                     .values({input})
                     .singleAggregation({}, {"variadic_sum_wasm(c0, c1)"})
                     .planNode();
  EXPECT_THROW(AssertQueryBuilder(invalid).copyResults(pool()), VeloxUserError);
}

TEST_F(WasmAggregateTest, rowHugeintDefaultNullAndEmptyInput) {
  const int128_t large = HugeInt::build(0x7000000000000000, 0);
  auto values = makeRowVector(
      {"key", "number"},
      {makeFlatVector<int64_t>({0, 0, 1, 2}),
       makeNullableFlatVector<int128_t>({large, 7, std::nullopt, -9})});
  auto plan = PlanBuilder()
                  .values({values})
                  .partialAggregation({"key"}, {"strict_sum_wasm(number)"})
                  .localPartition({})
                  .intermediateAggregation()
                  .localPartition({})
                  .finalAggregation()
                  .planNode();
  AssertQueryBuilder(plan).assertResults(makeRowVector(
      {"key", "sum"},
      {makeFlatVector<int64_t>({0, 1, 2}),
       makeNullableFlatVector<int128_t>({large + 7, std::nullopt, -9})}));

  auto empty = makeRowVector({makeFlatVector<int128_t>({})});
  auto global = PlanBuilder()
                    .values({empty})
                    .singleAggregation({}, {"strict_sum_wasm(c0)"})
                    .planNode();
  AssertQueryBuilder(global).assertResults(
      makeRowVector({makeNullableFlatVector<int128_t>({std::nullopt})}));
}

TEST_F(WasmAggregateTest, rowAggregateWindowFramesAndConstants) {
  auto input = makeRowVector(
      {"p", "i", "v"},
      {makeFlatVector<int64_t>({0, 0, 0, 1, 1}),
       makeFlatVector<int64_t>({0, 1, 2, 0, 1}),
       makeNullableFlatVector<int64_t>({1, std::nullopt, 3, 4, 5})});
  auto plan =
      PlanBuilder()
          .values({input})
          .window(
              {"variadic_sum_wasm(2, v) over (partition by p order by i rows between unbounded preceding and current row)",
               "variadic_sum_wasm(2, v) over (partition by p order by i rows between 1 preceding and current row)",
               "variadic_sum_wasm(2, v) over (partition by p order by i rows between 1 preceding and 1 preceding)"})
          .planNode();
  auto expected = makeRowVector(
      {input->childAt(0),
       input->childAt(1),
       input->childAt(2),
       makeFlatVector<int64_t>({6, 6, 24, 24, 54}),
       makeFlatVector<int64_t>({6, 6, 18, 24, 54}),
       makeNullableFlatVector<int64_t>(
           {std::nullopt, 6, std::nullopt, std::nullopt, 24})});
  AssertQueryBuilder(plan)
      .config("wasm_aggregate_multiplier", "3")
      .assertResults(expected);
}

TEST_F(WasmAggregateTest, rowAggregateMasksDistinctAndOrdering) {
  auto input = makeRowVector(
      {"v", "keep"},
      {makeNullableFlatVector<int64_t>({3, 1, 1, std::nullopt, 2}),
       makeFlatVector<bool>({true, true, true, true, false})});
  auto mask = PlanBuilder()
                  .values({input})
                  .singleAggregation({}, {"variadic_sum_wasm(1, v)"}, {"keep"})
                  .planNode();
  AssertQueryBuilder(mask).assertResults(
      makeRowVector({makeFlatVector<int64_t>({5})}));
  auto distinct =
      PlanBuilder()
          .values({input})
          .singleAggregation({}, {"variadic_sum_wasm(distinct 1, v)"})
          .planNode();
  AssertQueryBuilder(distinct).assertResults(
      makeRowVector({makeFlatVector<int64_t>({6})}));
  auto ordered = PlanBuilder()
                     .values({input})
                     .singleAggregation(
                         {}, {"first_value_wasm(v order by v desc nulls last)"})
                     .planNode();
  AssertQueryBuilder(ordered).assertResults(
      makeRowVector({makeFlatVector<int64_t>({3})}));
}

TEST_F(WasmAggregateTest, rowTypedIntermediateFactoryBinding) {
  const auto intermediate =
      ROW({"value", "seen"}, {ARRAY(BIGINT()), BOOLEAN()});
  auto final = exec::Aggregate::create(
      "first_value_wasm",
      core::AggregationNode::Step::kFinal,
      {intermediate},
      ARRAY(BIGINT()),
      core::QueryConfig({}));
  ASSERT_TRUE(final->resultType()->equivalent(*ARRAY(BIGINT())));
  EXPECT_FALSE(final->supportsToIntermediate());
  VELOX_ASSERT_THROW(
      final->addRawInput(nullptr, SelectivityVector(0), {}, false),
      "Wasm UDAF instance was bound for intermediate input");
  VELOX_ASSERT_THROW(
      final->addSingleGroupRawInput(nullptr, SelectivityVector(0), {}, false),
      "Wasm UDAF instance was bound for intermediate input");
  EXPECT_THROW(
      exec::Aggregate::create(
          "first_value_wasm",
          core::AggregationNode::Step::kFinal,
          {VARBINARY()},
          ARRAY(BIGINT()),
          core::QueryConfig({})),
      VeloxUserError);
}

TEST_F(WasmAggregateTest, companionFamiliesUseNativeStageSemantics) {
  auto input = makeRowVector(
      {"value", "number"},
      {makeNullableFlatVector<double>({1.0, 3.0, std::nullopt, 8.0}),
       makeNullableFlatVector<int64_t>({1, 3, std::nullopt, 8})});
  auto ids = std::make_shared<core::PlanNodeIdGenerator>();
  auto partial = PlanBuilder(ids)
                     .values({input})
                     .singleAggregation(
                         {},
                         {"avg_f64_partial(value) as avg_state",
                          "checked_sum_wasm_partial(number) as sum_state"})
                     .planNode();
  auto merged = PlanBuilder(partial, ids)
                    .singleAggregation(
                        {},
                        {"avg_f64_merge(avg_state) as avg_state",
                         "checked_sum_wasm_merge(sum_state) as sum_state"})
                    .project(
                        {"avg_f64_extract(avg_state)",
                         "checked_sum_wasm_extract(sum_state)"})
                    .planNode();
  AssertQueryBuilder(merged).assertResults(makeRowVector(
      {makeFlatVector<double>({4.0}), makeFlatVector<int64_t>({12})}));
  auto final = PlanBuilder(partial, ids)
                   .singleAggregation(
                       {},
                       {"avg_f64_merge_extract(avg_state)",
                        "checked_sum_wasm_merge_extract(sum_state)"})
                   .planNode();
  AssertQueryBuilder(final).assertResults(makeRowVector(
      {makeFlatVector<double>({4.0}), makeFlatVector<int64_t>({12})}));
}

TEST_F(WasmAggregateTest, companionGenericNestedAndVariadicConstants) {
  auto input = makeRowVector(
      {"items", "number"},
      {makeNullableArrayVector<int64_t>(
           std::vector<std::optional<std::vector<std::optional<int64_t>>>>{
               std::vector<std::optional<int64_t>>{7, std::nullopt},
               std::vector<std::optional<int64_t>>{8},
               std::nullopt}),
       makeNullableFlatVector<int64_t>({1, 3, 8})});
  auto plan = PlanBuilder()
                  .values({input})
                  .singleAggregation(
                      {},
                      {"first_value_wasm_partial(items) as items_state",
                       "variadic_sum_wasm_partial(2, number) as sum_state"})
                  .singleAggregation(
                      {},
                      {"first_value_wasm_merge_extract(items_state)",
                       "variadic_sum_wasm_merge_extract(sum_state)"})
                  .planNode();
  AssertQueryBuilder(plan)
      .config("wasm_aggregate_multiplier", "3")
      .assertResults(makeRowVector(
          {makeNullableArrayVector<int64_t>(
               std::vector<std::vector<std::optional<int64_t>>>{
                   {7, std::nullopt}}),
           makeFlatVector<int64_t>({72})}));
}

TEST_F(WasmAggregateTest, intermediateRowAliasesFollowNativePositionalTypes) {
  auto states = makeRowVector(
      {"aliased_total", "aliased_count"},
      {makeFlatVector<int128_t>({10, 20, 30, 40}),
       makeFlatVector<int64_t>({2, 3, 4, 5})});
  states->setNull(2, true);
  auto input = makeRowVector(
      {"key", "state"}, {makeFlatVector<int64_t>({0, 0, 1, 2}), states});
  for (bool grouped : {false, true}) {
    const std::vector<std::string> keys =
        grouped ? std::vector<std::string>{"key"} : std::vector<std::string>{};
    auto expected = grouped
        ? makeRowVector(
              {makeFlatVector<int64_t>({0, 1, 2}),
               makeNullableFlatVector<int64_t>({30, std::nullopt, 40})})
        : makeRowVector({makeFlatVector<int64_t>({70})});
    auto merge = PlanBuilder()
                     .values({input})
                     .singleAggregation(
                         keys, {"positional_sum_wasm_merge_extract(state)"})
                     .planNode();
    AssertQueryBuilder(merge).assertResults(expected);
    // Native final operators construct the factory with original raw types,
    // then pass the positionally compatible intermediate to its merge method.
    auto final =
        PlanBuilder().values({input}).addNode([&](auto id, auto source) {
          core::AggregationNode::Aggregate aggregate;
          aggregate.call = std::make_shared<core::CallTypedExpr>(
              BIGINT(),
              std::vector<core::TypedExprPtr>{
                  std::make_shared<core::FieldAccessTypedExpr>(
                      states->type(), "state")},
              "positional_sum_wasm");
          aggregate.rawInputTypes = {BIGINT()};
          std::vector<core::FieldAccessTypedExprPtr> fields;
          if (grouped)
            fields.push_back(
                std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "key"));
          return std::make_shared<core::AggregationNode>(
              id,
              core::AggregationNode::Step::kFinal,
              fields,
              std::vector<core::FieldAccessTypedExprPtr>{},
              std::vector<std::string>{"result"},
              std::vector<core::AggregationNode::Aggregate>{aggregate},
              false,
              false,
              source);
        });
    AssertQueryBuilder(final.planNode()).assertResults(expected);
    auto roundTrip = PlanBuilder().values({input}).singleAggregation(
        keys, {"positional_sum_wasm_merge(state) as state"});
    if (grouped)
      roundTrip.project({"key", "positional_sum_wasm_extract(state)"});
    else
      roundTrip.project({"positional_sum_wasm_extract(state)"});
    AssertQueryBuilder(roundTrip.planNode()).assertResults(expected);
  }
  auto extract = PlanBuilder()
                     .values({input})
                     .project({"positional_sum_wasm_extract(state)"})
                     .planNode();
  AssertQueryBuilder(extract).assertResults(makeRowVector(
      {makeNullableFlatVector<int64_t>({10, 20, std::nullopt, 40})}));
  // Canonicalization must not make an explicitly named companion signature
  // accept aliases that the native SignatureBinder rejects.
  VELOX_ASSERT_USER_THROW(
      PlanBuilder().values({input}).singleAggregation(
          {}, {"variadic_sum_wasm_merge_extract(state)"}),
      "Aggregate function signature is not supported");
  EXPECT_EQ(
      states->type()->asRow().names(),
      (std::vector<std::string>{"aliased_total", "aliased_count"}));
}

TEST_F(WasmAggregateTest, constantIntermediateDoesNotBecomeRawConfiguration) {
  auto input = makeRowVector({"key"}, {makeFlatVector<int64_t>({0, 0, 1})});
  // Construct a real typed constant: the test SQL parser maps HUGEINT casts to
  // DOUBLE and cannot represent this native intermediate type through SQL.
  auto state = makeRowVector(
      {"total", "count"},
      {makeFlatVector<int128_t>({10}), makeFlatVector<int64_t>({2})});
  auto constant = std::make_shared<core::ConstantTypedExpr>(state);
  auto mergePlan = [&](const std::string& name, bool grouped, bool extract) {
    PlanBuilder builder;
    builder.values({input}).addNode([&](auto id, auto source) {
      const auto function = name + (extract ? "_merge_extract" : "_merge");
      core::AggregationNode::Aggregate aggregate;
      aggregate.call = std::make_shared<core::CallTypedExpr>(
          extract ? BIGINT() : state->type(),
          std::vector<core::TypedExprPtr>{constant},
          function);
      aggregate.rawInputTypes = {state->type()};
      std::vector<core::FieldAccessTypedExprPtr> keys;
      if (grouped)
        keys.push_back(
            std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "key"));
      return std::make_shared<core::AggregationNode>(
          id,
          core::AggregationNode::Step::kSingle,
          keys,
          std::vector<core::FieldAccessTypedExprPtr>{},
          std::vector<std::string>{"merged"},
          std::vector<core::AggregationNode::Aggregate>{aggregate},
          false,
          false,
          source);
    });
    return builder;
  };
  for (const std::string name :
       {"variadic_sum_wasm",
        "tail_map_variadic_wasm",
        "map_non_null_variadic_wasm"}) {
    SCOPED_TRACE(name);
    for (bool grouped : {false, true}) {
      auto expected = grouped ? makeRowVector(
                                    {makeFlatVector<int64_t>({0, 1}),
                                     makeFlatVector<int64_t>({20, 10})})
                              : makeRowVector({makeFlatVector<int64_t>({30})});
      AssertQueryBuilder(mergePlan(name, grouped, true).planNode())
          .config("wasm_aggregate_multiplier", "3")
          .assertResults(expected);
      auto merged = mergePlan(name, grouped, false);
      if (grouped)
        merged.project({"key", name + "_extract(merged)"});
      else
        merged.project({name + "_extract(merged)"});
      AssertQueryBuilder(merged.planNode())
          .config("wasm_aggregate_multiplier", "3")
          .assertResults(expected);
    }
    auto extract = PlanBuilder()
                       .values({input})
                       .addNode([&](auto id, auto source) {
                         auto call = std::make_shared<core::CallTypedExpr>(
                             BIGINT(),
                             std::vector<core::TypedExprPtr>{constant},
                             name + "_extract");
                         return std::make_shared<core::ProjectNode>(
                             id,
                             std::vector<std::string>{"result"},
                             std::vector<core::TypedExprPtr>{call},
                             source);
                       })
                       .planNode();
    AssertQueryBuilder(extract).assertResults(
        makeRowVector({makeFlatVector<int64_t>({10, 10, 10})}));
  }
}

TEST_F(WasmAggregateTest, rowOverflowReportsNativeUserErrorAndDiscardsState) {
  const int128_t maximum = std::numeric_limits<int128_t>::max();
  const auto input = makeFlatVector<int128_t>({maximum, 1});
  auto plan = PlanBuilder()
                  .values({makeRowVector({"value"}, {input})})
                  .singleAggregation({}, {"strict_sum_wasm(value)"})
                  .planNode();
  EXPECT_THROW(AssertQueryBuilder(plan).copyResults(pool()), VeloxUserError);

  // Exercise grouped and single-group raw/merge exports directly, then verify
  // that extraction cannot publish partially updated state after an error.
  for (bool merge : {false, true}) {
    for (bool single : {false, true}) {
      SCOPED_TRACE(fmt::format("merge={} single={}", merge, single));
      HashStringAllocator allocator(pool());
      auto aggregate = exec::Aggregate::create(
          "strict_sum_wasm",
          core::AggregationNode::Step::kSingle,
          {HUGEINT()},
          HUGEINT(),
          core::QueryConfig({}));
      aggregate->setAllocator(&allocator);
      constexpr int32_t offset = alignof(std::max_align_t);
      aggregate->setOffsets(
          offset,
          exec::RowContainer::nullByte(0),
          exec::RowContainer::nullMask(0),
          exec::RowContainer::initializedByte(0),
          exec::RowContainer::initializedMask(0),
          4);
      auto storage = AlignedBuffer::allocate<char>(
          offset + aggregate->accumulatorFixedWidthSize(), pool(), 0);
      char* group = storage->asMutable<char>();
      const vector_size_t index = 0;
      aggregate->initializeNewGroups(
          &group, folly::Range<const vector_size_t*>(&index, 1));
      // Mark the group non-NULL before the failure so extraction cannot take
      // the all-NULL fast path instead of checking the invalidated Store.
      aggregate->addSingleGroupRawInput(
          group, SelectivityVector(1), {makeFlatVector<int128_t>({0})}, false);
      char* groups[]{group, group};
      SelectivityVector rows(2);
      std::vector<VectorPtr> args{input};
      auto update = [&] {
        if (single) {
          if (merge)
            aggregate->addSingleGroupIntermediateResults(
                group, rows, args, false);
          else
            aggregate->addSingleGroupRawInput(group, rows, args, false);
        } else {
          if (merge)
            aggregate->addIntermediateResults(groups, rows, args, false);
          else
            aggregate->addRawInput(groups, rows, args, false);
        }
      };
      EXPECT_THROW(update(), VeloxUserError);
      VectorPtr output;
      EXPECT_THROW(
          aggregate->extractValues(&group, 1, &output), VeloxRuntimeError);
      EXPECT_THROW(
          aggregate->extractAccumulators(&group, 1, &output),
          VeloxRuntimeError);
      EXPECT_THROW(update(), VeloxRuntimeError);
      aggregate->destroy(folly::Range<char**>(&group, 1));
    }
  }
}

} // namespace
} // namespace facebook::velox::functions::wasm::test

namespace facebook::velox::functions::wasm::test {
namespace {
TEST_F(WasmAggregateTest, rowToIntermediateAndConcurrentExtraction) {
  HashStringAllocator allocator(pool());
  auto aggregate = exec::Aggregate::create(
      "strict_sum_wasm",
      core::AggregationNode::Step::kPartial,
      {HUGEINT()},
      HUGEINT(),
      core::QueryConfig({}));
  aggregate->setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  ASSERT_TRUE(aggregate->supportsToIntermediate());
  const int128_t large = HugeInt::build(0x7000000000000000, 0);
  std::vector<VectorPtr> args{
      makeNullableFlatVector<int128_t>({large, std::nullopt, 7, 9})};
  SelectivityVector selected(4);
  selected.setValid(3, false);
  selected.updateBounds();
  VectorPtr intermediate;
  aggregate->toIntermediate(selected, args, intermediate);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int128_t>({large, std::nullopt, 7, std::nullopt}),
      intermediate);
  SelectivityVector none(4, false);
  aggregate->toIntermediate(none, args, intermediate);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int128_t>(
          {std::nullopt, std::nullopt, std::nullopt, std::nullopt}),
      intermediate);

  auto storage = AlignedBuffer::allocate<char>(
      offset + aggregate->accumulatorFixedWidthSize(), pool(), 0);
  char* group = storage->asMutable<char>();
  const vector_size_t index = 0;
  aggregate->initializeNewGroups(
      &group, folly::Range<const vector_size_t*>(&index, 1));
  aggregate->addSingleGroupRawInput(group, selected, args, false);
  std::exception_ptr failures[2];
  VectorPtr outputs[2];
  auto extract = [&](size_t index) {
    try {
      for (int i = 0; i < 10; ++i)
        aggregate->extractAccumulators(&group, 1, &outputs[index]);
    } catch (...) {
      failures[index] = std::current_exception();
    }
  };
  std::thread first(extract, 0), second(extract, 1);
  first.join();
  second.join();
  for (size_t i = 0; i < 2; ++i) {
    if (failures[i])
      std::rethrow_exception(failures[i]);
    velox::test::assertEqualVectors(
        makeFlatVector<int128_t>({large + 7}), outputs[i]);
  }
  VectorPtr final;
  aggregate->extractValues(&group, 1, &final);
  velox::test::assertEqualVectors(makeFlatVector<int128_t>({large + 7}), final);
  aggregate->destroy(folly::Range<char**>(&group, 1));
}
} // namespace
} // namespace facebook::velox::functions::wasm::test

namespace facebook::velox::functions::wasm::test {
namespace {
TEST_F(WasmAggregateTest, rowFullTimestampAndArbitraryVarchar) {
  const Timestamp full(Timestamp::kMaxSeconds, Timestamp::kMaxNanos);
  const std::string bytes("\xff\x00\xfe", 3);
  auto input = makeRowVector(
      {makeFlatVector<Timestamp>({full, Timestamp(0, 0)}),
       makeFlatVector<StringView>({StringView(bytes), StringView("later")})});
  auto plan = PlanBuilder()
                  .values({input})
                  .partialAggregation(
                      {}, {"first_value_wasm(c0)", "first_value_wasm(c1)"})
                  .localPartition({})
                  .intermediateAggregation()
                  .localPartition({})
                  .finalAggregation()
                  .planNode();
  AssertQueryBuilder(plan).maxDrivers(1).assertResults(makeRowVector(
      {makeFlatVector<Timestamp>({full}),
       makeFlatVector<StringView>({StringView(bytes)})}));
}

TEST_F(WasmAggregateTest, ownedStateSizeTracksGrowthCompactionAndCleanup) {
  HashStringAllocator allocator(pool());
  auto aggregate = exec::Aggregate::create(
      "collect_strings_wasm",
      core::AggregationNode::Step::kSingle,
      {VARCHAR()},
      ARRAY(VARCHAR()),
      core::QueryConfig({}));
  EXPECT_FALSE(aggregate->isFixedSize());
  EXPECT_TRUE(aggregate->supportsCompact());
  aggregate->setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  constexpr int32_t sizeOffset = 4;
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      sizeOffset);
  auto storage0 = AlignedBuffer::allocate<char>(
      offset + aggregate->accumulatorFixedWidthSize(), pool(), 0);
  auto storage1 = AlignedBuffer::allocate<char>(
      offset + aggregate->accumulatorFixedWidthSize(), pool(), 0);
  char* groups[] = {
      storage0->asMutable<char>(),
      storage1->asMutable<char>(),
      storage0->asMutable<char>()};
  // Other variable-sized keys/aggregates share this counter; preserve them.
  folly::storeUnaligned<uint32_t>(groups[0] + sizeOffset, 50);
  folly::storeUnaligned<uint32_t>(groups[1] + sizeOffset, 70);
  const vector_size_t indices[] = {0, 1};
  aggregate->initializeNewGroups(
      groups, folly::Range<const vector_size_t*>(indices, 2));
  auto bytes = [&](int group) {
    return folly::loadUnaligned<uint32_t>(groups[group] + sizeOffset);
  };
  const auto created0 = bytes(0);
  const auto created1 = bytes(1);
  EXPECT_GT(created0, 50);
  EXPECT_EQ(created0 - 50, created1 - 70);
  std::vector<VectorPtr> args{
      makeFlatVector<std::string>({"hello", "ignored", "world"})};
  SelectivityVector rows(3);
  rows.setValid(1, false);
  rows.updateBounds();
  aggregate->addRawInput(groups, rows, args, false);
  const auto grown = bytes(0);
  EXPECT_GT(grown, created0);
  EXPECT_EQ(bytes(1), created1);
  VectorPtr result;
  aggregate->extractValues(groups, 1, &result);
  auto expected = makeArrayVector<std::string>({{"hello", "world"}});
  velox::test::assertEqualVectors(expected, result);
  // This small Store stays below the rebuilding threshold; only capacity
  // shrinks.
  EXPECT_EQ(aggregate->compact(folly::Range(groups, 1)), 0);
  EXPECT_LT(bytes(0), grown);
  EXPECT_EQ(bytes(1), created1);
  aggregate->extractValues(groups, 1, &result);
  velox::test::assertEqualVectors(expected, result);
  const auto compacted = bytes(0);
  std::vector<VectorPtr> intermediate{
      makeArrayVector<std::string>({{"again"}})};
  aggregate->addSingleGroupIntermediateResults(
      groups[0], SelectivityVector(1), intermediate, false);
  EXPECT_GT(bytes(0), compacted);
  aggregate->extractValues(groups, 1, &result);
  velox::test::assertEqualVectors(
      makeArrayVector<std::string>({{"hello", "world", "again"}}), result);
  aggregate->destroy(folly::Range(groups, 2));
  EXPECT_EQ(bytes(0), 50);
  EXPECT_EQ(bytes(1), 70);
  auto companion = exec::Aggregate::create(
      "collect_strings_wasm_partial",
      core::AggregationNode::Step::kSingle,
      {VARCHAR()},
      ARRAY(VARCHAR()),
      core::QueryConfig({}));
  EXPECT_FALSE(companion->isFixedSize());
  EXPECT_TRUE(companion->supportsCompact());
}

TEST_F(
    WasmAggregateTest,
    ownedVariableStateRunsNativeStagesAndRepeatedExtractCompanion) {
  auto input = makeRowVector({makeFlatVector<std::string>({"hello", "world"})});
  auto plan = PlanBuilder()
                  .values({input})
                  .partialAggregation({}, {"collect_strings_wasm(c0)"})
                  .localPartition({})
                  .finalAggregation()
                  .planNode();
  AssertQueryBuilder(plan).maxDrivers(1).assertResults(
      makeRowVector({makeArrayVector<std::string>({{"hello", "world"}})}));
  // Native extract owns a reusable aggregate but fresh temporary groups. Its
  // row size/flags must be initialized on every pool allocation.
  auto partial =
      PlanBuilder()
          .values({input})
          .singleAggregation({}, {"collect_strings_wasm_partial(c0)"})
          .project({"collect_strings_wasm_extract(a0)"})
          .planNode();
  AssertQueryBuilder(partial).assertResults(
      makeRowVector({makeArrayVector<std::string>({{"hello", "world"}})}));
}

TEST_F(
    WasmAggregateTest,
    retirementReleasesStorePagesAndIntermediateRestoresFreshState) {
  HashStringAllocator allocator(pool());
  auto aggregate = exec::Aggregate::create(
      "collect_strings_wasm",
      core::AggregationNode::Step::kSingle,
      {VARCHAR()},
      ARRAY(VARCHAR()),
      core::QueryConfig({}));
  aggregate->setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  auto storage = AlignedBuffer::allocate<char>(
      offset + aggregate->accumulatorFixedWidthSize(), pool(), 0);
  char* group = storage->asMutable<char>();
  const vector_size_t index = 0;
  const std::string value(2UL << 20, 'x');
  std::vector<VectorPtr> input{makeFlatVector<std::string>({value})};
  const auto before = pool()->usedBytes();
  aggregate->initializeNewGroups(
      &group, folly::Range<const vector_size_t*>(&index, 1));
  EXPECT_GT(pool()->usedBytes(), before);
  aggregate->addSingleGroupRawInput(group, SelectivityVector(1), input, false);
  VectorPtr saved;
  aggregate->extractAccumulators(&group, 1, &saved);
  const auto retained = pool()->usedBytes();
  aggregate->destroy(folly::Range(&group, 1));
  EXPECT_GT(retained - pool()->usedBytes(), 1UL << 20);
  EXPECT_EQ(folly::loadUnaligned<uint32_t>(group + 4), 0);
  aggregate->initializeNewGroups(
      &group, folly::Range<const vector_size_t*>(&index, 1));
  aggregate->addSingleGroupIntermediateResults(
      group, SelectivityVector(1), {saved}, false);
  VectorPtr restored;
  aggregate->extractValues(&group, 1, &restored);
  velox::test::assertEqualVectors(saved, restored);
  aggregate->destroy(folly::Range(&group, 1));
}

TEST_F(
    WasmAggregateTest,
    activeStoreRebuildKeepsOtherGroupsNullsAndHandleSequence) {
  HashStringAllocator allocator(pool());
  auto aggregate = exec::Aggregate::create(
      "collect_strings_wasm",
      core::AggregationNode::Step::kSingle,
      {VARCHAR()},
      ARRAY(VARCHAR()),
      core::QueryConfig({}));
  aggregate->setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  std::vector<BufferPtr> storage;
  std::vector<char*> groups;
  for (int i = 0; i < 3; ++i) {
    storage.push_back(
        AlignedBuffer::allocate<char>(
            offset + aggregate->accumulatorFixedWidthSize(), pool(), 0));
    groups.push_back(storage.back()->asMutable<char>());
    folly::storeUnaligned<uint32_t>(groups.back() + 4, 50 + i * 20);
  }
  const vector_size_t indices[] = {0, 1, 2};
  aggregate->initializeNewGroups(
      groups.data(), folly::Range<const vector_size_t*>(indices, 3));
  aggregate->addSingleGroupRawInput(
      groups[0],
      SelectivityVector(2),
      {makeFlatVector<std::string>({"hello", "world"})},
      false);
  std::vector<VectorPtr> large{
      makeFlatVector<std::string>({std::string(2UL << 20, 'x')})};
  aggregate->addSingleGroupRawInput(
      groups[2], SelectivityVector(1), large, false);
  // Retiring one group does not release the Store while other groups remain.
  aggregate->destroy(folly::Range(groups.data() + 2, 1));
  EXPECT_EQ(folly::loadUnaligned<uint32_t>(groups[2] + 4), 90);
  const auto untouchedBefore = folly::loadUnaligned<uint32_t>(groups[1] + 4);
  const auto before = pool()->usedBytes();
  const auto reclaimed = aggregate->compact(folly::Range(groups.data(), 1));
  EXPECT_GT(reclaimed, 1UL << 20);
  EXPECT_GT(before - pool()->usedBytes(), 1UL << 20);
  // Restore accounts every live group, including the empty unselected group.
  EXPECT_LT(folly::loadUnaligned<uint32_t>(groups[1] + 4), untouchedBefore);
  EXPECT_EQ(folly::loadUnaligned<uint32_t>(groups[0] + offset), 1);
  EXPECT_EQ(folly::loadUnaligned<uint32_t>(groups[1] + offset), 2);
  VectorPtr result;
  aggregate->extractValues(groups.data(), 2, &result);
  auto expected = makeArrayVector<std::string>({{"hello", "world"}, {}});
  expected->setNull(1, true);
  velox::test::assertEqualVectors(expected, result);
  // The retired handle 3 must not be reused even though only 1 and 2 migrated.
  std::memset(groups[2], 0, storage[2]->size());
  folly::storeUnaligned<uint32_t>(groups[2] + 4, 90);
  aggregate->initializeNewGroups(
      groups.data(), folly::Range<const vector_size_t*>(indices + 2, 1));
  EXPECT_EQ(folly::loadUnaligned<uint32_t>(groups[2] + offset), 4);
  aggregate->addSingleGroupRawInput(
      groups[0],
      SelectivityVector(1),
      {makeFlatVector<std::string>({"raw"})},
      false);
  aggregate->addSingleGroupIntermediateResults(
      groups[0],
      SelectivityVector(1),
      {makeArrayVector<std::string>({{"merged"}})},
      false);
  aggregate->extractValues(groups.data(), 1, &result);
  velox::test::assertEqualVectors(
      makeArrayVector<std::string>({{"hello", "world", "raw", "merged"}}),
      result);
  aggregate->destroy(folly::Range(groups.data(), 3));
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(folly::loadUnaligned<uint32_t>(groups[i] + 4), 50 + i * 20);
}

TEST_F(
    WasmAggregateTest,
    activeStoreRebuildPreservesVariadicPrivateConfiguration) {
  HashStringAllocator allocator(pool());
  auto aggregate = exec::Aggregate::create(
      "variadic_sum_wasm",
      core::AggregationNode::Step::kSingle,
      {BIGINT(), BIGINT()},
      BIGINT(),
      core::QueryConfig(
          std::unordered_map<std::string, std::string>{
              {"wasm_aggregate_multiplier", "3"}}));
  aggregate->setConstantInputs({makeConstant<int64_t>(2, 1), nullptr});
  aggregate->setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  auto storage = AlignedBuffer::allocate<char>(
      offset + aggregate->accumulatorFixedWidthSize(), pool(), 0);
  char* group = storage->asMutable<char>();
  const vector_size_t index = 0;
  aggregate->initializeNewGroups(
      &group, folly::Range<const vector_size_t*>(&index, 1));
  constexpr vector_size_t count = 65536;
  aggregate->addSingleGroupRawInput(
      group,
      SelectivityVector(count),
      {makeConstant<int64_t>(2, count),
       makeFlatVector<int64_t>(count, [](auto) { return 1; })},
      false);
  EXPECT_GT(aggregate->compact(folly::Range(&group, 1)), 64UL << 10);
  aggregate->addSingleGroupRawInput(
      group,
      SelectivityVector(1),
      {makeConstant<int64_t>(2, 1), makeFlatVector<int64_t>({7})},
      false);
  VectorPtr result;
  aggregate->extractValues(&group, 1, &result);
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({6 * (count + 7)}), result);
  // SQL intermediate has only sum/count; the factor resides in private State.
  VectorPtr intermediate;
  aggregate->extractAccumulators(&group, 1, &intermediate);
  aggregate->addSingleGroupIntermediateResults(
      group, SelectivityVector(1), {intermediate}, false);
  aggregate->extractValues(&group, 1, &result);
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({12 * (count + 7)}), result);
  aggregate->destroy(folly::Range(&group, 1));
  EXPECT_EQ(folly::loadUnaligned<uint32_t>(group + 4), 0);
}

TEST_F(
    WasmAggregateTest,
    activeStoreRebuildPreservesGenericNestedFirstAndSeenNull) {
  HashStringAllocator allocator(pool());
  auto input = makeRowVector(
      {makeFlatVector<int64_t>({7}),
       makeArrayVector<std::string>({{"a", "b"}})});
  auto aggregate = exec::Aggregate::create(
      "first_value_wasm",
      core::AggregationNode::Step::kSingle,
      {input->type()},
      input->type(),
      core::QueryConfig({}));
  aggregate->setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  std::vector<BufferPtr> storage;
  std::vector<char*> groups;
  for (int i = 0; i < 3; ++i) {
    storage.push_back(
        AlignedBuffer::allocate<char>(
            offset + aggregate->accumulatorFixedWidthSize(), pool(), 0));
    groups.push_back(storage.back()->asMutable<char>());
  }
  const vector_size_t indices[] = {0, 1, 2};
  aggregate->initializeNewGroups(
      groups.data(), folly::Range<const vector_size_t*>(indices, 3));
  aggregate->addSingleGroupRawInput(
      groups[0], SelectivityVector(1), {input}, false);
  auto null = makeRowVector(
      {makeFlatVector<int64_t>({9}),
       makeArrayVector<std::string>({{"later"}})});
  null->setNull(0, true);
  aggregate->addSingleGroupRawInput(
      groups[1], SelectivityVector(1), {null}, false);
  auto large = makeRowVector(
      {makeFlatVector<int64_t>({11}),
       makeArrayVector<std::string>({{std::string(2UL << 20, 'x')}})});
  aggregate->addSingleGroupRawInput(
      groups[2], SelectivityVector(1), {large}, false);
  aggregate->destroy(folly::Range(groups.data() + 2, 1));
  EXPECT_GT(aggregate->compact(folly::Range(groups.data(), 1)), 1UL << 20);
  // Both the generic nested value and the private seen flag must survive.
  auto later = makeRowVector(
      {makeFlatVector<int64_t>({99}),
       makeArrayVector<std::string>({{"ignored"}})});
  aggregate->addSingleGroupRawInput(
      groups[0], SelectivityVector(1), {later}, false);
  aggregate->addSingleGroupRawInput(
      groups[1], SelectivityVector(1), {later}, false);
  VectorPtr result;
  aggregate->extractValues(groups.data(), 2, &result);
  auto expected = makeRowVector(
      {makeFlatVector<int64_t>({7, 9}),
       makeArrayVector<std::string>({{"a", "b"}, {"later"}})});
  expected->setNull(1, true);
  velox::test::assertEqualVectors(expected, result);
  aggregate->extractValues(groups.data(), 2, &result);
  velox::test::assertEqualVectors(expected, result);
  aggregate->destroy(folly::Range(groups.data(), 2));
}

TEST_F(WasmAggregateTest, sqlLambdasRunSingleGroupedAndAllNativeStages) {
  auto input = makeRowVector(
      {"key", "value"},
      {makeFlatVector<int64_t>({0, 0, 1, 1, 2}),
       makeNullableFlatVector<int64_t>({1, 2, 3, 4, std::nullopt})});
  const std::string call = "reduce_wasm(value, 0, (s,x) -> s+x, (a,b) -> a+b)";
  auto expected = makeRowVector(
      {"key", "result"},
      {makeFlatVector<int64_t>({0, 1, 2}),
       makeNullableFlatVector<int64_t>({3, 7, std::nullopt})});
  auto single = PlanBuilder()
                    .values({input})
                    .singleAggregation({"key"}, {call})
                    .planNode();
  AssertQueryBuilder(single).assertResults(expected);
  auto staged = PlanBuilder()
                    .values({input, input})
                    .partialAggregation({"key"}, {call})
                    .localPartition({})
                    .intermediateAggregation()
                    .localPartition({})
                    .finalAggregation()
                    .planNode();
  AssertQueryBuilder(staged).maxDrivers(1).assertResults(makeRowVector(
      {"key", "result"},
      {makeFlatVector<int64_t>({0, 1, 2}),
       makeNullableFlatVector<int64_t>({6, 14, std::nullopt})}));
  auto global =
      PlanBuilder().values({input}).singleAggregation({}, {call}).planNode();
  AssertQueryBuilder(global).assertResults(
      makeRowVector({makeFlatVector<int64_t>({10})}));
  auto partial =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {}, {"reduce_wasm_partial(value, 0, (s,x) -> s+x, (a,b) -> a+b)"})
          .planNode();
  AssertQueryBuilder(partial).assertResults(makeRowVector({makeRowVector(
      {"value", "seen"},
      {makeFlatVector<int64_t>({10}), makeFlatVector<bool>({true})})}));
}

TEST_F(WasmAggregateTest, sqlLambdaParametersShadowInputColumnNames) {
  auto input = makeRowVector(
      {"key", "value"},
      {makeFlatVector<int64_t>({0, 0, 1, 1, 2}),
       makeNullableFlatVector<int64_t>({1, 2, 3, 4, std::nullopt})});
  auto expected = makeRowVector(
      {makeFlatVector<int64_t>({0, 1, 2}),
       makeNullableFlatVector<int64_t>({3, 7, std::nullopt})});
  for (const std::string name : {"reduce_agg", "reduce_wasm"}) {
    SCOPED_TRACE(name);
    const std::string call =
        name + "(value, 0, (key,value) -> key+value, (value,key) -> value+key)";
    auto single = PlanBuilder()
                      .values({input})
                      .singleAggregation({"key"}, {call})
                      .planNode();
    AssertQueryBuilder(single).assertResults(expected);
    auto staged = PlanBuilder()
                      .values({input})
                      .partialAggregation({"key"}, {call})
                      .localPartition({})
                      .intermediateAggregation()
                      .localPartition({})
                      .finalAggregation()
                      .planNode();
    AssertQueryBuilder(staged).maxDrivers(1).assertResults(expected);
    auto streaming = PlanBuilder()
                         .values({input})
                         .streamingAggregation(
                             {"key"},
                             {call},
                             {},
                             core::AggregationNode::Step::kSingle,
                             false)
                         .planNode();
    AssertQueryBuilder(streaming).assertResults(expected);
    auto global =
        PlanBuilder().values({input}).singleAggregation({}, {call}).planNode();
    AssertQueryBuilder(global).assertResults(
        makeRowVector({makeFlatVector<int64_t>({10})}));
  }
  auto companion =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {"key"},
              {"reduce_wasm_partial(value, 0, (key,value) -> key+value, (value,key) -> value+key)"})
          .planNode();
  AssertQueryBuilder(companion).assertResults(makeRowVector(
      {makeFlatVector<int64_t>({0, 1, 2}),
       makeRowVector(
           {"value", "seen"},
           {makeNullableFlatVector<int64_t>({3, 7, std::nullopt}),
            makeFlatVector<bool>({true, true, false})})}));
}

TEST_F(WasmAggregateTest, lambdaVariadicManifestKeepsFunctionBeforeTail) {
  const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  for (const auto& name : {"map_variadic_wasm", "map_const_variadic_wasm"}) {
    const auto it = std::find_if(
        declarations.aggregates.begin(),
        declarations.aggregates.end(),
        [&](const auto& value) { return value.name == name; });
    ASSERT_NE(it, declarations.aggregates.end());
    EXPECT_TRUE(it->signature->variableArity());
    EXPECT_EQ(it->lambdaCount, 1);
    ASSERT_EQ(it->signature->argumentTypes().size(), 4);
    EXPECT_EQ(it->signature->argumentTypeAt(0).baseName(), "T");
    EXPECT_EQ(it->signature->argumentTypeAt(1).baseName(), "bigint");
    EXPECT_TRUE(it->signature->isLambdaArgumentAt(2));
    EXPECT_EQ(it->signature->argumentTypeAt(3).baseName(), "T");
    EXPECT_EQ(
        it->signature->constantArguments(),
        (std::vector<bool>{
            false,
            true,
            false,
            std::string(name) == "map_const_variadic_wasm"}));
  }
}

TEST_F(WasmAggregateTest, sqlLambdaVariadicMatchesNativeSumAtAllStages) {
  auto input = makeRowVector(
      {"key", "head", "t1", "t2"},
      {makeFlatVector<int64_t>({0, 0, 1, 1, 2}),
       makeNullableFlatVector<int64_t>({1, 2, std::nullopt, 4, std::nullopt}),
       makeNullableFlatVector<int64_t>(
           {10, 20, 30, std::nullopt, std::nullopt}),
       makeNullableFlatVector<int64_t>(
           {100, std::nullopt, 300, 400, std::nullopt})});
  for (int tails : {0, 1, 2}) {
    std::string value = "coalesce(head,0)";
    std::string allNull = "head is null";
    std::string call = "map_variadic_wasm(head, 2, head -> head";
    for (int i = 1; i <= tails; ++i) {
      const auto name = fmt::format("t{}", i);
      value += "+coalesce(" + name + ",0)";
      allNull += " and " + name + " is null";
      call += "," + name;
    }
    call += ")";
    const std::string nativeValue =
        "if(" + allNull + ",cast(null as bigint),2*(" + value + ")) as mapped";
    for (bool grouped : {false, true}) {
      const std::vector<std::string> keys = grouped
          ? std::vector<std::string>{"key"}
          : std::vector<std::string>{};
      auto expected = AssertQueryBuilder(
                          PlanBuilder()
                              .values({input, input})
                              .project({"key", nativeValue})
                              .singleAggregation(keys, {"sum(mapped)"})
                              .planNode())
                          .copyResults(pool());
      for (bool staged : {false, true}) {
        SCOPED_TRACE(
            fmt::format(
                "tails={} grouped={} staged={}", tails, grouped, staged));
        PlanBuilder builder;
        builder.values({input, input});
        if (staged)
          builder.partialAggregation(keys, {call})
              .localPartition({})
              .intermediateAggregation()
              .localPartition({})
              .finalAggregation();
        else
          builder.singleAggregation(keys, {call});
        AssertQueryBuilder(builder.planNode())
            .maxDrivers(1)
            .assertResults(expected);
      }
    }
  }
  auto partial = PlanBuilder()
                     .values({input})
                     .singleAggregation(
                         {}, {"map_variadic_wasm_partial(head,2,x -> x,t1,t2)"})
                     .planNode();
  AssertQueryBuilder(partial).assertResults(makeRowVector({makeRowVector(
      {"total", "count"},
      {makeFlatVector<int128_t>({1734}), makeFlatVector<int64_t>({9})})}));
}

TEST_F(WasmAggregateTest, sqlLambdaVariadicBindsNestedTypesAfterFunction) {
  constexpr vector_size_t count = 5003;
  std::vector<std::vector<std::optional<int64_t>>> values(count);
  for (vector_size_t row = 0; row < count; ++row)
    values[row] = {row, std::nullopt};
  auto head = makeNullableArrayVector<int64_t>(values);
  auto tail = makeNullableArrayVector<int64_t>(values);
  for (vector_size_t row = 0; row < count; ++row) {
    head->setNull(row, row % 7 == 0 || row % 31 == 30);
    tail->setNull(row, row % 11 == 0 || row % 31 == 30);
  }
  auto input = makeRowVector(
      {"key", "head", "tail"},
      {makeFlatVector<int64_t>(count, [](auto row) { return row % 31; }),
       head,
       tail});
  const std::string nativeValue =
      "if(head is null and tail is null,cast(null as bigint),"
      "2*(coalesce(cardinality(head),0)+coalesce(cardinality(tail),0))) as mapped";
  for (bool grouped : {false, true}) {
    const std::vector<std::string> keys =
        grouped ? std::vector<std::string>{"key"} : std::vector<std::string>{};
    auto expected = AssertQueryBuilder(
                        PlanBuilder()
                            .values({input, input})
                            .project({"key", nativeValue})
                            .singleAggregation(keys, {"sum(mapped)"})
                            .planNode())
                        .copyResults(pool());
    for (const std::string call :
         {"map_variadic_wasm(head,2,x -> cardinality(x),tail)",
          "tail_map_variadic_wasm(2,x -> cardinality(x),head,tail)"}) {
      for (bool staged : {false, true}) {
        SCOPED_TRACE(
            fmt::format("{} grouped={} staged={}", call, grouped, staged));
        PlanBuilder builder;
        builder.values({input, input});
        if (staged)
          builder.partialAggregation(keys, {call})
              .localPartition({})
              .intermediateAggregation()
              .localPartition({})
              .finalAggregation();
        else
          builder.singleAggregation(keys, {call});
        AssertQueryBuilder(builder.planNode())
            .maxDrivers(1)
            .assertResults(expected);
      }
    }
  }
  VELOX_ASSERT_USER_THROW(
      PlanBuilder().values({input}).singleAggregation(
          {}, {"tail_map_variadic_wasm(2,x -> cardinality(x))"}),
      "Cannot infer lambda parameter types");
  EXPECT_THROW(
      PlanBuilder().values({input}).singleAggregation(
          {}, {"map_variadic_wasm(head,2,x -> cardinality(x),key)"}),
      VeloxException);
}

TEST_F(
    WasmAggregateTest,
    sqlLambdaVariadicProjectsConstantFlagsAcrossFunction) {
  auto input = makeRowVector(
      {"head", "tail"},
      {makeNullableFlatVector<int64_t>({1, 2, std::nullopt, 4, std::nullopt}),
       makeFlatVector<int64_t>({10, 20, 30, 40, 50})});
  for (const auto& [call, expected] :
       std::vector<std::pair<std::string, int64_t>>{
           {"map_const_variadic_wasm(head,2,x -> x,10,20)", 314},
           {"map_const_variadic_wasm(head,2,x -> x)", 14},
           {"map_const_variadic_wasm(head,2,x -> x,cast(null as bigint))",
            14}}) {
    auto plan =
        PlanBuilder().values({input}).singleAggregation({}, {call}).planNode();
    AssertQueryBuilder(plan).assertResults(
        makeRowVector({makeFlatVector<int64_t>({expected})}));
  }
  for (const std::string call :
       {"map_const_variadic_wasm(head,2,x -> x,tail)",
        "map_const_variadic_wasm(head,2,x -> x,10,tail)",
        "map_variadic_wasm(head,tail,x -> x,10)"}) {
    auto plan =
        PlanBuilder().values({input}).singleAggregation({}, {call}).planNode();
    VELOX_ASSERT_USER_THROW(
        AssertQueryBuilder(plan).copyResults(pool()), "must be constant");
  }
  auto nullLambda =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {}, {"map_variadic_wasm(head,2,x -> cast(null as bigint),tail)"})
          .planNode();
  VELOX_ASSERT_USER_THROW(
      AssertQueryBuilder(nullLambda).copyResults(pool()),
      "map lambda must return non-NULL BIGINT");
}

TEST_F(
    WasmAggregateTest,
    sqlLambdaHeterogeneousVariadicAndIndependentCompanions) {
  auto items = makeArrayVector<int64_t>({{1, 2}, {}, {3}, {}, {}});
  items->setNull(3, true);
  items->setNull(4, true);
  // The author only observes NULL; arbitrary VARCHAR bytes need no Rust UTF-8
  // decoding. Native predicates likewise inspect validity, not string content.
  const std::string bytes("\xff\0", 2);
  auto input = makeRowVector(
      {"key", "number", "text", "items"},
      {makeFlatVector<int64_t>({0, 0, 1, 1, 2}),
       makeNullableFlatVector<int64_t>({1, 2, std::nullopt, 4, std::nullopt}),
       makeNullableFlatVector<StringView>(
           {StringView(bytes),
            std::nullopt,
            StringView("abc"),
            std::nullopt,
            std::nullopt}),
       items});
  const std::string call =
      "map_non_null_variadic_wasm(2,n -> n+1,number,text,items)";
  const std::string nativeValue =
      "if(number is null and text is null and items is null,cast(null as bigint),"
      "4*(if(number is null,0,1)+if(text is null,0,1)+if(items is null,0,1))) as mapped";
  for (bool grouped : {false, true}) {
    const std::vector<std::string> keys =
        grouped ? std::vector<std::string>{"key"} : std::vector<std::string>{};
    auto expected = AssertQueryBuilder(
                        PlanBuilder()
                            .values({input})
                            .project({"key", nativeValue})
                            .singleAggregation(keys, {"sum(mapped)"})
                            .planNode())
                        .copyResults(pool());
    for (bool staged : {false, true}) {
      PlanBuilder builder;
      builder.values({input});
      if (staged)
        builder.partialAggregation(keys, {call})
            .intermediateAggregation()
            .finalAggregation();
      else
        builder.singleAggregation(keys, {call});
      AssertQueryBuilder(builder.planNode())
          .maxDrivers(1)
          .assertResults(expected);
    }
    // Merge/finalize need no expression in this example, and FUNCTION has
    // concrete types: standalone companions can use the typed intermediate.
    const std::string partial =
        "map_non_null_variadic_wasm_partial(2,n -> n+1,number,text,items) as part";
    auto mergeExtract =
        PlanBuilder()
            .values({input})
            .singleAggregation(keys, {partial})
            .singleAggregation(
                keys, {"map_non_null_variadic_wasm_merge_extract(part)"})
            .planNode();
    AssertQueryBuilder(mergeExtract).assertResults(expected);
    auto merge =
        PlanBuilder()
            .values({input})
            .singleAggregation(keys, {partial})
            .singleAggregation(
                keys, {"map_non_null_variadic_wasm_merge(part) as merged"});
    if (grouped)
      merge.project({"key", "map_non_null_variadic_wasm_extract(merged)"});
    else
      merge.project({"map_non_null_variadic_wasm_extract(merged)"});
    AssertQueryBuilder(merge.planNode()).assertResults(expected);
  }
  auto empty =
      PlanBuilder()
          .values({input})
          .singleAggregation({}, {"map_non_null_variadic_wasm(2,n -> n+1)"})
          .planNode();
  AssertQueryBuilder(empty).assertResults(
      makeRowVector({makeAllNullFlatVector<int64_t>(1)}));
}

TEST_F(WasmAggregateTest, independentCompanionsPermitErasedInputLambdaTypes) {
  auto arrays = makeArrayVector<int64_t>({{1, 2}, {3}, {}, {4}, {}});
  arrays->setNull(4, true);
  auto input = makeRowVector(
      {"key", "shard", "number", "items"},
      {makeFlatVector<int64_t>({0, 0, 1, 1, 2}),
       makeFlatVector<int64_t>({0, 1, 0, 1, 0}),
       makeNullableFlatVector<int64_t>({1, 2, 3, 4, std::nullopt}),
       arrays});
  for (bool nested : {false, true}) {
    const std::string value = nested ? "items" : "number";
    const std::string map = nested ? "x -> cardinality(x)" : "x -> x+1";
    const std::string partial = "map_variadic_wasm_partial(" + value + ",2," +
        map + "," + value + ") as part";
    const std::string nativeValue =
        nested ? "4*cardinality(items) as mapped" : "4*(number+1) as mapped";
    for (bool grouped : {false, true}) {
      SCOPED_TRACE(fmt::format("nested={} grouped={}", nested, grouped));
      const std::vector<std::string> keys = grouped
          ? std::vector<std::string>{"key"}
          : std::vector<std::string>{};
      const std::vector<std::string> partialKeys = grouped
          ? std::vector<std::string>{"key", "shard"}
          : std::vector<std::string>{"shard"};
      auto expected = AssertQueryBuilder(
                          PlanBuilder()
                              .values({input})
                              .project({"key", nativeValue})
                              .singleAggregation(keys, {"sum(mapped)"})
                              .planNode())
                          .copyResults(pool());
      auto mergeExtract =
          PlanBuilder()
              .values({input})
              .singleAggregation(partialKeys, {partial})
              .singleAggregation(
                  keys, {"map_variadic_wasm_merge_extract(part)"})
              .planNode();
      AssertQueryBuilder(mergeExtract).maxDrivers(1).assertResults(expected);
      auto merge = PlanBuilder()
                       .values({input})
                       .singleAggregation(partialKeys, {partial})
                       .singleAggregation(
                           keys, {"map_variadic_wasm_merge(part) as merged"});
      if (grouped)
        merge.project({"key", "map_variadic_wasm_extract(merged)"});
      else
        merge.project({"map_variadic_wasm_extract(merged)"});
      AssertQueryBuilder(merge.planNode())
          .maxDrivers(1)
          .assertResults(expected);
    }
  }
}

TEST_F(WasmAggregateTest, independentExtractDoesNotRequireUnusedLambdaTypes) {
  auto input = makeRowVector(
      {"key", "items"},
      {makeFlatVector<int64_t>({0, 0, 1, 2}),
       makeNullableArrayVector<int64_t>(
           std::vector<std::optional<std::vector<std::optional<int64_t>>>>{
               std::vector<std::optional<int64_t>>{1, 2},
               std::vector<std::optional<int64_t>>{3},
               std::vector<std::optional<int64_t>>{},
               std::nullopt})});
  auto extract =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {"key"},
              {"reduce_wasm_partial(items,0,(s,x) -> s+cardinality(x),(a,b) -> a+b) as part"})
          .project({"key", "reduce_wasm_extract(part)"})
          .planNode();
  AssertQueryBuilder(extract).assertResults(makeRowVector(
      {makeFlatVector<int64_t>({0, 1, 2}),
       makeNullableFlatVector<int64_t>({3, 0, std::nullopt})}));
}

TEST_F(WasmAggregateTest, independentMergeCannotInventLambdaAuthority) {
  auto input = makeRowVector(
      {"shard", "items"},
      {makeFlatVector<int64_t>({0, 1}),
       makeArrayVector<int64_t>({{1, 2}, {3}})});
  auto merge =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {"shard"},
              {"reduce_wasm_partial(items,0,(s,x) -> s+cardinality(x),(a,b) -> a+b) as part"})
          .singleAggregation({}, {"reduce_wasm_merge_extract(part)"})
          .planNode();
  VELOX_ASSERT_THROW(
      AssertQueryBuilder(merge).maxDrivers(1).copyResults(pool()),
      "Wasm aggregate lambda expressions are not bound");
}

TEST_F(WasmAggregateTest, sqlLambdaGenericNestedInputAndState) {
  auto input = makeRowVector({makeArrayVector<int64_t>({{1, 2}, {3}, {}})});
  auto plan =
      PlanBuilder()
          .values({input})
          .partialAggregation(
              {},
              {"reduce_wasm(c0, 0, (s,x) -> s+cardinality(x), (a,b) -> a+b)"})
          .finalAggregation()
          .planNode();
  AssertQueryBuilder(plan).assertResults(
      makeRowVector({makeFlatVector<int64_t>({3})}));
  auto rows = makeRowVector(
      {makeFlatVector<int64_t>({1, 2, 3}),
       makeArrayVector<int64_t>({{}, {}, {}})});
  auto nested =
      PlanBuilder()
          .values({rows})
          .singleAggregation(
              {},
              {"reduce_wasm(c0, c1, (s,x) -> concat(s,array[x]), (a,b) -> concat(a,b))"})
          .planNode();
  AssertQueryBuilder(nested).assertResults(
      makeRowVector({makeArrayVector<int64_t>({{1, 2, 3}})}));
}

TEST_F(WasmAggregateTest, sqlLambdaErrorsKeepNativeUserCategory) {
  auto input = makeRowVector({makeFlatVector<int64_t>({1, 0, 2})});
  auto plan = PlanBuilder()
                  .values({input})
                  .singleAggregation(
                      {}, {"reduce_wasm(c0, 0, (s,x) -> s+10/x, (a,b) -> a+b)"})
                  .planNode();
  VELOX_ASSERT_USER_THROW(
      AssertQueryBuilder(plan).copyResults(pool()), "division by zero");
  auto nullResult =
      PlanBuilder()
          .values({input})
          .singleAggregation(
              {},
              {"reduce_wasm(c0, 0, (s,x) -> cast(null as bigint), (a,b) -> a+b)"})
          .planNode();
  VELOX_ASSERT_USER_THROW(
      AssertQueryBuilder(nullResult).copyResults(pool()),
      "reduce lambda state cannot be NULL");
}

TEST_F(WasmAggregateTest, sqlLambdaPartialFinalWithForcedSpill) {
  constexpr vector_size_t groups = 256;
  std::vector<RowVectorPtr> input;
  for (int batch = 0; batch < 4; ++batch)
    input.push_back(makeRowVector(
        {makeFlatVector<int64_t>(groups, [](auto row) { return row; }),
         makeFlatVector<int64_t>(
             groups, [batch](auto row) { return row * 2 + batch; })}));
  core::PlanNodeId finalId;
  auto plan =
      PlanBuilder()
          .values(input)
          .partialAggregation(
              {"c0"}, {"reduce_wasm(c1, 0, (s,x) -> s+x, (a,b) -> a+b)"})
          .localPartition({})
          .finalAggregation()
          .capturePlanNodeId(finalId)
          .planNode();
  auto directory = TempDirectoryPath::create();
  TestScopedSpillInjection injection(100, ".*", 1);
  auto task =
      AssertQueryBuilder(plan)
          .spillDirectory(directory->getPath())
          .config(core::QueryConfig::kSpillEnabled, true)
          .config(core::QueryConfig::kAggregationSpillEnabled, true)
          .config(core::QueryConfig::kMaxOutputBatchRows, 64)
          .maxDrivers(1)
          .assertResults(makeRowVector(
              {makeFlatVector<int64_t>(groups, [](auto row) { return row; }),
               makeFlatVector<int64_t>(
                   groups, [](auto row) { return row * 8 + 6; })}));
  EXPECT_GT(exec::toPlanStats(task->taskStats()).at(finalId).spilledBytes, 0);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

TEST_F(
    WasmAggregateTest,
    sqlLambdaBatchesMatchNativeForLargeAndInterleavedGroups) {
  constexpr vector_size_t count = 9001;
  auto input = makeRowVector(
      {makeFlatVector<int64_t>(count, [](auto row) { return row % 31; }),
       makeFlatVector<int64_t>(count, [](auto row) { return row % 17; })});
  for (vector_size_t row = 0; row < count; row += 13)
    input->childAt(1)->setNull(row, true);
  auto plan = [&](const std::string& name, bool grouped, bool staged) {
    const std::vector<std::string> keys =
        grouped ? std::vector<std::string>{"c0"} : std::vector<std::string>{};
    const std::string call = name + "(c1, 0, (s,x) -> s+x, (a,b) -> a+b)";
    PlanBuilder builder;
    builder.values({input, input});
    if (staged)
      return builder.partialAggregation(keys, {call})
          .localPartition({})
          .intermediateAggregation()
          .localPartition({})
          .finalAggregation()
          .planNode();
    return builder.singleAggregation(keys, {call}).planNode();
  };
  for (bool grouped : {false, true}) {
    for (bool staged : {false, true}) {
      SCOPED_TRACE(fmt::format("grouped={} staged={}", grouped, staged));
      auto expected = AssertQueryBuilder(plan("reduce_agg", grouped, staged))
                          .maxDrivers(1)
                          .copyResults(pool());
      AssertQueryBuilder(plan("reduce_wasm", grouped, staged))
          .maxDrivers(1)
          .assertResults(expected);
    }
  }
}

TEST_F(
    WasmAggregateTest,
    lambdaBatchRespectsHostChunkLimitAndMixesRawWithMerge) {
  auto signature = ROW({"s", "x"}, {BIGINT(), BIGINT()});
  auto lambda = std::make_shared<core::LambdaTypedExpr>(
      signature,
      std::make_shared<core::CallTypedExpr>(
          BIGINT(),
          std::vector<core::TypedExprPtr>{
              std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "s"),
              std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "x")},
          "plus"));
  auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  auto manifest = *std::find_if(
      declarations.aggregates.begin(),
      declarations.aggregates.end(),
      [](const auto& value) { return value.name == "reduce_wasm"; });
  manifest.returnType.type = BIGINT();
  manifest.intermediateType.type =
      ROW({"value", "seen"}, {BIGINT(), BOOLEAN()});
  WasmOptions options;
  options.maxLambdaRowsPerCall = 3;
  options.maxLambdaEvaluations = 64;
  auto query = core::QueryCtx::create();
  HashStringAllocator allocator(pool());
  WasmAggregate aggregate(
      BIGINT(),
      manifest,
      WasmModule::compile(WASM_MODULE_PATH),
      options,
      {BIGINT(), BIGINT()},
      {},
      {lambda->type(), lambda->type()});
  aggregate.setLambdaExpressions(
      {lambda, lambda},
      std::make_shared<exec::SimpleExpressionEvaluator>(query.get(), pool()));
  aggregate.setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate.setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  std::vector<BufferPtr> storage;
  std::vector<char*> groups;
  for (int i = 0; i < 2; ++i) {
    storage.push_back(
        AlignedBuffer::allocate<char>(
            offset + aggregate.accumulatorFixedWidthSize(), pool(), 0));
    groups.push_back(storage.back()->asMutable<char>());
  }
  const vector_size_t indices[] = {0, 1};
  aggregate.initializeNewGroups(
      groups.data(), folly::Range<const vector_size_t*>(indices, 2));
  aggregate.addSingleGroupRawInput(
      groups[0],
      SelectivityVector(13),
      {makeFlatVector<int64_t>(13, [](auto) { return 1; }),
       makeConstant<int64_t>(0, 13)},
      false);
  auto intermediate = makeRowVector(
      {"value", "seen"},
      {makeFlatVector<int64_t>({5, 7, 11}),
       makeFlatVector<bool>({true, true, true})});
  for (auto group : groups) {
    aggregate.addSingleGroupIntermediateResults(
        group, SelectivityVector(3), {intermediate}, false);
    aggregate.addSingleGroupRawInput(
        group,
        SelectivityVector(2),
        {makeFlatVector<int64_t>({4, 6}), makeConstant<int64_t>(0, 2)},
        false);
  }
  VectorPtr result;
  aggregate.extractValues(groups.data(), 2, &result);
  velox::test::assertEqualVectors(makeFlatVector<int64_t>({46, 33}), result);
  aggregate.destroy(folly::Range(groups.data(), 2));
}

TEST_F(WasmAggregateTest, activeStoreRebuildRestoresLambdaAuthority) {
  auto signature = ROW({"s", "x"}, {VARCHAR(), VARCHAR()});
  auto field = [](const std::string& name) {
    return std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), name);
  };
  auto inputLambda =
      std::make_shared<core::LambdaTypedExpr>(signature, field("x"));
  auto combineLambda = std::make_shared<core::LambdaTypedExpr>(
      signature,
      std::make_shared<core::CallTypedExpr>(
          VARCHAR(),
          std::vector<core::TypedExprPtr>{field("s"), field("x")},
          "concat"));
  auto query = core::QueryCtx::create();
  HashStringAllocator allocator(pool());
  auto aggregate = exec::Aggregate::create(
      "reduce_wasm",
      core::AggregationNode::Step::kSingle,
      {VARCHAR(), VARCHAR(), inputLambda->type(), combineLambda->type()},
      VARCHAR(),
      core::QueryConfig({}));
  aggregate->setLambdaExpressions(
      {inputLambda, combineLambda},
      std::make_shared<exec::SimpleExpressionEvaluator>(query.get(), pool()));
  aggregate->setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate->setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  std::vector<BufferPtr> storage;
  std::vector<char*> groups;
  for (int i = 0; i < 2; ++i) {
    storage.push_back(
        AlignedBuffer::allocate<char>(
            offset + aggregate->accumulatorFixedWidthSize(), pool(), 0));
    groups.push_back(storage.back()->asMutable<char>());
  }
  const vector_size_t indices[] = {0, 1};
  aggregate->initializeNewGroups(
      groups.data(), folly::Range<const vector_size_t*>(indices, 2));
  aggregate->addSingleGroupRawInput(
      groups[0],
      SelectivityVector(1),
      {makeFlatVector<std::string>({"first"}),
       makeConstant<std::string>("", 1)},
      false);
  aggregate->addSingleGroupRawInput(
      groups[1],
      SelectivityVector(1),
      {makeFlatVector<std::string>({std::string(2UL << 20, 'x')}),
       makeConstant<std::string>("", 1)},
      false);
  aggregate->destroy(folly::Range(groups.data() + 1, 1));
  EXPECT_GT(aggregate->compact(folly::Range(groups.data(), 1)), 1UL << 20);
  aggregate->addSingleGroupRawInput(
      groups[0],
      SelectivityVector(1),
      {makeFlatVector<std::string>({"later"}),
       makeConstant<std::string>("", 1)},
      false);
  auto merged = makeRowVector(
      {"value", "seen"},
      {makeFlatVector<std::string>({"merged"}), makeFlatVector<bool>({true})});
  aggregate->addSingleGroupIntermediateResults(
      groups[0], SelectivityVector(1), {merged}, false);
  VectorPtr result;
  aggregate->extractValues(groups.data(), 1, &result);
  velox::test::assertEqualVectors(
      // Native reduce maps each new value and combines it with existing state.
      makeFlatVector<std::string>({"firstlatermerged"}),
      result);
  aggregate->destroy(folly::Range(groups.data(), 1));
}
} // namespace
} // namespace facebook::velox::functions::wasm::test
