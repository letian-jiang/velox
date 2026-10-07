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

#include <arrow/memory_pool.h>
#include <folly/json.h>
#include <array>
#include <limits>
#include <mutex>

#include <arrow/array/array_decimal.h>
#include <arrow/array/array_nested.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/array/util.h>
#include <arrow/buffer.h>
#include <arrow/c/bridge.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/util/key_value_metadata.h>

#include "velox/common/base/Exceptions.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/TypeBridge.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/arrow/Bridge.h"

namespace facebook::velox::functions::wasm {
namespace {

class BoundedArrowPool final
    : public arrow::MemoryPool,
      public std::enable_shared_from_this<BoundedArrowPool> {
 public:
  explicit BoundedArrowPool(memory::MemoryPool* pool)
      : pool_(pool->shared_from_this()) {}

  arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out)
      override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (size < 0 || size > kLimit - bytes_) {
      return arrow::Status::OutOfMemory(
          "Wasm Arrow allocation exceeds 64 MiB limit");
    }
    if (size > 0) {
      pool_->reportExternalAllocation(size);
    }
    auto status = arrow::default_memory_pool()->Allocate(size, alignment, out);
    if (!status.ok()) {
      if (size > 0)
        pool_->reportExternalFree(size);
      return status;
    }
    bytes_ += size;
    total_ += size;
    peak_ = std::max(peak_, bytes_);
    ++allocations_;
    // Arrow's buffers retain a raw MemoryPool*. Keep it alive until their last
    // Free, including after the imported Velox vector outlives this decoder.
    ++outstanding_;
    keepAlive_ = shared_from_this();
    return status;
  }

  arrow::Status Reallocate(
      int64_t oldSize,
      int64_t newSize,
      int64_t alignment,
      uint8_t** ptr) override {
    uint8_t* replacement = nullptr;
    auto status = Allocate(newSize, alignment, &replacement);
    if (!status.ok())
      return status;
    if (std::min(oldSize, newSize) > 0) {
      std::memcpy(replacement, *ptr, std::min(oldSize, newSize));
    }
    Free(*ptr, oldSize, alignment);
    *ptr = replacement;
    return arrow::Status::OK();
  }

  void Free(uint8_t* buffer, int64_t size, int64_t alignment) override {
    auto self = shared_from_this();
    std::lock_guard<std::mutex> lock(mutex_);
    arrow::default_memory_pool()->Free(buffer, size, alignment);
    if (size > 0)
      pool_->reportExternalFree(size);
    bytes_ -= size;
    if (--outstanding_ == 0)
      keepAlive_.reset();
  }
  int64_t bytes_allocated() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
  }
  int64_t max_memory() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_;
  }
  int64_t total_bytes_allocated() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_;
  }
  int64_t num_allocations() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocations_;
  }
  std::string backend_name() const override {
    return "velox-wasm-bounded";
  }

 private:
  static constexpr int64_t kLimit = 64UL << 20;
  std::shared_ptr<memory::MemoryPool> pool_;
  std::shared_ptr<BoundedArrowPool> keepAlive_;
  mutable std::mutex mutex_;
  int64_t bytes_{0}, total_{0}, peak_{0}, allocations_{0}, outstanding_{0};
};

template <typename T>
T unwrapArrow(arrow::Result<T> result, std::string_view operation) {
  VELOX_CHECK(
      result.ok(),
      "Arrow IPC {} failed: {}",
      operation,
      result.status().ToString());
  return std::move(result).ValueUnsafe();
}

void checkArrow(arrow::Status status, std::string_view operation) {
  VELOX_CHECK(
      status.ok(), "Arrow IPC {} failed: {}", operation, status.ToString());
}

constexpr std::string_view kTimestampMarker = "timestamp_seconds_nanos_v1";
constexpr std::string_view kHugeintMarker = "hugeint_high_low_v1";
constexpr std::string_view kVarcharMarker = "varchar_bytes_v1";
bool isWideInteger(const TypePtr& type) {
  return type->isHugeint() && !type->isDecimal();
}
bool containsWireScalar(const TypePtr& type) {
  if (isBridgedType(type) || type->isTimestamp() || isWideInteger(type) ||
      type->isVarchar()) {
    return true;
  }
  for (size_t i = 0; i < type->size(); ++i) {
    if (containsWireScalar(type->childAt(i))) {
      return true;
    }
  }
  return false;
}

bool containsBridgedType(const TypePtr& type) {
  if (isBridgedType(type)) {
    return true;
  }
  for (size_t i = 0; i < type->size(); ++i) {
    if (containsBridgedType(type->childAt(i))) {
      return true;
    }
  }
  return false;
}

// UDF results may be reused across disjoint selections. A native LazyVector
// loader is single-use, so decode the complete codec column on its first load.
// Unaccessed codec columns remain serialized, including nested ROW/ARRAY/MAP
// values. MAP keys and invocation-scoped handles are validated eagerly.
class CodecVectorLoader final : public VectorLoader {
 public:
  CodecVectorLoader(
      VectorPtr source,
      std::shared_ptr<const WasmTypeCodec> codec,
      memory::MemoryPool* pool,
      std::function<void()> onFailure)
      : pool_(pool->shared_from_this()),
        source_(std::move(source)),
        codec_(std::move(codec)),
        onFailure_(std::move(onFailure)) {}

 private:
  void loadInternal(
      RowSet /*rows*/,
      ValueHook* hook,
      vector_size_t resultSize,
      VectorPtr* result) override {
    if (failure_) {
      std::rethrow_exception(failure_);
    }
    // Never leave an empty/partially decoded vector visible after an error.
    // LazyVector::load marks itself loaded before invoking the loader.
    *result = nullptr;
    try {
      VELOX_CHECK_NULL(hook, "Wasm codec loader does not support ValueHook");
      VELOX_CHECK_NOT_NULL(source_);
      VELOX_CHECK_LE(resultSize, source_->size());
      auto decoded =
          BaseVector::create(codec_->type, source_->size(), pool_.get());
      const auto* payload =
          source_->as<RowVector>()->childAt(0)->as<SimpleVector<StringView>>();
      for (vector_size_t row = 0; row < source_->size(); ++row) {
        if (source_->isNullAt(row)) {
          decoded->setNull(row, true);
        } else {
          codec_->decode(payload->valueAt(row), *decoded, row);
          VELOX_CHECK(
              !decoded->isNullAt(row),
              "Custom codec decoded a non-null payload as NULL");
        }
      }
      *result = std::move(decoded);
      source_.reset();
    } catch (...) {
      if (onFailure_) {
        try {
          onFailure_();
        } catch (...) { /* Preserve the decoder failure. */
        }
      }
      try {
        try {
          throw;
        } catch (const VeloxRuntimeError&) {
          throw;
        } catch (const std::exception& error) {
          VELOX_FAIL("Wasm output codec decode failure: {}", error.what());
        } catch (...) {
          VELOX_FAIL("Wasm output codec decode failure");
        }
      } catch (...) {
        failure_ = std::current_exception();
        std::rethrow_exception(failure_);
      }
    }
  }

  std::shared_ptr<memory::MemoryPool> pool_;
  VectorPtr source_;
  std::shared_ptr<const WasmTypeCodec> codec_;
  std::function<void()> onFailure_;
  std::exception_ptr failure_;
};

