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

#include "velox/functions/wasm/Runtime.h"

#include <array>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <vector>

#include "velox/common/base/Exceptions.h"
#include "velox/functions/wasm/Abi.h"
#include "velox/functions/wasm/ArrowIpc.h"

namespace facebook::velox::functions::wasm {
namespace {

std::shared_ptr<WasmEngine> sharedEngine() {
  static auto engine = std::make_shared<WasmEngine>();
  return engine;
}

std::mutex& moduleCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, std::weak_ptr<WasmModule>>& moduleCache() {
  static std::unordered_map<std::string, std::weak_ptr<WasmModule>> cache;
  return cache;
}

std::filesystem::path canonicalWasmPath(const std::filesystem::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::canonical(path, error);
  VELOX_USER_CHECK(
      !error,
      "Cannot resolve Wasm module '{}': {}",
      path.string(),
      error.message());
  return canonical;
}

std::string moduleCacheKey(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  VELOX_USER_CHECK(
      !error,
      "Cannot inspect Wasm module '{}': {}",
      path.string(),
      error.message());
  const auto modified = std::filesystem::last_write_time(path, error);
  VELOX_USER_CHECK(
      !error,
      "Cannot inspect Wasm module '{}': {}",
      path.string(),
      error.message());
  return path.string() + ':' + std::to_string(size) + ':' +
      std::to_string(static_cast<int64_t>(modified.time_since_epoch().count()));
}

std::string byteVecToString(const wasm_byte_vec_t& bytes) {
  return std::string(bytes.data, bytes.size);
}

[[noreturn]] void throwWasmtimeError(
    const std::string& prefix,
    wasmtime_error_t* error,
    wasm_trap_t* trap) {
  std::string detail;
  if (error != nullptr) {
    wasm_byte_vec_t bytes;
    wasmtime_error_message(error, &bytes);
    detail = byteVecToString(bytes);
    wasm_byte_vec_delete(&bytes);
    wasmtime_error_delete(error);
  } else if (trap != nullptr) {
    wasm_byte_vec_t bytes;
    wasm_trap_message(trap, &bytes);
    detail = byteVecToString(bytes);
    wasm_byte_vec_delete(&bytes);
    wasm_trap_delete(trap);
  } else {
    detail = "unknown Wasmtime failure";
  }
  VELOX_USER_FAIL("{}: {}", prefix, detail);
}

void checkCall(
    const std::string& description,
    wasmtime_error_t* error,
    wasm_trap_t* trap) {
  if (error != nullptr || trap != nullptr) {
    throwWasmtimeError(description, error, trap);
  }
}

std::vector<uint8_t> readWasm(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  VELOX_USER_CHECK(input, "Cannot open Wasm module '{}'", path.string());
  const auto size = static_cast<int64_t>(input.tellg());
  VELOX_USER_CHECK_GE(size, 0, "Cannot read Wasm module '{}'", path.string());
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  input.seekg(0);
  input.read(
      reinterpret_cast<char*>(bytes.data()),
      static_cast<std::streamsize>(size));
  VELOX_USER_CHECK(input, "Cannot read Wasm module '{}'", path.string());
  return bytes;
}

wasmtime_extern_t getExport(
    wasmtime_context_t* context,
    const wasmtime_instance_t& instance,
    std::string_view name,
    wasmtime_extern_kind_t expectedKind) {
  wasmtime_extern_t item;
  VELOX_USER_CHECK(
      wasmtime_instance_export_get(
          context, &instance, name.data(), name.size(), &item),
      "Wasm module does not export '{}'",
      name);
  VELOX_USER_CHECK_EQ(
      item.kind, expectedKind, "Wasm export '{}' has the wrong kind", name);
  return item;
}

void validateFunctionType(
    wasmtime_context_t* context,
    const wasmtime_func_t& function,
    std::string_view name,
    std::initializer_list<wasm_valkind_t> parameters,
    std::initializer_list<wasm_valkind_t> results) {
  wasm_functype_t* type = wasmtime_func_type(context, &function);
  VELOX_CHECK_NOT_NULL(type);
  const auto* actualParameters = wasm_functype_params(type);
  const auto* actualResults = wasm_functype_results(type);
  bool matches = actualParameters->size == parameters.size() &&
      actualResults->size == results.size();
  if (matches) {
    size_t index = 0;
    for (const auto expected : parameters) {
      matches = matches &&
          wasm_valtype_kind(actualParameters->data[index++]) == expected;
    }
    index = 0;
    for (const auto expected : results) {
      matches = matches &&
          wasm_valtype_kind(actualResults->data[index++]) == expected;
    }
  }
  wasm_functype_delete(type);
  VELOX_USER_CHECK(
      matches, "Wasm export '{}' has an invalid ABI signature", name);
}

uint32_t readLane(const wasmtime_v128& value, size_t lane) {
  const auto offset = lane * sizeof(uint32_t);
  return static_cast<uint32_t>(value[offset]) |
      (static_cast<uint32_t>(value[offset + 1]) << 8) |
      (static_cast<uint32_t>(value[offset + 2]) << 16) |
      (static_cast<uint32_t>(value[offset + 3]) << 24);
}

} // namespace

WasmEngine::WasmEngine() {
  auto* config = wasm_config_new();
  VELOX_CHECK_NOT_NULL(config);
  wasmtime_config_wasm_simd_set(config, true);
  wasmtime_config_max_wasm_stack_set(config, 2UL << 20);
  engine_ = wasm_engine_new_with_config(config);
  VELOX_CHECK_NOT_NULL(engine_);
}

WasmEngine::~WasmEngine() {
  if (engine_ != nullptr) {
    wasm_engine_delete(engine_);
  }
}

WasmModule::WasmModule(
    std::shared_ptr<WasmEngine> engine,
    wasmtime_module_t* module,
    std::filesystem::path path)
    : engine_(std::move(engine)), module_(module), path_(std::move(path)) {}

std::shared_ptr<WasmModule> WasmModule::compile(
    const std::filesystem::path& inputPath) {
  const auto path = canonicalWasmPath(inputPath);
  const auto cacheKey = moduleCacheKey(path);
  std::lock_guard<std::mutex> lock(moduleCacheMutex());
  if (auto cached = moduleCache()[cacheKey].lock()) {
    return cached;
  }

  auto engine = sharedEngine();
  auto bytes = readWasm(path);
  wasmtime_module_t* module = nullptr;
  if (auto* error = wasmtime_module_new(
          engine->get(), bytes.data(), bytes.size(), &module)) {
    throwWasmtimeError(
        "Cannot compile Wasm module '" + path.string() + "'", error, nullptr);
  }

  wasm_importtype_vec_t imports;
  wasmtime_module_imports(module, &imports);
  const auto importCount = imports.size;
  wasm_importtype_vec_delete(&imports);
  if (importCount != 0) {
    wasmtime_module_delete(module);
    VELOX_USER_FAIL(
        "Wasm UDF module '{}' imports {} capabilities; imports are disabled",
        path.string(),
        importCount);
  }
  auto compiled = std::shared_ptr<WasmModule>(
      new WasmModule(std::move(engine), module, path));
  moduleCache()[cacheKey] = compiled;
  return compiled;
}

WasmModule::~WasmModule() {
  if (module_ != nullptr) {
    wasmtime_module_delete(module_);
  }
}

WasmInstance::WasmInstance(
    std::shared_ptr<WasmModule> module,
    std::string entrypoint,
    uint64_t memoryLimitBytes)
    : module_(std::move(module)), entrypointName_(std::move(entrypoint)) {
  VELOX_USER_CHECK_LE(
      memoryLimitBytes,
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
      "Wasm memory limit is too large");
  store_ = wasmtime_store_new(module_->engine_->get(), nullptr, nullptr);
  VELOX_CHECK_NOT_NULL(store_);
  try {
    context_ = wasmtime_store_context(store_);
    wasmtime_store_limiter(
        store_, static_cast<int64_t>(memoryLimitBytes), -1, 1, -1, 1);

    wasm_trap_t* trap = nullptr;
    auto* error = wasmtime_instance_new(
        context_, module_->module_, nullptr, 0, &instance_, &trap);
    checkCall("Cannot instantiate Wasm UDF module", error, trap);

    const auto memory =
        getExport(context_, instance_, kMemoryExport, WASMTIME_EXTERN_MEMORY);
    memory_ = memory.of.memory;
    alloc_ = getExport(context_, instance_, kAllocExport, WASMTIME_EXTERN_FUNC)
                 .of.func;
    free_ = getExport(context_, instance_, kFreeExport, WASMTIME_EXTERN_FUNC)
                .of.func;
    entrypoint_ =
        getExport(context_, instance_, entrypointName_, WASMTIME_EXTERN_FUNC)
            .of.func;
    functions_.emplace(entrypointName_, entrypoint_);

    validateFunctionType(
        context_, alloc_, kAllocExport, {WASM_I32}, {WASM_I32});
    validateFunctionType(
        context_, free_, kFreeExport, {WASM_I32, WASM_I32}, {});
    validateFunctionType(
        context_,
        entrypoint_,
        entrypointName_,
        {WASM_I32, WASM_I32},
        {WASMTIME_V128});
  } catch (...) {
    wasmtime_store_delete(store_);
    store_ = nullptr;
    context_ = nullptr;
    throw;
  }
}

WasmInstance::WasmInstance(
    std::shared_ptr<WasmModule> module,
    std::string countEntrypoint,
    std::vector<std::string> batchEntrypoints,
    std::vector<std::string> singleGroupEntrypoints,
    uint64_t memoryLimitBytes)
    : module_(std::move(module)), entrypointName_(std::move(countEntrypoint)) {
  VELOX_USER_CHECK_LE(
      memoryLimitBytes,
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
      "Wasm memory limit is too large");
  store_ = wasmtime_store_new(module_->engine_->get(), nullptr, nullptr);
  VELOX_CHECK_NOT_NULL(store_);
  try {
    context_ = wasmtime_store_context(store_);
    wasmtime_store_limiter(
        store_, static_cast<int64_t>(memoryLimitBytes), -1, 1, -1, 1);

    wasm_trap_t* trap = nullptr;
    auto* error = wasmtime_instance_new(
        context_, module_->module_, nullptr, 0, &instance_, &trap);
    checkCall("Cannot instantiate Wasm UDF module", error, trap);

    const auto memory =
        getExport(context_, instance_, kMemoryExport, WASMTIME_EXTERN_MEMORY);
    memory_ = memory.of.memory;
    alloc_ = getExport(context_, instance_, kAllocExport, WASMTIME_EXTERN_FUNC)
                 .of.func;
    free_ = getExport(context_, instance_, kFreeExport, WASMTIME_EXTERN_FUNC)
                .of.func;
    entrypoint_ =
        getExport(context_, instance_, entrypointName_, WASMTIME_EXTERN_FUNC)
            .of.func;
    functions_.emplace(entrypointName_, entrypoint_);

    validateFunctionType(
        context_, alloc_, kAllocExport, {WASM_I32}, {WASM_I32});
    validateFunctionType(
        context_, free_, kFreeExport, {WASM_I32, WASM_I32}, {});
    validateFunctionType(
        context_, entrypoint_, entrypointName_, {WASM_I32}, {WASMTIME_V128});
    for (auto& name : batchEntrypoints) {
      auto function =
          getExport(context_, instance_, name, WASMTIME_EXTERN_FUNC).of.func;
      validateFunctionType(
          context_, function, name, {WASM_I32, WASM_I32}, {WASMTIME_V128});
      VELOX_USER_CHECK(
          functions_.emplace(std::move(name), function).second,
          "Duplicate Wasm UDAF entrypoint");
    }
    for (auto& name : singleGroupEntrypoints) {
      auto function =
          getExport(context_, instance_, name, WASMTIME_EXTERN_FUNC).of.func;
      validateFunctionType(
          context_,
          function,
          name,
          {WASM_I32, WASM_I32, WASM_I32},
          {WASMTIME_V128});
      VELOX_USER_CHECK(
          functions_.emplace(std::move(name), function).second,
          "Duplicate Wasm UDAF entrypoint");
    }
  } catch (...) {
    wasmtime_store_delete(store_);
    store_ = nullptr;
    context_ = nullptr;
    throw;
  }
}

WasmInstance::~WasmInstance() {
  if (store_ != nullptr) {
    wasmtime_store_delete(store_);
  }
}

uint32_t WasmInstance::allocate(uint32_t size) {
  wasmtime_val_t argument{};
  argument.kind = WASMTIME_I32;
  argument.of.i32 = static_cast<int32_t>(size);
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  auto* error =
      wasmtime_func_call(context_, &alloc_, &argument, 1, &result, 1, &trap);
  checkCall("Wasm UDF allocator failed", error, trap);
  VELOX_USER_CHECK_EQ(
      result.kind, WASMTIME_I32, "Wasm allocator returned wrong type");
  const auto pointer = static_cast<uint32_t>(result.of.i32);
  validateRange(pointer, size);
  return pointer;
}

void WasmInstance::free(uint32_t pointer, uint32_t size) {
  if (size == 0) {
    return;
  }
  std::array<wasmtime_val_t, 2> arguments{};
  arguments[0].kind = WASMTIME_I32;
  arguments[0].of.i32 = static_cast<int32_t>(pointer);
  arguments[1].kind = WASMTIME_I32;
  arguments[1].of.i32 = static_cast<int32_t>(size);
  wasm_trap_t* trap = nullptr;
  auto* error = wasmtime_func_call(
      context_, &free_, arguments.data(), arguments.size(), nullptr, 0, &trap);
  checkCall("Wasm UDF deallocator failed", error, trap);
}

void WasmInstance::validateRange(uint32_t pointer, uint32_t size) const {
  const auto memorySize = wasmtime_memory_data_size(context_, &memory_);
  VELOX_USER_CHECK_LE(
      static_cast<uint64_t>(pointer) + size,
      memorySize,
      "Wasm UDF returned an out-of-bounds linear-memory range");
}

std::string WasmInstance::invoke(const ArrowIpcInput& input) {
  return invoke(entrypointName_, input);
}

const wasmtime_func_t& WasmInstance::function(std::string_view name) const {
  auto it = functions_.find(std::string(name));
  VELOX_USER_CHECK(
      it != functions_.end(), "Unknown Wasm UDF entrypoint '{}'", name);
  return it->second;
}

std::string WasmInstance::invoke(
    std::string_view entrypoint,
    const ArrowIpcInput& input) {
  return invokeInput(entrypoint, input, nullptr);
}

std::string WasmInstance::invokeSingleGroup(
    std::string_view entrypoint,
    uint32_t stateHandle,
    const ArrowIpcInput& input) {
  VELOX_USER_CHECK_NE(
      stateHandle, 0, "Wasm UDAF state handle must be non-zero");
  return invokeInput(entrypoint, input, &stateHandle);
}

std::string WasmInstance::invokeInput(
    std::string_view entrypoint,
    const ArrowIpcInput& input,
    const uint32_t* stateHandle) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto inputSize = input.size();
  const auto inputPointer = allocate(inputSize);
  try {
    // Allocation may grow linear memory, so acquire its base address only
    // after velox_wasm_alloc has returned.
    input.write(
        wasmtime_memory_data(context_, &memory_) + inputPointer, inputSize);
  } catch (...) {
    free(inputPointer, inputSize);
    throw;
  }

