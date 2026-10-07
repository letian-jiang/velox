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

#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>

#include "velox/buffer/Buffer.h"
#include "velox/common/base/CheckedArithmetic.h"
#include "velox/common/memory/Memory.h"
#include "velox/core/Expressions.h"
#include "velox/core/QueryCtx.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/RowContainer.h"
#include "velox/expression/Expr.h"
#include "velox/functions/Macros.h"
#include "velox/functions/Registerer.h"
#include "velox/functions/prestosql/ArrayFunctions.h"
#include "velox/functions/prestosql/aggregates/AverageAggregate.h"
#include "velox/functions/prestosql/aggregates/ReduceAgg.h"
#include "velox/functions/prestosql/aggregates/SumAggregate.h"
#include "velox/functions/prestosql/registration/RegistrationFunctions.h"
#include "velox/functions/wasm/ArrowIpc.h"
#include "velox/functions/wasm/Registration.h"
#include "velox/functions/wasm/TypeBridge.h"
#include "velox/type/tests/utils/CustomTypesForTesting.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

DEFINE_string(
    udf_module_path,
    WASM_MODULE_PATH,
    "Wasm module used for correctness checks and timing comparisons");
DEFINE_int32(udf_batch_rows, 1024, "Rows per WASM/native UDF benchmark batch");
DEFINE_int32(
    udf_generic_array_elements,
    32,
    "BIGINT elements per generic hash input ARRAY");
DEFINE_int32(udf_map_entries, 16, "Entries per complex-key lookup MAP");
DEFINE_int32(udf_map_key_elements, 8, "BIGINT elements per MAP key payload");
DEFINE_int32(
    udf_gather_array_elements,
    256,
    "BIGINT payload elements per gather ROW");
DEFINE_uint64(
    udf_fuel_per_call,
    100'000'000,
    "Explicit guest fuel policy for benchmark exports");
DEFINE_int32(
    udf_warmup_batches,
    100,
    "Untimed warmup batches per benchmark trial");

namespace facebook::velox::functions::test {
namespace {

template <typename T>
struct NativePrefixFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(
      out_type<Varchar>& out,
      const arg_type<Varchar>& prefix,
      const arg_type<Varchar>& value) {
    out.resize(prefix.size() + value.size());
    std::memcpy(out.data(), prefix.data(), prefix.size());
    std::memcpy(out.data() + prefix.size(), value.data(), value.size());
    return true;
  }
  bool callNullable(
      out_type<Varchar>& out,
      const arg_type<Varchar>* prefix,
      const arg_type<Varchar>* value) {
    return prefix && value && call(out, *prefix, *value);
  }
};

template <typename T>
struct NativeGenericHashFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  void call(int64_t& out, const arg_type<Generic<T1>>& value) {
    out = static_cast<int64_t>(value.hash());
  }
  void callNullable(int64_t& out, const arg_type<Generic<T1>>* value) {
    out = static_cast<int64_t>(value ? value->hash() : BaseVector::kNullHash);
  }
};

template <typename T>
struct NativeMapLookupFunction {
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

template <typename T>
struct NativeVariadicSumFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  void call(int64_t& out, const arg_type<Variadic<int64_t>>& values) {
    callNullable(out, &values);
  }
  void callNullable(int64_t& out, const arg_type<Variadic<int64_t>>* values) {
    out = 0;
    if (values) {
      for (auto value : *values) {
        if (value.has_value())
          out = checkedPlus(out, value.value());
      }
    }
  }
};

class UdfBenchmark {
 public:
  enum class InputShape {
    kFlat,
    kNulls,
    kDictionary,
    kConstant,
    kSparse,
    kLargeString
  };
  UdfBenchmark()
      : pool_(memory::memoryManager()->addLeafPool("wasm-native-benchmark")),
        queryCtx_(core::QueryCtx::create()),
        execCtx_(pool_.get(), queryCtx_.get()) {
    functions::wasm::registerWasmTypeCodec(
        {velox::test::BIGINT_TYPE_WITH_CUSTOM_COMPARISON(),
         "benchmark_i64",
         1,
         [](const BaseVector& source, vector_size_t row) {
           const auto value = source.as<SimpleVector<int64_t>>()->valueAt(row);
           return std::string(
               reinterpret_cast<const char*>(&value), sizeof(value));
         },
         [counter = codecDecodes_](
             std::string_view bytes, BaseVector& target, vector_size_t row) {
           ++*counter;
           VELOX_CHECK_EQ(bytes.size(), sizeof(int64_t));
           int64_t value;
           std::memcpy(&value, bytes.data(), sizeof(value));
           target.as<FlatVector<int64_t>>()->set(row, value);
         }});
  }