// Scalar row ABI timestamps use a marked struct(seconds BIGINT,nanos BIGINT).
// Full-width HUGEINT similarly uses high and low halves, with a distinct
// marker. This retains Velox's full range, unlike Arrow's i64 nanosecond
// timestamp.
VectorPtr scalarWireVector(
    const VectorPtr& input,
    bool encode,
    const TypePtr& logicalType,
    memory::MemoryPool* pool,
    const std::shared_ptr<WasmOpaqueScope>& scope = nullptr,
    const ArrowIpcDecodeOptions* decodeOptions = nullptr) {
  if (!containsWireScalar(logicalType)) {
    return input;
  }
  auto vector = input;
  if (!vector->isFlatEncoding() &&
      vector->encoding() != VectorEncoding::Simple::ARRAY &&
      vector->encoding() != VectorEncoding::Simple::MAP &&
      vector->encoding() != VectorEncoding::Simple::ROW) {
    vector = BaseVector::copy(*input, pool);
    BaseVector::flattenVector(vector);
  }
  if (isBridgedType(logicalType)) {
    auto codec = findWasmTypeCodec(logicalType);
    if (codec) {
      if (encode) {
        auto payload = BaseVector::create<FlatVector<StringView>>(
            VARBINARY(), vector->size(), pool);
        for (vector_size_t row = 0; row < vector->size(); ++row) {
          if (vector->isNullAt(row)) {
            payload->setNull(row, true);
          } else {
            auto bytes = codec->encode(*vector, row);
            payload->set(row, StringView(bytes));
          }
        }
        return std::make_shared<RowVector>(
            pool,
            ROW({"payload"}, {VARBINARY()}),
            vector->nulls(),
            vector->size(),
            std::vector<VectorPtr>{payload});
      }
      auto source = vector->as<RowVector>();
      VELOX_CHECK(
          source && source->childrenSize() == 1,
          "Invalid custom codec wire value");
      auto payload = source->childAt(0)->as<SimpleVector<StringView>>();
      VELOX_CHECK_NOT_NULL(payload);
      // Validate payload presence before returning a lazy vector, including
      // columns that downstream expressions never access.
      for (vector_size_t row = 0; row < vector->size(); ++row) {
        VELOX_CHECK(
            vector->isNullAt(row) || !payload->isNullAt(row),
            "Custom codec payload must be non-null");
      }
      if (decodeOptions && decodeOptions->lazyCodecs) {
        // The LazyVector base may retain the decoded NULL bitmap after its
        // loader is destroyed. Keep the pool alive through the complete vector
        // destruction, including BaseVector, rather than only in the loader.
        return VectorPtr(
            new LazyVector(
                pool,
                logicalType,
                vector->size(),
                std::make_unique<CodecVectorLoader>(
                    vector,
                    codec,
                    pool,
                    decodeOptions->onDeferredCodecFailure)),
            [owner = pool->shared_from_this()](BaseVector* lazy) {
              (void)owner;
              delete lazy;
            });
      }
      auto result = BaseVector::create(logicalType, vector->size(), pool);
      for (vector_size_t row = 0; row < vector->size(); ++row) {
        if (vector->isNullAt(row)) {
          result->setNull(row, true);
        } else {
          VELOX_CHECK(
              !payload->isNullAt(row), "Custom codec payload must be non-null");
          codec->decode(payload->valueAt(row), *result, row);
          VELOX_CHECK(
              !result->isNullAt(row),
              "Custom codec decoded a non-null payload as NULL");
        }
      }
      return result;
    }
    VELOX_CHECK(
        logicalType->kind() == TypeKind::OPAQUE,
        "No Wasm codec registered for custom type {}",
        logicalType->toString());
    VELOX_CHECK_NOT_NULL(
        scope, "OPAQUE transport requires an invocation scope");
    if (encode) {
      auto tokens = BaseVector::create<FlatVector<int64_t>>(
          BIGINT(), vector->size(), pool);
      auto handles = BaseVector::create<FlatVector<int64_t>>(
          BIGINT(), vector->size(), pool);
      auto source = vector->as<SimpleVector<std::shared_ptr<void>>>();
      for (vector_size_t row = 0; row < vector->size(); ++row) {
        if (vector->isNullAt(row)) {
          tokens->setNull(row, true);
          handles->setNull(row, true);
        } else {
          tokens->set(row, scope->token());
          handles->set(row, scope->add(logicalType, source->valueAt(row)));
        }
      }
      return std::make_shared<RowVector>(
          pool,
          ROW({"scope", "handle"}, {BIGINT(), BIGINT()}),
          vector->nulls(),
          vector->size(),
          std::vector<VectorPtr>{tokens, handles});
    }
    auto source = vector->as<RowVector>();
    VELOX_CHECK(
        source && source->childrenSize() == 2, "Invalid OPAQUE wire value");
    auto tokens = source->childAt(0)->as<SimpleVector<int64_t>>();
    auto handles = source->childAt(1)->as<SimpleVector<int64_t>>();
    VELOX_CHECK(tokens && handles, "Invalid OPAQUE handle fields");
    auto result = BaseVector::create<FlatVector<std::shared_ptr<void>>>(
        logicalType, vector->size(), pool);
    for (vector_size_t row = 0; row < vector->size(); ++row) {
      if (vector->isNullAt(row)) {
        result->setNull(row, true);
      } else {
        VELOX_CHECK(
            !tokens->isNullAt(row) && !handles->isNullAt(row),
            "OPAQUE handle fields must be non-null");
        result->set(
            row,
            scope->resolve(
                tokens->valueAt(row), handles->valueAt(row), logicalType));
      }
    }
    return result;
  }
  if (logicalType->isVarchar()) {
    if (encode) {
      auto source = vector->as<FlatVector<StringView>>();
      VELOX_CHECK_NOT_NULL(source);
      auto bytes = std::make_shared<FlatVector<StringView>>(
          pool,
          VARBINARY(),
          source->nulls(),
          source->size(),
          source->values(),
          std::vector<BufferPtr>(source->stringBuffers()));
      return std::make_shared<RowVector>(
          pool,
          ROW({"bytes"}, {VARBINARY()}),
          source->nulls(),
          source->size(),
          std::vector<VectorPtr>{bytes});
    }
    if (vector->type()->isVarchar()) {
      return vector;
    } // Legacy UTF-8 ABI.
    auto source = vector->as<RowVector>();
    VELOX_CHECK(
        source && source->childrenSize() == 1 &&
            source->childAt(0)->type()->isVarbinary(),
        "Invalid VARCHAR wire value");
    auto bytes = source->childAt(0)->as<FlatVector<StringView>>();
    VELOX_CHECK_NOT_NULL(bytes);
    for (vector_size_t i = 0; i < source->size(); ++i) {
      VELOX_CHECK(
          source->isNullAt(i) || !bytes->isNullAt(i),
          "VARCHAR bytes must be non-null");
    }
    return std::make_shared<FlatVector<StringView>>(
        pool,
        logicalType,
        source->nulls(),
        source->size(),
        bytes->values(),
        std::vector<BufferPtr>(bytes->stringBuffers()));
  }
  if (isWideInteger(logicalType)) {
    if (encode) {
      auto high = BaseVector::create<FlatVector<int64_t>>(
          BIGINT(), vector->size(), pool);
      auto low = BaseVector::create<FlatVector<int64_t>>(
          BIGINT(), vector->size(), pool);
      auto source = vector->as<SimpleVector<int128_t>>();
      for (vector_size_t i = 0; i < vector->size(); ++i) {
        if (source->isNullAt(i)) {
          high->setNull(i, true);
          low->setNull(i, true);
        } else {
          auto bits = static_cast<__uint128_t>(source->valueAt(i));
          high->set(i, static_cast<int64_t>(bits >> 64));
          low->set(i, static_cast<int64_t>(bits));
        }
      }
      return std::make_shared<RowVector>(
          pool,
          ROW({"high", "low"}, {BIGINT(), BIGINT()}),
          vector->nulls(),
          vector->size(),
          std::vector<VectorPtr>{high, low});
    }
    auto source = vector->as<RowVector>();
    VELOX_CHECK(
        source && source->childrenSize() == 2 &&
            source->childAt(0)->type()->isBigint() &&
            source->childAt(1)->type()->isBigint(),
        "Invalid hugeint wire value");
    auto high = source->childAt(0)->as<SimpleVector<int64_t>>();
    auto low = source->childAt(1)->as<SimpleVector<int64_t>>();
    auto result = BaseVector::create<FlatVector<int128_t>>(
        logicalType, source->size(), pool);
    for (vector_size_t i = 0; i < source->size(); ++i) {
      if (source->isNullAt(i)) {
        result->setNull(i, true);
        continue;
      }
      VELOX_CHECK(
          !high->isNullAt(i) && !low->isNullAt(i),
          "Hugeint components must be non-null");
      auto bits =
          (static_cast<__uint128_t>(static_cast<uint64_t>(high->valueAt(i)))
           << 64) |
          static_cast<uint64_t>(low->valueAt(i));
      result->set(i, static_cast<int128_t>(bits));
    }
    return result;
  }
  if (logicalType->isTimestamp()) {
    if (encode) {
      auto seconds = BaseVector::create<FlatVector<int64_t>>(
          BIGINT(), vector->size(), pool);
      auto nanos = BaseVector::create<FlatVector<int64_t>>(
          BIGINT(), vector->size(), pool);
      auto source = vector->as<SimpleVector<Timestamp>>();
      for (vector_size_t i = 0; i < vector->size(); ++i) {
        if (source->isNullAt(i)) {
          seconds->setNull(i, true);
          nanos->setNull(i, true);
        } else {
          auto value = source->valueAt(i);
          seconds->set(i, value.getSeconds());
          nanos->set(i, value.getNanos());
        }
      }
      return std::make_shared<RowVector>(
          pool,
          ROW({"seconds", "nanos"}, {BIGINT(), BIGINT()}),
          vector->nulls(),
          vector->size(),
          std::vector<VectorPtr>{seconds, nanos});
    }
    if (vector->type()->isTimestamp()) {
      return vector;
    } // Existing ABI v1 modules.
    auto row = vector->as<RowVector>();
    VELOX_CHECK(
        row && row->childrenSize() == 2 &&
            row->childAt(0)->type()->isBigint() &&
            row->childAt(1)->type()->isBigint(),
        "Invalid timestamp wire value");
    auto seconds = row->childAt(0)->as<SimpleVector<int64_t>>();
    auto nanos = row->childAt(1)->as<SimpleVector<int64_t>>();
    auto result = BaseVector::create<FlatVector<Timestamp>>(
        logicalType, row->size(), pool);
    for (vector_size_t i = 0; i < row->size(); ++i) {
      if (row->isNullAt(i)) {
        result->setNull(i, true);
        continue;
      }
      VELOX_CHECK(
          !seconds->isNullAt(i) && !nanos->isNullAt(i),
          "Timestamp components must be non-null");
      auto sec = seconds->valueAt(i);
      auto nano = nanos->valueAt(i);
      VELOX_CHECK(
          sec >= Timestamp::kMinSeconds && sec <= Timestamp::kMaxSeconds &&
              nano >= 0 && nano <= Timestamp::kMaxNanos,
          "Timestamp components outside Velox range");
      result->set(i, Timestamp(sec, nano));
    }
    return result;
  }
  if (logicalType->isArray()) {
    auto source = vector->as<ArrayVector>();
    auto child = scalarWireVector(
        source->elements(),
        encode,
        logicalType->childAt(0),
        pool,
        scope,
        decodeOptions);
    return std::make_shared<ArrayVector>(
        pool,
        encode ? ARRAY(child->type()) : logicalType,
        source->nulls(),
        source->size(),
        source->offsets(),
        source->sizes(),
        child);
  }
  if (logicalType->isMap()) {
    auto source = vector->as<MapVector>();
    auto keys = scalarWireVector(
        source->mapKeys(), encode, logicalType->childAt(0), pool, scope);
    auto values = scalarWireVector(
        source->mapValues(),
        encode,
        logicalType->childAt(1),
        pool,
        scope,
        decodeOptions);
    return std::make_shared<MapVector>(
        pool,
        encode ? MAP(keys->type(), values->type()) : logicalType,
        source->nulls(),
        source->size(),
        source->offsets(),
        source->sizes(),
        keys,
        values);
  }
  auto source = vector->as<RowVector>();
  std::vector<VectorPtr> children;
  std::vector<TypePtr> types;
  for (size_t i = 0; i < source->childrenSize(); ++i) {
    auto child = scalarWireVector(
        source->childAt(i),
        encode,
        logicalType->childAt(i),
        pool,
        scope,
        decodeOptions);
    types.push_back(child->type());
    children.push_back(std::move(child));
  }
  auto type = encode ? ROW(logicalType->asRow().names(), std::move(types))
                     : logicalType;
  return std::make_shared<RowVector>(
      pool, type, source->nulls(), source->size(), std::move(children));
}

