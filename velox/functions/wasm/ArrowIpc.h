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
#include <memory>
#include <optional>
#include <string>

#include "velox/expression/VectorFunction.h"

namespace arrow {
class RecordBatch;
}

namespace facebook::velox::functions::wasm {

class ArrowIpcInput final {
 public:
  explicit ArrowIpcInput(std::shared_ptr<arrow::RecordBatch> batch);
  ~ArrowIpcInput();

  uint32_t size() const;
  void write(uint8_t* destination, uint32_t capacity) const;

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
    memory::MemoryPool* pool);

GatheredArrowBatch gatherToArrowIpcWithStateHandles(
    const SelectivityVector& rows,
    const std::vector<uint32_t>& stateHandles,
    const std::vector<VectorPtr>& arguments,
    memory::MemoryPool* pool);

VectorPtr decodeArrowIpcResult(
    std::string_view ipc,
    const TypePtr& outputType,
    vector_size_t expectedRows,
    memory::MemoryPool* pool,
    std::vector<std::optional<std::string>>* errors = nullptr);

void scatterArrowResult(
    const SelectivityVector& rows,
    const VectorPtr& compactResult,
    const TypePtr& outputType,
    memory::MemoryPool* pool,
    VectorPtr& result);

} // namespace facebook::velox::functions::wasm
