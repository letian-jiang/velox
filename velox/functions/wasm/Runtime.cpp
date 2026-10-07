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
#include "velox/functions/wasm/LinearMemory.h"

#include <folly/ScopeGuard.h>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <vector>

#include "velox/common/base/Exceptions.h"
#include "velox/common/base/Status.h"
#include "velox/functions/wasm/Abi.h"
#include "velox/functions/wasm/ArrowIpc.h"
#ifdef VELOX_WASM_HAS_EXEC
#include "velox/exec/Driver.h"
#include "velox/exec/Task.h"
#endif

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
  VELOX_CHECK(
      !error,
      "Cannot resolve Wasm module '{}': {}",
      path.string(),
      error.message());
  return canonical;
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
  VELOX_FAIL("{}: {}", prefix, detail);
}

void checkCall(
    const std::string& description,
    wasmtime_error_t* error,
    wasm_trap_t* trap) {
  if (error != nullptr || trap != nullptr) {
    throwWasmtimeError(description, error, trap);
  }
}

wasmtime_extern_t getExport(
    wasmtime_context_t* context,
    const wasmtime_instance_t& instance,
    std::string_view name,
    wasmtime_extern_kind_t expectedKind) {
  wasmtime_extern_t item;
  VELOX_CHECK(
      wasmtime_instance_export_get(
          context, &instance, name.data(), name.size(), &item),
      "Wasm module does not export '{}'",
      name);
  VELOX_CHECK_EQ(
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
  VELOX_CHECK(matches, "Wasm export '{}' has an invalid ABI signature", name);
}

uint32_t readLane(const wasmtime_v128& value, size_t lane) {
  const auto offset = lane * sizeof(uint32_t);
  return static_cast<uint32_t>(value[offset]) |
      (static_cast<uint32_t>(value[offset + 1]) << 8) |
      (static_cast<uint32_t>(value[offset + 2]) << 16) |
      (static_cast<uint32_t>(value[offset + 3]) << 24);
}

} // namespace

std::function<bool()> currentWasmCancellationCheck() {
#ifdef VELOX_WASM_HAS_EXEC
  if (auto* driver = exec::driverThreadContext()) {
    auto token = driver->driverCtx()->task->getCancellationToken();
    return [token]() { return token.isCancellationRequested(); };
  }
#endif
  return {};
}

WasmEngine::WasmEngine() {
  auto* config = wasm_config_new();
  VELOX_CHECK_NOT_NULL(config);
  wasmtime_config_wasm_simd_set(config, true);
  wasmtime_config_consume_fuel_set(config, true);
  wasmtime_config_epoch_interruption_set(config, true);
  wasmtime_config_max_wasm_stack_set(config, 2UL << 20);
  configureWasmLinearMemory(config);
  engine_ = wasm_engine_new_with_config(config);
  VELOX_CHECK_NOT_NULL(engine_);
  epochTicker_ = std::jthread([this](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      wasmtime_engine_increment_epoch(engine_);
    }
  });
}

WasmEngine::~WasmEngine() {
  epochTicker_.request_stop();
  if (epochTicker_.joinable())
    epochTicker_.join();
  if (engine_ != nullptr) {
    wasm_engine_delete(engine_);
  }
}

WasmModule::WasmModule(
    std::shared_ptr<WasmEngine> engine,
    wasmtime_module_t* module,
    std::filesystem::path path)
    : engine_(std::move(engine)), module_(module), path_(std::move(path)) {}

std::string WasmModule::read(
    const std::filesystem::path& path,
    uint64_t maxBytes) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  VELOX_CHECK(input, "Cannot open Wasm module '{}'", path.string());
  const auto size = static_cast<int64_t>(input.tellg());
  VELOX_CHECK_GE(size, 0, "Cannot read Wasm module");
  VELOX_CHECK_LE(
      static_cast<uint64_t>(size), maxBytes, "Wasm module exceeds size limit");
  std::string bytes(static_cast<size_t>(size), '\0');
  input.seekg(0);
  input.read(bytes.data(), size);
  VELOX_CHECK(input, "Cannot read Wasm module '{}'", path.string());
  return bytes;
}

std::shared_ptr<WasmModule> WasmModule::compile(
    const std::filesystem::path& inputPath) {
  auto path = canonicalWasmPath(inputPath);
  return compile(path, read(path));
}

