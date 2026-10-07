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

#include "velox/functions/wasm/TypeBridge.h"

#include <folly/json.h>
#include <atomic>
#include "velox/vector/FlatVector.h"

namespace facebook::velox::functions::wasm {
namespace {
std::mutex codecMutex;
std::vector<std::shared_ptr<const WasmTypeCodec>> codecs;
std::atomic<uint64_t> nextScope{1};
bool sameBridgeType(const TypePtr& a, const TypePtr& b) {
  if (a->kind() != b->kind()) {
    return false;
  }
  // OpaqueType::equivalent intentionally ignores C++ typeIndex; a capability
  // or codec must enforce it before handing an object to a typed callback.
  if (a->kind() == TypeKind::OPAQUE) {
    auto x = std::dynamic_pointer_cast<const OpaqueType>(a);
    auto y = std::dynamic_pointer_cast<const OpaqueType>(b);
    return x && y && x->typeIndex() == y->typeIndex() && a->equivalent(*b);
  }
  return a->equivalent(*b);
}

} // namespace

bool isBridgedType(const TypePtr& type) {
  if (type->isDate() || type->isDecimal()) {
    return false;
  }
  return type->kind() == TypeKind::OPAQUE ||
      std::string_view(type->name()) != TypeKindName::toName(type->kind());
}
std::string bridgeTypeIdentity(const TypePtr& type) {
  if (type->kind() == TypeKind::OPAQUE &&
      std::string_view(type->name()) == "OPAQUE") {
    auto opaque = std::dynamic_pointer_cast<const OpaqueType>(type);
    const auto& aliases = getOpaqueAliasByTypeIndex();
    auto it = aliases.find(opaque->typeIndex());
    VELOX_USER_CHECK(
        it != aliases.end(),
        "Wasm OPAQUE requires a registered stable type alias");
    return "opaque:" + it->second;
  }
  folly::json::serialization_opts options;
  options.sort_keys = true;
  return folly::json::serialize(type->serialize(), options);
}
void registerWasmTypeCodec(WasmTypeCodec codec) {
  VELOX_USER_CHECK_NOT_NULL(codec.type);
  VELOX_USER_CHECK(
      isBridgedType(codec.type), "Codec requires a custom or OPAQUE type");
  VELOX_USER_CHECK(
      !codec.id.empty() && codec.version > 0 && codec.encode && codec.decode,
      "Codec requires an ID, positive version, encoder and decoder");
  std::lock_guard<std::mutex> lock(codecMutex);
  for (const auto& existing : codecs) {
    VELOX_USER_CHECK(
        !sameBridgeType(existing->type, codec.type),
        "Wasm type codec already registered for {}",
        codec.type->toString());
  }
  codecs.push_back(std::make_shared<const WasmTypeCodec>(std::move(codec)));
}
std::shared_ptr<const WasmTypeCodec> findWasmTypeCodec(const TypePtr& type) {
  if (!isBridgedType(type)) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(codecMutex);
  for (const auto& codec : codecs) {
    if (sameBridgeType(codec->type, type)) {
      return codec;
    }
  }
  return nullptr;
}
void registerWasmOpaqueSerializationCodec(
    const OpaqueTypePtr& type,
    std::string id,
    uint32_t version) {
  auto serialize = type->getSerializeFunc();
  auto deserialize = type->getDeserializeFunc();
  registerWasmTypeCodec(
      {type,
       std::move(id),
       version,
       [serialize](const BaseVector& vector, vector_size_t row) {
         return serialize(
             vector.as<SimpleVector<std::shared_ptr<void>>>()->valueAt(row));
       },
       [deserialize](
           std::string_view bytes, BaseVector& vector, vector_size_t row) {
         auto object = deserialize(std::string(bytes));
         VELOX_USER_CHECK_NOT_NULL(
             object, "OPAQUE decoder returned a null object");
         vector.as<FlatVector<std::shared_ptr<void>>>()->set(
             row, std::move(object));
       }});
}
WasmOpaqueScope::WasmOpaqueScope() : token_(nextScope.fetch_add(1)) {
  VELOX_CHECK_NE(token_, 0, "OPAQUE scope ID exhausted");
}
uint64_t WasmOpaqueScope::add(
    const TypePtr& type,
    std::shared_ptr<void> object) {
  VELOX_USER_CHECK_NOT_NULL(object, "Non-null OPAQUE value has no object");
  entries_.push_back({type, std::move(object)});
  return entries_.size();
}
std::shared_ptr<void> WasmOpaqueScope::resolve(
    uint64_t token,
    uint64_t handle,
    const TypePtr& type) const {
  VELOX_USER_CHECK(
      token == token_ && handle > 0 && handle <= entries_.size(),
      "Invalid or expired Wasm OPAQUE handle");
  const auto& entry = entries_[handle - 1];
  VELOX_USER_CHECK(
      sameBridgeType(entry.type, type), "Wasm OPAQUE handle type mismatch");
  return entry.object;
}
} // namespace facebook::velox::functions::wasm