std::shared_ptr<arrow::DataType> annotateScalarWireType(
    const std::shared_ptr<arrow::DataType>& physical,
    const TypePtr& logical) {
  if (!containsWireScalar(logical)) {
    return physical;
  }
  if (isBridgedType(logical)) {
    std::unordered_map<std::string, std::string> properties{
        {"velox.type_identity", bridgeTypeIdentity(logical)}};
    if (auto codec = findWasmTypeCodec(logical)) {
      properties["velox.logical_type"] = "custom_codec_v1";
      properties["velox.codec_id"] = codec->id;
      properties["velox.codec_version"] = std::to_string(codec->version);
      return arrow::struct_({arrow::field(
          "payload",
          arrow::binary(),
          true,
          std::make_shared<arrow::KeyValueMetadata>(properties))});
    }
    VELOX_CHECK(
        logical->kind() == TypeKind::OPAQUE, "Custom type requires a codec");
    properties["velox.logical_type"] = "opaque_handle_v1";
    return arrow::struct_(
        {arrow::field(
             "scope",
             arrow::int64(),
             true,
             std::make_shared<arrow::KeyValueMetadata>(properties)),
         arrow::field("handle", arrow::int64(), true)});
  }
  if (logical->isVarchar()) {
    auto metadata = std::make_shared<arrow::KeyValueMetadata>(
        std::unordered_map<std::string, std::string>{
            {"velox.logical_type", std::string(kVarcharMarker)}});
    return arrow::struct_(
        {arrow::field("bytes", arrow::binary(), true, metadata)});
  }
  if (isWideInteger(logical)) {
    auto metadata = std::make_shared<arrow::KeyValueMetadata>(
        std::unordered_map<std::string, std::string>{
            {"velox.logical_type", std::string(kHugeintMarker)}});
    return arrow::struct_(
        {arrow::field("high", arrow::int64(), true, metadata),
         arrow::field("low", arrow::int64(), true)});
  }
  if (logical->isTimestamp()) {
    auto metadata = std::make_shared<arrow::KeyValueMetadata>(
        std::unordered_map<std::string, std::string>{
            {"velox.logical_type", std::string(kTimestampMarker)}});
    return arrow::struct_(
        {arrow::field("seconds", arrow::int64(), true, metadata),
         arrow::field("nanos", arrow::int64(), true)});
  }
  if (logical->isArray()) {
    auto type = std::static_pointer_cast<arrow::ListType>(physical);
    return arrow::list(type->value_field()->WithType(
        annotateScalarWireType(type->value_type(), logical->childAt(0))));
  }
  if (logical->isMap()) {
    auto type = std::static_pointer_cast<arrow::MapType>(physical);
    auto entries = arrow::struct_(
        {type->key_field()->WithType(
             annotateScalarWireType(type->key_type(), logical->childAt(0))),
         type->item_field()->WithType(
             annotateScalarWireType(type->item_type(), logical->childAt(1)))});
    return std::make_shared<arrow::MapType>(
        type->field(0)->WithType(entries), type->keys_sorted());
  }
  std::vector<std::shared_ptr<arrow::Field>> fields;
  for (size_t i = 0; i < logical->size(); ++i) {
    fields.push_back(physical->field(i)->WithType(annotateScalarWireType(
        physical->field(i)->type(), logical->childAt(i))));
  }
  return arrow::struct_(std::move(fields));
}