  std::array<wasmtime_val_t, 3> arguments{};
  const size_t inputOffset = stateHandle == nullptr ? 0 : 1;
  if (stateHandle != nullptr) {
    arguments[0].kind = WASMTIME_I32;
    arguments[0].of.i32 = static_cast<int32_t>(*stateHandle);
  }
  arguments[inputOffset].kind = WASMTIME_I32;
  arguments[inputOffset].of.i32 = static_cast<int32_t>(inputPointer);
  arguments[inputOffset + 1].kind = WASMTIME_I32;
  arguments[inputOffset + 1].of.i32 = static_cast<int32_t>(inputSize);
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  auto* error = wasmtime_func_call(
      context_,
      &function(entrypoint),
      arguments.data(),
      inputOffset + 2,
      &result,
      1,
      &trap);
  if (error != nullptr || trap != nullptr) {
    free(inputPointer, inputSize);
    checkCall("Wasm UDF invocation failed", error, trap);
  }
  return copyResult(entrypoint, result, inputPointer, inputSize);
}

std::string WasmInstance::invokeCount(
    std::string_view entrypoint,
    uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  wasmtime_val_t argument{};
  argument.kind = WASMTIME_I32;
  argument.of.i32 = static_cast<int32_t>(count);
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  auto* error = wasmtime_func_call(
      context_, &function(entrypoint), &argument, 1, &result, 1, &trap);
  checkCall("Wasm UDAF create invocation failed", error, trap);
  return copyResult(entrypoint, result, 0, 0);
}

