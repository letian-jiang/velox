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

#include "velox/functions/java/ArrowIpc.h"

#include <limits>

#include <arrow/array/builder_primitive.h>
#include <arrow/buffer.h>
#include <arrow/c/bridge.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include "velox/common/base/Exceptions.h"
#include "velox/vector/arrow/Bridge.h"

namespace facebook::velox::functions::java {
namespace {

template <typename T>
T unwrapArrow(arrow::Result<T> result, std::string_view operation) {
  VELOX_USER_CHECK(
      result.ok(),
      "Java UDF Arrow IPC {} failed: {}",
      operation,
      result.status().ToString());
  return std::move(result).ValueUnsafe();
}

void checkArrow(arrow::Status status, std::string_view operation) {
  VELOX_USER_CHECK(
      status.ok(),
      "Java UDF Arrow IPC {} failed: {}",
      operation,
      status.ToString());
}

std::shared_ptr<arrow::RecordBatch> exportRecordBatch(
    const RowVectorPtr& rowVector,
    memory::MemoryPool* pool) {
  ArrowOptions options;
  options.flattenDictionary = true;
  options.flattenConstant = true;
  options.timestampUnit = TimestampUnit::kNano;
  options.useDecimalTypeWidth = false;
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
  vector_size_t* rawIndices = nullptr;
  if (!arguments.empty()) {
    indices = allocateIndices(rowCount, pool);
    rawIndices = indices->asMutable<vector_size_t>();
    vector_size_t compactRow = 0;
    rows.applyToSelected(
        [&](vector_size_t row) { rawIndices[compactRow++] = row; });
  }

  std::vector<std::string> names;
  std::vector<TypePtr> argumentTypes;
  std::vector<VectorPtr> compactArguments;
  names.reserve(arguments.size());
  argumentTypes.reserve(arguments.size());
  compactArguments.reserve(arguments.size());
  for (size_t index = 0; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    VELOX_USER_CHECK_GE(
        argument->size(), rows.end(), "Java UDF argument is too short");
    names.push_back("arg" + std::to_string(index));
    argumentTypes.push_back(argument->type());
    auto compactArgument = BaseVector::create(argument->type(), rowCount, pool);
    SelectivityVector compactRows(rowCount);
    compactArgument->copy(argument.get(), compactRows, rawIndices);
    compactArguments.push_back(std::move(compactArgument));
  }
  auto compact = std::make_shared<RowVector>(
      pool,
      ROW(std::move(names), std::move(argumentTypes)),
      nullptr,
      rowCount,
      std::move(compactArguments));
  return exportRecordBatch(compact, pool);
}

std::string serializeRecordBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch) {
  arrow::ipc::DictionaryFieldMapper mapper(*batch->schema());
  VELOX_USER_CHECK_EQ(
      mapper.num_dicts(),
      0,
      "Java UDF Arrow IPC must not contain dictionary-encoded columns");
  auto options = arrow::ipc::IpcWriteOptions::Defaults();
  options.metadata_version = arrow::ipc::MetadataVersion::V5;
  options.codec = nullptr;
  auto sink = unwrapArrow(
      arrow::io::BufferOutputStream::Create(), "output stream creation");
  auto writer = unwrapArrow(
      arrow::ipc::MakeStreamWriter(sink, batch->schema(), options),
      "stream writer creation");
  checkArrow(writer->WriteRecordBatch(*batch), "record batch write");
  checkArrow(writer->Close(), "stream writer close");
  auto buffer = unwrapArrow(sink->Finish(), "output stream finish");
  VELOX_USER_CHECK_LE(
      buffer->size(),
      std::numeric_limits<int32_t>::max(),
      "Java UDF Arrow IPC input exceeds the JNI byte-array limit");
  return std::string(
      reinterpret_cast<const char*>(buffer->data()), buffer->size());
}

} // namespace

GatheredArrowBatch gatherToArrowIpc(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool) {
  auto batch = gatherToArrowRecordBatch(rows, arguments, pool);
  return {
      .ipc = serializeRecordBatch(batch),
      .rowCount = rows.countSelected(),
  };
}

