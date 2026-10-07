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

namespace facebook::velox::functions::wasm {

constexpr uint32_t kAbiVersion = 1;
constexpr uint32_t kNativeStatusAbiVersion = 2;
constexpr uint32_t kStateAccountingAbiVersion = 3;
constexpr const char* kMemoryExport = "memory";
constexpr const char* kAllocExport = "velox_wasm_alloc";
constexpr const char* kFreeExport = "velox_wasm_free";
// 0 succeeds; 1 carries a legacy UTF-8 invocation failure. Tagged values
// encode a non-OK velox::StatusCode and carry its UTF-8 message directly.
constexpr uint32_t kNativeStatusBase = 0x100;

struct AbiResult {
  uint32_t status;
  uint32_t dataPtr;
  uint32_t dataLen;
  uint32_t reserved;
};

} // namespace facebook::velox::functions::wasm