std::shared_ptr<arrow::ArrayData> withArrowType(
    const std::shared_ptr<arrow::ArrayData>& input,
    const std::shared_ptr<arrow::DataType>& type) {
  auto data = input->Copy();
  data->type = type;
  for (size_t i = 0; i < data->child_data.size(); ++i) {
    data->child_data[i] =
        withArrowType(data->child_data[i], type->field(i)->type());
  }
  return data;
}

// MAP has the same IPC layout as LIST<STRUCT<key,value>>, but Arrow's MAP
// constructor aborts on NULL entries/keys before ValidateFull can return
// Status. Read untrusted maps as lists first, then validate and restore their
// types.
bool containsArrowMap(const std::shared_ptr<arrow::DataType>& type) {
  if (type->id() == arrow::Type::MAP)
    return true;
  for (const auto& field : type->fields()) {
    if (containsArrowMap(field->type()))
      return true;
  }
  return false;
}

std::shared_ptr<arrow::DataType> safeMapReadType(
    const std::shared_ptr<arrow::DataType>& type) {
  if (!containsArrowMap(type))
    return type;
  std::vector<std::shared_ptr<arrow::Field>> fields;
  bool changed = type->id() == arrow::Type::MAP;
  for (const auto& field : type->fields()) {
    auto child = safeMapReadType(field->type());
    changed |= child != field->type();
    fields.push_back(
        child == field->type() ? field : field->WithType(std::move(child)));
  }
  if (!changed)
    return type;
  switch (type->id()) {
    case arrow::Type::MAP:
    case arrow::Type::LIST:
      return arrow::list(fields.at(0));
    case arrow::Type::STRUCT:
      return arrow::struct_(std::move(fields));
    default:
      VELOX_FAIL("Unsupported Wasm MAP container");
  }
}

void validateMapArrayData(
    const std::shared_ptr<arrow::ArrayData>& data,
    const std::shared_ptr<arrow::DataType>& declared) {
  if (declared->id() == arrow::Type::MAP) {
    const auto& entries = data->child_data.at(0);
    VELOX_CHECK_EQ(
        arrow::MakeArray(entries)->null_count(),
        0,
        "Wasm MAP entry contains NULL");
    VELOX_CHECK_EQ(
        arrow::MakeArray(entries->child_data.at(0))->null_count(),
        0,
        "Wasm MAP key contains NULL");
  }
  for (size_t i = 0; i < data->child_data.size(); ++i) {
    validateMapArrayData(data->child_data[i], declared->field(i)->type());
  }
}

// Native intermediate ROWs can be positionally compatible without equal field
// names. Keep every leaf type (including codec and OPAQUE identity) exact.
bool typesMatchExceptRowNames(const TypePtr& actual, const TypePtr& expected) {
  if (*actual == *expected) {
    return true;
  }
  if (isBridgedType(actual) || isBridgedType(expected) ||
      actual->kind() != expected->kind() ||
      actual->size() != expected->size() ||
      !(actual->isRow() || actual->isArray() || actual->isMap())) {
    return false;
  }
  for (size_t i = 0; i < actual->size(); ++i) {
    if (!typesMatchExceptRowNames(actual->childAt(i), expected->childAt(i))) {
      return false;
    }
  }
  return true;
}

std::shared_ptr<arrow::DataType> canonicalIntermediateWireType(
    const std::shared_ptr<arrow::DataType>& physical,
    const TypePtr& logical) {
  // Wire structs for VARCHAR, HUGEINT, TIMESTAMP and codecs are not SQL ROWs.
  if (isBridgedType(logical)) {
    return physical;
  }
  if (logical->isRow()) {
    VELOX_CHECK(physical->id() == arrow::Type::STRUCT);
    VELOX_CHECK_EQ(physical->num_fields(), logical->size());
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (size_t i = 0; i < logical->size(); ++i) {
      fields.push_back(
          physical->field(i)
              ->WithName(logical->asRow().nameOf(i))
              ->WithType(canonicalIntermediateWireType(
                  physical->field(i)->type(), logical->childAt(i))));
    }
    return arrow::struct_(std::move(fields));
  }
  if (logical->isArray()) {
    auto type = std::static_pointer_cast<arrow::ListType>(physical);
    return arrow::list(
        type->value_field()->WithType(canonicalIntermediateWireType(
            type->value_type(), logical->childAt(0))));
  }
  if (logical->isMap()) {
    auto type = std::static_pointer_cast<arrow::MapType>(physical);
    auto entries = arrow::struct_(
        {type->key_field()->WithType(canonicalIntermediateWireType(
             type->key_type(), logical->childAt(0))),
         type->item_field()->WithType(canonicalIntermediateWireType(
             type->item_type(), logical->childAt(1)))});
    return std::make_shared<arrow::MapType>(
        type->field(0)->WithType(entries), type->keys_sorted());
  }
  return physical;
}

std::shared_ptr<arrow::RecordBatch> canonicalizeIntermediateBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::vector<VectorPtr>& arguments,
    const TypePtr& expected) {
  if (!expected) {
    return batch;
  }
  VELOX_CHECK_EQ(arguments.size(), 1);
  VELOX_CHECK_EQ(batch->num_columns(), 1);
  const auto& actual = arguments[0]->type();
  VELOX_CHECK(
      typesMatchExceptRowNames(actual, expected),
      "Wasm UDAF intermediate type mismatch: {} vs. {}",
      actual->toString(),
      expected->toString());
  if (*actual == *expected) {
    return batch;
  }
  auto type = canonicalIntermediateWireType(batch->column(0)->type(), expected);
  // Only schema/ArrayData descriptors change. Buffers, offsets, null counts
  // and ownership remain intact, and caller-owned vectors are never mutated.
  return arrow::RecordBatch::Make(
      arrow::schema(
          {batch->schema()->field(0)->WithType(type)},
          batch->schema()->metadata()),
      batch->num_rows(),
      {arrow::MakeArray(withArrowType(batch->column(0)->data(), type))});
}

