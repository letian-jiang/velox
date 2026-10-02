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

#include "velox/functions/wasm/WasmVectorFunction.h"

#include "velox/functions/wasm/ArrowIpc.h"

namespace facebook::velox::functions::wasm {

WasmVectorFunction::WasmVectorFunction(
    std::shared_ptr<WasmModule> module,
    std::string entrypoint)
    : instance_(std::move(module), std::move(entrypoint)) {}

void WasmVectorFunction::apply(
    const SelectivityVector& rows,
    std::vector<VectorPtr>& arguments,
    const TypePtr& outputType,
    exec::EvalCtx& context,
    VectorPtr& result) const {
  if (!rows.hasSelections()) {
    return;
  }
  try {
    applyInternal(rows, arguments, outputType, context, result);
  } catch (const VeloxRuntimeError&) {
    throw;
  } catch (const std::exception&) {
    context.setErrors(rows, std::current_exception());
  }
}

void WasmVectorFunction::applyInternal(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    const TypePtr& outputType,
    exec::EvalCtx& context,
    VectorPtr& result) const {
  auto gathered = gatherToArrowIpc(rows, arguments, context.pool());
  auto outputIpc = instance_.invoke(gathered.input);
  std::vector<std::optional<std::string>> errors;
  auto compactResult = decodeArrowIpcResult(
      outputIpc, outputType, gathered.rowCount, context.pool(), &errors);
  scatterArrowResult(rows, compactResult, outputType, context.pool(), result);
  if (!errors.empty()) {
    vector_size_t compactRow = 0;
    rows.applyToSelected([&](vector_size_t row) {
      const auto index = compactRow++;
      if (!errors[index].has_value()) {
        return;
      }
      try {
        VELOX_USER_FAIL("Wasm scalar UDF failed: {}", errors[index].value());
      } catch (const VeloxRuntimeError&) {
        context.setVeloxExceptionError(row, std::current_exception());
      }
    });
  }
}

} // namespace facebook::velox::functions::wasm