  // Diagnostic, not a native Simple Function comparison: identical guest
  // execution and output IPC; only host codec materialization policy changes.
  // Input gathering is prepared once, outside timing, in all three cases.
  void outputCodec(bool lazy, bool used, unsigned iterations) {
    folly::BenchmarkSuspender suspender;
    const auto type = velox::test::BIGINT_TYPE_WITH_CUSTOM_COMPARISON();
    auto plain = values<int64_t>(BIGINT(), 42);
    auto encoded = values<int64_t>(type, 7);
    const auto rowType = ROW({"plain", "encoded"}, {BIGINT(), type});
    auto input = std::make_shared<RowVector>(
        pool_.get(),
        rowType,
        nullptr,
        FLAGS_udf_batch_rows,
        std::vector<VectorPtr>{plain, encoded});
    SelectivityVector rows(FLAGS_udf_batch_rows);
    auto gathered =
        functions::wasm::gatherToArrowIpc(rows, {input}, pool_.get(), rowType);
    functions::wasm::WasmOptions options;
    options.fuelPerCall = FLAGS_udf_fuel_per_call;
    functions::wasm::WasmInstance instance(
        functions::wasm::WasmModule::compile(FLAGS_udf_module_path),
        "generic_identity",
        options,
        pool_.get());
    const functions::wasm::ArrowIpcDecodeOptions decodeOptions{
        .lazyCodecs = lazy};
    auto execute = [&] {
      std::vector<std::optional<std::string>> errors;
      auto result = functions::wasm::decodeOwnedArrowIpcResult(
          instance.invoke(gathered.input),
          rowType,
          FLAGS_udf_batch_rows,
          pool_.get(),
          &errors,
          gathered.input.opaqueScope(),
          decodeOptions);
      auto child = result->as<RowVector>()->childAt(used ? 1 : 0);
      // Materialize the selected field, as a consuming native reader would.
      auto loaded = BaseVector::loadedVectorShared(child);
      folly::doNotOptimizeAway(loaded->as<SimpleVector<int64_t>>()->valueAt(0));
      return loaded;
    };
    *codecDecodes_ = 0;
    auto result = execute();
    for (vector_size_t row = 0; row < FLAGS_udf_batch_rows; ++row) {
      VELOX_CHECK_EQ(
          result->as<SimpleVector<int64_t>>()->valueAt(row), used ? 7 : 42);
    }
    VELOX_CHECK_EQ(*codecDecodes_, (lazy && !used) ? 0 : FLAGS_udf_batch_rows);
    for (int32_t i = 0; i < FLAGS_udf_warmup_batches; ++i) {
      execute();
    }
    suspender.dismiss();
    for (unsigned i = 0; i < iterations; ++i) {
      folly::doNotOptimizeAway(execute());
    }
    suspender.rehire();
  }