std::shared_ptr<WasmModule> WasmModule::compile(
    const std::filesystem::path& path,
    std::string_view bytes) {
  // Equality compares the complete immutable contents, not a path/mtime or
  // a potentially colliding digest. Never hold the cache lock during JIT work.
  const std::string cacheKey(bytes);
  {
    std::lock_guard<std::mutex> lock(moduleCacheMutex());
    for (auto it = moduleCache().begin(); it != moduleCache().end();) {
      if (it->second.expired()) {
        it = moduleCache().erase(it);
      } else {
        ++it;
      }
    }
    if (auto it = moduleCache().find(cacheKey); it != moduleCache().end()) {
      if (auto cached = it->second.lock()) {
        return cached;
      }
    }
  }

  auto engine = sharedEngine();
  wasmtime_module_t* module = nullptr;
  if (auto* error = wasmtime_module_new(
          engine->get(),
          reinterpret_cast<const uint8_t*>(bytes.data()),
          bytes.size(),
          &module)) {
    throwWasmtimeError(
        "Cannot compile Wasm module '" + path.string() + "'", error, nullptr);
  }

  wasm_importtype_vec_t imports;
  wasmtime_module_imports(module, &imports);
  std::vector<WasmModule::LambdaImport> lambdaImports;
  bool valid = true;
  for (size_t i = 0; i < imports.size; ++i) {
    const auto* import = imports.data[i];
    const auto* space = wasm_importtype_module(import);
    const auto* name = wasm_importtype_name(import);
    const std::string_view namespaceName(space->data, space->size);
    const std::string_view functionName(name->data, name->size);
    const auto* external = wasm_importtype_type(import);
    const bool batchCall = functionName == "lambda_call_batch";
    const bool call = functionName == "lambda_call" || batchCall;
    valid &= namespaceName == "velox_udf_v1" &&
        (call || functionName == "lambda_result") &&
        wasm_externtype_kind(external) == WASM_EXTERN_FUNC;
    if (wasm_externtype_kind(external) == WASM_EXTERN_FUNC) {
      const auto* type = wasm_externtype_as_functype_const(external);
      const auto* params = wasm_functype_params(type);
      const auto* results = wasm_functype_results(type);
      valid &= params->size == (batchCall ? 4 : 3) && results->size == 1;
      for (size_t j = 0; j < params->size; ++j)
        valid &= wasm_valtype_kind(params->data[j]) == WASM_I32;
      if (results->size == 1)
        valid &=
            wasm_valtype_kind(results->data[0]) == (call ? WASM_I64 : WASM_I32);
    }
    lambdaImports.push_back(
        batchCall  ? WasmModule::LambdaImport::kCallBatch
            : call ? WasmModule::LambdaImport::kCall
                   : WasmModule::LambdaImport::kResult);
  }
  wasm_importtype_vec_delete(&imports);
  if (!valid) {
    wasmtime_module_delete(module);
    VELOX_FAIL(
        "Wasm UDF module '{}' has unsupported imports; imports are disabled except the typed velox_udf_v1 lambda protocol",
        path.string());
  }
  auto compiled = std::shared_ptr<WasmModule>(
      new WasmModule(std::move(engine), module, path));
  compiled->lambdaImports_ = std::move(lambdaImports);
  {
    std::lock_guard<std::mutex> lock(moduleCacheMutex());
    auto& entry = moduleCache()[cacheKey];
    if (auto cached = entry.lock()) {
      return cached;
    }
    entry = compiled;
  }
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
    uint64_t memoryLimitBytes,
    uint64_t fuelPerCall)
    : WasmInstance(
          std::move(module),
          std::move(entrypoint),
          WasmOptions{
              .memoryLimitBytes = memoryLimitBytes,
              .fuelPerCall = fuelPerCall}) {}

