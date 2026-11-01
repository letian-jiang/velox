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

#include "velox/functions/prestosql/ArrayFunctions.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"

using facebook::velox::test::assertEqualVectors;

namespace facebook::velox::functions::test {

namespace {

class ArrayFrequencyTest : public functions::test::FunctionBaseTest {
 protected:
  static void SetUpTestCase() {
    FunctionBaseTest::SetUpTestCase();
    registerFunction<
        ParameterBinder<ArrayFrequencyFunction, int64_t>,
        Map<int64_t, int32_t>,
        Array<int64_t>>({"frequency_reference_bigint"});
    registerFunction<
        ParameterBinder<ArrayFrequencyFunction, Varchar>,
        Map<Varchar, int32_t>,
        Array<Varchar>>({"frequency_reference_varchar"});
  }

  void testArrayFrequency(const VectorPtr& expected, const VectorPtr& input) {
    auto result =
        evaluate<BaseVector>("array_frequency(C0)", makeRowVector({input}));
    assertEqualVectors(expected, result);
  }

  VectorPtr check(const VectorPtr& input) {
    const auto type = input->type()->childAt(0);
    const auto name = type->isBigint() ? "frequency_reference_bigint"
                                       : "frequency_reference_varchar";
    auto data = makeRowVector({input});
    auto expected = evaluate(std::string(name) + "(c0)", data);
    auto actual = evaluate("array_frequency(c0)", data);
    assertEqualVectors(expected, actual);
    DecodedVector expectedRows(*expected);
    DecodedVector actualRows(*actual);
    const auto* expectedMap = expectedRows.base()->as<MapVector>();
    const auto* actualMap = actualRows.base()->as<MapVector>();
    for (vector_size_t row = 0; row < input->size(); ++row) {
      if (actual->isNullAt(row)) {
        continue;
      }
      const auto expectedRow = expectedRows.index(row);
      const auto actualRow = actualRows.index(row);
      for (vector_size_t i = 0; i < expectedMap->sizeAt(expectedRow); ++i) {
        EXPECT_TRUE(expectedMap->mapKeys()->equalValueAt(
            actualMap->mapKeys().get(),
            expectedMap->offsetAt(expectedRow) + i,
            actualMap->offsetAt(actualRow) + i));
      }
    }
    return actual;
  }
};

} // namespace

TEST_F(ArrayFrequencyTest, integerArray) {
  auto array = makeNullableArrayVector<int64_t>(
      {{2, 1, 1, -2},
       {},
       {1, 2, 1, 1, 1, 1},
       {-1, std::nullopt, -1, -1},
       {std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::max(),
        1,
        std::nullopt,
        0,
        1,
        std::nullopt,
        0}});

  auto expected = makeMapVector<int64_t, int>(
      {{{1, 2}, {2, 1}, {-2, 1}},
       {},
       {{1, 5}, {2, 1}},
       {{-1, 3}},
       {{std::numeric_limits<int64_t>::max(), 2}, {1, 2}, {0, 2}}});

  testArrayFrequency(expected, array);
}

TEST_F(ArrayFrequencyTest, integerArrayWithoutNull) {
  auto array =
      makeArrayVector<int64_t>({{2, 1, 1, -2}, {}, {1, 2, 1, 1, 1, 1}});

  auto expected = makeMapVector<int64_t, int>(
      {{{1, 2}, {2, 1}, {-2, 1}}, {}, {{1, 5}, {2, 1}}});

  testArrayFrequency(expected, array);
}

TEST_F(ArrayFrequencyTest, varcharArray) {
  auto array = makeNullableArrayVector<StringView>({
      {"hello"_sv, "world"_sv, "!"_sv, "!"_sv, "!"_sv},
      {},
      {"hello"_sv, "world"_sv, std::nullopt, "!"_sv, "!"_sv},
      {"helloworldhelloworld"_sv,
       "helloworldhelloworld"_sv,
       std::nullopt,
       "!"_sv,
       "!"_sv},
  });

  auto expected = makeMapVector<StringView, int>(
      {{{"hello"_sv, 1}, {"world"_sv, 1}, {"!"_sv, 3}},
       {},
       {{"hello"_sv, 1}, {"world"_sv, 1}, {"!"_sv, 2}},
       {{"helloworldhelloworld"_sv, 2}, {"!"_sv, 2}}});

  testArrayFrequency(expected, array);
}

TEST_F(ArrayFrequencyTest, varcharArrayWithoutNull) {
  auto array = makeNullableArrayVector<StringView>({
      {"hello"_sv, "world"_sv, "!"_sv, "!"_sv, "!"_sv},
      {},
      {"helloworldhelloworld"_sv, "helloworldhelloworld"_sv, "!"_sv, "!"_sv},
  });
  auto expected = makeMapVector<StringView, int>(
      {{{"hello"_sv, 1}, {"world"_sv, 1}, {"!"_sv, 3}},
       {},
       {{"helloworldhelloworld"_sv, 2}, {"!"_sv, 2}}});

  testArrayFrequency(expected, array);
}

TEST_F(ArrayFrequencyTest, legacyIntegerType) {
  auto input = makeArrayVector<int32_t>({{3, 1, 3, 2}, {}, {9, 9}});
  auto expected =
      makeMapVector<int32_t, int32_t>({{{3, 2}, {1, 1}, {2, 1}}, {}, {{9, 2}}});
  testArrayFrequency(expected, input);
}

TEST_F(ArrayFrequencyTest, emptyNullAndRehash) {
  auto array = makeNullableArrayVector<int64_t>(
      {{3, 1, 3, std::nullopt, 2, 1}, {}, {std::nullopt}, {9, 9}});
  array->setNull(3, true);
  auto result = check(array);
  DecodedVector decoded(*result);
  const auto* map = decoded.base()->as<MapVector>();
  ASSERT_NE(map, nullptr);
  EXPECT_TRUE(map->mapKeys()->isFlatEncoding());
  BaseVector::flattenVector(result);
  EXPECT_TRUE(result->as<MapVector>()->mapKeys()->isFlatEncoding());
  assertEqualVectors(
      evaluate("array_frequency(c0)", makeRowVector({array})), result);

  check(makeArrayVector<int64_t>(
      8, [](auto) { return 2048; }, [](auto i) { return int64_t(i % 1024); }));
}

TEST_F(ArrayFrequencyTest, encodedArraysAndElements) {
  auto array = makeNullableArrayVector<int64_t>(
      {{3, 1, 3, std::nullopt}, {2, 2, 2}, {}, {9, 8, 9, 8}});
  // Use a non-identity mapping plus a dictionary null independent of the
  // underlying vector's nulls. Output indices must address this wrapper.
  const auto n = array->elements()->size();
  array->setElements(BaseVector::wrapInDictionary(
      makeNulls(n, [](auto i) { return i == 1; }),
      makeIndices(n, [n](auto i) { return n - 1 - i; }),
      n,
      array->elements()));
  auto dictionary = BaseVector::wrapInDictionary(
      nullptr, makeIndices(7, [](auto i) { return (i * 3) % 4; }), 7, array);
  auto result = check(dictionary);
  BaseVector::flattenVector(result);
  assertEqualVectors(
      evaluate("array_frequency(c0)", makeRowVector({dictionary})), result);
  check(BaseVector::wrapInConstant(7, 1, array));
  check(BaseVector::wrapInConstant(7, 2, array));

  array->setElements(BaseVector::wrapInConstant(12, 0, array->elements()));
  check(array);
}

TEST_F(ArrayFrequencyTest, stringsSurviveInputReleaseAndFlatten) {
  auto array = makeNullableArrayVector<StringView>(
      {{"a long string outside StringView inline storage"_sv,
        "another long string outside inline storage"_sv,
        "a long string outside StringView inline storage"_sv,
        std::nullopt},
       {"short"_sv, "short"_sv},
       {}});
  auto result = check(array);
  auto expected = makeMapVector<StringView, int32_t>(
      {{{"a long string outside StringView inline storage"_sv, 2},
        {"another long string outside inline storage"_sv, 1}},
       {{"short"_sv, 2}},
       {}});
  array.reset();
  assertEqualVectors(expected, result);
  BaseVector::flattenVector(result);
  assertEqualVectors(expected, result);
}

TEST_F(ArrayFrequencyTest, conditionalRowsAndExistingResult) {
  auto left = makeNullableArrayVector<int64_t>(
      {{3, 3, 1}, {2, std::nullopt}, {}, {9, 9}, {4, 5, 4}, {7}});
  auto right = makeNullableArrayVector<int64_t>(
      {{8}, {6, 6, 1}, {std::nullopt}, {}, {4}, {2, 3, 2}});
  auto data =
      makeRowVector({makeFlatVector<int64_t>({0, 1, 2, 3, 4, 5}), left, right});
  auto expected = evaluate(
      "if(c0 % 2 = 0, frequency_reference_bigint(c1), "
      "frequency_reference_bigint(c2))",
      data);
  auto actual = evaluate(
      "if(c0 % 2 = 0, array_frequency(c1), array_frequency(c2))", data);
  assertEqualVectors(expected, actual);
  BaseVector::flattenVector(actual);
  assertEqualVectors(expected, actual);
}

} // namespace facebook::velox::functions::test