bool scalarWireSchemaValid(
    const std::shared_ptr<arrow::DataType>& type,
    const TypePtr& logical) {
  if (logical->isDecimal()) {
    const auto [precision, scale] = getDecimalPrecisionScale(*logical);
    return type->Equals(arrow::decimal128(precision, scale));
  }
  if (logical->isPrimitiveType() && !containsWireScalar(logical)) {
    switch (logical->kind()) {
      case TypeKind::BOOLEAN:
        return type->id() == arrow::Type::BOOL;
      case TypeKind::TINYINT:
        return type->id() == arrow::Type::INT8;
      case TypeKind::SMALLINT:
        return type->id() == arrow::Type::INT16;
      case TypeKind::INTEGER:
        return type->id() ==
            (logical->isDate() ? arrow::Type::DATE32 : arrow::Type::INT32);
      case TypeKind::BIGINT:
        return type->id() == arrow::Type::INT64;
      case TypeKind::REAL:
        return type->id() == arrow::Type::FLOAT;
      case TypeKind::DOUBLE:
        return type->id() == arrow::Type::DOUBLE;
      case TypeKind::VARBINARY:
        return type->id() == arrow::Type::BINARY;
      case TypeKind::UNKNOWN:
        return type->id() == arrow::Type::NA;
      default:
        return false;
    }
  }
  if (isBridgedType(logical)) {
    auto codec = findWasmTypeCodec(logical);
    auto expected = annotateScalarWireType(nullptr, logical);
    return type->Equals(expected, true);
  }
  if (logical->isVarchar()) {
    if (type->id() == arrow::Type::STRING) {
      return true;
    }
    if (type->id() != arrow::Type::STRUCT || type->num_fields() != 1) {
      return false;
    }
    auto field = type->field(0);
    auto metadata = field->metadata();
    if (!metadata) {
      return false;
    }
    auto index = metadata->FindKey("velox.logical_type");
    return index >= 0 && metadata->value(index) == kVarcharMarker &&
        field->name() == "bytes" && field->type()->id() == arrow::Type::BINARY;
  }
  if (logical->isTimestamp() || isWideInteger(logical)) {
    if (logical->isTimestamp() && type->id() == arrow::Type::TIMESTAMP) {
      return true;
    }
    if (type->id() != arrow::Type::STRUCT || type->num_fields() != 2) {
      return false;
    }
    auto field = type->field(0);
    auto metadata = field->metadata();
    if (!metadata) {
      return false;
    }
    auto index = metadata->FindKey("velox.logical_type");
    return index >= 0 &&
        metadata->value(index) ==
        (logical->isTimestamp() ? kTimestampMarker : kHugeintMarker) &&
        field->name() == (logical->isTimestamp() ? "seconds" : "high") &&
        type->field(1)->name() == (logical->isTimestamp() ? "nanos" : "low") &&
        field->type()->id() == arrow::Type::INT64 &&
        type->field(1)->type()->id() == arrow::Type::INT64;
  }
  if (logical->isArray()) {
    return type->id() == arrow::Type::LIST &&
        scalarWireSchemaValid(type->field(0)->type(), logical->childAt(0));
  }
  if (logical->isMap()) {
    if (type->id() != arrow::Type::MAP) {
      return false;
    }
    auto map = std::static_pointer_cast<arrow::MapType>(type);
    return scalarWireSchemaValid(map->key_type(), logical->childAt(0)) &&
        scalarWireSchemaValid(map->item_type(), logical->childAt(1));
  }
  if (type->id() != arrow::Type::STRUCT ||
      type->num_fields() != logical->size()) {
    return false;
  }
  for (size_t i = 0; i < logical->size(); ++i) {
    if (!scalarWireSchemaValid(type->field(i)->type(), logical->childAt(i))) {
      return false;
    }
  }
  return true;
}

