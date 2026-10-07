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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_set>

#include <gtest/gtest.h>

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/functions/Macros.h"
#include "velox/functions/Registerer.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Registration.h"
#include "velox/functions/wasm/TypeBridge.h"
#include "velox/type/tests/utils/CustomTypesForTesting.h"

namespace facebook::velox::functions::wasm::test {
namespace {

template <typename T>
struct NativeMapLookup {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(
      int64_t& out,
      const arg_type<Map<Generic<T1>, int64_t>>& map,
      const arg_type<Generic<T1>>& key) {
    auto entry = map.find(key);
    if (entry == map.end() || !entry->second.has_value())
      return false;
    out = entry->second.value();
    return true;
  }
};

class WasmScalarTest : public functions::test::FunctionBaseTest {
 protected:
  static void SetUpTestCase() {
    FunctionBaseTest::SetUpTestCase();
    registerFunction<
        NativeMapLookup,
        int64_t,
        Map<Generic<T1>, int64_t>,
        Generic<T1>>({"native_map_lookup"});
    const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
    std::unordered_set<std::string> names;
    for (const auto& scalar : declarations.scalars)
      names.insert(scalar.name);
    ASSERT_EQ(
        registerWasmModule(WASM_MODULE_PATH),
        names.size() + declarations.aggregates.size());
  }
};

TEST_F(WasmScalarTest, loadsEveryUdfFromOneModule) {
  const auto manifests = loadEmbeddedManifests(WASM_MODULE_PATH);
  ASSERT_EQ(manifests.scalars.size(), 77);
  ASSERT_EQ(manifests.aggregates.size(), 18);

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

TEST_F(WasmScalarTest, rustGenericSignaturesBindAndExecute) {
  auto integers = makeNullableFlatVector<int64_t>({7, std::nullopt, 9});
  auto strings =
      makeNullableFlatVector<std::string>({"hello", std::nullopt, "world"});
  auto arrays = makeArrayVector<int64_t>({{1, 2}, {}, {3}});
  auto nested = makeRowVector({strings, arrays});
  for (const auto& values :
       std::vector<VectorPtr>{integers, strings, arrays, nested}) {
    auto input = makeRowVector({values});
    velox::test::assertEqualVectors(
        values, evaluate("rust_identity(c0)", input));
    velox::test::assertEqualVectors(
        values, evaluate("rust_owned_identity(c0)", input));
    velox::test::assertEqualVectors(values, evaluate("rust_first(c0)", input));
    velox::test::assertEqualVectors(
        values, evaluate("rust_first(c0,c0,c0)", input));
    velox::test::assertEqualVectors(
        values, evaluate("rust_coalesce(c0,c0,c0)", input));
    auto pairInput = makeRowVector({values, values});
    velox::test::assertEqualVectors(
        evaluate("array_constructor(c0,c1)", pairInput),
        evaluate("rust_pair(c0,c1)", pairInput));
  }
  velox::test::assertEqualVectors(
      strings, evaluate("rust_ascii_dispatch(c0)", makeRowVector({strings})));
  auto ascii = makeFlatVector<std::string>({"a", "b", "c"});
  velox::test::assertEqualVectors(
      ascii, evaluate("rust_ascii_dispatch(c0)", makeRowVector({ascii})));
  auto utf8 = makeFlatVector<std::string>({"élan", "hello", "world"});
  velox::test::assertEqualVectors(
      utf8, evaluate("rust_ascii_dispatch(c0)", makeRowVector({utf8})));
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int64_t>({1, std::nullopt, 3}),
      evaluate("rust_head_dispatch(c0)", makeRowVector({arrays})));
  auto childNulls = makeArrayVector<int64_t>({{1, 2}, {}, {3}});
  childNulls->elements()->setNull(0, true);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int64_t>({std::nullopt, std::nullopt, 3}),
      evaluate("rust_head_dispatch(c0)", makeRowVector({childNulls})));
  auto nullableEqualityInput = makeRowVector({childNulls, childNulls});
  velox::test::assertEqualVectors(
      evaluate("c0=c1", nullableEqualityInput),
      evaluate("rust_equal(c0,c1)", nullableEqualityInput));
  auto tails = makeRowVector({integers, makeFlatVector<int64_t>({10, 20, 30})});
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({7, 20, 9}),
      evaluate("rust_coalesce(c0,c1)", tails));
  auto named = makeRowVector({"identifier", "payload"}, {integers, arrays});
  velox::test::assertEqualVectors(
      integers, evaluate("rust_row_first(c0)", makeRowVector({named})));
  auto results = evaluate("try(rust_array_first(c0))", makeRowVector({arrays}));
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int64_t>({1, std::nullopt, 3}), results);
  VELOX_ASSERT_USER_THROW(
      evaluate("rust_array_first(c0)", makeRowVector({arrays})), "empty array");
  EXPECT_EQ(
      exec::resolveVectorFunction("rust_first", {BIGINT(), VARCHAR()}),
      nullptr);
  EXPECT_EQ(exec::resolveVectorFunction("rust_first", {}), nullptr);
  EXPECT_EQ(
      exec::resolveVectorFunction("rust_known_identity", {UNKNOWN()}), nullptr);
  EXPECT_NE(
      exec::resolveVectorFunction("rust_known_identity", {ARRAY(UNKNOWN())}),
      nullptr);
  EXPECT_EQ(
      exec::resolveVectorFunction("rust_identity", {UNKNOWN()})->kind(),
      TypeKind::UNKNOWN);
  auto unknown = BaseVector::createNullConstant(UNKNOWN(), 3, pool());
  velox::test::assertEqualVectors(
      unknown, evaluate("rust_identity(c0)", makeRowVector({unknown})));
  EXPECT_NE(
      exec::resolveVectorFunction(
          "rust_equal", {ARRAY(UNKNOWN()), ARRAY(UNKNOWN())}),
      nullptr);
  EXPECT_NE(
      exec::resolveVectorFunction(
          "rust_known_equal", {ARRAY(UNKNOWN()), ARRAY(UNKNOWN())}),
      nullptr);
  EXPECT_EQ(
      exec::resolveVectorFunction("rust_known_equal", {UNKNOWN(), UNKNOWN()}),
      nullptr);
  EXPECT_NE(
      exec::resolveVectorFunction("rust_equal", {UNKNOWN(), UNKNOWN()}),
      nullptr);
  auto map = MAP(VARCHAR(), BIGINT());
  EXPECT_NE(exec::resolveVectorFunction("rust_equal", {map, map}), nullptr);
  EXPECT_EQ(exec::resolveVectorFunction("rust_less", {map, map}), nullptr);
  EXPECT_EQ(
      exec::resolveVectorFunction("rust_less", {ARRAY(map), ARRAY(map)}),
      nullptr);
  auto opaque = OPAQUE<int64_t>();
  EXPECT_EQ(
      exec::resolveVectorFunction("rust_equal", {opaque, opaque}), nullptr);
}