  // Host transport diagnostic: gather and serialize the same ROW payload,
  // with a flat/dictionary/constant tag. No guest or native SFI is timed.
  void gatherRow(unsigned iterations, InputShape shape) {
    folly::BenchmarkSuspender suspender;
    const vector_size_t count = FLAGS_udf_batch_rows;
    const vector_size_t items = FLAGS_udf_gather_array_elements;
    auto payload = BaseVector::create<FlatVector<int64_t>>(
        BIGINT(), count * items, pool_.get());
    auto offsets = allocateOffsets(count, pool_.get());
    auto sizes = allocateSizes(count, pool_.get());
    auto indices = allocateIndices(count, pool_.get());
    for (vector_size_t row = 0; row < count; ++row) {
      offsets->asMutable<vector_size_t>()[row] = row * items;
      sizes->asMutable<vector_size_t>()[row] = items;
      indices->asMutable<vector_size_t>()[row] = count - row - 1;
      for (vector_size_t item = 0; item < items; ++item)
        payload->set(row * items + item, item);
    }
    auto arrays = std::make_shared<ArrayVector>(
        pool_.get(), ARRAY(BIGINT()), nullptr, count, offsets, sizes, payload);
    auto tags = values<int64_t>(BIGINT(), 7);
    if (shape == InputShape::kDictionary)
      tags = BaseVector::wrapInDictionary(nullptr, indices, count, tags);
    if (shape == InputShape::kConstant)
      tags = BaseVector::wrapInConstant(count, 0, tags);
    auto type = ROW({"payload", "tag"}, {arrays->type(), BIGINT()});
    auto input = std::make_shared<RowVector>(
        pool_.get(),
        type,
        nullptr,
        count,
        std::vector<VectorPtr>{arrays, tags});
    SelectivityVector rows(count);
    auto first =
        functions::wasm::gatherToArrowIpc(rows, {input}, pool_.get(), type);
    std::string output(first.input.size(), '\0');
    auto execute = [&] {
      auto gathered =
          functions::wasm::gatherToArrowIpc(rows, {input}, pool_.get(), type);
      gathered.input.write(
          reinterpret_cast<uint8_t*>(output.data()), output.size());
      folly::doNotOptimizeAway(output);
    };
    execute();
    auto decoded = functions::wasm::decodeOwnedArrowIpcResult(
        output, type, count, pool_.get());
    for (vector_size_t row = 0; row < count; ++row)
      VELOX_CHECK(decoded->equalValueAt(input.get(), row, row));
    for (int32_t warmup = 0; warmup < FLAGS_udf_warmup_batches; ++warmup)
      execute();
    suspender.dismiss();
    for (unsigned i = 0; i < iterations; ++i)
      execute();
    suspender.rehire();
  }

  void scalar(
      const std::string& name,
      bool strings,
      unsigned iterations,
      InputShape shape = InputShape::kFlat) {
    folly::BenchmarkSuspender suspender;
    const bool variadic = name.find("variadic") != std::string::npos;
    const std::string text =
        shape == InputShape::kLargeString ? std::string(2048, 'x') : "value";
    std::vector<VectorPtr> arguments;
    if (strings) {
      arguments = {
          values<StringView>(VARCHAR(), StringView("prefix-")),
          values<StringView>(VARCHAR(), StringView(text))};
    } else {
      arguments = {
          values<int64_t>(BIGINT(), 20), values<int64_t>(BIGINT(), 22)};
    }
    if (variadic) {
      arguments.clear();
      for (int64_t value = 1; value <= 8; ++value)
        arguments.push_back(values<int64_t>(BIGINT(), value));
    }
    if (shape == InputShape::kNulls) {
      for (vector_size_t row = 0; row < FLAGS_udf_batch_rows; row += 4) {
        arguments.back()->setNull(row, true);
      }
    } else if (shape == InputShape::kDictionary) {
      auto indices = allocateIndices(FLAGS_udf_batch_rows, pool_.get());
      for (vector_size_t row = 0; row < FLAGS_udf_batch_rows; ++row)
        indices->asMutable<vector_size_t>()[row] =
            FLAGS_udf_batch_rows - row - 1;
      for (auto& arg : arguments)
        arg = BaseVector::wrapInDictionary(
            nullptr, indices, FLAGS_udf_batch_rows, arg);
    } else if (shape == InputShape::kConstant) {
      for (auto& arg : arguments)
        arg = BaseVector::wrapInConstant(FLAGS_udf_batch_rows, 0, arg);
    }
    const auto type = arguments.front()->type();
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    std::vector<core::TypedExprPtr> fields;
    for (size_t i = 0; i < arguments.size(); ++i) {
      names.push_back("c" + std::to_string(i));
      types.push_back(type);
      fields.push_back(
          std::make_shared<core::FieldAccessTypedExpr>(type, names.back()));
    }
    auto input = std::make_shared<RowVector>(
        pool_.get(),
        ROW(std::move(names), std::move(types)),
        nullptr,
        FLAGS_udf_batch_rows,
        std::move(arguments));
    auto call =
        std::make_shared<core::CallTypedExpr>(type, std::move(fields), name);
    exec::ExprSet expression({call}, &execCtx_);
    exec::EvalCtx context(&execCtx_, &expression, input.get());
    SelectivityVector rows(input->size());
    if (shape == InputShape::kSparse) {
      rows.clearAll();
      for (vector_size_t row = 0; row < input->size(); row += 8)
        rows.setValid(row, true);
      rows.updateBounds();
    }
    std::vector<VectorPtr> result(1);
    // Warm up expression evaluation and validate the batch before measuring
    // steady state.
    expression.eval(rows, context, result);
    const auto joined = "prefix-" + text;
    auto expected = strings ? values<StringView>(VARCHAR(), StringView(joined))
                            : values<int64_t>(BIGINT(), variadic ? 36 : 42);
    if (shape == InputShape::kNulls) {
      for (vector_size_t row = 0; row < FLAGS_udf_batch_rows; row += 4)
        expected->setNull(row, true);
    }
    rows.applyToSelected([&](vector_size_t row) {
      VELOX_CHECK(result[0]->equalValueAt(expected.get(), row, row));
    });
    for (int32_t i = 0; i < FLAGS_udf_warmup_batches; ++i) {
      expression.eval(rows, context, result);
    }
    suspender.dismiss();
    for (unsigned i = 0; i < iterations; ++i) {
      expression.eval(rows, context, result);
      folly::doNotOptimizeAway(result);
    }
    suspender.rehire();
  }

