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
#include <limits>
#include <numeric>
#include <optional>
#include <set>
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
#include "velox/expression/SpecialFormRegistry.h"
#include "velox/expression/TryExpr.h"
#include "velox/functions/wasm/ArrowIpc.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Registration.h"
#include "velox/functions/wasm/Runtime.h"
#include "velox/functions/wasm/TypeBridge.h"
#include "velox/functions/wasm/tests/WasmTestUtils.h"
#include "velox/type/tests/utils/CustomTypesForTesting.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::functions::wasm::test {
namespace {

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

void testGenericAndVariadic(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  const auto integers = makeFlat<int64_t>(BIGINT(), {7, std::nullopt, 9}, pool);
  auto identity = evaluateCall(
      "generic_identity", BIGINT(), makeRow({integers}, pool), execCtx);
  check(
      identity->equalValueAt(integers.get(), 0, 0) && identity->isNullAt(1),
      "generic bigint identity failed");
  auto strings = makeFlat<StringView>(
      VARCHAR(),
      {StringView("hello"), std::nullopt, StringView("world")},
      pool);
  identity = evaluateCall(
      "generic_identity", VARCHAR(), makeRow({strings}, pool), execCtx);
  check(
      identity->equalValueAt(strings.get(), 0, 0) && identity->isNullAt(1),
      "generic varchar identity failed");
  auto array = BaseVector::create(ARRAY(BIGINT()), 3, pool);
  array->setNull(1, true);
  auto nested = evaluateCall(
      "generic_array_identity", array->type(), makeRow({array}, pool), execCtx);
  check(
      nested->equalValueAt(array.get(), 0, 0) && nested->isNullAt(1),
      "nested generic identity failed");
  check(
      exec::resolveVectorFunction("generic_array_identity", {ARRAY(VARCHAR())})
          ->equivalent(*ARRAY(VARCHAR())),
      "nested generic return binding failed");
  check(
      exec::resolveVectorFunction(
          "generic_first", {BIGINT(), BIGINT(), BIGINT()})
          ->isBigint(),
      "homogeneous variadic binding failed");
  check(
      exec::resolveVectorFunction("generic_first", {BIGINT(), VARCHAR()}) ==
          nullptr,
      "conflicting generic tail was accepted");
  check(
      exec::resolveVectorFunction("generic_first", {}) == nullptr,
      "missing fixed argument was accepted");
  auto first = evaluateCall(
      "generic_first",
      VARCHAR(),
      makeRow({strings, strings, strings}, pool),
      execCtx);
  check(
      first->equalValueAt(strings.get(), 2, 2),
      "generic variadic execution failed");
  const auto mapType = MAP(VARCHAR(), BIGINT());
  check(
      exec::resolveVectorFunction("orderable_identity", {mapType}) == nullptr,
      "orderable constraint accepted a map");
  check(
      exec::resolveVectorFunction("comparable_identity", {mapType}) != nullptr,
      "comparable constraint rejected a comparable map");
  check(
      exec::resolveVectorFunction("known_identity", {UNKNOWN()}) == nullptr,
      "known constraint accepted unknown");
  check(
      exec::resolveVectorFunction("generic_identity", {UNKNOWN()})->kind() ==
          TypeKind::UNKNOWN,
      "unconstrained generic rejected unknown");
  auto unknown = BaseVector::createNullConstant(UNKNOWN(), 3, pool);
  identity = evaluateCall(
      "generic_identity", UNKNOWN(), makeRow({unknown}, pool), execCtx);
  check(
      identity->isNullAt(0) && identity->isNullAt(2),
      "unknown generic execution failed");
  auto counted = evaluateCall(
      "variadic_count",
      BIGINT(),
      makeRow({integers, strings, array}, pool),
      execCtx);
  check(
      counted->as<SimpleVector<int64_t>>()->valueAt(1) == 3,
      "heterogeneous variadic count failed");
  auto summed = evaluateCall(
      "variadic_sum", BIGINT(), makeRow({integers, integers}, pool), execCtx);
  check(
      summed->as<SimpleVector<int64_t>>()->valueAt(0) == 14 &&
          summed->as<SimpleVector<int64_t>>()->valueAt(1) == 0,
      "nullable variadic sum failed");
  // Exercise empty-schema IPC directly, retaining the selected row count.
  const auto& manifest = scalarManifest("variadic_sum");
  WasmInstance instance(
      WasmModule::compile(manifest.wasmPath), manifest.entrypoint);
  auto gathered = gatherToArrowIpc(SelectivityVector(3), {}, pool, BIGINT());
  std::vector<std::optional<std::string>> emptyErrors;
  auto emptySum = decodeArrowIpcResult(
      instance.invoke(gathered.input), BIGINT(), 3, pool, &emptyErrors);
  check(
      emptySum->as<SimpleVector<int64_t>>()->valueAt(2) == 0,
      "zero variadic arguments lost row count");
  auto emptyInput = std::make_shared<RowVector>(
      pool, ROW(std::vector<TypePtr>{}), nullptr, 3, std::vector<VectorPtr>{});
  auto emptyCount =
      evaluateCall("variadic_count", BIGINT(), emptyInput, execCtx);
  check(
      emptyCount->as<SimpleVector<int64_t>>()->valueAt(2) == 0,
      "zero-argument expression failed");
  auto concrete = evaluateCall(
      "priority_identity", BIGINT(), makeRow({integers}, pool), execCtx);
  check(
      concrete->as<SimpleVector<int64_t>>()->valueAt(0) == 42,
      "concrete overload did not take priority");
  auto generic = evaluateCall(
      "priority_identity", VARCHAR(), makeRow({strings}, pool), execCtx);
  check(
      generic->equalValueAt(strings.get(), 0, 0),
      "generic overload fallback failed");
}

void testRustGenericExports(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto integers = makeFlat<int64_t>(BIGINT(), {7, std::nullopt, 9}, pool);
  auto strings = makeFlat<StringView>(
      VARCHAR(),
      {StringView("hello"), std::nullopt, StringView("world")},
      pool);
  for (auto values : std::vector<VectorPtr>{integers, strings}) {
    for (const std::string name :
         {"rust_identity",
          "rust_owned_identity",
          "rust_first",
          "rust_known_identity"}) {
      auto result =
          evaluateCall(name, values->type(), makeRow({values}, pool), execCtx);
      for (vector_size_t row = 0; row < values->size(); ++row)
        check(
            result->equalValueAt(values.get(), row, row),
            "Rust generic export changed a value");
    }
    auto first = evaluateCall(
        "rust_first",
        values->type(),
        makeRow({values, values, values}, pool),
        execCtx);
    check(
        first->equalValueAt(values.get(), 2, 2),
        "Rust variadic export changed a value");
  }
  auto rest = makeFlat<int64_t>(BIGINT(), {10, 20, 30}, pool);
  auto coalesced = evaluateCall(
      "rust_coalesce", BIGINT(), makeRow({integers, rest}, pool), execCtx);
  check(
      coalesced->as<SimpleVector<int64_t>>()->valueAt(1) == 20,
      "Rust variadic access failed");
  auto sum =
      evaluateCall("rust_add", BIGINT(), makeRow({rest, rest}, pool), execCtx);
  check(
      sum->as<SimpleVector<int64_t>>()->valueAt(2) == 60,
      "Rust bigint instantiation failed");
  auto doubles = makeFlat<double>(DOUBLE(), {1.5, 2.25, 3.0}, pool);
  auto doubleSum = evaluateCall(
      "rust_add", DOUBLE(), makeRow({doubles, doubles}, pool), execCtx);
  check(
      doubleSum->as<SimpleVector<double>>()->valueAt(1) == 4.5,
      "Rust double instantiation failed");
  auto arrays = BaseVector::create(ARRAY(BIGINT()), 3, pool);
  arrays->setNull(1, true);
  auto identity = evaluateCall(
      "rust_identity", arrays->type(), makeRow({arrays}, pool), execCtx);
  check(
      identity->equalValueAt(arrays.get(), 0, 0) && identity->isNullAt(1),
      "Rust nested identity failed");
  auto pair = evaluateCall(
      "rust_pair", ARRAY(BIGINT()), makeRow({integers, rest}, pool), execCtx);
  auto pairValues = pair->as<ArrayVector>();
  check(
      pairValues && pairValues->sizeAt(1) == 2 &&
          pairValues->elements()->isNullAt(pairValues->offsetAt(1)),
      "Rust generic writer lost a child NULL");
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  auto numbers = makeFlat<double>(DOUBLE(), {nan, 1.0, -0.0}, pool);
  auto other = makeFlat<double>(DOUBLE(), {nan, nan, 0.0}, pool);
  auto equalResult = evaluateCall(
      "rust_equal", BOOLEAN(), makeRow({numbers, other}, pool), execCtx);
  auto equality = equalResult->as<SimpleVector<bool>>();
  check(
      equality->valueAt(0) && !equality->valueAt(1) && equality->valueAt(2),
      "Rust SQL equality lost NaN semantics");
  auto knownEquality = evaluateCall(
      "rust_known_equal", BOOLEAN(), makeRow({numbers, other}, pool), execCtx);
  check(
      knownEquality->equalValueAt(equalResult.get(), 0, 0),
      "Rust intersected constraint execution failed");
  check(
      exec::resolveVectorFunction("rust_first", {BIGINT(), VARCHAR()}) ==
          nullptr,
      "Rust variadic accepted conflicting type variables");
  check(
      exec::resolveVectorFunction(
          "rust_less", {MAP(VARCHAR(), BIGINT()), MAP(VARCHAR(), BIGINT())}) ==
          nullptr,
      "Rust orderable bound accepted MAP");
  check(
      exec::resolveVectorFunction(
          "rust_equal", {OPAQUE<int64_t>(), OPAQUE<int64_t>()}) == nullptr,
      "Rust comparable bound accepted OPAQUE");
  check(
      exec::resolveVectorFunction("rust_known_identity", {ARRAY(UNKNOWN())}) !=
          nullptr,
      "Rust known bound rejected a known ARRAY root");
  auto unknown = BaseVector::createNullConstant(UNKNOWN(), 3, pool);
  auto unknownResult = evaluateCall(
      "rust_identity", UNKNOWN(), makeRow({unknown}, pool), execCtx);
  check(
      unknownResult->isNullAt(0) && unknownResult->isNullAt(2),
      "Rust generic rejected UNKNOWN");
}

void testRowGenericErrors(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  exec::registerFunctionCallToSpecialForm(
      "try", std::make_unique<exec::TryCallToSpecialForm>());
  auto offsets = AlignedBuffer::allocate<vector_size_t>(4, pool);
  auto sizes = AlignedBuffer::allocate<vector_size_t>(4, pool);
  const std::vector<vector_size_t> offsetValues{0, 1, 1, 2};
  const std::vector<vector_size_t> sizeValues{1, 0, 1, 0};
  std::copy(
      offsetValues.begin(),
      offsetValues.end(),
      offsets->asMutable<vector_size_t>());
  std::copy(
      sizeValues.begin(), sizeValues.end(), sizes->asMutable<vector_size_t>());
  auto array = std::make_shared<ArrayVector>(
      pool,
      ARRAY(BIGINT()),
      nullptr,
      4,
      offsets,
      sizes,
      makeFlat<int64_t>(BIGINT(), {42, 7}, pool));
  array->setNull(3, true);
  auto input = makeRow({array}, pool);
  auto call = std::make_shared<core::CallTypedExpr>(
      BIGINT(),
      std::vector<core::TypedExprPtr>{
          std::make_shared<core::FieldAccessTypedExpr>(ARRAY(BIGINT()), "c0")},
      "generic_array_first");
  auto guarded = std::make_shared<core::CallTypedExpr>(
      BIGINT(), std::vector<core::TypedExprPtr>{call}, "try");
  exec::ExprSet expressions({guarded}, &execCtx);
  exec::EvalCtx context(&execCtx, &expressions, input.get());
  std::vector<VectorPtr> result(1);
  expressions.eval(SelectivityVector(4), context, result);
  auto values = result[0]->as<SimpleVector<int64_t>>();
  check(
      values->valueAt(0) == 42 && values->isNullAt(1) &&
          values->valueAt(2) == 7 && values->isNullAt(3),
      "generic row errors failed to preserve successful rows under TRY");
  auto left = makeFlat<int64_t>(
      BIGINT(), {20, std::numeric_limits<int64_t>::max(), 7}, pool);
  auto right = makeFlat<int64_t>(BIGINT(), {22, 1, std::nullopt}, pool);
  const auto& manifest = scalarManifest("variadic_sum");
  WasmInstance instance(
      WasmModule::compile(manifest.wasmPath), manifest.entrypoint);
  auto gathered =
      gatherToArrowIpc(SelectivityVector(3), {left, right}, pool, BIGINT());
  std::vector<std::optional<std::string>> errors;
  auto sum = decodeArrowIpcResult(
      instance.invoke(gathered.input), BIGINT(), 3, pool, &errors);
  auto sums = sum->as<SimpleVector<int64_t>>();
  check(
      sums->valueAt(0) == 42 && sums->isNullAt(1) && sums->valueAt(2) == 7 &&
          errors[1] == "sum overflow" && !errors[0] && !errors[2],
      "typed variadic row errors failed to isolate overflow");
}

void testDecimalGenerics(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  for (const auto precision : {10, 38}) {
    auto type = DECIMAL(precision, 4);
    check(
        exec::resolveVectorFunction("decimal_identity", {type})
            ->equivalent(*type),
        "decimal precision/scale binding failed");
    VectorPtr values;
    if (precision <= 18)
      values = makeFlat<int64_t>(type, {123456, std::nullopt, -70000}, pool);
    else
      values = makeFlat<int128_t>(
          type,
          {static_cast<int128_t>(123456),
           std::nullopt,
           -(static_cast<int128_t>(1) << 90)},
          pool);
    auto result = evaluateCall(
        "decimal_identity", type, makeRow({values}, pool), execCtx);
    for (vector_size_t row = 0; row < 3; ++row)
      check(
          result->equalValueAt(values.get(), row, row),
          "decimal Arrow round trip changed a value");
  }
  auto inputType = DECIMAL(18, 4);
  auto outputType = DECIMAL(19, 4);
  check(
      exec::resolveVectorFunction("decimal_widen", {inputType})
          ->equivalent(*outputType),
      "decimal precision constraint did not resolve");
  check(
      exec::resolveVectorFunction("priority_identity", {inputType})
          ->equivalent(*outputType),
      "decimal integer parameters were incorrectly ranked as generic SQL types");
  auto decimalValues =
      makeFlat<int64_t>(inputType, {123456, std::nullopt, -70000}, pool);
  auto widened = evaluateCall(
      "decimal_widen", outputType, makeRow({decimalValues}, pool), execCtx);
  auto widenedValues = widened->as<SimpleVector<int128_t>>();
  check(
      widenedValues->valueAt(0) == 123456 && widenedValues->isNullAt(1) &&
          widenedValues->valueAt(2) == -70000,
      "decimal widening across the short/long boundary changed values");
  check(
      exec::resolveVectorFunction("decimal_identity", {BIGINT()}) == nullptr,
      "decimal signature accepted bigint");
}

void testScalarInitialization(memory::MemoryPool* pool) {
  auto query = core::QueryCtx::Builder()
                   .queryConfig(
                       core::QueryConfig(
                           {{"wasm_prefix_suffix", "!"},
                            {"private_token", "must-not-be-sent"}}))
                   .build();
  core::ExecCtx execCtx(pool, query.get());
  auto input = makeRow(
      {makeFlat<StringView>(
          VARCHAR(), {StringView("one"), StringView("two")}, pool)},
      pool);
  auto run = [&](const std::optional<std::string>& prefix) {
    auto constant =
        BaseVector::create<FlatVector<StringView>>(VARCHAR(), 1, pool);
    if (prefix)
      constant->set(0, StringView(*prefix));
    else
      constant->setNull(0, true);
    auto call = std::make_shared<core::CallTypedExpr>(
        VARCHAR(),
        std::vector<core::TypedExprPtr>{
            std::make_shared<core::ConstantTypedExpr>(constant),
            std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "c0")},
        "initialized_prefix");
    exec::ExprSet expression({call}, &execCtx);
    for (int batch = 0; batch < 2; ++batch) {
      exec::EvalCtx context(&execCtx, &expression, input.get());
      std::vector<VectorPtr> results(1);
      expression.eval(SelectivityVector(2), context, results);
      auto* strings = results[0]->as<SimpleVector<StringView>>();
      const auto expected = prefix.value_or("<null>");
      check(
          strings->valueAt(0).str() == expected + "one!" &&
              strings->valueAt(1).str() == expected + "two!",
          "constant/config initialization or instance isolation failed");
    }
  };
  run("first:");
  run("second:");
  run(std::nullopt);
  bool failed = false;
  try {
    evaluateCall("initialized_prefix", VARCHAR(), input, execCtx);
  } catch (const std::exception& error) {
    failed =
        std::string(error.what()).find("must be constant") != std::string::npos;
  }
  check(failed, "nonconstant required argument was accepted");
  auto prefix = BaseVector::wrapInConstant(
      2, 0, makeFlat<StringView>(VARCHAR(), {StringView("fail")}, pool));
  auto function = exec::getVectorFunction(
      "initialized_prefix", {VARCHAR()}, {prefix}, query->queryConfig());
  exec::ExprSet emptyExpressions({}, &execCtx);
  std::vector<VectorPtr> arguments{prefix};
  for (int attempt = 0; attempt < 2; ++attempt) {
    exec::EvalCtx context(&execCtx, &emptyExpressions, input.get());
    VectorPtr result;
    // An unselected branch never runs initialization.
    function->apply(
        SelectivityVector(2, false), arguments, VARCHAR(), context, result);
    check(!result, "unselected scalar initialized or produced a result");
    failed = false;
    try {
      function->apply(
          SelectivityVector(2), arguments, VARCHAR(), context, result);
    } catch (const std::exception& error) {
      failed =
          std::string(error.what()).find("requested initialization failure") !=
          std::string::npos;
    }
    check(failed, "initialization failure was lost or retried");
  }
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
    check(
        arrayResult->equalValueAt(array.get(), row, row),
        "array_identity changed a complex row");
  }
  auto dictionaryIndices = AlignedBuffer::allocate<vector_size_t>(3, pool);
  dictionaryIndices->asMutable<vector_size_t>()[0] = 2;
  dictionaryIndices->asMutable<vector_size_t>()[1] = 0;
  dictionaryIndices->asMutable<vector_size_t>()[2] = 1;
  auto dictionary =
      BaseVector::wrapInDictionary(nullptr, dictionaryIndices, 3, array);
  auto dictionaryResult = evaluateCall(
      "array_identity", ARRAY(BIGINT()), makeRow({dictionary}, pool), execCtx);
  for (vector_size_t row = 0; row < 3; ++row) {
    check(
        dictionaryResult->equalValueAt(dictionary.get(), row, row),
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
    check(
        mapResult->equalValueAt(map.get(), row, row),
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
      std::vector<VectorPtr>{
          makeFlat<int64_t>(BIGINT(), {1, 2}, pool), strings});
  auto rowResult = evaluateCall(
      "row_identity", rowType, makeRow({nestedRow}, pool), execCtx);
  for (vector_size_t row = 0; row < 2; ++row) {
    check(
        rowResult->equalValueAt(nestedRow.get(), row, row),
        "row_identity changed a nested row");
  }
}

ArrayVectorPtr nestedArray(
    const VectorPtr& elements,
    const std::vector<vector_size_t>& offsets,
    const std::vector<vector_size_t>& sizes,
    memory::MemoryPool* pool) {
  auto offsetBuffer =
      AlignedBuffer::allocate<vector_size_t>(offsets.size(), pool);
  auto sizeBuffer = AlignedBuffer::allocate<vector_size_t>(sizes.size(), pool);
  std::copy(
      offsets.begin(), offsets.end(), offsetBuffer->asMutable<vector_size_t>());
  std::copy(sizes.begin(), sizes.end(), sizeBuffer->asMutable<vector_size_t>());
  return std::make_shared<ArrayVector>(
      pool,
      ARRAY(elements->type()),
      nullptr,
      offsets.size(),
      offsetBuffer,
      sizeBuffer,
      elements);
}

void compareGenericWithNative(
    const VectorPtr& input,
    memory::MemoryPool* pool,
    core::ExecCtx& execCtx,
    bool customCodec = false) {
  auto hash = evaluateCall(
      customCodec ? "codec_hash" : "generic_hash",
      BIGINT(),
      makeRow({input}, pool),
      execCtx);
  auto hashes = hash->as<SimpleVector<int64_t>>();
  for (vector_size_t row = 0; row < input->size(); ++row) {
    check(
        hashes->valueAt(row) == static_cast<int64_t>(input->hashValueAt(row)),
        "Wasm/native hash mismatch for " + input->type()->toString() + " row " +
            std::to_string(row));
  }
  auto indices = AlignedBuffer::allocate<vector_size_t>(input->size(), pool);
  for (vector_size_t row = 0; row < input->size(); ++row) {
    indices->asMutable<vector_size_t>()[row] = input->size() - row - 1;
  }
  auto other =
      BaseVector::wrapInDictionary(nullptr, indices, input->size(), input);
  auto compared = evaluateCall(
      customCodec ? "codec_compare" : "generic_compare",
      BIGINT(),
      makeRow({input, other}, pool),
      execCtx);
  auto equal = evaluateCall(
      customCodec ? "codec_equal" : "generic_equal",
      BOOLEAN(),
      makeRow({input, other}, pool),
      execCtx);
  auto sqlEqual = evaluateCall(
      customCodec ? "codec_equal_sql" : "generic_equal_sql",
      BOOLEAN(),
      makeRow({input, other}, pool),
      execCtx);
  for (vector_size_t row = 0; row < input->size(); ++row) {
    auto expected =
        input->compare(other.get(), row, row, CompareFlags{}).value();
    check(
        compared->as<SimpleVector<int64_t>>()->valueAt(row) ==
            (expected < 0       ? -1
                 : expected > 0 ? 1
                                : 0),
        "Wasm/native comparison mismatch for " + input->type()->toString());
    auto flags =
        CompareFlags::equality(CompareFlags::NullHandlingMode::kNullAsValue);
    check(
        equal->as<SimpleVector<bool>>()->valueAt(row) ==
            (input->compare(other.get(), row, row, flags).value() == 0),
        "Wasm/native null-safe equality mismatch");
    flags.nullHandlingMode =
        CompareFlags::NullHandlingMode::kNullAsIndeterminate;
    auto expectedEqual = input->compare(other.get(), row, row, flags);
    auto result = sqlEqual->as<SimpleVector<bool>>();
    check(
        expectedEqual ? !result->isNullAt(row) &&
                result->valueAt(row) == (expectedEqual.value() == 0)
                      : result->isNullAt(row),
        "Wasm/native SQL equality mismatch");
  }
  auto wrappedResult = evaluateCall(
      "generic_wrap", ARRAY(input->type()), makeRow({input}, pool), execCtx);
  auto wrapped = wrappedResult->as<ArrayVector>();
  for (vector_size_t row = 0; row < input->size(); ++row) {
    check(
        !wrapped->isNullAt(row) && wrapped->sizeAt(row) == 1 &&
            wrapped->elements()->equalValueAt(
                input.get(), wrapped->offsetAt(row), row),
        "generic writer did not preserve nested value/null");
  }
}

void testNestedViewsAndWriters(
    memory::MemoryPool* pool,
    core::ExecCtx& execCtx) {
  auto elements = makeFlat<int64_t>(BIGINT(), {20, std::nullopt, 22, 7}, pool);
  auto array = nestedArray(elements, {0, 3, 3, 3}, {3, 0, 1, 1}, pool);
  array->setNull(3, true);
  auto dispatchedSum = evaluateCall(
      "array_sum_dispatch", BIGINT(), makeRow({array}, pool), execCtx);
  auto dispatchedSums = dispatchedSum->as<SimpleVector<int64_t>>();
  check(
      dispatchedSums->valueAt(0) == 42 && dispatchedSums->valueAt(1) == 0 &&
          dispatchedSums->valueAt(2) == 7 && dispatchedSums->isNullAt(3),
      "null-free dispatch/fallback changed NULL semantics");
  auto asciiStrings = makeFlat<StringView>(
      VARCHAR(),
      {StringView("velox"), StringView("élan"), StringView(""), std::nullopt},
      pool);
  auto upper = evaluateCall(
      "upper_dispatch", VARCHAR(), makeRow({asciiStrings}, pool), execCtx);
  auto upperStrings = upper->as<SimpleVector<StringView>>();
  check(
      upperStrings->valueAt(0) == StringView("VELOX") &&
          upperStrings->valueAt(1) == StringView("ÉLAN") &&
          upperStrings->valueAt(2) == StringView("") &&
          upperStrings->isNullAt(3),
      "ASCII dispatch/unicode fallback changed string semantics");
  auto incremented = evaluateCall(
      "nested_array_increment", array->type(), makeRow({array}, pool), execCtx);
  auto expected = nestedArray(
      makeFlat<int64_t>(BIGINT(), {21, std::nullopt, 23, 8}, pool),
      {0, 3, 3, 3},
      {3, 0, 1, 1},
      pool);
  expected->setNull(3, true);
  for (vector_size_t row = 0; row < 4; ++row) {
    check(
        incremented->equalValueAt(expected.get(), row, row),
        "typed array writer lost nested nulls or values");
  }
  auto sumResult = evaluateCall(
      "nullfree_array_sum", BIGINT(), makeRow({array}, pool), execCtx);
  auto sums = sumResult->as<SimpleVector<int64_t>>();
  check(
      sums->isNullAt(0) && sums->valueAt(1) == 0 && sums->valueAt(2) == 7 &&
          sums->isNullAt(3),
      "null-free adapter did not recursively filter NULL inputs");
  compareGenericWithNative(array, pool, execCtx);

  auto mapOffsets = AlignedBuffer::allocate<vector_size_t>(2, pool);
  auto mapSizes = AlignedBuffer::allocate<vector_size_t>(2, pool);
  mapOffsets->asMutable<vector_size_t>()[0] = 0;
  mapOffsets->asMutable<vector_size_t>()[1] = 2;
  mapSizes->asMutable<vector_size_t>()[0] = 2;
  mapSizes->asMutable<vector_size_t>()[1] = 2;
  auto maps = std::make_shared<MapVector>(
      pool,
      MAP(VARCHAR(), array->type()),
      nullptr,
      2,
      mapOffsets,
      mapSizes,
      makeFlat<StringView>(
          VARCHAR(),
          {StringView("a"), StringView("b"), StringView("b"), StringView("a")},
          pool),
      nestedArray(elements, {0, 3, 3, 0}, {3, 1, 1, 3}, pool));
  auto copied = evaluateCall(
      "nested_map_copy", maps->type(), makeRow({maps}, pool), execCtx);
  for (vector_size_t row = 0; row < 2; ++row) {
    check(
        copied->equalValueAt(maps.get(), row, row),
        "typed nested map writer changed values");
  }
  compareGenericWithNative(maps, pool, execCtx);

  auto strings = nestedArray(
      makeFlat<StringView>(
          VARCHAR(),
          {StringView("first"), std::nullopt, StringView("third")},
          pool),
      {0, 2},
      {2, 1},
      pool);
  auto rows = std::make_shared<RowVector>(
      pool,
      ROW({"id", "values"}, {BIGINT(), strings->type()}),
      nullptr,
      2,
      std::vector<VectorPtr>{
          makeFlat<int64_t>(BIGINT(), {42, std::nullopt}, pool), strings});
  copied = evaluateCall(
      "nested_row_copy", rows->type(), makeRow({rows}, pool), execCtx);
  for (vector_size_t row = 0; row < 2; ++row) {
    check(
        copied->equalValueAt(rows.get(), row, row),
        "typed row writer changed fields or child nulls");
  }
  compareGenericWithNative(rows, pool, execCtx);

  auto zeroInput = std::make_shared<RowVector>(
      pool, ROW(std::vector<TypePtr>{}), nullptr, 3, std::vector<VectorPtr>{});
  auto type = ROW({"items"}, {ARRAY(MAP(VARCHAR(), ARRAY(BIGINT())))});
  auto constructedResult =
      evaluateCall("make_nested", type, zeroInput, execCtx);
  BaseVector::flattenVector(constructedResult);
  auto constructed = constructedResult->as<RowVector>();
  auto arrays = constructed->childAt(0)->as<ArrayVector>();
  auto innerMaps = arrays->elements()->as<MapVector>();
  auto innerArrays = innerMaps->mapValues()->as<ArrayVector>();
  check(
      constructed->size() == 3 && arrays->sizeAt(0) == 2 &&
          innerMaps->isNullAt(1) &&
          innerArrays->elements()->as<SimpleVector<int64_t>>()->valueAt(0) ==
              42 &&
          innerArrays->elements()->isNullAt(1),
      "constructed row/array/map/array result did not survive IPC");
}

void testStructuredStatus(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto input = makeRow({makeFlat<int64_t>(BIGINT(), {42, 1, 7}, pool)}, pool);
  auto result = evaluateCall("status_result", BIGINT(), input, execCtx, true);
  auto values = result->as<SimpleVector<int64_t>>();
  check(
      values->valueAt(0) == 42 && values->isNullAt(1) &&
          values->valueAt(2) == 7,
      "structured user error was not isolated to its row");
  auto fatalInput = makeRow({makeFlat<int64_t>(BIGINT(), {2}, pool)}, pool);
  bool failed = false;
  try {
    evaluateCall("status_result", BIGINT(), fatalInput, execCtx, true);
  } catch (const VeloxRuntimeError& error) {
    failed = std::string(error.what()).find("typed system error") !=
        std::string::npos;
  }
  check(failed, "TRY swallowed a native system status");
}

void testOverloadMetadata(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  const auto integer =
      exec::resolveVectorFunctionWithMetadata("mixed_metadata", {BIGINT()});
  const auto string =
      exec::resolveVectorFunctionWithMetadata("mixed_metadata", {VARCHAR()});
  check(
      integer && integer->second.deterministic &&
          integer->second.defaultNullBehavior,
      "integer overload lost metadata");
  check(
      string && !string->second.deterministic &&
          !string->second.defaultNullBehavior,
      "string overload lost metadata");
  const auto unbound = exec::getVectorFunctionMetadata("mixed_metadata");
  check(
      unbound && !unbound->deterministic && !unbound->defaultNullBehavior,
      "unbound metadata must be conservative");
  auto integers = makeFlat<int64_t>(BIGINT(), {42, std::nullopt}, pool);
  auto result = evaluateCall(
      "mixed_metadata", BIGINT(), makeRow({integers}, pool), execCtx);
  check(
      result->equalValueAt(integers.get(), 0, 0) && result->isNullAt(1),
      "integer overload null filtering failed");
  auto strings = makeFlat<StringView>(
      VARCHAR(), {StringView("hello"), std::nullopt}, pool);
  result = evaluateCall(
      "mixed_metadata", VARCHAR(), makeRow({strings}, pool), execCtx);
  check(
      result->as<SimpleVector<StringView>>()->valueAt(1) ==
          StringView("NULL received"),
      "nullable string overload did not receive NULL");
  std::vector<TypePtr> coercions;
  auto coerced = exec::resolveVectorFunctionWithMetadataWithCoercions(
      "mixed_metadata", {SMALLINT()}, coercions, TypeCoercer::defaults());
  check(
      coerced && coerced->second.defaultNullBehavior &&
          coerced->second.deterministic && coercions.size() == 1 &&
          coercions[0]->isBigint(),
      "coercion lost signature metadata");
}

void testGenericComparisonAndHash(
    memory::MemoryPool* pool,
    core::ExecCtx& execCtx) {
  auto hugeints = makeFlat<int128_t>(
      HUGEINT(),
      {std::numeric_limits<int128_t>::min(),
       std::numeric_limits<int128_t>::max(),
       -(int128_t(1) << 100),
       int128_t(0),
       int128_t(-1),
       std::nullopt},
      pool);
  compareGenericWithNative(hugeints, pool, execCtx);
  auto hugeintIdentity = evaluateCall(
      "hugeint_identity", HUGEINT(), makeRow({hugeints}, pool), execCtx);
  for (vector_size_t row = 0; row < hugeints->size(); ++row) {
    check(
        hugeintIdentity->equalValueAt(hugeints.get(), row, row),
        "full-width hugeint scalar lost bits");
  }
  compareGenericWithNative(
      nestedArray(hugeints, {0, 2, 4}, {2, 2, 2}, pool), pool, execCtx);
  compareGenericWithNative(
      makeFlat<int64_t>(BIGINT(), {42, -1, 0, std::nullopt}, pool),
      pool,
      execCtx);
  compareGenericWithNative(
      makeFlat<int32_t>(INTEGER(), {42, -1, 0, std::nullopt}, pool),
      pool,
      execCtx);
  compareGenericWithNative(
      makeFlat<int8_t>(TINYINT(), {42, -1, 0, std::nullopt}, pool),
      pool,
      execCtx);
  compareGenericWithNative(
      makeFlat<bool>(BOOLEAN(), {true, false, std::nullopt}, pool),
      pool,
      execCtx);
  compareGenericWithNative(
      makeFlat<double>(
          DOUBLE(),
          {0.0,
           -0.0,
           std::numeric_limits<double>::quiet_NaN(),
           -std::numeric_limits<double>::quiet_NaN(),
           std::numeric_limits<double>::infinity(),
           std::nullopt},
          pool),
      pool,
      execCtx);
  compareGenericWithNative(
      makeFlat<float>(
          REAL(),
          {0.0F,
           -0.0F,
           std::numeric_limits<float>::quiet_NaN(),
           -std::numeric_limits<float>::quiet_NaN(),
           std::nullopt},
          pool),
      pool,
      execCtx);
  const std::string invalidUtf8("\xff\0\xfe", 3);
  auto rawVarchars = makeFlat<StringView>(
      VARCHAR(),
      {StringView("velox"), StringView(invalidUtf8), std::nullopt},
      pool);
  compareGenericWithNative(rawVarchars, pool, execCtx);
  compareGenericWithNative(
      nestedArray(rawVarchars, {0, 1}, {1, 2}, pool), pool, execCtx);
  auto bytesIdentity = evaluateCall(
      "varchar_bytes_identity",
      VARCHAR(),
      makeRow({rawVarchars}, pool),
      execCtx);
  for (vector_size_t row = 0; row < rawVarchars->size(); ++row) {
    check(
        bytesIdentity->equalValueAt(rawVarchars.get(), row, row),
        "VARCHAR byte view changed raw bytes");
  }
  auto utf8Result = evaluateCall(
      "upper_dispatch", VARCHAR(), makeRow({rawVarchars}, pool), execCtx, true);
  check(
      utf8Result->as<SimpleVector<StringView>>()->valueAt(0) ==
              StringView("VELOX") &&
          utf8Result->isNullAt(1) && utf8Result->isNullAt(2),
      "UTF-8 decoding error was not isolated to its row");
  const std::vector<std::string> text{
      "",
      "a",
      "1234567",
      "12345678",
      "1234567890123456",
      "12345678901234567",
      std::string(24, 'x'),
      std::string(33, 'z')};
  std::vector<std::optional<StringView>> views;
  for (const auto& string : text) {
    views.emplace_back(StringView(string));
  }
  views.emplace_back(std::nullopt);
  compareGenericWithNative(
      makeFlat<StringView>(VARCHAR(), views, pool), pool, execCtx);
  compareGenericWithNative(
      makeFlat<StringView>(VARBINARY(), views, pool), pool, execCtx);
  compareGenericWithNative(
      makeFlat<int32_t>(DATE(), {0, -1, 20, std::nullopt}, pool),
      pool,
      execCtx);
  auto timestamps = makeFlat<Timestamp>(
      TIMESTAMP(),
      {Timestamp(-14831769600LL, 123456789),
       Timestamp(16725225600LL, 987654321),
       Timestamp(Timestamp::kMinSeconds, 0),
       Timestamp(Timestamp::kMaxSeconds, 999999999),
       Timestamp::fromNanos(-1),
       std::nullopt},
      pool);
  compareGenericWithNative(timestamps, pool, execCtx);
  auto timestampIdentity = evaluateCall(
      "timestamp_identity", TIMESTAMP(), makeRow({timestamps}, pool), execCtx);
  for (vector_size_t row = 0; row < timestamps->size(); ++row) {
    check(
        timestampIdentity->equalValueAt(timestamps.get(), row, row),
        "full-range timestamp scalar lost precision");
  }
  auto timestampArrays = nestedArray(timestamps, {0, 2, 4}, {2, 2, 2}, pool);
  compareGenericWithNative(timestampArrays, pool, execCtx);
  compareGenericWithNative(
      makeFlat<int64_t>(DECIMAL(10, 4), {12345, -12345, std::nullopt}, pool),
      pool,
      execCtx);
  compareGenericWithNative(
      makeFlat<int128_t>(
          DECIMAL(38, 4),
          {int128_t(12345), -(int128_t(1) << 100), std::nullopt},
          pool),
      pool,
      execCtx);
}

void testSparseGatherAndScatter(memory::MemoryPool* pool) {
  auto left = makeFlat<int64_t>(BIGINT(), {1, 20, 3, 40}, pool);
  auto right = makeFlat<int64_t>(BIGINT(), {10, 20, 30, 40}, pool);
  SelectivityVector rows(4, false);
  rows.setValid(0, true);
  rows.setValid(2, true);
  rows.updateBounds();

  auto gathered = gatherToArrowIpc(rows, {left, right}, pool, BIGINT());
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
  std::vector<std::optional<std::string>> errors;
  auto compact = decodeArrowIpcResult(
      instance.invoke(gathered.input),
      BIGINT(),
      gathered.rowCount,
      pool,
      &errors);
  check(
      std::none_of(
          errors.begin(),
          errors.end(),
          [](const auto& error) { return error.has_value(); }),
      "sparse scalar unexpectedly failed a row");
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
  auto gathered = gatherToArrowIpc(rows, {input}, pool, INTEGER());
  const auto& manifest = scalarManifest("binary_length");
  WasmInstance instance(
      WasmModule::compile(manifest.wasmPath), manifest.entrypoint);
  std::vector<std::optional<std::string>> errors;
  auto result = decodeArrowIpcResult(
      instance.invoke(gathered.input), INTEGER(), 1, pool, &errors);
  check(
      errors.size() == 1 && !errors[0],
      "large binary scalar unexpectedly failed");
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
  auto gathered = gatherToArrowIpc(rows, {dividends, divisors}, pool, BIGINT());
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
  aggregate->addSingleGroupRawInput(groups.pointers[0], rows, {input}, false);
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
  check(
      lists->sizeAt(0) == 2 && lists->sizeAt(1) == 0,
      "array_collect returned wrong list lengths");
  auto* elements = lists->elements()->as<SimpleVector<int64_t>>();
  check(
      elements != nullptr && elements->valueAt(lists->offsetAt(0)) == 3 &&
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
  partial->extractAccumulators(partialGroups.pointers.data(), 1, &intermediate);
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
  check(
      mergedResult->equalValueAt(result.get(), 0, 0),
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

struct HandleObject {
  int64_t value;
};
struct SerializedObject {
  int64_t value;
};
struct OtherHandleObject {};
std::string encodeInteger(int64_t value) {
  std::string bytes(8, '\0');
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint64_t>(value) >> (i * 8);
  }
  return bytes;
}
int64_t decodeInteger(std::string_view bytes) {
  VELOX_USER_CHECK_EQ(bytes.size(), 8, "Invalid integer codec bytes");
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) {
    bits |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i])) << (i * 8);
  }
  return static_cast<int64_t>(bits);
}
void registerBridgeTestTypes() {
  registerOpaqueType<HandleObject>("wasm_handle_object");
  registerOpaqueType<OtherHandleObject>("wasm_other_handle");
  registerOpaqueType<SerializedObject>("wasm_serialized_object");
  OpaqueType::registerSerialization<SerializedObject>(
      "wasm_serialized_object",
      [](const auto& v) { return encodeInteger(v->value); },
      [](const auto& bytes) {
        return std::make_shared<SerializedObject>(
            SerializedObject{decodeInteger(bytes)});
      });
  registerWasmOpaqueSerializationCodec(
      OPAQUE<SerializedObject>(), "opaque_i64", 1);
  auto type = facebook::velox::test::BIGINT_TYPE_WITH_CUSTOM_COMPARISON();
  registerWasmTypeCodec(
      {type,
       "low8_i64",
       1,
       [](const BaseVector& v, vector_size_t row) {
         return encodeInteger(v.as<SimpleVector<int64_t>>()->valueAt(row));
       },
       [](std::string_view bytes, BaseVector& v, vector_size_t row) {
         v.as<FlatVector<int64_t>>()->set(row, decodeInteger(bytes));
       }});
}
void testOwnedIpcLifetime(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  const std::string bytes(65536, 'b');
  VectorPtr result;
  {
    auto source = makeFlat<StringView>(
        VARBINARY(),
        {StringView(bytes), std::nullopt, StringView("short")},
        pool);
    result = evaluateCall(
        "generic_identity", VARBINARY(), makeRow({source}, pool), execCtx);
  }
  auto nestedSource = nestedArray(
      makeFlat<StringView>(
          VARCHAR(), {StringView(bytes), StringView("later")}, pool),
      {0},
      {2},
      pool);
  auto nestedResult = evaluateCall(
      "generic_identity",
      nestedSource->type(),
      makeRow({nestedSource}, pool),
      execCtx);
  nestedSource.reset();
  std::vector<std::string> memoryNoise(32, std::string(65536, 'x'));
  check(
      result->as<SimpleVector<StringView>>()->valueAt(0).str() == bytes &&
          result->isNullAt(1),
      "owned IPC buffer did not outlive the caller/input");
  auto strings = nestedResult->as<ArrayVector>()
                     ->elements()
                     ->as<SimpleVector<StringView>>();
  check(
      strings->valueAt(0).str() == bytes &&
          strings->valueAt(1).str() == "later",
      "nested VARCHAR wire lost its owned Arrow buffers");
  auto writable = result;
  BaseVector::ensureWritable(
      SelectivityVector(result->size()), result->type(), pool, writable);
  writable->as<FlatVector<StringView>>()->set(0, StringView("changed"));
  check(
      result->as<SimpleVector<StringView>>()->valueAt(0).str() == bytes,
      "writable result modified retained IPC storage");
}