TEST_F(WasmScalarTest, rustGenericComparisonUsesVeloxNaNSemantics) {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  auto input = makeRowVector(
      {makeFlatVector<double>({nan, 1.0, -0.0}),
       makeFlatVector<double>({nan, nan, 0.0})});
  velox::test::assertEqualVectors(
      makeFlatVector<bool>({true, false, true}),
      evaluate("rust_equal(c0,c1)", input));
  velox::test::assertEqualVectors(
      evaluate("c0 < c1", input), evaluate("rust_less(c0,c1)", input));
  velox::test::assertEqualVectors(
      evaluate("rust_equal(c0,c1)", input),
      evaluate("rust_known_equal(c0,c1)", input));
  auto nested = makeRowVector({makeArrayVector<int64_t>({{1, 2}, {3}, {}})});
  velox::test::assertEqualVectors(
      makeFlatVector<bool>({true, true, true}),
      evaluate("rust_equal(c0,c0)", nested));
}

TEST_F(WasmScalarTest, rustNumericGenericsProduceConcreteOverloads) {
  for (const auto& values : std::vector<VectorPtr>{
           makeFlatVector<int64_t>({20, 21}),
           makeFlatVector<double>({1.5, 2.25})}) {
    auto input = makeRowVector({values, values});
    velox::test::assertEqualVectors(
        evaluate("c0+c1", input), evaluate("rust_add(c0,c1)", input));
  }
  EXPECT_EQ(
      exec::resolveVectorFunction("rust_add", {VARCHAR(), VARCHAR()}), nullptr);
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
  auto result =
      evaluate<SimpleVector<int64_t>>("if(c0 > 0, add_i64(c0, c1), c1)", input);
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
  auto result = evaluate<SimpleVector<StringView>>("prefix(c0, c1)", input);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<std::string>({"ax", std::nullopt, "cz"}), result);
}

