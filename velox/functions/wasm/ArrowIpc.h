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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "velox/expression/VectorFunction.h"

namespace arrow {
class RecordBatch;
}

namespace facebook::velox::functions::wasm {

class WasmOpaqueScope;

class ArrowIpcInput final {
 public:
  explicit ArrowIpcInput(
      std::shared_ptr<arrow::RecordBatch> batch,
      std::shared_ptr<WasmOpaqueScope> opaqueScope = nullptr);
  ~ArrowIpcInput();

  uint32_t size() const;
  void write(uint8_t* destination, uint32_t capacity) const;
  std::shared_ptr<WasmOpaqueScope> opaqueScope() const;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

struct GatheredArrowBatch {
  ArrowIpcInput input;
  vector_size_t rowCount;
};

GatheredArrowBatch gatherToArrowIpc(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool,
    const TypePtr& returnType = nullptr,
    std::optional<bool> asciiInputs = std::nullopt,
    const TypePtr& canonicalIntermediateType = nullptr);

// Null lambda type entries preserve slots erased by an independent merge
// signature. They cannot be evaluated and are explicitly marked in metadata.
ArrowIpcInput makeScalarInitializationInput(
    const std::vector<exec::VectorFunctionArg>& arguments,
    const TypePtr& returnType,
    const std::unordered_map<std::string, std::string>& config,
    memory::MemoryPool* pool,
    bool rowApi = false,
    const TypePtr& intermediateType = nullptr,
    const std::vector<TypePtr>& lambdaTypes = {},
    uint64_t maxLambdaRows = 65'536,
    std::optional<bool> rawAggregateInputTypes = std::nullopt);

GatheredArrowBatch gatherToArrowIpcWithStateHandles(
    const SelectivityVector& rows,
    const std::vector<uint32_t>& stateHandles,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool,
    bool rowApi = false,
    const TypePtr& canonicalIntermediateType = nullptr);

VectorPtr decodeArrowIpcResult(
    std::string_view ipc,
    const TypePtr& outputType,
    vector_size_t expectedRows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors = nullptr,
    const std::shared_ptr<WasmOpaqueScope>& opaqueScope = nullptr);

// Deferred codec decoding is column-granular: the first load decodes all rows
// so cached UDF results remain safe for subsequent consumers of other rows.
// Wire/schema/handle validation remains eager. The failure callback must not
// throw and must safely outlive the expression (e.g. capture a weak Store).
struct ArrowIpcDecodeOptions {
  bool lazyCodecs{false};
  std::function<void()> onDeferredCodecFailure;
};

// Transfers IPC storage to the Arrow owner; imported vectors retain it.
// Avoids deep-copying buffers that are already owned by the host result.
VectorPtr decodeOwnedArrowIpcResult(
    std::string ipc,
    const TypePtr& outputType,
    vector_size_t expectedRows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors = nullptr,
    const std::shared_ptr<WasmOpaqueScope>& opaqueScope = nullptr,
    const ArrowIpcDecodeOptions& options = {});

void scatterArrowResult(
    const SelectivityVector& rows,
    const VectorPtr& compactResult,
    const TypePtr& outputType,
    memory::MemoryPool* pool,
    VectorPtr& result);

} // namespace facebook::velox::functions::wasm