WasmInstance::WasmInstance(
    std::shared_ptr<WasmModule> module,
    std::string entrypoint,
    const WasmOptions& options,
    memory::MemoryPool* pool,
    std::function<bool()> cancelled)
    : module_(std::move(module)),
      entrypointName_(std::move(entrypoint)),
      fuelPerCall_(options.fuelPerCall),
      options_(options),
      cancelled_(std::move(cancelled)),
      memoryAccounting_(
          std::make_shared<WasmMemoryAccounting>(options.memoryLimitBytes)) {
  const auto memoryLimitBytes = options.memoryLimitBytes;
  VELOX_CHECK_LE(options.tableElements, static_cast<uint64_t>(INT64_MAX));
  VELOX_CHECK_LE(options.tables, static_cast<uint64_t>(INT64_MAX));
  VELOX_CHECK_GT(options.tables, 0);
  VELOX_CHECK_GT(options.memoryLimitBytes, 0);
  VELOX_CHECK_LE(options.maxInputBytes, UINT32_MAX);
  VELOX_CHECK_LE(options.maxOutputBytes, UINT32_MAX);
  VELOX_CHECK_GT(fuelPerCall_, 0, "Wasm fuel budget must be positive");
  VELOX_CHECK_LE(
      memoryLimitBytes,
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
      "Wasm memory limit is too large");
  if (pool != nullptr)
    setMemoryPool(pool);
  try {
    store_ = wasmtime_store_new(module_->engine_->get(), nullptr, nullptr);
    VELOX_CHECK_NOT_NULL(store_);
    context_ = wasmtime_store_context(store_);
    wasmtime_store_limiter(
        store_,
        static_cast<int64_t>(memoryLimitBytes),
        static_cast<int64_t>(options.tableElements),
        1,
        static_cast<int64_t>(options.tables),
        1);

    wasmtime_store_epoch_deadline_callback(store_, checkEpoch, this, nullptr);
    beginInvocation();
    resetFuel();
    wasm_trap_t* trap = nullptr;
    WasmMemoryScope memoryOwner(memoryAccounting_);
    wasmtime_error_t* error = nullptr;
    instantiate(&trap, &error);
    memoryAccounting_->checkFailure(error, trap);
    checkHostFailure(error, trap);
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
    invalidateUnlocked();
    throw;
  }
}

WasmInstance::WasmInstance(
    std::shared_ptr<WasmModule> module,
    std::string countEntrypoint,
    std::vector<std::string> batchEntrypoints,
    std::vector<std::string> singleGroupEntrypoints,
    uint64_t memoryLimitBytes,
    uint64_t fuelPerCall)
    : WasmInstance(
          std::move(module),
          std::move(countEntrypoint),
          std::move(batchEntrypoints),
          std::move(singleGroupEntrypoints),
          WasmOptions{
              .memoryLimitBytes = memoryLimitBytes,
              .fuelPerCall = fuelPerCall}) {}

WasmInstance::WasmInstance(
    std::shared_ptr<WasmModule> module,
    std::string countEntrypoint,
    std::vector<std::string> batchEntrypoints,
    std::vector<std::string> singleGroupEntrypoints,
    const WasmOptions& options,
    memory::MemoryPool* pool,
    std::function<bool()> cancelled)
    : module_(std::move(module)),
      entrypointName_(std::move(countEntrypoint)),
      fuelPerCall_(options.fuelPerCall),
      options_(options),
      cancelled_(std::move(cancelled)),
      memoryAccounting_(
          std::make_shared<WasmMemoryAccounting>(options.memoryLimitBytes)) {
  const auto memoryLimitBytes = options.memoryLimitBytes;
  VELOX_CHECK_LE(options.tableElements, static_cast<uint64_t>(INT64_MAX));
  VELOX_CHECK_LE(options.tables, static_cast<uint64_t>(INT64_MAX));
  VELOX_CHECK_GT(options.tables, 0);
  VELOX_CHECK_GT(options.memoryLimitBytes, 0);
  VELOX_CHECK_LE(options.maxInputBytes, UINT32_MAX);
  VELOX_CHECK_LE(options.maxOutputBytes, UINT32_MAX);
  VELOX_CHECK_GT(fuelPerCall_, 0, "Wasm fuel budget must be positive");
  VELOX_CHECK_LE(
      memoryLimitBytes,
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
      "Wasm memory limit is too large");
  if (pool != nullptr)
    setMemoryPool(pool);
  try {
    store_ = wasmtime_store_new(module_->engine_->get(), nullptr, nullptr);
    VELOX_CHECK_NOT_NULL(store_);
    context_ = wasmtime_store_context(store_);
    wasmtime_store_limiter(
        store_,
        static_cast<int64_t>(memoryLimitBytes),
        static_cast<int64_t>(options.tableElements),
        1,
        static_cast<int64_t>(options.tables),
        1);

    wasmtime_store_epoch_deadline_callback(store_, checkEpoch, this, nullptr);
    beginInvocation();
    resetFuel();
    wasm_trap_t* trap = nullptr;
    WasmMemoryScope memoryOwner(memoryAccounting_);
    wasmtime_error_t* error = nullptr;
    instantiate(&trap, &error);
    memoryAccounting_->checkFailure(error, trap);
    checkHostFailure(error, trap);
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
      VELOX_CHECK(
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
      VELOX_CHECK(
          functions_.emplace(std::move(name), function).second,
          "Duplicate Wasm UDAF entrypoint");
    }
  } catch (...) {
    invalidateUnlocked();
    throw;
  }
}