  void arraySum(
      const std::string& name,
      unsigned iterations,
      bool genericHash = false) {
    folly::BenchmarkSuspender suspender;
    const vector_size_t items =
        genericHash ? FLAGS_udf_generic_array_elements : 16;
    auto elements = BaseVector::create<FlatVector<int64_t>>(
        BIGINT(), FLAGS_udf_batch_rows * items, pool_.get());
    auto offsets = allocateOffsets(FLAGS_udf_batch_rows, pool_.get());
    auto sizes = allocateSizes(FLAGS_udf_batch_rows, pool_.get());
    for (vector_size_t row = 0; row < FLAGS_udf_batch_rows; ++row) {
      offsets->asMutable<vector_size_t>()[row] = row * items;
      sizes->asMutable<vector_size_t>()[row] = items;
      for (vector_size_t item = 0; item < items; ++item) {
        elements->set(row * items + item, item + 1);
      }
    }
    auto arrays = std::make_shared<ArrayVector>(
        pool_.get(),
        ARRAY(BIGINT()),
        nullptr,
        FLAGS_udf_batch_rows,
        offsets,
        sizes,
        elements);
    auto input = std::make_shared<RowVector>(
        pool_.get(),
        ROW({"values"}, {arrays->type()}),
        nullptr,
        FLAGS_udf_batch_rows,
        std::vector<VectorPtr>{arrays});
    auto call = std::make_shared<core::CallTypedExpr>(
        BIGINT(),
        std::vector<core::TypedExprPtr>{
            std::make_shared<core::FieldAccessTypedExpr>(
                arrays->type(), "values")},
        name);
    exec::ExprSet expression({call}, &execCtx_);
    exec::EvalCtx context(&execCtx_, &expression, input.get());
    SelectivityVector rows(input->size());
    std::vector<VectorPtr> result(1);
    expression.eval(rows, context, result);
    auto values = result[0]->as<SimpleVector<int64_t>>();
    for (vector_size_t row = 0; row < rows.size(); ++row) {
      const auto expected =
          genericHash ? static_cast<int64_t>(arrays->hashValueAt(row)) : 136;
      VELOX_CHECK(!values->isNullAt(row) && values->valueAt(row) == expected);
    }
    for (int32_t i = 0; i < FLAGS_udf_warmup_batches; ++i) {
      expression.eval(rows, context, result);
    }
    suspender.dismiss();
    for (unsigned i = 0; i < iterations; ++i) {
      expression.eval(rows, context, result);
      folly::doNotOptimizeAway(result);
    }
    suspender.rehire();
  }

