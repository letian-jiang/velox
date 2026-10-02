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

#include <optional>

#include <gtest/gtest.h>

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Registration.h"

namespace facebook::velox::functions::wasm::test {
namespace {

class WasmScalarTest : public functions::test::FunctionBaseTest {
 protected:
  static void SetUpTestSuite() {
    ASSERT_EQ(registerWasmModule(WASM_MODULE_PATH), 23);
  }
};

TEST_F(WasmScalarTest, loadsEveryUdfFromOneModule) {
  const auto manifests = loadEmbeddedManifests(WASM_MODULE_PATH);
  ASSERT_EQ(manifests.scalars.size(), 17);
  ASSERT_EQ(manifests.aggregates.size(), 7);

  auto sum = evaluate<SimpleVector<int64_t>>(
      "add_i64(c0, c1)",
      makeRowVector({
          makeFlatVector<int64_t>({2}),
          makeFlatVector<int64_t>({3}),
      }));
  EXPECT_EQ(sum->valueAt(0), 5);

  auto negated = evaluate<SimpleVector<bool>>(
      "not_bool(c0)", makeRowVector({makeFlatVector<bool>({true})}));
  EXPECT_FALSE(negated->valueAt(0));
}

TEST_F(WasmScalarTest, batchesRowsAndReturnsValues) {
  auto input = makeRowVector({
      makeFlatVector<int64_t>({1, 20, -4, 100}),
      makeFlatVector<int64_t>({2, 22, 10, -1}),
  });
  auto result = evaluate<SimpleVector<int64_t>>("add_i64(c0, c1)", input);
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({3, 42, 6, 99}), result);
}

TEST_F(WasmScalarTest, dispatchesScalarOverloads) {
  auto integers = evaluate<SimpleVector<int64_t>>(
      "overloaded_add(c0, c1)",
      makeRowVector({
          makeFlatVector<int64_t>({1, 20}),
          makeFlatVector<int64_t>({2, 22}),
      }));
  velox::test::assertEqualVectors(makeFlatVector<int64_t>({3, 42}), integers);

  auto doubles = evaluate<SimpleVector<double>>(
      "overloaded_add(c0, c1)",
      makeRowVector({
          makeFlatVector<double>({1.5, 20.25}),
          makeFlatVector<double>({2.25, 22.5}),
      }));
  velox::test::assertEqualVectors(
      makeFlatVector<double>({3.75, 42.75}), doubles);
}

TEST_F(WasmScalarTest, preservesDefaultNullBehavior) {
  auto input = makeRowVector({
      makeNullableFlatVector<int64_t>({1, std::nullopt, 3}),
      makeNullableFlatVector<int64_t>({10, 20, std::nullopt}),
  });
  auto result = evaluate<SimpleVector<int64_t>>("add_i64(c0, c1)", input);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int64_t>({11, std::nullopt, std::nullopt}),
      result);
}

TEST_F(WasmScalarTest, preservesUnselectedConditionalRows) {
  auto input = makeRowVector({
      makeFlatVector<int64_t>({1, -1, 3, -1}),
      makeFlatVector<int64_t>({10, 20, 30, 40}),
  });
  auto result = evaluate<SimpleVector<int64_t>>(
      "if(c0 > 0, add_i64(c0, c1), c1)", input);
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({11, 20, 33, 40}), result);
}

TEST_F(WasmScalarTest, supportsConstantAndDictionaryArguments) {
  auto base = makeFlatVector<int64_t>({5, 10, 20, 40});
  auto indices = makeIndices({3, 0, 2, 1});
  auto dictionary = BaseVector::wrapInDictionary(nullptr, indices, 4, base);
  auto input = makeRowVector({
      dictionary,
      BaseVector::createConstant(BIGINT(), variant(int64_t{2}), 4, pool()),
  });
  auto result = evaluate<SimpleVector<int64_t>>("add_i64(c0, c1)", input);
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({42, 7, 22, 12}), result);
}

TEST_F(WasmScalarTest, passesNullsWhenDefaultNullBehaviorIsDisabled) {
  auto input = makeRowVector({
      makeFlatVector<std::string>({"a", "b", "c"}),
      makeNullableFlatVector<std::string>({"x", std::nullopt, "z"}),
  });
  auto result =
      evaluate<SimpleVector<StringView>>("prefix(c0, c1)", input);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<std::string>({"ax", std::nullopt, "cz"}), result);
}

TEST_F(WasmScalarTest, supportsBooleanAndMixedWidthNumericFunctions) {
  auto booleans = evaluate<SimpleVector<bool>>(
      "not_bool(c0)",
      makeRowVector({makeFlatVector<bool>({true, false, true})}));
  velox::test::assertEqualVectors(
      makeFlatVector<bool>({false, true, false}), booleans);

  auto integers = evaluate<SimpleVector<int64_t>>(
      "widen(c0, c1, c2)",
      makeRowVector({
          makeFlatVector<int8_t>({1, -2}),
          makeFlatVector<int16_t>({20, 30}),
          makeFlatVector<int32_t>({300, -400}),
      }));
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({321, -372}), integers);

  auto floating = evaluate<SimpleVector<double>>(
      "hypotenuse(c0, c1)",
      makeRowVector({
          makeFlatVector<float>({3.0f, 5.0f}),
          makeFlatVector<double>({4.0, 12.0}),
      }));
  velox::test::assertEqualVectors(
      makeFlatVector<double>({5.0, 13.0}), floating);
}

TEST_F(WasmScalarTest, supportsBinaryDateAndTimestampFunctions) {
  auto binary = evaluate<SimpleVector<int32_t>>(
      "binary_length(c0)",
      makeRowVector({makeFlatVector<StringView>(
          {StringView("abc"), StringView("de")}, VARBINARY())}));
  velox::test::assertEqualVectors(makeFlatVector<int32_t>({3, 2}), binary);

  auto dates = evaluate<SimpleVector<int32_t>>(
      "next_date(c0)",
      makeRowVector({makeFlatVector<int32_t>({0, 20'000}, DATE())}));
  velox::test::assertEqualVectors(
      makeFlatVector<int32_t>({1, 20'001}, DATE()), dates);

  const auto timestamp = Timestamp::fromNanos(1'234'567'890);
  auto timestamps = evaluate<SimpleVector<Timestamp>>(
      "timestamp_identity(c0)",
      makeRowVector({makeFlatVector<Timestamp>({timestamp})}));
  velox::test::assertEqualVectors(
      makeFlatVector<Timestamp>({timestamp}), timestamps);
}

TEST_F(WasmScalarTest, supportsPerRowBusinessErrors) {
  auto input = makeRowVector({
      makeFlatVector<int64_t>({84, 20, 21, 100}),
      makeFlatVector<int64_t>({2, 0, 3, 0}),
  });
  auto result = evaluate<SimpleVector<int64_t>>(
      "try(checked_divide(c0, c1))", input);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int64_t>({42, std::nullopt, 7, std::nullopt}),
      result);

  VELOX_ASSERT_THROW(
      evaluate("checked_divide(c0, c1)", input), "division by zero");
}

} // namespace
} // namespace facebook::velox::functions::wasm::test