TEST_F(WasmScalarTest, cachesAsciiInputsAndVerifiesActualResultBytes) {
  auto ascii = makeNullableFlatVector<std::string>({"velox", std::nullopt, ""});
  auto result = evaluate<SimpleVector<StringView>>(
      "upper_dispatch(c0)", makeRowVector({ascii}));
  velox::test::assertEqualVectors(
      makeNullableFlatVector<std::string>({"VELOX", std::nullopt, ""}), result);
  EXPECT_EQ(ascii->isAscii(SelectivityVector(ascii->size())), true);
  EXPECT_EQ(result->isAscii(SelectivityVector(result->size())), true);

  auto mixed = makeFlatVector<std::string>({"velox", "élan", ""});
  result = evaluate<SimpleVector<StringView>>(
      "upper_dispatch(c0)", makeRowVector({mixed}));
  velox::test::assertEqualVectors(
      makeFlatVector<std::string>({"VELOX", "ÉLAN", ""}), result);
  EXPECT_EQ(mixed->isAscii(SelectivityVector(mixed->size())), false);
  EXPECT_EQ(result->isAscii(SelectivityVector(result->size())), false);

  // A function without callAscii also publishes verified output metadata.
  result = evaluate<SimpleVector<StringView>>(
      "prefix(c0, c1)",
      makeRowVector(
          {makeFlatVector<std::string>({"é", ""}),
           makeFlatVector<std::string>({"ascii", "ascii"})}));
  EXPECT_EQ(result->isAscii(SelectivityVector(result->size())), false);

  auto dictionary =
      BaseVector::wrapInDictionary(nullptr, makeIndices({2, 0}), 2, mixed);
  result = evaluate<SimpleVector<StringView>>(
      "upper_dispatch(c0)", makeRowVector({dictionary}));
  velox::test::assertEqualVectors(
      makeFlatVector<std::string>({"", "VELOX"}), result);
  // Expr may wrap the verified vector in a dictionary after apply. The
  // dictionary's own ASCII cache is separate, as for native vector functions.
  DecodedVector decoded(*result);
  EXPECT_EQ(
      decoded.base()->as<SimpleVector<StringView>>()->isAscii(
          SelectivityVector(result->size()), decoded.indices()),
      true);

  const auto manifests = loadEmbeddedManifests(WASM_MODULE_PATH);
  const auto upper = std::find_if(
      manifests.scalars.begin(),
      manifests.scalars.end(),
      [](const auto& manifest) { return manifest.name == "upper_dispatch"; });
  ASSERT_NE(upper, manifests.scalars.end());
  EXPECT_TRUE(upper->hasAscii);
}

TEST_F(WasmScalarTest, variadicViewsShortCircuitAndReportAccessedErrorsPerRow) {
  const std::string invalidUtf8(1, static_cast<char>(0xff));
  auto input = makeRowVector({
      makeNullableFlatVector<std::string>(
          {"first", std::nullopt, std::nullopt, std::nullopt}),
      makeNullableFlatVector<std::string>(
          {invalidUtf8, invalidUtf8, "tail", std::nullopt}),
  });
  auto result = evaluate<SimpleVector<StringView>>(
      "try(variadic_first_text(c0, c1))", input);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<std::string>(
          {"first", std::nullopt, "tail", std::nullopt}),
      result);
  VELOX_ASSERT_USER_THROW(
      evaluate("variadic_first_text(c0, c1)", input), "VARCHAR is not UTF-8");

  auto sums = makeRowVector({
      makeFlatVector<int64_t>({20, std::numeric_limits<int64_t>::max(), 7}),
      makeNullableFlatVector<int64_t>({22, 1, std::nullopt}),
  });
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int64_t>({42, std::nullopt, 7}),
      evaluate<SimpleVector<int64_t>>("try(variadic_sum(c0, c1))", sums));
  VELOX_ASSERT_USER_THROW(
      evaluate("variadic_sum(c0, c1)", sums), "sum overflow");
}