std::string WasmInstance::copyResult(
    std::string_view entrypoint,
    const wasmtime_val_t& result,
    uint32_t inputPointer,
    uint32_t inputSize) {
  if (result.kind != WASMTIME_V128) {
    free(inputPointer, inputSize);
    VELOX_USER_FAIL("Wasm UDF returned wrong type");
  }

  const AbiResult abiResult{
      readLane(result.of.v128, 0),
      readLane(result.of.v128, 1),
      readLane(result.of.v128, 2),
      readLane(result.of.v128, 3)};
  const auto memorySize = wasmtime_memory_data_size(context_, &memory_);
  if (static_cast<uint64_t>(abiResult.dataPtr) + abiResult.dataLen >
      memorySize) {
    free(inputPointer, inputSize);
    VELOX_USER_FAIL("Wasm UDF returned an out-of-bounds linear-memory range");
  }
  const auto inputEnd = static_cast<uint64_t>(inputPointer) + inputSize;
  const auto outputEnd =
      static_cast<uint64_t>(abiResult.dataPtr) + abiResult.dataLen;
  const bool aliasesInput = inputSize != 0 && abiResult.dataLen != 0 &&
      inputPointer < outputEnd && abiResult.dataPtr < inputEnd;
  if (aliasesInput) {
    free(inputPointer, inputSize);
    VELOX_USER_FAIL("Wasm UDF output buffer aliases its input buffer");
  }
  std::string output;
  try {
    output.assign(
        reinterpret_cast<const char*>(
            wasmtime_memory_data(context_, &memory_) + abiResult.dataPtr),
        abiResult.dataLen);
  } catch (...) {
    free(abiResult.dataPtr, abiResult.dataLen);
    free(inputPointer, inputSize);
    throw;
  }
  free(abiResult.dataPtr, abiResult.dataLen);
  free(inputPointer, inputSize);

  VELOX_USER_CHECK_EQ(
      abiResult.reserved, 0, "Wasm UDF returned non-zero reserved ABI bits");
  VELOX_USER_CHECK_EQ(
      abiResult.status, 0, "Wasm UDF '{}' failed: {}", entrypoint, output);
  return output;
}

} // namespace facebook::velox::functions::wasm