  // Search for the last key. Earlier keys differ in the first ROW field;
  // native/borrowed readers stop there without reading their ARRAY payloads.
  void mapLookup(const std::string& name, unsigned iterations) {
    folly::BenchmarkSuspender suspender;
    const vector_size_t entries = FLAGS_udf_map_entries;
    const vector_size_t items = FLAGS_udf_map_key_elements;
    const vector_size_t keys = FLAGS_udf_batch_rows * entries;
    auto ids =
        BaseVector::create<FlatVector<int64_t>>(BIGINT(), keys, pool_.get());
    auto payloadValues = BaseVector::create<FlatVector<int64_t>>(
        BIGINT(), keys * items, pool_.get());
    auto payloadOffsets = allocateOffsets(keys, pool_.get());
    auto payloadSizes = allocateSizes(keys, pool_.get());
    auto mapValues =
        BaseVector::create<FlatVector<int64_t>>(BIGINT(), keys, pool_.get());
    for (vector_size_t key = 0; key < keys; ++key) {
      ids->set(key, key % entries);
      mapValues->set(key, key % entries);
      payloadOffsets->asMutable<vector_size_t>()[key] = key * items;
      payloadSizes->asMutable<vector_size_t>()[key] = items;
      for (vector_size_t item = 0; item < items; ++item)
        payloadValues->set(key * items + item, item + 1);
    }
    auto payloads = std::make_shared<ArrayVector>(
        pool_.get(),
        ARRAY(BIGINT()),
        nullptr,
        keys,
        payloadOffsets,
        payloadSizes,
        payloadValues);
    auto keyType = ROW({"id", "payload"}, {BIGINT(), payloads->type()});
    auto keyRows = std::make_shared<RowVector>(
        pool_.get(),
        keyType,
        nullptr,
        keys,
        std::vector<VectorPtr>{ids, payloads});
    auto offsets = allocateOffsets(FLAGS_udf_batch_rows, pool_.get());
    auto sizes = allocateSizes(FLAGS_udf_batch_rows, pool_.get());
    auto indices = allocateIndices(FLAGS_udf_batch_rows, pool_.get());
    for (vector_size_t row = 0; row < FLAGS_udf_batch_rows; ++row) {
      offsets->asMutable<vector_size_t>()[row] = row * entries;
      sizes->asMutable<vector_size_t>()[row] = entries;
      indices->asMutable<vector_size_t>()[row] = (row + 1) * entries - 1;
    }
    auto maps = std::make_shared<MapVector>(
        pool_.get(),
        MAP(keyType, BIGINT()),
        nullptr,
        FLAGS_udf_batch_rows,
        offsets,
        sizes,
        keyRows,
        mapValues);
    auto lookup = BaseVector::wrapInDictionary(
        nullptr, indices, FLAGS_udf_batch_rows, keyRows);
    auto input = std::make_shared<RowVector>(
        pool_.get(),
        ROW({"map", "key"}, {maps->type(), keyType}),
        nullptr,
        FLAGS_udf_batch_rows,
        std::vector<VectorPtr>{maps, lookup});
    auto call = std::make_shared<core::CallTypedExpr>(
        BIGINT(),
        std::vector<core::TypedExprPtr>{
            std::make_shared<core::FieldAccessTypedExpr>(maps->type(), "map"),
            std::make_shared<core::FieldAccessTypedExpr>(keyType, "key")},
        name);
    exec::ExprSet expression({call}, &execCtx_);
    exec::EvalCtx context(&execCtx_, &expression, input.get());
    SelectivityVector rows(FLAGS_udf_batch_rows);
    std::vector<VectorPtr> result(1);
    expression.eval(rows, context, result);
    for (vector_size_t row = 0; row < rows.size(); ++row) {
      auto values = result[0]->as<SimpleVector<int64_t>>();
      VELOX_CHECK(!values->isNullAt(row));
      VELOX_CHECK_EQ(values->valueAt(row), entries - 1);
    }
    for (int32_t i = 0; i < FLAGS_udf_warmup_batches; ++i)
      expression.eval(rows, context, result);
    suspender.dismiss();
    for (unsigned i = 0; i < iterations; ++i) {
      expression.eval(rows, context, result);
      folly::doNotOptimizeAway(result);
    }
    suspender.rehire();
  }