void testTypeBridge(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto type = facebook::velox::test::BIGINT_TYPE_WITH_CUSTOM_COMPARISON();
  auto values = makeFlat<int64_t>(type, {1, 257, 258, -1, std::nullopt}, pool);
  compareGenericWithNative(values, pool, execCtx, true);
  compareGenericWithNative(
      makeRow(
          {values, makeFlat<int64_t>(BIGINT(), {1, 2, 3, 4, 5}, pool)}, pool),
      pool,
      execCtx,
      true);
  compareGenericWithNative(
      nestedArray(values, {0, 2, 4}, {2, 2, 1}, pool), pool, execCtx, true);
  auto mapOffsets = allocateOffsets(2, pool);
  auto mapSizes = allocateSizes(2, pool);
  mapOffsets->asMutable<vector_size_t>()[0] = 0;
  mapOffsets->asMutable<vector_size_t>()[1] = 2;
  mapSizes->asMutable<vector_size_t>()[0] = 2;
  mapSizes->asMutable<vector_size_t>()[1] = 3;
  auto map = std::make_shared<MapVector>(
      pool,
      MAP(BIGINT(), type),
      nullptr,
      2,
      mapOffsets,
      mapSizes,
      makeFlat<int64_t>(BIGINT(), {2, 1, 1, 2, 3}, pool),
      values);
  compareGenericWithNative(map, pool, execCtx, true);
  auto changed =
      evaluateCall("codec_increment", type, makeRow({values}, pool), execCtx);
  for (vector_size_t i = 0; i < values->size(); ++i) {
    check(
        changed->isNullAt(i) == values->isNullAt(i),
        "custom codec NULL changed");
    if (!values->isNullAt(i)) {
      check(
          changed->as<SimpleVector<int64_t>>()->valueAt(i) ==
              values->valueAt(i) + 1,
          "guest custom writer lost its logical type/value");
    }
  }
  auto wrong = evaluateCall(
      "codec_wrong_version", type, makeRow({values}, pool), execCtx, true);
  for (vector_size_t i = 0; i < wrong->size(); ++i) {
    check(
        wrong->isNullAt(i),
        "wrong codec version was not a row-local TRY error");
  }

  auto object = std::make_shared<HandleObject>(HandleObject{42});
  auto handles = makeFlat<std::shared_ptr<void>>(
      OPAQUE<HandleObject>(), {object, std::nullopt, object}, pool);
  auto result = evaluateCall(
      "opaque_identity", handles->type(), makeRow({handles}, pool), execCtx);
  check(result->isNullAt(1), "OPAQUE NULL changed");
  check(
      result->as<SimpleVector<std::shared_ptr<void>>>()->valueAt(0).get() ==
          object.get(),
      "OPAQUE identity copied the host object");
  auto nested = nestedArray(handles, {0, 1, 2}, {1, 1, 1}, pool);
  auto nestedResult = evaluateCall(
      "generic_identity", nested->type(), makeRow({nested}, pool), execCtx);
  auto nestedObjects = nestedResult->as<ArrayVector>()
                           ->elements()
                           ->as<SimpleVector<std::shared_ptr<void>>>();
  check(
      nestedObjects->valueAt(0).get() == object.get() &&
          nestedObjects->isNullAt(1),
      "nested OPAQUE handle ownership changed");
  auto wrapped = evaluateCall(
      "generic_wrap",
      ARRAY(handles->type()),
      makeRow({handles}, pool),
      execCtx);
  check(
      wrapped->as<ArrayVector>()
              ->elements()
              ->as<SimpleVector<std::shared_ptr<void>>>()
              ->valueAt(0)
              .get() == object.get(),
      "OPAQUE GenericWriter lost identity");
  bool rejected = false;
  try {
    evaluateCall(
        "opaque_forge", handles->type(), makeRow({handles}, pool), execCtx);
  } catch (const std::exception& e) {
    rejected =
        std::string(e.what()).find("Invalid or expired") != std::string::npos;
  }
  check(rejected, "forged OPAQUE handle was accepted");
  auto scope = std::make_shared<WasmOpaqueScope>();
  auto handle = scope->add(handles->type(), object);
  WasmOpaqueScope otherScope;
  rejected = false;
  try {
    scope->resolve(otherScope.token(), handle, handles->type());
  } catch (const VeloxUserError&) {
    rejected = true;
  }
  check(rejected, "foreign invocation scope accepted");
  rejected = false;
  try {
    scope->resolve(scope->token(), handle, OPAQUE<OtherHandleObject>());
  } catch (const VeloxUserError&) {
    rejected = true;
  }
  check(rejected, "foreign OPAQUE type accepted");
  // Result remains valid after the invocation scope and input vectors
  // disappear.
  scope.reset();
  handles.reset();
  nested.reset();
  wrapped.reset();
  object.reset();
  check(
      std::static_pointer_cast<HandleObject>(
          result->as<SimpleVector<std::shared_ptr<void>>>()->valueAt(0))
              ->value == 42,
      "OPAQUE result lost shared ownership");

  auto serialized = makeFlat<std::shared_ptr<void>>(
      OPAQUE<SerializedObject>(),
      {std::make_shared<SerializedObject>(SerializedObject{41}), std::nullopt},
      pool);
  auto decoded = evaluateCall(
      "codec_increment",
      serialized->type(),
      makeRow({serialized}, pool),
      execCtx);
  check(
      decoded->isNullAt(1) &&
          std::static_pointer_cast<SerializedObject>(
              decoded->as<SimpleVector<std::shared_ptr<void>>>()->valueAt(0))
                  ->value == 42,
      "OPAQUE serde codec did not execute guest logic");
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

    registerBridgeTestTypes();
    const auto manifests = loadEmbeddedManifests(WASM_MODULE_PATH);
    std::set<std::string> scalarNames;
    for (const auto& scalar : manifests.scalars)
      scalarNames.insert(scalar.name);
    check(
        registerWasmModule(WASM_MODULE_PATH) ==
            scalarNames.size() + manifests.aggregates.size(),
        "cannot register Wasm module");
    testRegistryAndDefaultNulls(pool.get(), execCtx);
    testScalarOverloads(pool.get(), execCtx);
    testGenericAndVariadic(pool.get(), execCtx);
    testRustGenericExports(pool.get(), execCtx);
    testRowGenericErrors(pool.get(), execCtx);
    testDecimalGenerics(pool.get(), execCtx);
    testScalarInitialization(pool.get());
    testNullableStrings(pool.get(), execCtx);
    testComplexTypes(pool.get(), execCtx);
    testNestedViewsAndWriters(pool.get(), execCtx);
    testGenericComparisonAndHash(pool.get(), execCtx);
    testTypeBridge(pool.get(), execCtx);
    testOwnedIpcLifetime(pool.get(), execCtx);
    testOverloadMetadata(pool.get(), execCtx);
    testStructuredStatus(pool.get(), execCtx);
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
