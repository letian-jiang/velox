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

#include "velox/functions/wasm/ArrowIpc.h"

#include <array>
#include <limits>

#include <arrow/array/builder_primitive.h>
#include <arrow/buffer.h>
#include <arrow/c/bridge.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include "velox/common/base/Exceptions.h"
#include "velox/vector/arrow/Bridge.h"

namespace facebook::velox::functions::wasm {
namespace {

template <typename T>
T unwrapArrow(arrow::Result<T> result, std::string_view operation) {
  VELOX_USER_CHECK(
      result.ok(),
      "Arrow IPC {} failed: {}",
      operation,
      result.status().ToString());
  return std::move(result).ValueUnsafe();
}

void checkArrow(arrow::Status status, std::string_view operation) {
  VELOX_USER_CHECK(
      status.ok(), "Arrow IPC {} failed: {}", operation, status.ToString());
}

std::shared_ptr<arrow::RecordBatch> exportRecordBatch(
    const RowVectorPtr& rowVector,
    memory::MemoryPool* pool) {
  ArrowOptions options;
  options.flattenDictionary = true;
  options.flattenConstant = true;
  ArrowSchema schema;
  ArrowArray array;
  exportToArrow(rowVector, schema, options);
  exportToArrow(rowVector, array, pool, options);
  auto arrowSchema = unwrapArrow(arrow::ImportSchema(&schema), "schema import");
  return unwrapArrow(
      arrow::ImportRecordBatch(&array, arrowSchema), "record batch import");
}

std::shared_ptr<arrow::RecordBatch> gatherToArrowRecordBatch(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool) {
  const auto rowCount = rows.countSelected();
  BufferPtr indices;
  if (!arguments.empty()) {
    indices = allocateIndices(rowCount, pool);
    auto* rawIndices = indices->asMutable<vector_size_t>();
    vector_size_t compactRow = 0;
    rows.applyToSelected(
        [&](vector_size_t row) { rawIndices[compactRow++] = row; });
  }

  std::vector<TypePtr> argumentTypes;
  std::vector<VectorPtr> compactArguments;
  argumentTypes.reserve(arguments.size());
  compactArguments.reserve(arguments.size());
  for (const auto& argument : arguments) {
    VELOX_USER_CHECK_GE(
        argument->size(), rows.end(), "Wasm UDF argument is too short");
    argumentTypes.push_back(argument->type());
    if (argument->typeKind() == TypeKind::ARRAY ||
        argument->typeKind() == TypeKind::MAP ||
        argument->typeKind() == TypeKind::ROW) {
      // The Arrow bridge cannot flatten dictionary encodings around nested
      // vectors. Copy only selected rows into a plain complex vector.
      auto compact = BaseVector::create(argument->type(), rowCount, pool);
      SelectivityVector compactRows(rowCount);
      compact->copy(argument.get(), compactRows, indices->as<vector_size_t>());
      compactArguments.push_back(std::move(compact));
    } else {
      compactArguments.push_back(
          BaseVector::wrapInDictionary(nullptr, indices, rowCount, argument));
    }
  }
  auto compact = std::make_shared<RowVector>(
      pool,
      ROW(std::move(argumentTypes)),
      nullptr,
      rowCount,
      std::move(compactArguments));
  return exportRecordBatch(compact, pool);
}

void ownVariableWidthValues(VectorPtr& vector, memory::MemoryPool* pool) {
  switch (vector->typeKind()) {
    case TypeKind::VARCHAR:
    case TypeKind::VARBINARY: {
      auto* source = vector->as<SimpleVector<StringView>>();
      VELOX_USER_CHECK_NOT_NULL(source, "Wasm UDF string result is not scalar");
      auto owned = BaseVector::create<FlatVector<StringView>>(
          vector->type(), vector->size(), pool);
      for (vector_size_t row = 0; row < vector->size(); ++row) {
        if (source->isNullAt(row)) {
          owned->setNull(row, true);
        } else {
          owned->set(row, source->valueAt(row));
        }
      }
      vector = std::move(owned);
      return;
    }
    case TypeKind::ARRAY:
      ownVariableWidthValues(vector->as<ArrayVector>()->elements(), pool);
      return;
    case TypeKind::MAP: {
      auto* map = vector->as<MapVector>();
      ownVariableWidthValues(map->mapKeys(), pool);
      ownVariableWidthValues(map->mapValues(), pool);
      return;
    }
    case TypeKind::ROW:
      for (auto& child : vector->as<RowVector>()->children()) {
        ownVariableWidthValues(child, pool);
      }
      return;
    default:
      return;
  }
}

} // namespace

struct ArrowIpcInput::Impl {
  explicit Impl(const std::shared_ptr<arrow::RecordBatch>& batch)
      : options(arrow::ipc::IpcWriteOptions::Defaults()) {
    arrow::ipc::DictionaryFieldMapper mapper(*batch->schema());
    VELOX_USER_CHECK_EQ(
        mapper.num_dicts(),
        0,
        "Wasm Arrow IPC input must not contain dictionary-encoded columns");
    checkArrow(
        arrow::ipc::GetSchemaPayload(
            *batch->schema(), options, mapper, &schemaPayload),
        "schema payload creation");
    checkArrow(
        arrow::ipc::GetRecordBatchPayload(*batch, options, &batchPayload),
        "record batch payload creation");
    const auto totalSize = arrow::ipc::GetPayloadSize(schemaPayload, options) +
        arrow::ipc::GetPayloadSize(batchPayload, options) + kEndOfStreamSize;
    VELOX_USER_CHECK_LE(
        totalSize,
        std::numeric_limits<uint32_t>::max(),
        "Arrow IPC input exceeds the Wasm ABI size limit");
    size = static_cast<uint32_t>(totalSize);
  }