TEST_F(WasmScalarTest, defersCodecColumnsThroughNativeLazyDereference) {
  auto type = velox::test::BIGINT_TYPE_WITH_CUSTOM_COMPARISON();
  auto decoded = std::make_shared<std::atomic<size_t>>(0);
  registerWasmTypeCodec(
      {type,
       "query_lazy_i64",
       1,
       [](const BaseVector& source, vector_size_t row) {
         const auto value = source.as<SimpleVector<int64_t>>()->valueAt(row);
         return std::string(
             reinterpret_cast<const char*>(&value), sizeof(value));
       },
       [decoded](
           std::string_view bytes, BaseVector& target, vector_size_t row) {
         ++*decoded;
         VELOX_USER_CHECK_EQ(
             bytes.size(), sizeof(int64_t), "invalid query codec bytes");
         int64_t value;
         std::memcpy(&value, bytes.data(), sizeof(value));
         VELOX_USER_CHECK_NE(value, 999, "injected query codec failure");
         target.as<FlatVector<int64_t>>()->set(row, value);
       }});
  auto plain = makeFlatVector<int64_t>({10, 20, 30});
  auto encoded = makeFlatVector<int64_t>({7, 8, 9}, type);
  auto nested = makeRowVector({"plain", "encoded"}, {plain, encoded});
  auto input = makeRowVector({nested});
  auto plan = exec::test::PlanBuilder()
                  .values({input})
                  .project({"generic_identity(c0) AS result"})
                  .lazyDereference({"result.plain"})
                  .planNode();
  exec::test::AssertQueryBuilder(plan).assertResults(makeRowVector({plain}));
  EXPECT_EQ(decoded->load(), 0);

  plan = exec::test::PlanBuilder()
             .values({input})
             .project({"generic_identity(c0) AS result"})
             .lazyDereference({"result.encoded"})
             .planNode();
  exec::test::AssertQueryBuilder(plan).assertResults(makeRowVector({encoded}));
  EXPECT_EQ(decoded->load(), 3);

  // Native ordinary ROW dereference can load all children. It must still work,
  // and this test does not claim all operators preserve lazy columns.
  *decoded = 0;
  velox::test::assertEqualVectors(
      plain,
      evaluate<SimpleVector<int64_t>>("generic_identity(c0).plain", input));
  EXPECT_EQ(decoded->load(), 3);

  // A codec exception while reading an output is a fatal bridge failure even
  // when the codec throws a UserError and SQL wraps the dereference in TRY.
  encoded->set(1, 999);
  VELOX_ASSERT_THROW(
      evaluate("try(generic_identity(c0).encoded)", input),
      "injected query codec failure");
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
  auto result =
      evaluate<SimpleVector<int64_t>>("try(checked_divide(c0, c1))", input);
  velox::test::assertEqualVectors(
      makeNullableFlatVector<int64_t>({42, std::nullopt, 7, std::nullopt}),
      result);

  VELOX_ASSERT_USER_THROW(
      evaluate("checked_divide(c0, c1)", input), "division by zero");
  VELOX_ASSERT_USER_THROW(
      evaluate(
          "status_result(c0)", makeRowVector({makeFlatVector<int64_t>({1})})),
      "typed user error");
  VELOX_ASSERT_RUNTIME_THROW(
      evaluate(
          "try(status_result(c0))",
          makeRowVector({makeFlatVector<int64_t>({2})})),
      "typed system error");
}

