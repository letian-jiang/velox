/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <wasmtime.h>
#include <exception>
#include <memory>
#include "velox/common/memory/MemoryPool.h"

namespace facebook::velox::functions::wasm {

// Owned by the Store and its linear-memory callback. All accesses take place
// inside the calling WasmInstance's lock (including destruction/late
// attachment). Virtual address reservations/guard pages do not become
// accessible allocations.
struct WasmMemoryAccounting {
  explicit WasmMemoryAccounting(uint64_t limit) : limit(limit) {}
  void setPool(memory::MemoryPool* newPool);
  void allocate(uint64_t bytes);
  void free(uint64_t bytes) noexcept;
  void checkFailure(wasmtime_error_t* error, wasm_trap_t* trap) const;

  const uint64_t limit;
  uint64_t bytes{0};
  std::shared_ptr<memory::MemoryPool> pool;
  std::exception_ptr failure;
};

// Wasmtime's engine-wide creator has no Store argument. Only instantiation
// needs this thread-local scope; each created memory captures its own owner for
// all subsequent growth, including growth on a different Driver thread.
class WasmMemoryScope {
 public:
  explicit WasmMemoryScope(std::shared_ptr<WasmMemoryAccounting> accounting);
  ~WasmMemoryScope();
  WasmMemoryScope(const WasmMemoryScope&) = delete;
  WasmMemoryScope& operator=(const WasmMemoryScope&) = delete;

 private:
  std::shared_ptr<WasmMemoryAccounting> previous_;
};

void configureWasmLinearMemory(wasm_config_t* config);

} // namespace facebook::velox::functions::wasm