void WasmInstance::addBatchEntrypoint(const std::string& name) {
  auto function =
      getExport(context_, instance_, name, WASMTIME_EXTERN_FUNC).of.func;
  validateFunctionType(
      context_, function, name, {WASM_I32, WASM_I32}, {WASMTIME_V128});
  functions_.emplace(name, function);
}

void WasmInstance::instantiate(wasm_trap_t** trap, wasmtime_error_t** error) {
  std::vector<wasmtime_extern_t> imports;
  for (const auto import : module_->lambdaImports_) {
    const bool call = import != WasmModule::LambdaImport::kResult;
    wasm_valtype_vec_t parameters;
    wasm_valtype_vec_new_uninitialized(
        &parameters, import == WasmModule::LambdaImport::kCallBatch ? 4 : 3);
    for (size_t i = 0; i < parameters.size; ++i)
      parameters.data[i] = wasm_valtype_new_i32();
    wasm_valtype_t* resultType =
        call ? wasm_valtype_new_i64() : wasm_valtype_new_i32();
    wasm_valtype_vec_t result;
    wasm_valtype_vec_new(&result, 1, &resultType);
    auto* type = wasm_functype_new(&parameters, &result);
    wasmtime_func_t function;
    wasmtime_func_new(
        context_,
        type,
        call ? lambdaCall : lambdaResult,
        this,
        nullptr,
        &function);
    wasm_functype_delete(type);
    wasmtime_extern_t external{};
    external.kind = WASMTIME_EXTERN_FUNC;
    external.of.func = function;
    imports.push_back(external);
  }
  *error = wasmtime_instance_new(
      context_,
      module_->module_,
      imports.data(),
      imports.size(),
      &instance_,
      trap);
}

void WasmInstance::setLambdaCallback(LambdaCallback callback) {
  if (!callback) {
    setLambdaCallback(LambdaBatchCallback{});
    return;
  }
  setLambdaCallback(
      [callback = std::move(callback)](
          uint32_t index, std::string_view request, uint32_t rows) {
        VELOX_CHECK_EQ(
            rows, 1, "Single-row lambda provider cannot evaluate a batch");
        return callback(index, request);
      });
}

void WasmInstance::setLambdaCallback(LambdaBatchCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  VELOX_CHECK_NOT_NULL(store_, "Wasm Store is invalidated");
  lambdaCallback_ = std::move(callback);
}

void WasmInstance::checkHostFailure(
    wasmtime_error_t* error,
    wasm_trap_t* trap) {
  if (!hostFailure_)
    return;
  auto failure = std::exchange(hostFailure_, nullptr);
  if (error)
    wasmtime_error_delete(error);
  if (trap)
    wasm_trap_delete(trap);
  std::rethrow_exception(failure);
}

void WasmInstance::checkLambdaDeadline() const {
  VELOX_CHECK(!cancelled_ || !cancelled_(), "Wasm invocation cancelled");
  VELOX_CHECK(
      std::chrono::steady_clock::now() < deadline_,
      "Wasm invocation deadline exceeded");
}