  void aggregate(
      const std::string& name,
      bool average,
      unsigned iterations,
      bool rowUdf = false,
      bool reduce = false) {
    folly::BenchmarkSuspender suspender;
    const TypePtr type = average ? TypePtr{DOUBLE()} : TypePtr{BIGINT()};
    auto input =
        average ? values<double>(type, 42.0) : values<int64_t>(type, 1);
    HashStringAllocator allocator(pool_.get());
    core::QueryConfig config({});
    auto signature = ROW({"s", "x"}, {type, type});
    auto lambda = std::make_shared<core::LambdaTypedExpr>(
        signature,
        std::make_shared<core::CallTypedExpr>(
            type,
            std::vector<core::TypedExprPtr>{
                std::make_shared<core::FieldAccessTypedExpr>(type, "s"),
                std::make_shared<core::FieldAccessTypedExpr>(type, "x")},
            "native_plus"));
    const std::vector<TypePtr> argumentTypes = reduce
        ? std::vector<TypePtr>{type, type, lambda->type(), lambda->type()}
        : rowUdf ? std::vector<TypePtr>{BIGINT(), type}
                 : std::vector<TypePtr>{type};
    auto aggregate = exec::Aggregate::create(
        name,
        core::AggregationNode::Step::kSingle,
        argumentTypes,
        type,
        config);
    aggregate->setAllocator(&allocator);
    auto factor = (rowUdf || reduce) ? values<int64_t>(BIGINT(), reduce ? 0 : 1)
                                     : nullptr;
    if (rowUdf)
      aggregate->setConstantInputs({factor, nullptr});
    if (reduce) {
      aggregate->setConstantInputs({nullptr, factor});
      aggregate->setLambdaExpressions(
          {lambda, lambda},
          std::make_shared<exec::SimpleExpressionEvaluator>(
              queryCtx_.get(), pool_.get()));
    }
    // Keep the flags separate from an aligned native accumulator.
    constexpr int32_t kOffset = alignof(std::max_align_t);
    VELOX_CHECK_LE(aggregate->accumulatorAlignmentSize(), kOffset);
    aggregate->setOffsets(
        kOffset,
        exec::RowContainer::nullByte(0),
        exec::RowContainer::nullMask(0),
        exec::RowContainer::initializedByte(0),
        exec::RowContainer::initializedMask(0),
        4);
    const auto storageBytes = kOffset + aggregate->accumulatorFixedWidthSize();
    auto storage = AlignedBuffer::allocate<char>(storageBytes, pool_.get(), 0);
    char* group = storage->asMutable<char>();
    const vector_size_t index = 0;
    aggregate->initializeNewGroups(
        &group, folly::Range<const vector_size_t*>(&index, 1));
    SelectivityVector rows(input->size());
    const std::vector<VectorPtr> arguments = reduce
        ? std::vector<VectorPtr>{input, factor}
        : rowUdf ? std::vector<VectorPtr>{factor, input}
                 : std::vector<VectorPtr>{input};
    VectorPtr result = BaseVector::create(type, 1, pool_.get());
    aggregate->addSingleGroupRawInput(group, rows, arguments, false);
    aggregate->extractValues(&group, 1, &result);
    VELOX_CHECK_EQ(result->size(), 1);
    VELOX_CHECK(!result->isNullAt(0));
    if (average) {
      VELOX_CHECK_EQ(result->as<SimpleVector<double>>()->valueAt(0), 42.0);
    } else {
      VELOX_CHECK_EQ(
          result->as<SimpleVector<int64_t>>()->valueAt(0),
          FLAGS_udf_batch_rows);
    }
    for (int32_t i = 0; i < FLAGS_udf_warmup_batches; ++i) {
      aggregate->addSingleGroupRawInput(group, rows, arguments, false);
      aggregate->extractValues(&group, 1, &result);
    }
    suspender.dismiss();
    for (unsigned i = 0; i < iterations; ++i) {
      aggregate->addSingleGroupRawInput(group, rows, arguments, false);
      aggregate->extractValues(&group, 1, &result);
      folly::doNotOptimizeAway(result);
    }
    suspender.rehire();
    VELOX_CHECK_EQ(result->size(), 1);
    VELOX_CHECK(!result->isNullAt(0));
    if (average) {
      VELOX_CHECK_EQ(result->as<SimpleVector<double>>()->valueAt(0), 42.0);
    } else {
      VELOX_CHECK_EQ(
          result->as<SimpleVector<int64_t>>()->valueAt(0),
          (static_cast<int64_t>(iterations) + FLAGS_udf_warmup_batches + 1) *
              FLAGS_udf_batch_rows);
    }
    aggregate->destroy(folly::Range<char**>(&group, 1));
  }

 private:
  template <typename T>
  VectorPtr values(const TypePtr& type, T value) {
    auto vector = BaseVector::create<FlatVector<T>>(
        type, FLAGS_udf_batch_rows, pool_.get());
    for (vector_size_t row = 0; row < vector->size(); ++row) {
      vector->set(row, value);
    }
    return vector;
  }

  std::shared_ptr<memory::MemoryPool> pool_;
  std::shared_ptr<core::QueryCtx> queryCtx_;
  core::ExecCtx execCtx_;
  std::shared_ptr<size_t> codecDecodes_{std::make_shared<size_t>(0)};
};

std::unique_ptr<UdfBenchmark> benchmark;

