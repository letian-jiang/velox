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

#include "velox/expression/VectorFunction.h"

#include <folly/container/F14Map.h>
#include <limits>
#include "velox/expression/Expr.h"
#include "velox/functions/lib/RowsTranslationUtil.h"

namespace facebook::velox::functions {
namespace {

template <typename T>
class ArrayFrequencyDictionaryFunction final : public exec::VectorFunction {
 public:
  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const override {
    VectorPtr localResult;
    if (args[0]->isConstantEncoding()) {
      auto* constant = args[0]->as<ConstantVector<ComplexType>>();
      const auto index = constant->index();
      exec::LocalSingleRow singleRow(context, index);
      localResult =
          applyFlat(*singleRow, constant->valueVector(), outputType, context);
      localResult = BaseVector::wrapInConstant(rows.end(), index, localResult);
    } else {
      localResult = applyFlat(rows, args[0], outputType, context);
    }
    context.moveOrCopyResult(localResult, rows, result);
  }

 private:
  VectorPtr applyFlat(
      const SelectivityVector& rows,
      const VectorPtr& arg,
      const TypePtr& outputType,
      exec::EvalCtx& context) const {
    const auto* array = arg->as<ArrayVector>();
    auto inputElements = array->elements();
    int64_t elementCount = 0;
    rows.applyToSelected([&](auto row) { elementCount += array->sizeAt(row); });
    VELOX_CHECK_LE(elementCount, std::numeric_limits<vector_size_t>::max());
    auto* pool = context.pool();
    auto keyIndices = allocateIndices(elementCount, pool);
    auto offsets = allocateOffsets(rows.end(), pool);
    auto sizes = allocateSizes(rows.end(), pool);
    auto frequencies =
        BaseVector::create<FlatVector<int32_t>>(INTEGER(), elementCount, pool);
    auto* rawIndices = keyIndices->template asMutable<vector_size_t>();
    auto* rawOffsets = offsets->template asMutable<vector_size_t>();
    auto* rawSizes = sizes->template asMutable<vector_size_t>();
    auto* rawFrequencies = frequencies->mutableRawValues();

    struct Count {
      int32_t frequency;
      vector_size_t index;
    };
    // Keep the hot frequency counter with its hash entry. Output positions,
    // rather than iterators, remain valid when insertion rehashes the table.
    folly::F14FastMap<T, Count> counts;
    vector_size_t cursor = 0;
    auto processRows = [&](auto valueAt, auto isNullAt) {
      rows.applyToSelected([&](auto row) {
        counts.clear();
        rawOffsets[row] = cursor;
        const auto end = array->offsetAt(row) + array->sizeAt(row);
        for (auto i = array->offsetAt(row); i < end; ++i) {
          if (isNullAt(i)) {
            continue;
          }
          auto [it, inserted] =
              counts.try_emplace(valueAt(i), Count{0, cursor});
          ++it->second.frequency;
          if (inserted) {
            rawIndices[cursor++] = i;
          }
        }
        // Frequencies follow the first-occurrence positions, independent of
        // the hash table's iteration order. Only visit the distinct entries.
        for (const auto& [key, count] : counts) {
          rawFrequencies[count.index] = count.frequency;
        }
        rawSizes[row] = cursor - rawOffsets[row];
      });
    };

    if (inputElements->isFlatEncoding() && !inputElements->mayHaveNulls()) {
      const auto* values = inputElements->as<FlatVector<T>>()->rawValues();
      processRows(
          [&](auto i) { return values[i]; }, [](auto) { return false; });
    } else {
      auto elementRows = toElementRows(inputElements->size(), rows, array);
      exec::LocalDecodedVector elements(context, *inputElements, elementRows);
      processRows(
          [&](auto i) { return elements->template valueAt<T>(i); },
          [&](auto i) { return elements->isNullAt(i); });
    }

    keyIndices->setSize(cursor * sizeof(vector_size_t));
    frequencies->resize(cursor);
    auto keys = BaseVector::transpose(keyIndices, std::move(inputElements));
    return std::make_shared<MapVector>(
        pool,
        outputType,
        nullptr,
        rows.end(),
        std::move(offsets),
        std::move(sizes),
        std::move(keys),
        std::move(frequencies));
  }
};
std::vector<exec::FunctionSignaturePtr> signatures() {
  return {
      exec::FunctionSignatureBuilder()
          .returnType("map(bigint,integer)")
          .argumentType("array(bigint)")
          .build(),
      exec::FunctionSignatureBuilder()
          .returnType("map(varchar,integer)")
          .argumentType("array(varchar)")
          .build()};
}

std::shared_ptr<exec::VectorFunction> create(
    const std::string& /*name*/,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& /*config*/) {
  if (inputArgs[0].type->childAt(0)->isBigint()) {
    return std::make_shared<ArrayFrequencyDictionaryFunction<int64_t>>();
  }
  return std::make_shared<ArrayFrequencyDictionaryFunction<StringView>>();
}
} // namespace

VELOX_DECLARE_STATEFUL_VECTOR_FUNCTION(
    udf_array_frequency,
    signatures(),
    create);

} // namespace facebook::velox::functions
