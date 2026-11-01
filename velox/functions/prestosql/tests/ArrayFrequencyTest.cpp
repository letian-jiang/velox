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

#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"

using facebook::velox::test::assertEqualVectors;

namespace facebook::velox::functions::test {

namespace {

class ArrayFrequencyTest : public functions::test::FunctionBaseTest {
 protected:
  VectorPtr testArrayFrequency(
      const VectorPtr& expected,
      const VectorPtr& input) {
    auto actual = evaluate("array_frequency(c0)", makeRowVector({input}));
    assertEqualVectors(expected, actual);
    DecodedVector expectedRows(*expected);
    DecodedVector actualRows(*actual);
    const auto* expectedMap = expectedRows.base()->as<MapVector>();
    const auto* actualMap = actualRows.base()->as<MapVector>();
    for (vector_size_t row = 0; row < input->size(); ++row) {
      if (expected->isNullAt(row)) {
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

  template <typename T>
  void testIntegralType() {
    const auto minimum = std::numeric_limits<T>::min();
    const auto maximum = std::numeric_limits<T>::max();
    auto input = makeNullableArrayVector<T>(
        {{3, 1, 3, std::nullopt, minimum, maximum, minimum},
         {},
         {std::nullopt}});
    auto expected = makeMapVector<T, int32_t>(
        {{{3, 2}, {1, 1}, {minimum, 2}, {maximum, 1}}, {}, {}});
    testArrayFrequency(expected, input);
  }

  template <typename T>
  void testFloatingPointType() {
    const auto nan = std::numeric_limits<T>::quiet_NaN();
    const auto infinity = std::numeric_limits<T>::infinity();
    auto input = makeNullableArrayVector<T>(
        {{nan, nan},
         {2, nan, 2, infinity, -infinity, infinity, std::nullopt, -nan},
         {-0.0, 0.0, -0.0},
         {0.0, -0.0},
         {}});
    auto expected = makeMapVector<T, int32_t>(
        {{},
         {{2, 2}, {infinity, 2}, {-infinity, 1}},
         {{-0.0, 3}},
         {{0.0, 2}},
         {}});
    VectorPtr result = testArrayFrequency(expected, input);
    const auto* map = result->as<MapVector>();
    const auto* keys = map->mapKeys()->asFlatVector<T>();
    EXPECT_TRUE(std::signbit(keys->valueAt(map->offsetAt(2))));
    EXPECT_FALSE(std::signbit(keys->valueAt(map->offsetAt(3))));
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
      {{{2, 1}, {1, 2}, {-2, 1}},
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
      {{{2, 1}, {1, 2}, {-2, 1}}, {}, {{1, 5}, {2, 1}}});

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

TEST_F(ArrayFrequencyTest, scalarTypes) {
  testIntegralType<int8_t>();
  testIntegralType<int16_t>();
  testIntegralType<int32_t>();
  testIntegralType<int128_t>();

  testArrayFrequency(
      makeMapVector<bool, int32_t>({{{true, 2}, {false, 1}}, {}, {}}),
      makeNullableArrayVector<bool>(
          {{true, false, std::nullopt, true}, {}, {std::nullopt}}));

  testArrayFrequency(
      makeMapVector<int32_t, int32_t>(
          {{{3, 2}, {1, 1}}, {}, {}}, MAP(DATE(), INTEGER())),
      makeNullableArrayVector<int32_t>(
          {{3, 1, std::nullopt, 3}, {}, {std::nullopt}}, ARRAY(DATE())));

  testArrayFrequency(
      makeMapVector<Timestamp, int32_t>(
          {{{Timestamp(3, 1), 2}, {Timestamp(3, 2), 1}}, {}, {}}),
      makeNullableArrayVector<Timestamp>(
          {{Timestamp(3, 1), Timestamp(3, 2), std::nullopt, Timestamp(3, 1)},
           {},
           {std::nullopt}}));
}

TEST_F(ArrayFrequencyTest, floatingPointArray) {
  testFloatingPointType<float>();
  testFloatingPointType<double>();
}

TEST_F(ArrayFrequencyTest, emptyNullAndRehash) {
  auto array = makeNullableArrayVector<int64_t>(
      {{3, 1, 3, std::nullopt, 2, 1}, {}, {std::nullopt}, {9, 9}});
  array->setNull(3, true);
  auto expected =
      makeMapVector<int64_t, int32_t>({{{3, 2}, {1, 2}, {2, 1}}, {}, {}, {}});
  expected->setNull(3, true);
  testArrayFrequency(expected, array);

  testArrayFrequency(
      makeMapVector<int64_t, int32_t>(
          8,
          [](auto) { return 1024; },
          [](auto i) { return int64_t(i % 1024); },
          [](auto) { return 2; }),
      makeArrayVector<int64_t>(
          8,
          [](auto) { return 2048; },
          [](auto i) { return int64_t(i % 1024); }));
}

TEST_F(ArrayFrequencyTest, encodedArraysAndElements) {
  auto array = makeNullableArrayVector<int64_t>(
      {{3, 1, 3, std::nullopt}, {2, 2, 2}, {}, {9, 8, 9, 8}});
  // Combine a reversed dictionary with nulls in its wrapper and base.
  const auto numElements = array->elements()->size();
  array->setElements(BaseVector::wrapInDictionary(
      makeNulls(numElements, [](auto i) { return i == 1; }),
      makeIndices(
          numElements, [numElements](auto i) { return numElements - 1 - i; }),
      numElements,
      array->elements()));
  auto expected = makeMapVector<int64_t, int32_t>(
      {{{8, 2}, {9, 1}}, {{2, 3}}, {}, {{3, 2}, {1, 1}}});
  auto indices = makeIndices(7, [](auto i) { return (i * 3) % 4; });
  testArrayFrequency(
      BaseVector::wrapInDictionary(nullptr, indices, 7, expected),
      BaseVector::wrapInDictionary(nullptr, indices, 7, array));
  for (vector_size_t row : {1, 2}) {
    testArrayFrequency(
        BaseVector::wrapInConstant(7, row, expected),
        BaseVector::wrapInConstant(7, row, array));
  }

  array->setElements(
      BaseVector::wrapInConstant(numElements, 0, array->elements()));
  testArrayFrequency(
      makeMapVector<int64_t, int32_t>({{{8, 4}}, {{8, 3}}, {}, {{8, 4}}}),
      array);
}

TEST_F(ArrayFrequencyTest, stringsSurviveInputReleaseAndFlatten) {
  auto array = makeNullableArrayVector<StringView>(
      {{"a long string outside StringView inline storage"_sv,
        "another long string outside inline storage"_sv,
        "a long string outside StringView inline storage"_sv,
        std::nullopt},
       {"short"_sv, "short"_sv},
       {}});
  auto expected = makeMapVector<StringView, int32_t>(
      {{{"a long string outside StringView inline storage"_sv, 2},
        {"another long string outside inline storage"_sv, 1}},
       {{"short"_sv, 2}},
       {}});
  auto result = testArrayFrequency(expected, array);
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
  auto expected = makeMapVector<int64_t, int32_t>(
      {{{3, 2}, {1, 1}},
       {{6, 2}, {1, 1}},
       {},
       {},
       {{4, 2}, {5, 1}},
       {{2, 2}, {3, 1}}});
  auto actual = evaluate(
      "if(c0 % 2 = 0, array_frequency(c1), array_frequency(c2))", data);
  assertEqualVectors(expected, actual);
  BaseVector::flattenVector(actual);
  assertEqualVectors(expected, actual);
}

} // namespace facebook::velox::functions::test
