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

#include <cstddef>
#include <filesystem>
#include "velox/functions/wasm/Runtime.h"

namespace facebook::velox::functions::wasm {

/// Registers every scalar and aggregate declaration embedded in a .wasm file.
/// Startup-only: finish native registration first, and do not run queries or
/// modify native/window registries concurrently. Concurrent WASM registrations
/// are serialized. Publishes all declarations or throws without mutation.
/// Adds disjoint scalar overloads; overwrite replaces only matching signatures.
/// Never shadows a native Simple Function, even with overwrite=true.
/// Returns the number of distinct scalar/aggregate SQL names in this module.
size_t registerWasmModule(
    const std::filesystem::path& wasmPath,
    bool overwrite = false,
    const WasmOptions& options = {});

} // namespace facebook::velox::functions::wasm