TEST_F(WasmScalarTest, fixedRowSignaturesUseDeclaredNames) {
  auto aliases = makeRowVector(
      {"identifier", "labels"},
      {makeNullableFlatVector<int64_t>({7, 42, std::nullopt, 10}),
       makeArrayVector<std::string>({{"a", "b"}, {"c"}, {}, {}})});
  aliases->setNull(3, true);
  auto input = makeRowVector({aliases});
  VELOX_ASSERT_USER_THROW(
      evaluate("named_row_length(c0)", input),
      "Scalar function signature is not supported");
  auto named = makeRowVector({"id", "values"}, aliases->children());
  named->setNull(3, true);
  auto namedInput = makeRowVector({named});
  auto result = evaluate("named_row_length(c0)", namedInput);
  auto native = evaluate("c0.id + cardinality(c0.values)", namedInput);
  velox::test::assertEqualVectors(native, result);
  EXPECT_EQ(
      aliases->type()->asRow().names(),
      (std::vector<std::string>{"identifier", "labels"}));
}

TEST_F(WasmScalarTest, mapsPreserveNestedNullKeysAcceptedByNative) {
  auto source = makeRowVector(
      {makeNullableFlatVector<int64_t>({std::nullopt, 7}),
       makeFlatVector<int64_t>({1, 2}),
       makeFlatVector<int64_t>({10, 20})});
  // map() restricts indeterminate keys, but native map_from_entries and
  // Simple Function readers/writers can represent non-NULL complex keys
  // containing NULL children. Transport must preserve these native values.
  auto native = evaluate(
      "map_from_entries(array_constructor(row_constructor("
      "row_constructor(c0, c1), c2)))",
      source);
  ASSERT_TRUE(native->as<MapVector>()->mapKeys()->containsNullAt(0));
  ASSERT_FALSE(native->as<MapVector>()->mapKeys()->isNullAt(0));
  auto input = makeRowVector({native});
  for (const std::string name :
       {"generic_identity", "owned_generic_identity"}) {
    auto output = evaluate(name + "(c0)", input);
    velox::test::assertEqualVectors(native, output);
  }
  auto lookupInput =
      makeRowVector({native, native->as<MapVector>()->mapKeys()});
  auto nativeLookup = evaluate("native_map_lookup(c0, c1)", lookupInput);
  auto wasmLookup = evaluate("map_lookup(c0, c1)", lookupInput);
  velox::test::assertEqualVectors(
      makeFlatVector<int64_t>({10, 20}), nativeLookup);
  velox::test::assertEqualVectors(nativeLookup, wasmLookup);
  velox::test::assertEqualVectors(
      nativeLookup, evaluate("rust_map_lookup(c0, c1)", lookupInput));
  auto hashes = evaluate("generic_hash(c0)", input);
  for (vector_size_t row = 0; row < native->size(); ++row)
    EXPECT_EQ(
        hashes->as<SimpleVector<int64_t>>()->valueAt(row),
        static_cast<int64_t>(native->hashValueAt(row)));
  auto equal = evaluate("generic_equal(c0, c0)", input);
  velox::test::assertEqualVectors(makeFlatVector<bool>({true, true}), equal);
  auto missingKey = makeRowVector(
      {makeFlatVector<int64_t>({99, 100}), makeFlatVector<int64_t>({1, 2})});
  auto missing = makeRowVector({native, missingKey});
  velox::test::assertEqualVectors(
      evaluate("native_map_lookup(c0, c1)", missing),
      evaluate("map_lookup(c0, c1)", missing));
  velox::test::assertEqualVectors(
      evaluate("native_map_lookup(c0,c1)", missing),
      evaluate("rust_map_lookup(c0,c1)", missing));
}

TEST_F(WasmScalarTest, preservesNamedRowReturnType) {
  auto nested = makeRowVector(
      {"a", "b"},
      {makeFlatVector<int64_t>({7, 42}),
       makeArrayVector<std::string>({{"first", "second"}, {"third"}})});
  auto input = makeRowVector({nested});
  auto result = evaluate("row_identity(c0)", input);
  EXPECT_EQ(result->type()->asRow().names(), nested->type()->asRow().names());
  velox::test::assertEqualVectors(nested, result);
  auto field = evaluate("row_identity(c0).a", input);
  velox::test::assertEqualVectors(makeFlatVector<int64_t>({7, 42}), field);
}

} // namespace
} // namespace facebook::velox::functions::wasm::test
