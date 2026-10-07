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

#pragma once

#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "velox/vector/BaseVector.h"

namespace facebook::velox::functions::wasm {

// Register before executing queries. Callbacks operate on non-null flat values.
// The codec ID/version identify a portable byte format implemented by the
// guest.
struct WasmTypeCodec {
  TypePtr type;
  std::string id;
  uint32_t version{1};
  std::function<std::string(const BaseVector&, vector_size_t)> encode;
  std::function<void(std::string_view, BaseVector&, vector_size_t)> decode;
};
void registerWasmTypeCodec(WasmTypeCodec codec);
std::shared_ptr<const WasmTypeCodec> findWasmTypeCodec(const TypePtr& type);
void registerWasmOpaqueSerializationCodec(
    const OpaqueTypePtr& type,
    std::string id,
    uint32_t version = 1);
bool isBridgedType(const TypePtr& type);
std::string bridgeTypeIdentity(const TypePtr& type);

// Invocation-scoped capabilities. Results retain shared ownership of resolved
// objects, but a guest cannot dereference a handle or reuse it in another call.
class WasmOpaqueScope {
 public:
  WasmOpaqueScope();
  uint64_t token() const {
    return token_;
  }
  uint64_t add(const TypePtr& type, std::shared_ptr<void> object);
  std::shared_ptr<void>
  resolve(uint64_t token, uint64_t handle, const TypePtr& type) const;

 private:
  struct Entry {
    TypePtr type;
    std::shared_ptr<void> object;
  };
  uint64_t token_;
  std::vector<Entry> entries_;
};

} // namespace facebook::velox::functions::wasm