wasm_trap_t* WasmInstance::lambdaCall(
    void* data,
    wasmtime_caller_t* caller,
    const wasmtime_val_t* args,
    size_t argumentCount,
    wasmtime_val_t* results,
    size_t) noexcept {
  auto* owner = static_cast<WasmInstance*>(data);
  try {
    owner->checkLambdaDeadline();
    VELOX_CHECK(
        owner->lambdaCallback_,
        "Wasm aggregate lambda capability is not bound");
    VELOX_CHECK(!owner->lambdaOutput_, "Wasm lambda result was not consumed");
    VELOX_CHECK_LT(
        owner->lambdaCalls_++,
        owner->options_.maxLambdaCalls,
        "Wasm lambda call limit exceeded");
    const auto rows =
        argumentCount == 4 ? static_cast<uint32_t>(args[3].of.i32) : 1;
    VELOX_CHECK_GT(rows, 0, "Wasm lambda batch must contain rows");
    VELOX_CHECK_LE(
        rows,
        owner->options_.maxLambdaRowsPerCall,
        "Wasm lambda batch exceeds row limit");
    VELOX_CHECK_LE(
        rows,
        owner->options_.maxLambdaEvaluations,
        "Wasm lambda evaluation limit exceeded");
    VELOX_CHECK_LE(
        owner->lambdaEvaluations_,
        owner->options_.maxLambdaEvaluations - rows,
        "Wasm lambda evaluation limit exceeded");
    owner->lambdaEvaluations_ += rows;
    VELOX_CHECK_LT(
        owner->lambdaToken_,
        UINT32_MAX,
        "Wasm lambda result token space exhausted");
    VELOX_CHECK_NOT_NULL(owner->pool_);
    wasmtime_extern_t memory;
    VELOX_CHECK(
        wasmtime_caller_export_get(caller, "memory", 6, &memory) &&
            memory.kind == WASMTIME_EXTERN_MEMORY,
        "Wasm lambda caller must export memory");
    auto* context = wasmtime_caller_context(caller);
    const auto pointer = static_cast<uint32_t>(args[1].of.i32);
    const auto size = static_cast<uint32_t>(args[2].of.i32);
    VELOX_CHECK_LE(
        size,
        owner->options_.maxInputBytes,
        "Wasm lambda request exceeds size limit");
    VELOX_CHECK_LE(
        static_cast<uint64_t>(pointer) + size,
        wasmtime_memory_data_size(context, &memory.of.memory),
        "Wasm lambda request is out of bounds");
    const auto output = owner->lambdaCallback_(
        static_cast<uint32_t>(args[0].of.i32),
        std::string_view(
            reinterpret_cast<const char*>(
                wasmtime_memory_data(context, &memory.of.memory) + pointer),
            size),
        rows);
    owner->checkLambdaDeadline();
    VELOX_CHECK_LE(
        output.size(),
        owner->options_.maxOutputBytes,
        "Wasm lambda response exceeds size limit");
    auto buffer =
        AlignedBuffer::allocate<uint8_t>(output.size(), owner->pool_.get());
    output.write(buffer->asMutable<uint8_t>(), output.size());
    owner->lambdaOutput_ = std::move(buffer);
    ++owner->lambdaToken_;
    results[0].kind = WASMTIME_I64;
    results[0].of.i64 = static_cast<int64_t>(
        (static_cast<uint64_t>(owner->lambdaToken_) << 32) | output.size());
    return nullptr;
  } catch (...) {
    if (!owner->hostFailure_)
      owner->hostFailure_ = std::current_exception();
    constexpr std::string_view message = "Wasm native lambda callback failed";
    return wasmtime_trap_new(message.data(), message.size());
  }
}

wasm_trap_t* WasmInstance::lambdaResult(
    void* data,
    wasmtime_caller_t* caller,
    const wasmtime_val_t* args,
    size_t,
    wasmtime_val_t* results,
    size_t) noexcept {
  auto* owner = static_cast<WasmInstance*>(data);
  try {
    owner->checkLambdaDeadline();
    VELOX_CHECK(
        owner->lambdaOutput_ &&
            static_cast<uint32_t>(args[0].of.i32) == owner->lambdaToken_,
        "Unknown or expired Wasm lambda result token");
    const auto pointer = static_cast<uint32_t>(args[1].of.i32);
    const auto size = static_cast<uint32_t>(args[2].of.i32);
    VELOX_CHECK_EQ(
        size,
        owner->lambdaOutput_->size(),
        "Wasm lambda response length mismatch");
    wasmtime_extern_t memory;
    VELOX_CHECK(
        wasmtime_caller_export_get(caller, "memory", 6, &memory) &&
            memory.kind == WASMTIME_EXTERN_MEMORY,
        "Wasm lambda caller must export memory");
    auto* context = wasmtime_caller_context(caller);
    VELOX_CHECK_LE(
        static_cast<uint64_t>(pointer) + size,
        wasmtime_memory_data_size(context, &memory.of.memory),
        "Wasm lambda response is out of bounds");
    std::memcpy(
        wasmtime_memory_data(context, &memory.of.memory) + pointer,
        owner->lambdaOutput_->as<uint8_t>(),
        size);
    owner->lambdaOutput_.reset();
    results[0].kind = WASMTIME_I32;
    results[0].of.i32 = 0;
    return nullptr;
  } catch (...) {
    if (!owner->hostFailure_)
      owner->hostFailure_ = std::current_exception();
    constexpr std::string_view message = "Wasm native lambda result failed";
    return wasmtime_trap_new(message.data(), message.size());
  }
}