BENCHMARK(wasmOutputCodecEagerUnused, iterations) {
  benchmark->outputCodec(false, false, iterations);
}
BENCHMARK(wasmOutputCodecLazyUnused, iterations) {
  benchmark->outputCodec(true, false, iterations);
}
BENCHMARK(wasmOutputCodecLazyUsed, iterations) {
  benchmark->outputCodec(true, true, iterations);
}
BENCHMARK_DRAW_LINE();

BENCHMARK(arrowGatherPlainRow, iterations) {
  benchmark->gatherRow(iterations, UdfBenchmark::InputShape::kFlat);
}
BENCHMARK(arrowGatherDictionaryLeafRow, iterations) {
  benchmark->gatherRow(iterations, UdfBenchmark::InputShape::kDictionary);
}
BENCHMARK(arrowGatherConstantLeafRow, iterations) {
  benchmark->gatherRow(iterations, UdfBenchmark::InputShape::kConstant);
}
BENCHMARK_DRAW_LINE();

BENCHMARK(nativeAdd, iterations) {
  benchmark->scalar("native_plus", false, iterations);
}
BENCHMARK_RELATIVE(wasmAdd, iterations) {
  benchmark->scalar("add_i64", false, iterations);
}
BENCHMARK_DRAW_LINE();
BENCHMARK(nativePrefix, iterations) {
  benchmark->scalar("native_simple_prefix", true, iterations);
}
BENCHMARK_RELATIVE(wasmPrefix, iterations) {
  benchmark->scalar("prefix", true, iterations);
}
#define SCALAR_SHAPE_BENCHMARK(label, nativeName, wasmName, strings, shape) \
  BENCHMARK_DRAW_LINE();                                                    \
  BENCHMARK(native##label, iterations) {                                    \
    benchmark->scalar(                                                      \
        nativeName, strings, iterations, UdfBenchmark::InputShape::shape);  \
  }                                                                         \
  BENCHMARK_RELATIVE(wasm##label, iterations) {                             \
    benchmark->scalar(                                                      \
        wasmName, strings, iterations, UdfBenchmark::InputShape::shape);    \
  }

SCALAR_SHAPE_BENCHMARK(AddNulls, "native_plus", "add_i64", false, kNulls)
SCALAR_SHAPE_BENCHMARK(
    AddDictionary,
    "native_plus",
    "add_i64",
    false,
    kDictionary)
SCALAR_SHAPE_BENCHMARK(AddConstant, "native_plus", "add_i64", false, kConstant)
SCALAR_SHAPE_BENCHMARK(AddSparse, "native_plus", "add_i64", false, kSparse)
SCALAR_SHAPE_BENCHMARK(
    PrefixNulls,
    "native_simple_prefix",
    "prefix",
    true,
    kNulls)
SCALAR_SHAPE_BENCHMARK(
    PrefixLarge,
    "native_simple_prefix",
    "prefix",
    true,
    kLargeString)
SCALAR_SHAPE_BENCHMARK(
    VariadicSum,
    "native_variadic_sum",
    "variadic_sum",
    false,
    kFlat)
#undef SCALAR_SHAPE_BENCHMARK

BENCHMARK_DRAW_LINE();
BENCHMARK(nativeGenericArrayHash, iterations) {
  benchmark->arraySum("native_generic_hash", iterations, true);
}
BENCHMARK_RELATIVE(wasmGenericArrayHash, iterations) {
  benchmark->arraySum("generic_hash", iterations, true);
}
BENCHMARK_DRAW_LINE();
BENCHMARK(nativeMapLookup, iterations) {
  benchmark->mapLookup("native_map_lookup", iterations);
}
BENCHMARK_RELATIVE(wasmMapLookup, iterations) {
  benchmark->mapLookup("map_lookup", iterations);
}
BENCHMARK_DRAW_LINE();
BENCHMARK(nativeVectorConcat, iterations) {
  benchmark->scalar("native_concat", true, iterations);
}
BENCHMARK_DRAW_LINE();
BENCHMARK(nativeArraySum, iterations) {
  benchmark->arraySum("native_simple_array_sum", iterations);
}
BENCHMARK_RELATIVE(wasmArraySum, iterations) {
  benchmark->arraySum("array_sum_dispatch", iterations);
}
BENCHMARK_DRAW_LINE();
BENCHMARK(nativeAverage, iterations) {
  benchmark->aggregate("native_avg", true, iterations);
}
BENCHMARK_RELATIVE(wasmAverage, iterations) {
  benchmark->aggregate("avg_f64", true, iterations);
}
BENCHMARK_DRAW_LINE();
BENCHMARK(nativeSum, iterations) {
  benchmark->aggregate("native_sum", false, iterations);
}
BENCHMARK_RELATIVE(wasmSum, iterations) {
  benchmark->aggregate("sum_i64", false, iterations);
}
BENCHMARK_RELATIVE(wasmRowSum, iterations) {
  benchmark->aggregate("variadic_sum_wasm", false, iterations, true);
}
BENCHMARK_DRAW_LINE();
BENCHMARK(nativeReduce, iterations) {
  benchmark->aggregate("native_reduce", false, iterations, false, true);
}
BENCHMARK_RELATIVE(wasmReduce, iterations) {
  benchmark->aggregate("reduce_wasm", false, iterations, false, true);
}

} // namespace
} // namespace facebook::velox::functions::test

