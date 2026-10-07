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
#include "velox/functions/wasm/LinearMemory.h"

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include "velox/common/base/Exceptions.h"

namespace facebook::velox::functions::wasm {
namespace {
thread_local std::shared_ptr<WasmMemoryAccounting> currentAccounting;

class GuardedLinearMemory {
 public:
  GuardedLinearMemory(
      std::shared_ptr<WasmMemoryAccounting> accounting,
      size_t minimum,
      size_t maximum,
      size_t reservation,
      size_t guard)
      : accounting_(std::move(accounting)),
        maximum_(std::min<uint64_t>(maximum, accounting_->limit)) {
    const auto page = ::sysconf(_SC_PAGESIZE);
    VELOX_CHECK_GT(page, 0, "Cannot determine system page size");
    VELOX_CHECK_EQ(65536 % page, 0, "Unsupported Wasm memory page alignment");
    const size_t pageBytes = page;
    // Honor the full reservation and guard requested by Wasmtime: JIT code
    // can elide bounds checks based on both. The pointer never moves on growth.
    capacity_ = std::max<size_t>({reservation, minimum, maximum_});
    VELOX_CHECK_LE(capacity_, SIZE_MAX - pageBytes + 1);
    capacity_ = (capacity_ + pageBytes - 1) / pageBytes * pageBytes;
    guard = std::max(guard, pageBytes);
    VELOX_CHECK_EQ(guard % pageBytes, 0);
    VELOX_CHECK_LE(
        capacity_, SIZE_MAX - guard, "Wasm virtual reservation overflow");
    mappingBytes_ = capacity_ + guard;
    void* mapping = ::mmap(
        nullptr, mappingBytes_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VELOX_CHECK(
        mapping != MAP_FAILED,
        "Cannot reserve Wasm virtual memory: {}",
        std::strerror(errno));
    base_ = static_cast<uint8_t*>(mapping);
    try {
      grow(minimum);
    } catch (...) {
      ::munmap(base_, mappingBytes_);
      throw;
    }
  }
  ~GuardedLinearMemory() {
    // No guest pointers remain accessible after the Wasmtime finalizer runs.
    ::munmap(base_, mappingBytes_);
    accounting_->free(size_);
  }
  void grow(size_t requested) {
    VELOX_CHECK_GE(requested, size_, "Wasm memory cannot shrink");
    VELOX_CHECK_LE(requested, maximum_, "Wasm memory exceeds configured limit");
    VELOX_CHECK_EQ(requested % 65536, 0, "Unsupported Wasm memory page size");
    if (requested == size_)
      return;
    const auto extra = requested - size_;
    // Native arbitration must succeed before pages become readable/writable.
    accounting_->allocate(extra);
    if (::mprotect(base_ + size_, extra, PROT_READ | PROT_WRITE) != 0) {
      const auto failure = errno;
      accounting_->free(extra);
      VELOX_FAIL("Cannot commit Wasm memory: {}", std::strerror(failure));
    }
    size_ = requested;
  }
  static uint8_t* get(void* env, size_t* size, size_t* capacity) noexcept {
    auto* self = static_cast<GuardedLinearMemory*>(env);
    *size = self->size_;
    *capacity = self->capacity_;
    return self->base_;
  }
  static wasmtime_error_t* growCallback(void* env, size_t requested) noexcept {
    auto* self = static_cast<GuardedLinearMemory*>(env);
    try {
      self->grow(requested);
      return nullptr;
    } catch (...) {
      // C++ exceptions must never unwind through Rust. Even if guest code
      // handles memory.grow's -1, the query allocation failure remains fatal.
      self->accounting_->failure = std::current_exception();
      return wasmtime_error_new("Wasm linear memory allocation failed");
    }
  }
  static void destroy(void* env) noexcept {
    delete static_cast<GuardedLinearMemory*>(env);
  }

 private:
  std::shared_ptr<WasmMemoryAccounting> accounting_;
  size_t maximum_;
  size_t capacity_{0};
  size_t mappingBytes_{0};
  size_t size_{0};
  uint8_t* base_{nullptr};
};

wasmtime_error_t* createMemory(
    void*,
    const wasm_memorytype_t* type,
    size_t minimum,
    size_t maximum,
    size_t reservation,
    size_t guard,
    wasmtime_linear_memory_t* result) noexcept {
  try {
    VELOX_CHECK_NOT_NULL(
        currentAccounting, "Wasm memory creation requires a Store owner");
    VELOX_CHECK(
        !wasmtime_memorytype_isshared(type),
        "Shared guest memory is unsupported");
    VELOX_CHECK_EQ(
        wasmtime_memorytype_page_size(type),
        65536,
        "Unsupported Wasm memory page size");
    auto memory = std::make_unique<GuardedLinearMemory>(
        currentAccounting, minimum, maximum, reservation, guard);
    *result = {
        memory.release(),
        GuardedLinearMemory::get,
        GuardedLinearMemory::growCallback,
        GuardedLinearMemory::destroy};
    return nullptr;
  } catch (...) {
    if (currentAccounting)
      currentAccounting->failure = std::current_exception();
    return wasmtime_error_new("Wasm linear memory creation failed");
  }
}
} // namespace

void WasmMemoryAccounting::setPool(memory::MemoryPool* newPool) {
  if (pool) {
    VELOX_CHECK(pool.get() == newPool, "Wasm Store cannot change memory pool");
    return;
  }
  auto owner = newPool->shared_from_this();
  if (bytes)
    owner->reportExternalAllocation(bytes);
  pool = std::move(owner);
}
void WasmMemoryAccounting::allocate(uint64_t extra) {
  VELOX_CHECK_LE(extra, limit - bytes, "Wasm memory exceeds configured limit");
  if (pool)
    pool->reportExternalAllocation(extra);
  bytes += extra;
}
void WasmMemoryAccounting::free(uint64_t released) noexcept {
  if (!released)
    return;
  if (pool)
    pool->reportExternalFree(released);
  bytes -= released;
}
void WasmMemoryAccounting::checkFailure(
    wasmtime_error_t* error,
    wasm_trap_t* trap) const {
  if (!failure)
    return;
  if (error)
    wasmtime_error_delete(error);
  if (trap)
    wasm_trap_delete(trap);
  try {
    std::rethrow_exception(failure);
  } catch (const VeloxRuntimeError&) {
    throw;
  } catch (const std::exception& cause) {
    VELOX_FAIL("Wasm linear memory failure: {}", cause.what());
  } catch (...) {
    VELOX_FAIL("Unknown Wasm linear memory failure");
  }
}
WasmMemoryScope::WasmMemoryScope(
    std::shared_ptr<WasmMemoryAccounting> accounting)
    : previous_(std::move(currentAccounting)) {
  currentAccounting = std::move(accounting);
}
WasmMemoryScope::~WasmMemoryScope() {
  currentAccounting = std::move(previous_);
}
void configureWasmLinearMemory(wasm_config_t* config) {
  wasmtime_memory_creator_t creator{nullptr, createMemory, nullptr};
  wasmtime_config_host_memory_creator_set(config, &creator);
  // Shared memories use a distinct Wasmtime allocation path; this isolated
  // ABI has no guest thread service. Do not let them bypass native accounting.
  wasmtime_config_wasm_threads_set(config, false);
  wasmtime_config_memory_init_cow_set(config, false);
}
} // namespace facebook::velox::functions::wasm