folly::dynamic wireDescription(const TypePtr& type) {
  if (isBridgedType(type)) {
    folly::dynamic node =
        folly::dynamic::object("identity", bridgeTypeIdentity(type));
    if (auto codec = findWasmTypeCodec(type)) {
      node["kind"] = "codec";
      node["id"] = codec->id;
      node["version"] = codec->version;
    } else {
      VELOX_CHECK(
          type->kind() == TypeKind::OPAQUE, "Custom type requires a codec");
      node["kind"] = "opaque";
    }
    return node;
  }
  if (type->isArray() || type->isMap() || type->isRow()) {
    folly::dynamic node = folly::dynamic::object(
        "kind",
        type->isArray()     ? "array"
            : type->isMap() ? "map"
                            : "row");
    node["children"] = folly::dynamic::array();
    for (size_t i = 0; i < type->size(); ++i) {
      node["children"].push_back(wireDescription(type->childAt(i)));
    }
    if (type->isRow()) {
      node["names"] = folly::dynamic::array();
      for (const auto& name : type->asRow().names()) {
        node["names"].push_back(name);
      }
    }
    return node;
  }
  return folly::dynamic::object("kind", "sql")("type", signatureType(type));
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

bool canExportToArrow(const VectorPtr& vector) {
  if (vector->isFlatEncoding()) {
    return true;
  }
  // The native bridge can flatten scalar dictionary/constant leaves. A
  // complex parent need not copy all its other children just for such a leaf.
  // UNKNOWN and encoded complex values still need the gathering fallback.
  if (vector->isScalar() && vector->typeKind() != TypeKind::UNKNOWN &&
      (vector->encoding() == VectorEncoding::Simple::DICTIONARY ||
       vector->isConstantEncoding())) {
    auto values = vector->valueVector().get();
    if (!values)
      return true;
    // wrappedVector() on a LazyVector loads every base row. Inspect wrappers
    // without loading so the fallback can retain native selective loading.
    while (values->encoding() == VectorEncoding::Simple::DICTIONARY ||
           values->isConstantEncoding()) {
      values = values->valueVector().get();
      if (!values)
        return false;
    }
    return values->isFlatEncoding();
  }
  if (auto array = vector->as<ArrayVector>()) {
    return canExportToArrow(array->elements());
  }
  if (auto map = vector->as<MapVector>()) {
    return canExportToArrow(map->mapKeys()) &&
        canExportToArrow(map->mapValues());
  }
  if (auto row = vector->as<RowVector>()) {
    return std::all_of(
        row->children().begin(), row->children().end(), [](const auto& child) {
          return child && canExportToArrow(child);
        });
  }
  return false;
}

std::shared_ptr<arrow::RecordBatch> gatherToArrowRecordBatch(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool,
    bool rowScalars = false,
    const std::shared_ptr<WasmOpaqueScope>& scope = nullptr) {
  const auto rowCount = rows.countSelected();
  if (arguments.empty()) {
    return arrow::RecordBatch::Make(
        arrow::schema({}),
        rowCount,
        std::vector<std::shared_ptr<arrow::Array>>{});
  }
  BufferPtr indices;
  auto gatherIndices = [&]() -> const BufferPtr& {
    if (!indices) {
      indices = allocateIndices(rowCount, pool);
      auto* rawIndices = indices->asMutable<vector_size_t>();
      vector_size_t compactRow = 0;
      rows.applyToSelected(
          [&](vector_size_t row) { rawIndices[compactRow++] = row; });
    }
    return indices;
  };

  std::vector<TypePtr> argumentTypes;
  std::vector<VectorPtr> compactArguments;
  argumentTypes.reserve(arguments.size());
  compactArguments.reserve(arguments.size());
  for (const auto& original : arguments) {
    auto argument = original;
    if (argument->isConstantEncoding() && argument->size() < rows.end()) {
      VELOX_CHECK_GT(argument->size(), 0, "Empty Wasm UDF constant argument");
      argument = BaseVector::wrapInConstant(rows.size(), 0, argument);
    }
    VELOX_CHECK_GE(
        argument->size(), rows.end(), "Wasm UDF argument is too short");
    argumentTypes.push_back(argument->type());
    if (rows.isAllSelected() && argument->size() == rowCount &&
        canExportToArrow(argument) &&
        argument->typeKind() != TypeKind::UNKNOWN) {
      compactArguments.push_back(argument);
    } else if (argument->typeKind() == TypeKind::UNKNOWN) {
      // UNKNOWN is always null and is not classified as a scalar by the bridge.
      compactArguments.push_back(BaseVector::create(UNKNOWN(), rowCount, pool));
    } else if (
        argument->typeKind() == TypeKind::ARRAY ||
        argument->typeKind() == TypeKind::MAP ||
        argument->typeKind() == TypeKind::ROW) {
      // The Arrow bridge cannot flatten dictionary encodings around nested
      // vectors. Copy only selected rows into a plain complex vector.
      auto compact = BaseVector::create(argument->type(), rowCount, pool);
      SelectivityVector compactRows(rowCount);
      compact->copy(
          argument.get(), compactRows, gatherIndices()->as<vector_size_t>());
      compactArguments.push_back(std::move(compact));
    } else {
      compactArguments.push_back(
          BaseVector::wrapInDictionary(
              nullptr, gatherIndices(), rowCount, argument));
    }
  }
  if (rowScalars) {
    for (size_t i = 0; i < compactArguments.size(); ++i) {
      compactArguments[i] = scalarWireVector(
          compactArguments[i], true, argumentTypes[i], pool, scope);
    }
  }
  auto logicalRowType = ROW(std::vector<TypePtr>(argumentTypes));
  for (size_t i = 0; i < argumentTypes.size(); ++i) {
    argumentTypes[i] = compactArguments[i]->type();
  }
  auto compact = std::make_shared<RowVector>(
      pool,
      ROW(std::move(argumentTypes)),
      nullptr,
      rowCount,
      std::move(compactArguments));
  auto batch = exportRecordBatch(compact, pool);
  if (!rowScalars || !containsWireScalar(logicalRowType)) {
    return batch;
  }
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> columns;
  for (size_t i = 0; i < compact->childrenSize(); ++i) {
    auto type = annotateScalarWireType(
        batch->column(i)->type(), logicalRowType->childAt(i));
    fields.push_back(batch->schema()->field(i)->WithType(type));
    columns.push_back(
        arrow::MakeArray(withArrowType(batch->column(i)->data(), type)));
  }
  return arrow::RecordBatch::Make(
      arrow::schema(std::move(fields)), rowCount, std::move(columns));
}

void ownVariableWidthValues(VectorPtr& vector, memory::MemoryPool* pool) {
  switch (vector->typeKind()) {
    case TypeKind::VARCHAR:
    case TypeKind::VARBINARY: {
      auto* source = vector->as<SimpleVector<StringView>>();
      VELOX_CHECK_NOT_NULL(source, "Wasm UDF string result is not scalar");
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
  explicit Impl(
      const std::shared_ptr<arrow::RecordBatch>& batch,
      std::shared_ptr<WasmOpaqueScope> scope)
      : options(arrow::ipc::IpcWriteOptions::Defaults()),
        opaqueScope(std::move(scope)) {
    arrow::ipc::DictionaryFieldMapper mapper(*batch->schema());
    VELOX_CHECK_EQ(
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
    VELOX_CHECK_LE(
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
  std::shared_ptr<WasmOpaqueScope> opaqueScope;
};

ArrowIpcInput::ArrowIpcInput(
    std::shared_ptr<arrow::RecordBatch> batch,
    std::shared_ptr<WasmOpaqueScope> scope)
    : impl_(std::make_shared<Impl>(std::move(batch), std::move(scope))) {}

std::shared_ptr<WasmOpaqueScope> ArrowIpcInput::opaqueScope() const {
  return impl_->opaqueScope;
}

ArrowIpcInput::~ArrowIpcInput() = default;

uint32_t ArrowIpcInput::size() const {
  return impl_->size;
}

void ArrowIpcInput::write(uint8_t* destination, uint32_t capacity) const {
  VELOX_CHECK_EQ(
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
  VELOX_CHECK_EQ(
      unwrapArrow(writer.Tell(), "output size check"),
      capacity,
      "Arrow IPC writer produced an unexpected size");
}

GatheredArrowBatch gatherToArrowIpc(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool,
    const TypePtr& returnType,
    std::optional<bool> asciiInputs,
    const TypePtr& canonicalIntermediateType) {
  const bool bridged = (returnType && containsBridgedType(returnType)) ||
      std::any_of(arguments.begin(), arguments.end(), [](const auto& arg) {
                         return containsBridgedType(arg->type());
                       });
  auto scope = bridged ? std::make_shared<WasmOpaqueScope>() : nullptr;
  auto batch = gatherToArrowRecordBatch(
      rows, arguments, pool, returnType != nullptr, scope);
  batch = canonicalizeIntermediateBatch(
      batch, arguments, canonicalIntermediateType);
  if (returnType) {
    std::unordered_map<std::string, std::string> metadata{
        {"velox.return_type", signatureType(returnType)}};
    if (asciiInputs.has_value()) {
      metadata["velox.scalar.ascii_inputs"] = *asciiInputs ? "true" : "false";
    }
    if (containsBridgedType(returnType)) {
      metadata["velox.return_wire_type"] =
          folly::toJson(wireDescription(returnType));
    }
    batch = batch->ReplaceSchemaMetadata(
        std::make_shared<arrow::KeyValueMetadata>(metadata));
  }
  return {
      .input = ArrowIpcInput(std::move(batch), std::move(scope)),
      .rowCount = rows.countSelected(),
  };
}

ArrowIpcInput makeScalarInitializationInput(
    const std::vector<exec::VectorFunctionArg>& arguments,
    const TypePtr& returnType,
    const std::unordered_map<std::string, std::string>& config,
    memory::MemoryPool* pool,
    bool rowApi,
    const TypePtr& intermediateType,
    const std::vector<TypePtr>& lambdaTypes,
    uint64_t maxLambdaRows,
    std::optional<bool> rawAggregateInputTypes) {
  std::vector<VectorPtr> constants;
  std::string mask;
  std::unordered_map<std::string, std::string> metadata;
  metadata["velox.return_type"] = signatureType(returnType);
  if (rawAggregateInputTypes.has_value()) {
    metadata["velox.aggregate.input_types"] =
        *rawAggregateInputTypes ? "raw" : "intermediate";
    if (!*rawAggregateInputTypes) {
      VELOX_CHECK(intermediateType && arguments.size() == 1);
      VELOX_CHECK(
          typesMatchExceptRowNames(arguments[0].type, intermediateType));
      metadata["velox.aggregate.intermediate_constant"] =
          arguments[0].constantValue ? "true" : "false";
    }
  }
  if (intermediateType) {
    metadata["velox.intermediate_type"] = signatureType(intermediateType);
    if (containsBridgedType(intermediateType)) {
      metadata["velox.intermediate_wire_type"] =
          folly::toJson(wireDescription(intermediateType));
    }
  }
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& arg = arguments[i];
    const auto& inputType =
        rawAggregateInputTypes == false ? intermediateType : arg.type;
    metadata["velox.argument_type." + std::to_string(i)] =
        signatureType(inputType);
    // Raw constant indices must never reinterpret intermediate data as user
    // configuration. Keep intermediate constants in their separate channel.
    mask +=
        arg.constantValue && rawAggregateInputTypes.value_or(true) ? '1' : '0';
    auto value = BaseVector::create(inputType, 1, pool);
    if (arg.constantValue) {
      value->copy(arg.constantValue.get(), 0, 0, 1);
    } else {
      value->setNull(0, true);
    }
    constants.push_back(std::move(value));
  }
  metadata["velox.lambda.count"] = std::to_string(lambdaTypes.size());
  metadata["velox.lambda.max_rows"] = std::to_string(
      std::min<uint64_t>(
          maxLambdaRows, std::numeric_limits<vector_size_t>::max()));
  for (size_t i = 0; i < lambdaTypes.size(); ++i) {
    const auto& function = lambdaTypes[i];
    metadata["velox.lambda.types_bound." + std::to_string(i)] =
        function ? "true" : "false";
    if (!function) {
      continue;
    }
    VELOX_CHECK(function->isFunction() && function->size() > 0);
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    for (size_t j = 0; j + 1 < function->size(); ++j) {
      names.push_back("arg" + std::to_string(j));
      types.push_back(function->childAt(j));
    }
    for (const auto& [kind, type] :
         std::vector<std::pair<std::string, TypePtr>>{
             {"input", ROW(std::move(names), std::move(types))},
             {"output", function->childAt(function->size() - 1)}}) {
      const auto key = "velox.lambda." + kind + "." + std::to_string(i);
      metadata[key] = signatureType(type);
      if (containsBridgedType(type))
        metadata[key + ".wire"] = folly::toJson(wireDescription(type));
    }
  }
  metadata["velox.constant_arguments"] = std::move(mask);
  for (const auto& [key, value] : config) {
    metadata["velox.config." + key] = value;
  }
  auto scope = std::make_shared<WasmOpaqueScope>();
  if (containsBridgedType(returnType)) {
    metadata["velox.return_wire_type"] =
        folly::toJson(wireDescription(returnType));
  }
  auto batch = gatherToArrowRecordBatch(
      SelectivityVector(1), constants, pool, rowApi, scope);
  return ArrowIpcInput(
      batch->ReplaceSchemaMetadata(
          std::make_shared<arrow::KeyValueMetadata>(metadata)),
      std::move(scope));
}

GatheredArrowBatch gatherToArrowIpcWithStateHandles(
    const SelectivityVector& rows,
    const std::vector<uint32_t>& stateHandles,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool,
    bool rowApi,
    const TypePtr& canonicalIntermediateType) {
  VELOX_CHECK_GE(
      stateHandles.size(), rows.end(), "Missing Wasm UDAF state handles");
  auto scope = rowApi ? std::make_shared<WasmOpaqueScope>() : nullptr;
  auto batch = gatherToArrowRecordBatch(rows, arguments, pool, rowApi, scope);
  batch = canonicalizeIntermediateBatch(
      batch, arguments, canonicalIntermediateType);

  arrow::UInt32Builder builder;
  checkArrow(builder.Reserve(rows.countSelected()), "state handle reserve");
  rows.applyToSelected([&](vector_size_t row) {
    VELOX_CHECK_NE(
        stateHandles[row], 0, "Wasm UDAF state handle must be non-zero");
    checkArrow(builder.Append(stateHandles[row]), "state handle append");
  });
  auto handles = unwrapArrow(builder.Finish(), "state handle finish");
  auto field =
      arrow::field("__velox_wasm_state_handle", arrow::uint32(), false);
  batch = unwrapArrow(batch->AddColumn(0, field, handles), "state column add");
  return {
      .input = ArrowIpcInput(std::move(batch), std::move(scope)),
      .rowCount = rows.countSelected(),
  };
}

static VectorPtr decodeArrowIpcBuffer(
    std::shared_ptr<arrow::Buffer> buffer,
    const TypePtr& outputType,
    vector_size_t expectedRows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors,
    const std::shared_ptr<WasmOpaqueScope>& opaqueScope,
    bool retainStorage,
    const ArrowIpcDecodeOptions* decodeOptions = nullptr) {
  VELOX_CHECK_LE(
      buffer->size(), 64UL << 20, "Wasm IPC output exceeds size limit");
  auto arrowPool = std::make_shared<BoundedArrowPool>(pool);
  auto options = arrow::ipc::IpcReadOptions::Defaults();
  options.memory_pool = arrowPool.get();
  options.use_threads = false;
  options.max_recursion_depth = 64;
  auto input = std::make_shared<arrow::io::BufferReader>(buffer);
  auto messages = arrow::ipc::MessageReader::Open(input);
  auto schemaMessage =
      unwrapArrow(messages->ReadNextMessage(), "schema message read");
  VELOX_CHECK_NOT_NULL(
      schemaMessage, "Wasm UDF returned an empty Arrow IPC stream");
  VELOX_CHECK(
      schemaMessage->type() == arrow::ipc::MessageType::SCHEMA,
      "Wasm UDF result must begin with an Arrow schema");
  arrow::ipc::DictionaryMemo memo;
  auto declaredSchema =
      unwrapArrow(arrow::ipc::ReadSchema(*schemaMessage, &memo), "schema read");
  if (errors == nullptr) {
    VELOX_CHECK_EQ(
        declaredSchema->num_fields(),
        1,
        "Wasm UDF result must contain exactly one column");
  } else {
    VELOX_CHECK(
        declaredSchema->num_fields() == 1 || declaredSchema->num_fields() == 2,
        "Wasm scalar UDF result must contain one value column and an optional "
        "error column");
    if (declaredSchema->num_fields() == 2) {
      const auto& errorField = declaredSchema->field(1);
      VELOX_CHECK_EQ(
          errorField->name(),
          "__velox_wasm_error",
          "Wasm scalar UDF error column has the wrong name");
      VELOX_CHECK(
          errorField->type()->Equals(arrow::utf8()),
          "Wasm scalar UDF error column must be Arrow Utf8");
      VELOX_CHECK(
          errorField->nullable(),
          "Wasm scalar UDF error column must be nullable");
    }
  }
  // Reject unsupported/dictionary schemas before Arrow constructs their arrays.
  VELOX_CHECK(
      scalarWireSchemaValid(declaredSchema->field(0)->type(), outputType),
      "Invalid Wasm result wire schema");
  const bool safeMaps = containsArrowMap(declaredSchema->field(0)->type());
  auto readSchema = declaredSchema;
  if (safeMaps) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (const auto& field : declaredSchema->fields()) {
      fields.push_back(field->WithType(safeMapReadType(field->type())));
    }
    readSchema = arrow::schema(std::move(fields), declaredSchema->metadata());
  }
  auto message =
      unwrapArrow(messages->ReadNextMessage(), "record batch message read");
  VELOX_CHECK_NOT_NULL(message, "Wasm UDF returned no Arrow record batch");
  VELOX_CHECK(
      message->type() == arrow::ipc::MessageType::RECORD_BATCH,
      "Wasm UDF result must contain exactly one record batch");
  auto batch = unwrapArrow(
      arrow::ipc::ReadRecordBatch(*message, readSchema, nullptr, options),
      "record batch read");
  VELOX_CHECK_EQ(
      batch->num_rows(), expectedRows, "Wasm UDF returned the wrong row count");
  auto extra =
      unwrapArrow(messages->ReadNextMessage(), "trailing record batch read");
  VELOX_CHECK_NULL(
      extra, "Wasm UDF result must contain exactly one record batch");
  VELOX_CHECK_EQ(
      unwrapArrow(input->Tell(), "stream position read"),
      buffer->size(),
      "Wasm UDF result contains trailing bytes");
  if (safeMaps) {
    checkArrow(batch->ValidateFull(), "full result validation");
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (int i = 0; i < batch->num_columns(); ++i) {
      validateMapArrayData(
          batch->column(i)->data(), declaredSchema->field(i)->type());
      columns.push_back(
          arrow::MakeArray(withArrowType(
              batch->column(i)->data(), declaredSchema->field(i)->type())));
    }
    batch = arrow::RecordBatch::Make(
        declaredSchema, batch->num_rows(), std::move(columns));
  }

  checkArrow(batch->ValidateFull(), "full result validation");
  VELOX_CHECK(
      scalarWireSchemaValid(batch->column(0)->type(), outputType),
      "Invalid Wasm result wire schema");
  auto validateDimensions = [&](auto&& self,
                                const arrow::ArrayData& data) -> void {
    VELOX_CHECK_LE(
        data.length,
        std::numeric_limits<vector_size_t>::max(),
        "Wasm Arrow child exceeds Velox index range");
    VELOX_CHECK_LE(
        data.offset,
        std::numeric_limits<vector_size_t>::max(),
        "Wasm Arrow offset exceeds Velox index range");
    VELOX_CHECK_LE(
        data.offset + data.length,
        std::numeric_limits<vector_size_t>::max(),
        "Wasm Arrow slice exceeds Velox index range");
    for (const auto& child : data.child_data)
      self(self, *child);
  };
  for (const auto& column : batch->columns()) {
    validateDimensions(validateDimensions, *column->data());
  }
  ArrowSchema schema;
  ArrowArray array;
  checkArrow(arrow::ExportSchema(*batch->schema(), &schema), "schema export");
  checkArrow(arrow::ExportRecordBatch(*batch, &array), "record batch export");
  auto row = std::dynamic_pointer_cast<RowVector>(
      importFromArrowAsOwner(schema, array, pool));
  VELOX_CHECK_NOT_NULL(row, "Wasm UDF result is not an Arrow struct batch");
  auto result = row->childAt(0);
  if (errors != nullptr) {
    errors->clear();
    if (row->childrenSize() == 2) {
      errors->assign(expectedRows, std::nullopt);
      auto errorVector = row->childAt(1)->as<SimpleVector<StringView>>();
      VELOX_CHECK_NOT_NULL(
          errorVector, "Wasm scalar UDF error result is not varchar");
      for (vector_size_t index = 0; index < expectedRows; ++index) {
        if (!errorVector->isNullAt(index)) {
          VELOX_CHECK(
              result->isNullAt(index),
              "Wasm scalar UDF returned both a value and an error at compact "
              "row {}",
              index);
          (*errors)[index] = errorVector->valueAt(index).str();
        }
      }
    }
  }
  if (containsWireScalar(outputType)) {
    VELOX_CHECK(
        scalarWireSchemaValid(batch->column(0)->type(), outputType),
        "Invalid scalar wire schema");
    result = scalarWireVector(
        result, false, outputType, pool, opaqueScope, decodeOptions);
  }
  VELOX_CHECK(
      result->type()->equivalent(*outputType),
      "Wasm UDF returned type '{}', expected '{}'",
      result->type()->toString(),
      outputType->toString());
  if (retainStorage) {
    return result;
  }
  // Generic copies can retain StringViews into the Arrow IPC buffer. Deep-copy
  // variable-width leaves at every nesting level before that buffer is freed.
  auto owned = BaseVector::copy(*result, pool);
  ownVariableWidthValues(owned, pool);
  return owned;
}

VectorPtr decodeArrowIpcResult(
    std::string_view ipc,
    const TypePtr& outputType,
    vector_size_t rows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors,
    const std::shared_ptr<WasmOpaqueScope>& scope) {
  auto buffer = std::make_shared<arrow::Buffer>(
      reinterpret_cast<const uint8_t*>(ipc.data()), ipc.size());
  return decodeArrowIpcBuffer(
      std::move(buffer), outputType, rows, pool, errors, scope, false);
}
VectorPtr decodeOwnedArrowIpcResult(
    std::string ipc,
    const TypePtr& outputType,
    vector_size_t rows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors,
    const std::shared_ptr<WasmOpaqueScope>& scope,
    const ArrowIpcDecodeOptions& options) {
  const auto size = static_cast<int64_t>(ipc.size());
  auto ownerPool = pool->shared_from_this();
  if (size > 0)
    ownerPool->reportExternalAllocation(size);
  std::shared_ptr<arrow::Buffer> storage;
  try {
    storage = arrow::Buffer::FromString(std::move(ipc));
  } catch (...) {
    if (size > 0)
      ownerPool->reportExternalFree(size);
    throw;
  }
  auto ownedBuffer = std::shared_ptr<arrow::Buffer>(
      storage.get(), [storage, ownerPool, size](arrow::Buffer*) mutable {
        storage.reset();
        if (size > 0)
          ownerPool->reportExternalFree(size);
      });
  return decodeArrowIpcBuffer(
      std::move(ownedBuffer),
      outputType,
      rows,
      pool,
      errors,
      scope,
      true,
      &options);
}

void scatterArrowResult(
    const SelectivityVector& rows,
    const VectorPtr& compactResult,
    const TypePtr& outputType,
    memory::MemoryPool* pool,
    VectorPtr& result) {
  VELOX_CHECK_EQ(rows.countSelected(), compactResult->size());
  if (rows.isAllSelected()) {
    result = compactResult;
    return;
  }
  BaseVector::ensureWritable(rows, outputType, pool, result);
  auto sourceRows = allocateIndices(rows.end(), pool);
  auto* rawSourceRows = sourceRows->asMutable<vector_size_t>();
  vector_size_t compactRow = 0;
  rows.applyToSelected(
      [&](vector_size_t row) { rawSourceRows[row] = compactRow++; });
  result->copy(compactResult.get(), rows, rawSourceRows);
}

} // namespace facebook::velox::functions::wasm