int main(int argc, char** argv) {
  using namespace facebook::velox;
  using namespace facebook::velox::functions::test;
  folly::Init init{&argc, &argv};
  try {
    VELOX_USER_CHECK_GT(
        FLAGS_udf_batch_rows, 0, "Benchmark batch size must be positive");
    VELOX_USER_CHECK_GT(
        FLAGS_udf_generic_array_elements,
        0,
        "Generic ARRAY size must be positive");
    VELOX_USER_CHECK_LE(
        static_cast<int64_t>(FLAGS_udf_batch_rows) *
            std::max(FLAGS_udf_generic_array_elements, 16),
        std::numeric_limits<vector_size_t>::max(),
        "Benchmark ARRAY element count exceeds Velox vector index range");
    VELOX_USER_CHECK_GT(FLAGS_udf_map_entries, 0);
    VELOX_USER_CHECK_GT(FLAGS_udf_gather_array_elements, 0);
    VELOX_USER_CHECK_LE(
        static_cast<int64_t>(FLAGS_udf_batch_rows) *
            FLAGS_udf_gather_array_elements,
        std::numeric_limits<vector_size_t>::max(),
        "Benchmark gather payload exceeds Velox vector index range");
    VELOX_USER_CHECK_GT(FLAGS_udf_map_key_elements, 0);
    VELOX_USER_CHECK_LE(
        static_cast<int64_t>(FLAGS_udf_batch_rows) * FLAGS_udf_map_entries *
            FLAGS_udf_map_key_elements,
        std::numeric_limits<vector_size_t>::max(),
        "Benchmark MAP key element count exceeds Velox index range");
    VELOX_USER_CHECK_GE(
        FLAGS_udf_warmup_batches,
        0,
        "Benchmark warmup count must be nonnegative");
    functions::prestosql::registerCheckedArithmeticFunctions("native_");
    functions::prestosql::registerStringFunctions("native_");
    facebook::velox::
        registerFunction<NativePrefixFunction, Varchar, Varchar, Varchar>(
            {"native_simple_prefix"});
    facebook::velox::
        registerFunction<functions::ArraySumFunction, int64_t, Array<int64_t>>(
            {"native_simple_array_sum"});
    facebook::velox::
        registerFunction<NativeGenericHashFunction, int64_t, Generic<T1>>(
            {"native_generic_hash"});
    facebook::velox::
        registerFunction<NativeVariadicSumFunction, int64_t, Variadic<int64_t>>(
            {"native_variadic_sum"});
    facebook::velox::registerFunction<
        NativeMapLookupFunction,
        int64_t,
        Map<Generic<T1>, int64_t>,
        Generic<T1>>({"native_map_lookup"});
    aggregate::prestosql::registerAverageAggregate({"native_avg"}, false, true);
    aggregate::prestosql::registerSumAggregate({"native_sum"}, false, true);
    aggregate::prestosql::registerReduceAgg({"native_reduce"}, false, true);
    functions::wasm::WasmOptions options;
    options.fuelPerCall = FLAGS_udf_fuel_per_call;
    functions::wasm::registerWasmModule(FLAGS_udf_module_path, false, options);
    memory::MemoryManager::Options memoryOptions;
    memoryOptions.trackDefaultUsage = true;
    memory::MemoryManager::initialize(memoryOptions);
    benchmark = std::make_unique<UdfBenchmark>();
    folly::runBenchmarks();
    benchmark.reset();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "WASM/native benchmark failed: " << error.what() << '\n';
    benchmark.reset();
    return 1;
  }
}
