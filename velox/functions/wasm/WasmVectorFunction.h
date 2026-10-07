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

#include <exception>
#include <mutex>
#include <unordered_map>

#include "velox/expression/VectorFunction.h"
#include "velox/functions/wasm/ArrowIpc.h"
#include "velox/functions/wasm/Runtime.h"

namespace facebook::velox::functions::wasm {

struct ScalarInitialization {
  std::string entrypoint;
  std::vector<exec::VectorFunctionArg> arguments;
  std::unordered_map<std::string, std::string> config;
};

class WasmVectorFunction final : public exec::VectorFunction {
 public:
  WasmVectorFunction(
      std::shared_ptr<WasmModule> module,
      std::string entrypoint,
      ScalarInitialization initialization = {},
      bool rowApi = false,
      const WasmOptions& options = {},
      bool hasAscii = false);

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& arguments,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const override;

  bool ensureStringEncodingSetAtAllInputs() const override {
    return hasAscii_;
  }

 private:
  void applyInternal(
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& arguments,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const;

  std::shared_ptr<WasmModule> module_;
  std::string entrypoint_;
  WasmOptions options_;
  mutable std::shared_ptr<WasmInstance> instance_;
  mutable std::once_flag instanceOnce_;
  mutable ArrowIpcDecodeOptions decodeOptions_;
  ScalarInitialization initialization_;
  bool rowApi_;
  bool hasAscii_;
  mutable std::once_flag initializeOnce_;
  mutable std::exception_ptr initializationError_;
};

} // namespace facebook::velox::functions::wasm