WasmInstance::~WasmInstance() {
  invalidate();
}

void WasmInstance::addCountEntrypoint(const std::string& name) {
  auto function =
      getExport(context_, instance_, name, WASMTIME_EXTERN_FUNC).of.func;
  validateFunctionType(context_, function, name, {WASM_I32}, {WASMTIME_V128});
  functions_.emplace(name, function);
}
void WasmInstance::addSingleGroupEntrypoint(const std::string& name) {
  auto function =
      getExport(context_, instance_, name, WASMTIME_EXTERN_FUNC).of.func;
  validateFunctionType(
      context_,
      function,
      name,
      {WASM_I32, WASM_I32, WASM_I32},
      {WASMTIME_V128});
  functions_.emplace(name, function);
}

void WasmInstance::invalidate() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  invalidateUnlocked();
}

void WasmInstance::invalidateUnlocked() noexcept {
  lambdaOutput_.reset();
  hostFailure_ = nullptr;
  if (store_ != nullptr) {
    wasmtime_store_delete(store_);
    store_ = nullptr;
    context_ = nullptr;
  }
}

void WasmInstance::setMemoryPool(memory::MemoryPool* pool) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pool_) {
    VELOX_CHECK(pool_.get() == pool, "Wasm Store cannot change memory pool");
    return;
  }
  memoryAccounting_->setPool(pool);
  pool_ = pool->shared_from_this();
}

uint64_t WasmInstance::linearMemoryBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return memoryAccounting_->bytes;
}

void WasmInstance::setCancellationCheck(std::function<bool()> cancelled) {
  std::lock_guard<std::mutex> lock(mutex_);
  cancelled_ = std::move(cancelled);
}

void WasmInstance::beginInvocation() {
  VELOX_CHECK(!lambdaOutput_, "Wasm lambda result was not consumed");
  lambdaCalls_ = 0;
  lambdaEvaluations_ = 0;
  VELOX_CHECK_GT(options_.maxCallMillis, 0, "Wasm deadline must be positive");
  VELOX_CHECK_LE(
      options_.maxCallMillis, 86'400'000, "Wasm deadline exceeds one day");
  deadline_ = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(options_.maxCallMillis);
  VELOX_CHECK(!cancelled_ || !cancelled_(), "Wasm task cancelled");
  wasmtime_context_set_epoch_deadline(context_, 1);
}

wasmtime_error_t* WasmInstance::checkEpoch(
    wasmtime_context_t*,
    void* data,
    uint64_t* delta,
    wasmtime_update_deadline_kind_t* kind) {
  auto& instance = *static_cast<WasmInstance*>(data);
  try {
    if (instance.cancelled_ && instance.cancelled_()) {
      return wasmtime_error_new("Wasm task cancelled");
    }
    if (std::chrono::steady_clock::now() >= instance.deadline_) {
      return wasmtime_error_new("Wasm invocation deadline exceeded");
    }
    *delta = 1;
    *kind = WASMTIME_UPDATE_DEADLINE_CONTINUE;
    return nullptr;
  } catch (...) {
    return wasmtime_error_new("Wasm cancellation check failed");
  }
}

void WasmInstance::resetFuel() {
  if (auto* error = wasmtime_context_set_fuel(context_, fuelPerCall_)) {
    throwWasmtimeError("Cannot set Wasm UDF fuel budget", error, nullptr);
  }
}

uint32_t WasmInstance::allocate(uint32_t size) {
  resetFuel();
  wasmtime_val_t argument{};
  argument.kind = WASMTIME_I32;
  argument.of.i32 = static_cast<int32_t>(size);
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  auto* error =
      wasmtime_func_call(context_, &alloc_, &argument, 1, &result, 1, &trap);
  memoryAccounting_->checkFailure(error, trap);
  checkHostFailure(error, trap);
  checkCall("Wasm UDF allocator failed", error, trap);
  VELOX_CHECK_EQ(
      result.kind, WASMTIME_I32, "Wasm allocator returned wrong type");
  const auto pointer = static_cast<uint32_t>(result.of.i32);
  validateRange(pointer, size);
  return pointer;
}