GatheredArrowBatch gatherToArrowIpcWithStateHandles(
    const SelectivityVector& rows,
    const std::vector<uint32_t>& stateHandles,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool) {
  VELOX_USER_CHECK_GE(
      stateHandles.size(), rows.end(), "Missing Java UDAF state handles");
  auto batch = gatherToArrowRecordBatch(rows, arguments, pool);

  arrow::UInt32Builder builder;
  checkArrow(builder.Reserve(rows.countSelected()), "state handle reserve");
  rows.applyToSelected([&](vector_size_t row) {
    VELOX_USER_CHECK_NE(
        stateHandles[row], 0, "Java UDAF state handle must be non-zero");
    checkArrow(builder.Append(stateHandles[row]), "state handle append");
  });
  auto handles = unwrapArrow(builder.Finish(), "state handle finish");
  auto field =
      arrow::field("__velox_java_state_handle", arrow::uint32(), false);
  batch = unwrapArrow(batch->AddColumn(0, field, handles), "state column add");
  return {
      .ipc = serializeRecordBatch(batch),
      .rowCount = rows.countSelected(),
  };
}

VectorPtr decodeArrowIpcResult(
    std::string_view ipc,
    const TypePtr& outputType,
    vector_size_t expectedRows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors) {
  VELOX_USER_CHECK_LE(
      ipc.size(),
      std::numeric_limits<int32_t>::max(),
      "Java UDF Arrow IPC output exceeds the supported size");
  auto buffer = arrow::Buffer::FromString(std::string(ipc));
  auto input = std::make_shared<arrow::io::BufferReader>(buffer);
  auto reader = unwrapArrow(
      arrow::ipc::RecordBatchStreamReader::Open(input),
      "stream reader creation");
  auto batch = unwrapArrow(reader->Next(), "record batch read");
  VELOX_USER_CHECK_NOT_NULL(
      batch, "Java UDF returned an empty Arrow IPC stream");
  VELOX_USER_CHECK_EQ(
      batch->num_rows(), expectedRows, "Java UDF returned the wrong row count");
  if (errors == nullptr) {
    VELOX_USER_CHECK_EQ(
        batch->num_columns(),
        1,
        "Java UDF result must contain exactly one column");
  } else {
    VELOX_USER_CHECK(
        batch->num_columns() == 1 || batch->num_columns() == 2,
        "Java scalar UDF result must contain one value column and an optional error column");
    if (batch->num_columns() == 2) {
      const auto& errorField = batch->schema()->field(1);
      VELOX_USER_CHECK_EQ(
          errorField->name(),
          "__velox_java_error",
          "Java scalar UDF error column has the wrong name");
      VELOX_USER_CHECK(
          errorField->type()->Equals(arrow::utf8()),
          "Java scalar UDF error column must be Arrow Utf8");
    }
  }
  auto extra = unwrapArrow(reader->Next(), "trailing record batch read");
  VELOX_USER_CHECK_NULL(
      extra, "Java UDF result must contain exactly one record batch");

  ArrowSchema schema;
  ArrowArray array;
  checkArrow(arrow::ExportSchema(*batch->schema(), &schema), "schema export");
  checkArrow(arrow::ExportRecordBatch(*batch, &array), "record batch export");
  auto row = std::dynamic_pointer_cast<RowVector>(
      importFromArrowAsOwner(schema, array, pool));
  VELOX_USER_CHECK_NOT_NULL(
      row, "Java UDF result is not an Arrow struct batch");
  auto result = row->childAt(0);
  VELOX_USER_CHECK(
      result->type()->equivalent(*outputType),
      "Java UDF returned type '{}', expected '{}'",
      result->type()->toString(),
      outputType->toString());

  if (errors != nullptr) {
    errors->clear();
    if (row->childrenSize() == 2) {
      errors->assign(expectedRows, std::nullopt);
      auto errorVector = row->childAt(1)->as<SimpleVector<StringView>>();
      VELOX_USER_CHECK_NOT_NULL(
          errorVector, "Java scalar UDF error result is not varchar");
      for (vector_size_t index = 0; index < expectedRows; ++index) {
        if (!errorVector->isNullAt(index)) {
          VELOX_USER_CHECK(
              result->isNullAt(index),
              "Java scalar UDF returned both a value and an error at row {}",
              index);
          (*errors)[index] = errorVector->valueAt(index).str();
        }
      }
    }
  }

  if (outputType->kind() == TypeKind::VARCHAR ||
      outputType->kind() == TypeKind::VARBINARY) {
    auto source = result->as<SimpleVector<StringView>>();
    VELOX_USER_CHECK_NOT_NULL(source, "Java UDF string result is not scalar");
    auto owned = BaseVector::create<FlatVector<StringView>>(
        outputType, expectedRows, pool);
    for (vector_size_t index = 0; index < expectedRows; ++index) {
      if (source->isNullAt(index)) {
        owned->setNull(index, true);
      } else {
        owned->set(index, source->valueAt(index));
      }
    }
    return owned;
  }
  return BaseVector::copy(*result, pool);
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

} // namespace facebook::velox::functions::java
