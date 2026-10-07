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

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "velox/common/memory/MemoryPool.h"

#include <wasmtime.h>
#include "velox/buffer/Buffer.h"

namespace facebook::velox::functions::wasm {

class ArrowIpcInput;
struct WasmMemoryAccounting;

// Deployment policy, captured by the registered factories. Limits apply to
// each Store; module compilation still needs a process-level deployment budget.
struct WasmOptions {
  uint64_t memoryLimitBytes{64UL << 20};
  uint64_t fuelPerCall{100'000'000};
  uint64_t tableElements{10'000};
  uint64_t tables{1};
  uint64_t maxInputBytes{64UL << 20};
  uint64_t maxOutputBytes{64UL << 20};
  uint64_t maxModuleBytes{64UL << 20};
  uint64_t maxCallMillis{1'000};
  uint64_t maxLambdaCalls{1'000'000};
  uint64_t maxLambdaRowsPerCall{65'536};
  uint64_t maxLambdaEvaluations{1'000'000};
};

// Captures the current Driver's task token, or an empty check outside
// execution.
std::function<bool()> currentWasmCancellationCheck();

class WasmEngine final {
 public:
  WasmEngine();
  ~WasmEngine();

  WasmEngine(const WasmEngine&) = delete;
  WasmEngine& operator=(const WasmEngine&) = delete;

  wasm_engine_t* get() const {
    return engine_;
  }

 private:
  wasm_engine_t* engine_{nullptr};
  std::jthread epochTicker_;
};

class WasmModule final {
 public:
  static std::shared_ptr<WasmModule> compile(const std::filesystem::path& path);
  static std::string read(
      const std::filesystem::path& path,
      uint64_t maxBytes = 64UL << 20);
  static std::shared_ptr<WasmModule> compile(
      const std::filesystem::path& path,
      std::string_view bytes);

  ~WasmModule();

  WasmModule(const WasmModule&) = delete;
  WasmModule& operator=(const WasmModule&) = delete;

 private:
  friend class WasmInstance;

  WasmModule(
      std::shared_ptr<WasmEngine> engine,
      wasmtime_module_t* module,
      std::filesystem::path path);

  std::shared_ptr<WasmEngine> engine_;
  wasmtime_module_t* module_;
  std::filesystem::path path_;
  // Exact allowlisted function imports in module declaration order.
  enum class LambdaImport { kCall, kCallBatch, kResult };
  std::vector<LambdaImport> lambdaImports_;
};

class WasmInstance final {
 public:
  WasmInstance(
      std::shared_ptr<WasmModule> module,
      std::string entrypoint,
      uint64_t memoryLimitBytes = 64UL << 20,
      uint64_t fuelPerCall = 100'000'000);
  WasmInstance(
      std::shared_ptr<WasmModule> module,
      std::string entrypoint,
      const WasmOptions& options,
      memory::MemoryPool* pool = nullptr,
      std::function<bool()> cancelled = {});
  WasmInstance(
      std::shared_ptr<WasmModule> module,
      std::string countEntrypoint,
      std::vector<std::string> batchEntrypoints,
      std::vector<std::string> singleGroupEntrypoints,
      uint64_t memoryLimitBytes = 64UL << 20,
      uint64_t fuelPerCall = 100'000'000);
  WasmInstance(
      std::shared_ptr<WasmModule> module,
      std::string countEntrypoint,
      std::vector<std::string> batchEntrypoints,
      std::vector<std::string> singleGroupEntrypoints,
      const WasmOptions& options,
      memory::MemoryPool* pool = nullptr,
      std::function<bool()> cancelled = {});
  ~WasmInstance();

  WasmInstance(const WasmInstance&) = delete;
  WasmInstance& operator=(const WasmInstance&) = delete;

  // Adds another (ptr, len) -> v128 export before the instance is used.
  void addBatchEntrypoint(const std::string& name);
  void addCountEntrypoint(const std::string& name);
  void addSingleGroupEntrypoint(const std::string& name);
  // Permanently discard a compromised or failed Store. Safe during teardown.
  void invalidate() noexcept;
  void setMemoryPool(memory::MemoryPool* pool);
  uint64_t linearMemoryBytes() const;
  void setCancellationCheck(std::function<bool()> cancelled);
  using LambdaCallback =
      std::function<ArrowIpcInput(uint32_t, std::string_view)>;
  using LambdaBatchCallback =
      std::function<ArrowIpcInput(uint32_t, std::string_view, uint32_t)>;
  // Only a bound native aggregate supplies this authority. Scalar/start calls
  // may instantiate a shared module but cannot evaluate aggregate lambdas.
  void setLambdaCallback(LambdaCallback callback);
  void setLambdaCallback(LambdaBatchCallback callback);

  std::string invoke(const ArrowIpcInput& input);
  std::string invoke(std::string_view entrypoint, const ArrowIpcInput& input);
  std::string invokeSingleGroup(
      std::string_view entrypoint,
      uint32_t stateHandle,
      const ArrowIpcInput& input);
  std::string invokeCount(std::string_view entrypoint, uint32_t count);
  BufferPtr invokeCountBuffer(std::string_view entrypoint, uint32_t count);
  std::string invokeBytes(
      std::string_view entrypoint,
      std::string_view bytes,
      uint32_t prefix);

 private:
  // Each guest call, including instantiation and allocator calls, gets a fresh
  // finite budget. Cleanup must also be able to run after a fuel exhaustion.
  void resetFuel();
  void beginInvocation();
  void invalidateUnlocked() noexcept;
  void instantiate(wasm_trap_t** trap, wasmtime_error_t** error);
  void checkHostFailure(wasmtime_error_t* error, wasm_trap_t* trap);
  void checkLambdaDeadline() const;
  static wasm_trap_t* lambdaCall(
      void*,
      wasmtime_caller_t*,
      const wasmtime_val_t*,
      size_t,
      wasmtime_val_t*,
      size_t) noexcept;
  static wasm_trap_t* lambdaResult(
      void*,
      wasmtime_caller_t*,
      const wasmtime_val_t*,
      size_t,
      wasmtime_val_t*,
      size_t) noexcept;
  static wasmtime_error_t* checkEpoch(
      wasmtime_context_t*,
      void*,
      uint64_t*,
      wasmtime_update_deadline_kind_t*);
  uint32_t allocate(uint32_t size);
  void free(uint32_t pointer, uint32_t size);
  void validateRange(uint32_t pointer, uint32_t size) const;
  std::string invokeInput(
      std::string_view entrypoint,
      const ArrowIpcInput& input,
      const uint32_t* stateHandle);
  std::string invokeWrittenInput(
      std::string_view entrypoint,
      uint32_t inputSize,
      const std::function<void(uint8_t*, uint32_t)>& write,
      const uint32_t* prefix);
  std::string copyResult(
      std::string_view entrypoint,
      const wasmtime_val_t& result,
      uint32_t inputPointer,
      uint32_t inputSize,
      BufferPtr* owned = nullptr);
  const wasmtime_func_t& function(std::string_view name) const;

  std::shared_ptr<WasmModule> module_;
  std::string entrypointName_;
  uint64_t fuelPerCall_;
  WasmOptions options_;
  std::chrono::steady_clock::time_point deadline_;
  std::function<bool()> cancelled_;
  std::shared_ptr<memory::MemoryPool> pool_;
  std::shared_ptr<WasmMemoryAccounting> memoryAccounting_;
  wasmtime_store_t* store_{nullptr};
  wasmtime_context_t* context_{nullptr};
  wasmtime_instance_t instance_{};
  wasmtime_memory_t memory_{};
  wasmtime_func_t alloc_{};
  wasmtime_func_t free_{};
  wasmtime_func_t entrypoint_{};
  std::unordered_map<std::string, wasmtime_func_t> functions_;
  mutable std::mutex mutex_;
  LambdaBatchCallback lambdaCallback_;
  std::exception_ptr hostFailure_;
  BufferPtr lambdaOutput_;
  uint32_t lambdaToken_{0};
  uint64_t lambdaCalls_{0};
  uint64_t lambdaEvaluations_{0};
};

} // namespace facebook::velox::functions::wasm