void WasmInstance::free(uint32_t pointer, uint32_t size) {
  if (size == 0) {
    return;
  }
  resetFuel();
  std::array<wasmtime_val_t, 2> arguments{};
  arguments[0].kind = WASMTIME_I32;
  arguments[0].of.i32 = static_cast<int32_t>(pointer);
  arguments[1].kind = WASMTIME_I32;
  arguments[1].of.i32 = static_cast<int32_t>(size);
  wasm_trap_t* trap = nullptr;
  auto* error = wasmtime_func_call(
      context_, &free_, arguments.data(), arguments.size(), nullptr, 0, &trap);
  memoryAccounting_->checkFailure(error, trap);
  checkHostFailure(error, trap);
  checkCall("Wasm UDF deallocator failed", error, trap);
}

void WasmInstance::validateRange(uint32_t pointer, uint32_t size) const {
  const auto memorySize = wasmtime_memory_data_size(context_, &memory_);
  VELOX_CHECK_LE(
      static_cast<uint64_t>(pointer) + size,
      memorySize,
      "Wasm UDF returned an out-of-bounds linear-memory range");
}

std::string WasmInstance::invoke(const ArrowIpcInput& input) {
  return invoke(entrypointName_, input);
}

const wasmtime_func_t& WasmInstance::function(std::string_view name) const {
  auto it = functions_.find(std::string(name));
  VELOX_CHECK(it != functions_.end(), "Unknown Wasm UDF entrypoint '{}'", name);
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
  VELOX_CHECK_NE(stateHandle, 0, "Wasm UDAF state handle must be non-zero");
  return invokeInput(entrypoint, input, &stateHandle);
}

