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

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <wasmtime.h>

namespace facebook::velox::functions::wasm {

class ArrowIpcInput;

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
};

class WasmModule final {
 public:
  static std::shared_ptr<WasmModule> compile(const std::filesystem::path& path);

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
};

class WasmInstance final {
 public:
  WasmInstance(
      std::shared_ptr<WasmModule> module,
      std::string entrypoint,
      uint64_t memoryLimitBytes = 64UL << 20);
  WasmInstance(
      std::shared_ptr<WasmModule> module,
      std::string countEntrypoint,
      std::vector<std::string> batchEntrypoints,
      std::vector<std::string> singleGroupEntrypoints,
      uint64_t memoryLimitBytes = 64UL << 20);
  ~WasmInstance();

  WasmInstance(const WasmInstance&) = delete;
  WasmInstance& operator=(const WasmInstance&) = delete;

  std::string invoke(const ArrowIpcInput& input);
  std::string invoke(std::string_view entrypoint, const ArrowIpcInput& input);
  std::string invokeSingleGroup(
      std::string_view entrypoint,
      uint32_t stateHandle,
      const ArrowIpcInput& input);
  std::string invokeCount(std::string_view entrypoint, uint32_t count);

 private:
  uint32_t allocate(uint32_t size);
  void free(uint32_t pointer, uint32_t size);
  void validateRange(uint32_t pointer, uint32_t size) const;
  std::string invokeInput(
      std::string_view entrypoint,
      const ArrowIpcInput& input,
      const uint32_t* stateHandle);
  std::string copyResult(
      std::string_view entrypoint,
      const wasmtime_val_t& result,
      uint32_t inputPointer,
      uint32_t inputSize);
  const wasmtime_func_t& function(std::string_view name) const;

  std::shared_ptr<WasmModule> module_;
  std::string entrypointName_;
  wasmtime_store_t* store_{nullptr};
  wasmtime_context_t* context_{nullptr};
  wasmtime_instance_t instance_{};
  wasmtime_memory_t memory_{};
  wasmtime_func_t alloc_{};
  wasmtime_func_t free_{};
  wasmtime_func_t entrypoint_{};
  std::unordered_map<std::string, wasmtime_func_t> functions_;
  std::mutex mutex_;
};

} // namespace facebook::velox::functions::wasm