  static constexpr uint32_t kEndOfStreamSize = 8;
  arrow::ipc::IpcWriteOptions options;
  arrow::ipc::IpcPayload schemaPayload;
  arrow::ipc::IpcPayload batchPayload;
  uint32_t size;
};

ArrowIpcInput::ArrowIpcInput(std::shared_ptr<arrow::RecordBatch> batch)
    : impl_(std::make_shared<Impl>(std::move(batch))) {}

ArrowIpcInput::~ArrowIpcInput() = default;

uint32_t ArrowIpcInput::size() const {
  return impl_->size;
}

void ArrowIpcInput::write(uint8_t* destination, uint32_t capacity) const {
  VELOX_USER_CHECK_EQ(
      capacity, impl_->size, "Arrow IPC destination has the wrong size");
  auto buffer = std::make_shared<arrow::MutableBuffer>(destination, capacity);
  arrow::io::FixedSizeBufferWriter writer(buffer);
  int32_t metadataLength;
  checkArrow(
      arrow::ipc::WriteIpcPayload(
          impl_->schemaPayload, impl_->options, &writer, &metadataLength),
      "schema payload write");
  checkArrow(
      arrow::ipc::WriteIpcPayload(
          impl_->batchPayload, impl_->options, &writer, &metadataLength),
      "record batch payload write");
  static constexpr std::array<uint8_t, Impl::kEndOfStreamSize> kEndOfStream{
      0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
  checkArrow(
      writer.Write(kEndOfStream.data(), kEndOfStream.size()),
      "end-of-stream write");
  VELOX_USER_CHECK_EQ(
      unwrapArrow(writer.Tell(), "output size check"),
      capacity,
      "Arrow IPC writer produced an unexpected size");
}

GatheredArrowBatch gatherToArrowIpc(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool) {
  auto batch = gatherToArrowRecordBatch(rows, arguments, pool);
  return {
      .input = ArrowIpcInput(std::move(batch)),
      .rowCount = rows.countSelected(),
  };
}

GatheredArrowBatch gatherToArrowIpcWithStateHandles(
    const SelectivityVector& rows,
    const std::vector<uint32_t>& stateHandles,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool) {
  VELOX_USER_CHECK_GE(
      stateHandles.size(), rows.end(), "Missing Wasm UDAF state handles");
  auto batch = gatherToArrowRecordBatch(rows, arguments, pool);

  arrow::UInt32Builder builder;
  checkArrow(builder.Reserve(rows.countSelected()), "state handle reserve");
  rows.applyToSelected([&](vector_size_t row) {
    VELOX_USER_CHECK_NE(
        stateHandles[row], 0, "Wasm UDAF state handle must be non-zero");
    checkArrow(builder.Append(stateHandles[row]), "state handle append");
  });
  auto handles = unwrapArrow(builder.Finish(), "state handle finish");
  auto field =
      arrow::field("__velox_wasm_state_handle", arrow::uint32(), false);
  batch = unwrapArrow(batch->AddColumn(0, field, handles), "state column add");
  return {
      .input = ArrowIpcInput(std::move(batch)),
      .rowCount = rows.countSelected(),
  };
}

VectorPtr decodeArrowIpcResult(
    std::string_view ipc,
    const TypePtr& outputType,
    vector_size_t expectedRows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors) {
  auto buffer = std::make_shared<arrow::Buffer>(
      reinterpret_cast<const uint8_t*>(ipc.data()), ipc.size());
  auto input = std::make_shared<arrow::io::BufferReader>(buffer);
  auto reader = unwrapArrow(
      arrow::ipc::RecordBatchStreamReader::Open(input),
      "stream reader creation");
  auto batch = unwrapArrow(reader->Next(), "record batch read");
  VELOX_USER_CHECK_NOT_NULL(
      batch, "Wasm UDF returned an empty Arrow IPC stream");
  VELOX_USER_CHECK_EQ(
      batch->num_rows(), expectedRows, "Wasm UDF returned the wrong row count");
  if (errors == nullptr) {
    VELOX_USER_CHECK_EQ(
        batch->num_columns(),
        1,
        "Wasm UDF result must contain exactly one column");
  } else {
    VELOX_USER_CHECK(
        batch->num_columns() == 1 || batch->num_columns() == 2,
        "Wasm scalar UDF result must contain one value column and an optional "
        "error column");
    if (batch->num_columns() == 2) {
      const auto& errorField = batch->schema()->field(1);
      VELOX_USER_CHECK_EQ(
          errorField->name(),
          "__velox_wasm_error",
          "Wasm scalar UDF error column has the wrong name");
      VELOX_USER_CHECK(
          errorField->type()->Equals(arrow::utf8()),
          "Wasm scalar UDF error column must be Arrow Utf8");
      VELOX_USER_CHECK(
          errorField->nullable(),
          "Wasm scalar UDF error column must be nullable");
    }
  }
  auto extra = unwrapArrow(reader->Next(), "trailing record batch read");
  VELOX_USER_CHECK_NULL(
      extra, "Wasm UDF result must contain exactly one record batch");

  ArrowSchema schema;
  ArrowArray array;
  checkArrow(arrow::ExportSchema(*batch->schema(), &schema), "schema export");
  checkArrow(arrow::ExportRecordBatch(*batch, &array), "record batch export");
  auto row = std::dynamic_pointer_cast<RowVector>(
      importFromArrowAsOwner(schema, array, pool));
  VELOX_USER_CHECK_NOT_NULL(
      row, "Wasm UDF result is not an Arrow struct batch");
  auto result = row->childAt(0);
  VELOX_USER_CHECK(
      result->type()->equivalent(*outputType),
      "Wasm UDF returned type '{}', expected '{}'",
      result->type()->toString(),
      outputType->toString());
  if (errors != nullptr) {
    errors->clear();
    if (row->childrenSize() == 2) {
      errors->assign(expectedRows, std::nullopt);
      auto errorVector = row->childAt(1)->as<SimpleVector<StringView>>();
      VELOX_USER_CHECK_NOT_NULL(
          errorVector, "Wasm scalar UDF error result is not varchar");
      for (vector_size_t index = 0; index < expectedRows; ++index) {
        if (!errorVector->isNullAt(index)) {
          VELOX_USER_CHECK(
              result->isNullAt(index),
              "Wasm scalar UDF returned both a value and an error at compact "
              "row {}",
              index);
          (*errors)[index] = errorVector->valueAt(index).str();
        }
      }
    }
  }
  // Generic copies can retain StringViews into the Arrow IPC buffer. Deep-copy
  // variable-width leaves at every nesting level before that buffer is freed.
  auto owned = BaseVector::copy(*result, pool);
  ownVariableWidthValues(owned, pool);
  return owned;
}

void scatterArrowResult(
    const SelectivityVector& rows,
    const VectorPtr& compactResult,
    const TypePtr& outputType,
    memory::MemoryPool* pool,
    VectorPtr& result) {
  VELOX_CHECK_EQ(rows.countSelected(), compactResult->size());
  BaseVector::ensureWritable(rows, outputType, pool, result);
  auto sourceRows = allocateIndices(rows.end(), pool);
  auto* rawSourceRows = sourceRows->asMutable<vector_size_t>();
  vector_size_t compactRow = 0;
  rows.applyToSelected(
      [&](vector_size_t row) { rawSourceRows[row] = compactRow++; });
  result->copy(compactResult.get(), rows, rawSourceRows);
}

} // namespace facebook::velox::functions::wasm