std::string WasmInstance::invokeInput(
    std::string_view entrypoint,
    const ArrowIpcInput& input,
    const uint32_t* stateHandle) {
  return invokeWrittenInput(
      entrypoint,
      input.size(),
      [&](uint8_t* destination, uint32_t size) {
        input.write(destination, size);
      },
      stateHandle);
}
std::string WasmInstance::invokeBytes(
    std::string_view entrypoint,
    std::string_view bytes,
    uint32_t prefix) {
  VELOX_CHECK_LE(bytes.size(), std::numeric_limits<uint32_t>::max());
  return invokeWrittenInput(
      entrypoint,
      bytes.size(),
      [&](uint8_t* destination, uint32_t size) {
        std::memcpy(destination, bytes.data(), size);
      },
      &prefix);
}
std::string WasmInstance::invokeWrittenInput(
    std::string_view entrypoint,
    uint32_t inputSize,
    const std::function<void(uint8_t*, uint32_t)>& write,
    const uint32_t* stateHandle) {
  std::lock_guard<std::mutex> lock(mutex_);
  VELOX_CHECK_NOT_NULL(store_, "Wasm Store is invalidated");
  SCOPE_FAIL {
    invalidateUnlocked();
  };
  beginInvocation();
  VELOX_CHECK_LE(
      inputSize, options_.maxInputBytes, "Wasm input exceeds size limit");
  const auto inputPointer = allocate(inputSize);
  try {
    // Allocation may grow linear memory, so acquire its base address only
    // after velox_wasm_alloc has returned.
    write(wasmtime_memory_data(context_, &memory_) + inputPointer, inputSize);
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
  resetFuel();
  auto* error = wasmtime_func_call(
      context_,
      &function(entrypoint),
      arguments.data(),
      inputOffset + 2,
      &result,
      1,
      &trap);
  memoryAccounting_->checkFailure(error, trap);
  checkHostFailure(error, trap);
  if (error != nullptr || trap != nullptr) {
    try {
      free(inputPointer, inputSize);
    } catch (...) {
      // Preserve the invocation failure and release its Wasmtime error/trap
      // even if the guest deallocator also fails.
    }
    checkCall("Wasm UDF invocation failed", error, trap);
  }
  return copyResult(entrypoint, result, inputPointer, inputSize);
}

std::string WasmInstance::invokeCount(
    std::string_view entrypoint,
    uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  VELOX_CHECK_NOT_NULL(store_, "Wasm Store is invalidated");
  SCOPE_FAIL {
    invalidateUnlocked();
  };
  beginInvocation();
  wasmtime_val_t argument{};
  argument.kind = WASMTIME_I32;
  argument.of.i32 = static_cast<int32_t>(count);
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  resetFuel();
  auto* error = wasmtime_func_call(
      context_, &function(entrypoint), &argument, 1, &result, 1, &trap);
  memoryAccounting_->checkFailure(error, trap);
  checkHostFailure(error, trap);
  checkCall("Wasm UDAF create invocation failed", error, trap);
  return copyResult(entrypoint, result, 0, 0);
}

BufferPtr WasmInstance::invokeCountBuffer(
    std::string_view entrypoint,
    uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  VELOX_CHECK_NOT_NULL(store_, "Wasm Store is invalidated");
  SCOPE_FAIL {
    invalidateUnlocked();
  };
  beginInvocation();
  wasmtime_val_t argument{};
  argument.kind = WASMTIME_I32;
  argument.of.i32 = static_cast<int32_t>(count);
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  resetFuel();
  auto* error = wasmtime_func_call(
      context_, &function(entrypoint), &argument, 1, &result, 1, &trap);
  memoryAccounting_->checkFailure(error, trap);
  checkHostFailure(error, trap);
  checkCall("Wasm checkpoint invocation failed", error, trap);
  BufferPtr output;
  copyResult(entrypoint, result, 0, 0, &output);
  return output;
}

std::string WasmInstance::copyResult(
    std::string_view entrypoint,
    const wasmtime_val_t& result,
    uint32_t inputPointer,
    uint32_t inputSize,
    BufferPtr* owned) {
  VELOX_CHECK(!lambdaOutput_, "Wasm lambda result was not consumed");
  if (result.kind != WASMTIME_V128) {
    free(inputPointer, inputSize);
    VELOX_FAIL("Wasm UDF returned wrong type");
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
    VELOX_FAIL("Wasm UDF returned an out-of-bounds linear-memory range");
  }
  const auto inputEnd = static_cast<uint64_t>(inputPointer) + inputSize;
  const auto outputEnd =
      static_cast<uint64_t>(abiResult.dataPtr) + abiResult.dataLen;
  const bool aliasesInput = inputSize != 0 && abiResult.dataLen != 0 &&
      inputPointer < outputEnd && abiResult.dataPtr < inputEnd;
  if (aliasesInput) {
    free(inputPointer, inputSize);
    VELOX_FAIL("Wasm UDF output buffer aliases its input buffer");
  }
  VELOX_CHECK_LE(
      abiResult.dataLen,
      options_.maxOutputBytes,
      "Wasm output exceeds size limit");
  std::string output;
  try {
    const auto* data =
        wasmtime_memory_data(context_, &memory_) + abiResult.dataPtr;
    if (owned && abiResult.status == 0 && abiResult.reserved == 0) {
      VELOX_CHECK_NOT_NULL(pool_);
      *owned = AlignedBuffer::allocate<uint8_t>(abiResult.dataLen, pool_.get());
      std::memcpy((*owned)->asMutable<uint8_t>(), data, abiResult.dataLen);
    } else {
      output.assign(reinterpret_cast<const char*>(data), abiResult.dataLen);
    }
  } catch (...) {
    free(abiResult.dataPtr, abiResult.dataLen);
    free(inputPointer, inputSize);
    throw;
  }
  free(abiResult.dataPtr, abiResult.dataLen);
  free(inputPointer, inputSize);

  VELOX_CHECK_EQ(
      abiResult.reserved, 0, "Wasm UDF returned non-zero reserved ABI bits");
  if (abiResult.status > kNativeStatusBase &&
      abiResult.status <= kNativeStatusBase +
              static_cast<uint32_t>(StatusCode::kNotImplemented)) {
    const Status status(
        static_cast<StatusCode>(abiResult.status - kNativeStatusBase), output);
    // Match EvalCtx::setStatus: only UserError becomes a VeloxUserError.
    // Every failed export still invalidates its Store at the invocation guard.
    if (status.isUserError()) {
      VELOX_USER_FAIL("Wasm UDF '{}' failed: {}", entrypoint, status.message());
    }
    VELOX_FAIL("Wasm UDF '{}' failed: {}", entrypoint, status.message());
  }
  VELOX_CHECK_EQ(
      abiResult.status, 0, "Wasm UDF '{}' failed: {}", entrypoint, output);
  return output;
}

} // namespace facebook::velox::functions::wasm
