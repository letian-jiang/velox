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

#include "velox/expression/VectorFunction.h"
#include "velox/functions/wasm/Runtime.h"

namespace facebook::velox::functions::wasm {

class WasmVectorFunction final : public exec::VectorFunction {
 public:
  WasmVectorFunction(
      std::shared_ptr<WasmModule> module,
      std::string entrypoint);

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& arguments,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const override;

 private:
  void applyInternal(
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& arguments,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const;

  mutable WasmInstance instance_;
};

} // namespace facebook::velox::functions::wasm
