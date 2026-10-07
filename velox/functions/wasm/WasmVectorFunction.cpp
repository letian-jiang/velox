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

#include <charconv>

#include "velox/functions/wasm/ArrowIpc.h"
#include "velox/vector/LazyVector.h"

namespace facebook::velox::functions::wasm {
namespace {

// EvalCtx can immediately throw a validated user row error outside TRY. Keep
// its provenance separate from user exceptions raised by an invalid bridge.
struct ValidatedRowError {
  std::exception_ptr cause;
};

template <typename F>
void reportValidatedRowError(F&& report) {
  try {
    report();
  } catch (const VeloxUserError&) {
    throw ValidatedRowError{std::current_exception()};
  }
}

struct ParsedRowError {
  std::optional<StatusCode> code;
  std::string_view message;
};

ParsedRowError parseRowError(std::string_view message, bool rowApi) {
  constexpr std::string_view statusPrefix =
      "\x1e"
      "velox.status.v1:";
  if (rowApi && message.starts_with(statusPrefix)) {
    auto payload = message.substr(statusPrefix.size());
    auto newline = payload.find('\n');
    VELOX_CHECK(
        newline != std::string_view::npos, "Malformed scalar status payload");
    int code = 0;
    auto parsed =
        std::from_chars(payload.data(), payload.data() + newline, code);
    VELOX_CHECK(
        parsed.ec == std::errc{} && parsed.ptr == payload.data() + newline &&
            code >= 1 && code <= 11,
        "Invalid scalar status code");
    return {static_cast<StatusCode>(code), payload.substr(newline + 1)};
  }
  constexpr std::string_view messagePrefix =
      "\x1e"
      "velox.message.v1:";
  if (rowApi && message.starts_with(messagePrefix)) {
    message.remove_prefix(messagePrefix.size());
  }
  return {std::nullopt, message};
}

} // namespace

WasmVectorFunction::WasmVectorFunction(
    std::shared_ptr<WasmModule> module,
    std::string entrypoint,
    ScalarInitialization initialization,
    bool rowApi,
    const WasmOptions& options,
    bool hasAscii)
    : module_(std::move(module)),
      entrypoint_(std::move(entrypoint)),
      options_(options),
      initialization_(std::move(initialization)),
      rowApi_(rowApi),
      hasAscii_(hasAscii) {}

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
    std::call_once(instanceOnce_, [&]() {
      instance_ = std::make_shared<WasmInstance>(
          module_,
          entrypoint_,
          options_,
          context.pool(),
          currentWasmCancellationCheck());
      if (!initialization_.entrypoint.empty())
        instance_->addBatchEntrypoint(initialization_.entrypoint);
      decodeOptions_.lazyCodecs = true;
      decodeOptions_.onDeferredCodecFailure =
          [store = std::weak_ptr<WasmInstance>(instance_)] {
            if (auto live = store.lock()) {
              live->invalidate();
            }
          };
    });
    instance_->setCancellationCheck(currentWasmCancellationCheck());
    applyInternal(rows, arguments, outputType, context, result);
  } catch (const ValidatedRowError& error) {
    std::rethrow_exception(error.cause);
  } catch (const VeloxRuntimeError&) {
    if (instance_)
      instance_->invalidate();
    throw;
  } catch (const std::exception& error) {
    if (instance_)
      instance_->invalidate();
    VELOX_FAIL("Wasm execution or protocol failure: {}", error.what());
  }
}

void WasmVectorFunction::applyInternal(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& arguments,
    const TypePtr& outputType,
    exec::EvalCtx& context,
    VectorPtr& result) const {
  if (!initialization_.entrypoint.empty()) {
    std::call_once(initializeOnce_, [&]() {
      try {
        auto input = makeScalarInitializationInput(
            initialization_.arguments,
            outputType,
            initialization_.config,
            context.pool(),
            rowApi_);
        auto response = instance_->invoke(initialization_.entrypoint, input);
        auto acknowledgment = decodeOwnedArrowIpcResult(
            std::move(response), BOOLEAN(), 1, context.pool());
        auto value = acknowledgment->as<SimpleVector<bool>>();
        VELOX_CHECK(
            !value->isNullAt(0) && value->valueAt(0),
            "Wasm scalar initialization was not acknowledged");
      } catch (...) {
        initializationError_ = std::current_exception();
      }
    });
    if (initializationError_) {
      std::rethrow_exception(initializationError_);
    }
  }
  std::optional<bool> asciiInputs;
  if (hasAscii_) {
    asciiInputs = true;
    for (const auto& argument : arguments) {
      if (argument->type()->isVarchar()) {
        // Expr computes this for the selected rows, as it does for native SFI.
        // Direct callers with missing metadata safely use the regular path.
        const auto* strings = argument->as<SimpleVector<StringView>>();
        const auto ascii = strings ? strings->isAscii(rows) : std::nullopt;
        if (!ascii.value_or(false)) {
          asciiInputs = false;
          break;
        }
      }
    }
  }
  auto gathered = gatherToArrowIpc(
      rows,
      arguments,
      context.pool(),
      rowApi_ ? outputType : nullptr,
      asciiInputs);
  auto outputIpc = instance_->invoke(gathered.input);
  std::vector<std::optional<std::string>> errors;
  auto compactResult = decodeOwnedArrowIpcResult(
      std::move(outputIpc),
      outputType,
      gathered.rowCount,
      context.pool(),
      &errors,
      gathered.input.opaqueScope(),
      decodeOptions_);
  // Preserve the native top-level result contract. Nested codec fields can
  // remain lazy and be loaded by downstream readers/dereferences.
  if (compactResult->isLazy()) {
    compactResult = BaseVector::loadedVectorShared(compactResult);
  }
  // Validate every payload before a user error can short-circuit reporting.
  // A malformed later row must still be a fatal protocol error outside TRY.
  for (const auto& error : errors) {
    if (error.has_value()) {
      parseRowError(*error, rowApi_);
    }
  }
  scatterArrowResult(rows, compactResult, outputType, context.pool(), result);
  if (!errors.empty()) {
    vector_size_t compactRow = 0;
    rows.applyToSelected([&](vector_size_t row) {
      const auto index = compactRow++;
      if (!errors[index].has_value()) {
        return;
      }
      const auto error = parseRowError(errors[index].value(), rowApi_);
      if (error.code.has_value()) {
        reportValidatedRowError([&] {
          context.setStatus(
              row, Status(*error.code, std::string(error.message)));
        });
        return;
      }
      try {
        VELOX_USER_FAIL("Wasm scalar UDF failed: {}", error.message);
      } catch (const VeloxUserError&) {
        const auto cause = std::current_exception();
        reportValidatedRowError(
            [&] { context.setVeloxExceptionError(row, cause); });
      }
    });
  }
  if (outputType->isVarchar()) {
    // The guest is untrusted: derive result encoding from actual output bytes,
    // instead of assuming ASCII preservation from a guest declaration.
    result->asUnchecked<SimpleVector<StringView>>()->computeAndSetIsAscii(rows);
  }
}

} // namespace facebook::velox::functions::wasm
