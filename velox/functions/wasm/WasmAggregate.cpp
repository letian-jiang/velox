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

#include "velox/functions/wasm/WasmAggregate.h"
#include "velox/functions/wasm/Abi.h"

#include <folly/ScopeGuard.h>
#include <folly/lang/Bits.h>

#include <limits>
#include <unordered_set>

#include "velox/common/base/Exceptions.h"
#include "velox/core/ExpressionEvaluator.h"
#include "velox/expression/Expr.h"
#include "velox/functions/wasm/ArrowIpc.h"

namespace facebook::velox::functions::wasm {

WasmAggregate::WasmAggregate(
    TypePtr resultType,
    AggregateManifest manifest,
    std::shared_ptr<WasmModule> module,
    const WasmOptions& options,
    std::vector<TypePtr> inputTypes,
    std::unordered_map<std::string, std::string> config,
    std::vector<TypePtr> lambdaTypes,
    bool rawInputTypes)
    : Aggregate(std::move(resultType)),
      manifest_(std::move(manifest)),
      module_(std::move(module)),
      options_(options),
      inputTypes_(std::move(inputTypes)),
      rawInputTypes_(rawInputTypes),
      lambdaTypes_(std::move(lambdaTypes)),
      lambdaExprSets_(lambdaTypes_.size()),
      config_(std::move(config)) {
  if (manifest_.rowApi) {
    // AggregateInfo omits FUNCTION arguments from the value columns.
    for (size_t i = 0; i < manifest_.signature->argumentTypes().size(); ++i) {
      if (!manifest_.signature->isLambdaArgumentAt(i))
        rawConstantFlags_.push_back(
            manifest_.signature->constantArguments()[i]);
    }
  }
}

void WasmAggregate::setConstantInputs(const std::vector<VectorPtr>& inputs) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  VELOX_CHECK(
      !instance_, "Wasm UDAF constants must be set before initialization");
  VELOX_CHECK_LE(
      inputs.size(),
      inputTypes_.size(),
      "Wasm UDAF constant input count mismatch");
  constantInputs_ = inputs;
  constantInputs_.resize(inputTypes_.size());
}

int32_t WasmAggregate::accumulatorFixedWidthSize() const {
  return sizeof(uint32_t) + (reportsStateSize() ? sizeof(uint64_t) : 0);
}

bool WasmAggregate::accumulatorUsesExternalMemory() const {
  return true;
}

bool WasmAggregate::isFixedSize() const {
  return !reportsStateSize();
}

bool WasmAggregate::reportsStateSize() const {
  return manifest_.abiVersion == kStateAccountingAbiVersion;
}

bool WasmAggregate::supportsCompact() const {
  return reportsStateSize();
}

uint64_t WasmAggregate::stateSize(char* group) const {
  return folly::loadUnaligned<uint64_t>(group + offset_ + sizeof(uint32_t));
}

void WasmAggregate::setStateSize(char* group, uint64_t bytes) {
  const auto previous = stateSize(group);
  const auto rowBytes = folly::loadUnaligned<uint32_t>(group + rowSizeOffset_);
  // Several variable-sized keys/aggregates share this counter. Preserve a
  // saturated counter rather than subtracting an unknown truncated amount.
  const auto adjusted = rowBytes == std::numeric_limits<uint32_t>::max()
      ? rowBytes
      : std::min<uint64_t>(
            bytes >= previous
                ? static_cast<uint64_t>(rowBytes) + bytes - previous
                : static_cast<uint64_t>(rowBytes) -
                    std::min<uint64_t>(rowBytes, previous - bytes),
            std::numeric_limits<uint32_t>::max());
  folly::storeUnaligned<uint32_t>(group + rowSizeOffset_, adjusted);
  folly::storeUnaligned<uint64_t>(group + offset_ + sizeof(uint32_t), bytes);
}

void WasmAggregate::applyUsage(
    const std::unordered_map<uint32_t, char*>& groups,
    std::string_view report) {
  constexpr size_t stride = sizeof(uint32_t) + sizeof(uint64_t);
  VELOX_CHECK_EQ(
      report.size(),
      groups.size() * stride,
      "Wasm state report count mismatch");
  std::unordered_set<uint32_t> seen;
  std::vector<std::pair<char*, uint64_t>> updates;
  uint64_t total = stateBytes_;
  for (size_t offset = 0; offset < report.size(); offset += stride) {
    const auto state = folly::loadUnaligned<uint32_t>(report.data() + offset);
    const auto bytes = folly::loadUnaligned<uint64_t>(
        report.data() + offset + sizeof(uint32_t));
    const auto found = groups.find(state);
    VELOX_CHECK(
        found != groups.end() && seen.insert(state).second,
        "Unexpected or duplicate Wasm state report handle");
    VELOX_CHECK_LE(
        bytes,
        options_.memoryLimitBytes,
        "Wasm state report exceeds Store memory limit");
    total -= stateSize(found->second);
    updates.emplace_back(found->second, bytes);
  }
  // Remove all old sizes before adding the new ones: one group may shrink
  // while another grows, irrespective of the report's order.
  for (const auto& [group, bytes] : updates) {
    VELOX_CHECK_LE(
        bytes,
        options_.memoryLimitBytes - total,
        "Wasm live state reports exceed Store memory limit");
    total += bytes;
  }
  // Validate the complete report before publishing row sizes.
  for (const auto& [group, bytes] : updates)
    setStateSize(group, bytes);
  stateBytes_ = total;
}

uint64_t WasmAggregate::compact(folly::Range<char**> groups) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!reportsStateSize() || groups.empty())
    return 0;
  SCOPE_FAIL {
    failed_ = true;
    if (instance_)
      instance_->invalidate();
  };
  const auto originalPages = instance_ ? instance_->linearMemoryBytes() : 0;
  SelectivityVector rows(groups.size());
  std::vector<uint32_t> handles(groups.size());
  std::unordered_map<uint32_t, char*> unique;
  for (vector_size_t row = 0; row < groups.size(); ++row) {
    handles[row] = handle(groups[row]);
    VELOX_CHECK(
        unique.emplace(handles[row], groups[row]).second,
        "Duplicate compact group");
  }
  auto input =
      gatherToArrowIpcWithStateHandles(rows, handles, {}, allocator_->pool());
  ensureInstance();
  applyUsage(
      unique, instance_->invoke(manifest_.entrypoints.compact, input.input));
  // Rebuild the complete Store, even when native passes only one row batch.
  // Checkpointing preserves private update configuration, caches and handle
  // IDs; a SQL intermediate is not a general live-state checkpoint.
  if (manifest_.entrypoints.checkpoint.empty())
    return 0;
  const auto before = instance_->linearMemoryBytes();
  if (before <= baseMemoryBytes_ + (64UL << 10) ||
      stateBytes_ >= (before - baseMemoryBytes_ - (64UL << 10)) / 2)
    return 0;
  const auto estimate =
      stateBytes_ * 4 + activeHandles_.size() * 32 + (64UL << 10);
  if (estimate > allocator_->pool()->freeBytes() +
          allocator_->pool()->availableReservation())
    return 0;
  auto saved = instance_->invokeCountBuffer(
      manifest_.entrypoints.checkpoint,
      std::min<uint64_t>(
          options_.maxOutputBytes, std::numeric_limits<uint32_t>::max()));
  std::string_view checkpoint(
      reinterpret_cast<const char*>(saved->as<uint8_t>()), saved->size());
  VELOX_CHECK_GE(checkpoint.size(), 12, "Truncated Wasm checkpoint header");
  VELOX_CHECK_EQ(
      checkpoint.substr(0, 4), "VWS1", "Invalid Wasm checkpoint format");
  const auto next = folly::loadUnaligned<uint32_t>(checkpoint.data() + 4);
  const auto count = folly::loadUnaligned<uint32_t>(checkpoint.data() + 8);
  VELOX_CHECK_EQ(
      count, activeHandles_.size(), "Wasm checkpoint state count mismatch");
  std::unordered_set<uint32_t> seen;
  size_t offset = 12;
  for (uint32_t i = 0; i < count; ++i) {
    VELOX_CHECK_LE(
        offset + 8, checkpoint.size(), "Truncated Wasm checkpoint record");
    const auto state =
        folly::loadUnaligned<uint32_t>(checkpoint.data() + offset);
    const auto size =
        folly::loadUnaligned<uint32_t>(checkpoint.data() + offset + 4);
    VELOX_CHECK(
        state && activeHandles_.count(state) && seen.insert(state).second &&
            (!next || state < next),
        "Invalid or duplicate Wasm checkpoint handle");
    offset += 8;
    VELOX_CHECK_LE(
        size, checkpoint.size() - offset, "Truncated Wasm checkpoint State");
    offset += size;
  }
  VELOX_CHECK_EQ(offset, checkpoint.size(), "Trailing Wasm checkpoint bytes");
  // The snapshot is query-owned before discarding old pages. A failed restore
  // aborts the aggregate; no partially restored Store is ever published.
  instance_.reset();
  ensureInstance();
  auto usage = instance_->invokeBytes(
      manifest_.entrypoints.restore,
      checkpoint,
      std::min<uint64_t>(
          options_.memoryLimitBytes, std::numeric_limits<uint32_t>::max()));
  applyUsage(activeHandles_, usage);
  const auto after = instance_->linearMemoryBytes();
  return originalPages > after ? originalPages - after : 0;
}

ArrowIpcInput WasmAggregate::evaluateLambda(
    uint32_t index,
    std::string_view request,
    uint32_t rows) const {
  VELOX_CHECK_LE(rows, std::numeric_limits<vector_size_t>::max());
  VELOX_CHECK_EQ(
      lambdaExpressions_.size(),
      lambdaTypes_.size(),
      "Wasm aggregate lambda expressions are not bound");
  VELOX_CHECK_NOT_NULL(
      expressionEvaluator_, "Wasm aggregate lambda evaluator is not bound");
  VELOX_CHECK_LT(
      index, lambdaTypes_.size(), "Wasm aggregate lambda index out of bounds");
  VELOX_CHECK_NOT_NULL(
      lambdaTypes_[index], "Wasm aggregate lambda types are not bound");
  const auto& lambda = lambdaExpressions_[index];
  VELOX_CHECK_NOT_NULL(lambda);
  VELOX_CHECK(
      lambda->type()->equivalent(*lambdaTypes_[index]),
      "Wasm lambda expression type mismatch or unsupported captures");
  std::vector<std::string> names;
  for (size_t i = 0; i < lambda->signature()->size(); ++i)
    names.push_back("arg" + std::to_string(i));
  auto inputType = ROW(
      std::move(names), std::vector<TypePtr>(lambda->signature()->children()));
  auto input =
      decodeArrowIpcResult(request, inputType, rows, allocator_->pool());
  auto* row = input->as<RowVector>();
  VELOX_CHECK_NOT_NULL(row);
  for (vector_size_t i = 0; i < rows; ++i)
    VELOX_CHECK(!row->isNullAt(i), "Wasm lambda argument row cannot be NULL");
  RowVector arguments(
      allocator_->pool(),
      lambda->signature(),
      nullptr,
      rows,
      std::vector<VectorPtr>(row->children()));
  if (!lambdaExprSets_[index])
    lambdaExprSets_[index] = expressionEvaluator_->compile(lambda->body());
  VectorPtr result;
  expressionEvaluator_->evaluate(
      lambdaExprSets_[index].get(), SelectivityVector(rows), arguments, result);
  VELOX_CHECK(
      result && result->size() == rows &&
          result->type()->equivalent(*lambda->body()->type()),
      "Invalid native lambda result");
  return gatherToArrowIpc(
             SelectivityVector(rows),
             {result},
             allocator_->pool(),
             lambda->body()->type())
      .input;
}

void WasmAggregate::ensureInstance() const {
  VELOX_CHECK(!failed_, "Wasm aggregate is invalidated");
  if (!instance_) {
    instance_ = std::make_unique<WasmInstance>(
        module_,
        manifest_.entrypoints.create,
        std::vector<std::string>{
            manifest_.entrypoints.destroy,
            manifest_.entrypoints.update,
            manifest_.entrypoints.serialize,
            manifest_.entrypoints.merge,
            manifest_.entrypoints.finalize},
        std::vector<std::string>{
            manifest_.entrypoints.updateSingleGroup,
            manifest_.entrypoints.mergeSingleGroup},
        options_,
        allocator_->pool(),
        currentWasmCancellationCheck());
    if (!lambdaTypes_.empty()) {
      instance_->setLambdaCallback(
          [this](uint32_t index, std::string_view request, uint32_t rows) {
            return evaluateLambda(index, request, rows);
          });
    }
    if (!manifest_.entrypoints.checkpoint.empty()) {
      instance_->addCountEntrypoint(manifest_.entrypoints.checkpoint);
      instance_->addSingleGroupEntrypoint(manifest_.entrypoints.restore);
    }
    if (!manifest_.entrypoints.compact.empty())
      instance_->addBatchEntrypoint(manifest_.entrypoints.compact);
    if (!manifest_.entrypoints.toIntermediate.empty()) {
      instance_->addBatchEntrypoint(manifest_.entrypoints.toIntermediate);
    }
    if (!manifest_.initializeEntrypoint.empty()) {
      instance_->addBatchEntrypoint(manifest_.initializeEntrypoint);
      std::vector<exec::VectorFunctionArg> arguments;
      for (size_t i = 0; i < inputTypes_.size(); ++i) {
        auto constant = constantInputs_.empty() ? nullptr : constantInputs_[i];
        arguments.push_back({inputTypes_[i], std::move(constant)});
      }
      auto input = makeScalarInitializationInput(
          arguments,
          manifest_.returnType.type,
          config_,
          allocator_->pool(),
          true,
          manifest_.intermediateType.type,
          lambdaTypes_,
          options_.maxLambdaRowsPerCall,
          rawInputTypes_);
      requireNoOutput(
          manifest_.initializeEntrypoint,
          instance_->invoke(manifest_.initializeEntrypoint, input));
    }
    baseMemoryBytes_ = instance_->linearMemoryBytes();
  }
  instance_->setCancellationCheck(currentWasmCancellationCheck());
}

void WasmAggregate::validateRawConstants() const {
  VELOX_CHECK(
      rawInputTypes_, "Wasm UDAF instance was bound for intermediate input");
  if (!manifest_.rowApi)
    return;
  // Projected flags use the same value-only layout as the SDK input tuple.
  const auto& required = rawConstantFlags_;
  if (required.empty())
    return;
  for (size_t i = 0; i < inputTypes_.size(); ++i) {
    const auto index = std::min(i, required.size() - 1);
    VELOX_USER_CHECK(
        !required[index] || (!constantInputs_.empty() && constantInputs_[i]),
        "Wasm UDAF '{}' argument {} must be constant",
        manifest_.name,
        i);
  }
}

bool WasmAggregate::supportsToIntermediate() const {
  return rawInputTypes_ && !manifest_.entrypoints.toIntermediate.empty();
}

void WasmAggregate::toIntermediate(
    const SelectivityVector& rows,
    std::vector<VectorPtr>& args,
    VectorPtr& result) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  SCOPE_FAIL {
    if (instance_)
      instance_->invalidate();
  };
  VELOX_CHECK(
      supportsToIntermediate(), "Wasm UDAF does not support toIntermediate");
  validateRawConstants();
  auto filtered = filterRawRows(rows, args);
  if (!filtered.hasSelections()) {
    result = BaseVector::create(
        manifest_.intermediateType.type, rows.size(), allocator_->pool());
    for (vector_size_t row = 0; row < rows.size(); ++row)
      result->setNull(row, true);
    return;
  }
  validateRawConstants();
  ensureInstance();
  auto batch = gatherToArrowIpc(
      filtered, args, allocator_->pool(), manifest_.intermediateType.type);
  auto compact = decodeOwnedArrowIpcResult(
      instance_->invoke(manifest_.entrypoints.toIntermediate, batch.input),
      manifest_.intermediateType.type,
      filtered.countSelected(),
      allocator_->pool());
  result = BaseVector::create(
      manifest_.intermediateType.type, rows.size(), allocator_->pool());
  for (vector_size_t row = 0; row < rows.size(); ++row)
    result->setNull(row, true);
  scatterArrowResult(
      filtered,
      compact,
      manifest_.intermediateType.type,
      allocator_->pool(),
      result);
}

uint32_t WasmAggregate::handle(char* group) const {
  const auto state = folly::loadUnaligned<uint32_t>(group + offset_);
  VELOX_CHECK(activeHandles_.count(state), "Unknown Wasm UDAF group handle");
  return state;
}

void WasmAggregate::setHandle(char* group, uint32_t stateHandle) const {
  folly::storeUnaligned<uint32_t>(group + offset_, stateHandle);
}

void WasmAggregate::initializeNewGroupsInternal(
    char** groups,
    folly::Range<const vector_size_t*> indices) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  SCOPE_FAIL {
    if (instance_)
      instance_->invalidate();
  };

  for (auto index : indices) {
    setHandle(groups[index], 0);
    if (reportsStateSize())
      folly::storeUnaligned<uint64_t>(
          groups[index] + offset_ + sizeof(uint32_t), 0);
    if (manifest_.rowApi && manifest_.defaultNullBehavior)
      setNull(groups[index]);
  }
  ensureInstance();
  VELOX_CHECK_LE(
      indices.size(),
      std::numeric_limits<uint32_t>::max(),
      "Too many Wasm UDAF groups in one create batch");
  auto bytes = instance_->invokeCount(
      manifest_.entrypoints.create, static_cast<uint32_t>(indices.size()));
  const size_t stride = reportsStateSize() ? sizeof(uint32_t) + sizeof(uint64_t)
                                           : sizeof(uint32_t);
  VELOX_CHECK_EQ(
      bytes.size(),
      indices.size() * stride,
      "Wasm UDAF create returned the wrong number of state handles");
  std::unordered_set<uint32_t> unique;
  for (size_t i = 0; i < indices.size(); ++i) {
    const auto stateHandle = folly::loadUnaligned<uint32_t>(
        reinterpret_cast<const uint8_t*>(bytes.data()) + i * stride);
    VELOX_CHECK_NE(
        stateHandle, 0, "Wasm UDAF create returned invalid state handle 0");
    VELOX_CHECK(
        unique.insert(stateHandle).second && !activeHandles_.count(stateHandle),
        "Wasm UDAF create returned duplicate state handle {}",
        stateHandle);
    activeHandles_.emplace(stateHandle, groups[indices[i]]);
    setHandle(groups[indices[i]], stateHandle);
  }
  if (reportsStateSize()) {
    std::unordered_map<uint32_t, char*> created;
    for (auto index : indices)
      created.emplace(handle(groups[index]), groups[index]);
    applyUsage(created, bytes);
  }
}

SelectivityVector WasmAggregate::filterRawRows(
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args) const {
  SelectivityVector filtered(rows);
  if (!manifest_.defaultNullBehavior) {
    return filtered;
  }
  std::vector<DecodedVector> decoded(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    decoded[i].decode(*args[i], rows);
  }
  rows.applyToSelected([&](vector_size_t row) {
    for (const auto& argument : decoded) {
      if (argument.isNullAt(row)) {
        filtered.setValid(row, false);
        break;
      }
    }
  });
  filtered.updateBounds();
  return filtered;
}

SelectivityVector WasmAggregate::filterIntermediateRows(
    const SelectivityVector& rows,
    const VectorPtr& intermediate) const {
  SelectivityVector filtered(rows);
  if (manifest_.rowApi && !manifest_.defaultNullBehavior)
    return filtered;
  DecodedVector decoded(*intermediate, rows);
  rows.applyToSelected([&](vector_size_t row) {
    if (decoded.isNullAt(row)) {
      filtered.setValid(row, false);
    }
  });
  filtered.updateBounds();
  return filtered;
}

void WasmAggregate::requireNoOutput(
    std::string_view entrypoint,
    std::string output) const {
  VELOX_CHECK(
      output.empty(),
      "Wasm UDAF '{}' returned {} unexpected output bytes",
      entrypoint,
      output.size());
}

void WasmAggregate::invokeUpdate(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  SCOPE_FAIL {
    if (instance_)
      instance_->invalidate();
  };

  validateRawConstants();
  auto filtered = filterRawRows(rows, args);
  if (!filtered.hasSelections()) {
    return;
  }
  std::vector<uint32_t> handles(filtered.end());
  filtered.applyToSelected(
      [&](vector_size_t row) { handles[row] = handle(groups[row]); });
  auto batch = gatherToArrowIpcWithStateHandles(
      filtered, handles, args, allocator_->pool(), manifest_.rowApi);
  VELOX_CHECK_EQ(
      batch.rowCount,
      filtered.countSelected(),
      "Wasm UDAF update unexpectedly changed row selection");
  ensureInstance();
  auto output = instance_->invoke(manifest_.entrypoints.update, batch.input);
  if (reportsStateSize()) {
    std::unordered_map<uint32_t, char*> affected;
    filtered.applyToSelected([&](vector_size_t row) {
      affected.emplace(handles[row], groups[row]);
    });
    applyUsage(affected, output);
  } else {
    requireNoOutput(manifest_.entrypoints.update, std::move(output));
  }
  if (manifest_.rowApi && manifest_.defaultNullBehavior) {
    filtered.applyToSelected(
        [&](vector_size_t row) { clearNull(groups[row]); });
  }
}

void WasmAggregate::addRawInput(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  invokeUpdate(groups, rows, args);
}

void WasmAggregate::addIntermediateResults(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  SCOPE_FAIL {
    if (instance_)
      instance_->invalidate();
  };

  VELOX_CHECK_EQ(args.size(), 1, "Wasm UDAF merge expects one argument");
  auto filtered = filterIntermediateRows(rows, args[0]);
  if (!filtered.hasSelections()) {
    return;
  }
  std::vector<uint32_t> handles(filtered.end());
  filtered.applyToSelected(
      [&](vector_size_t row) { handles[row] = handle(groups[row]); });
  auto batch = gatherToArrowIpcWithStateHandles(
      filtered,
      handles,
      args,
      allocator_->pool(),
      manifest_.rowApi,
      manifest_.rowApi ? manifest_.intermediateType.type : nullptr);
  ensureInstance();
  auto output = instance_->invoke(manifest_.entrypoints.merge, batch.input);
  if (reportsStateSize()) {
    std::unordered_map<uint32_t, char*> affected;
    filtered.applyToSelected([&](vector_size_t row) {
      affected.emplace(handles[row], groups[row]);
    });
    applyUsage(affected, output);
  } else {
    requireNoOutput(manifest_.entrypoints.merge, std::move(output));
  }
  if (manifest_.rowApi && manifest_.defaultNullBehavior) {
    filtered.applyToSelected(
        [&](vector_size_t row) { clearNull(groups[row]); });
  }
}

void WasmAggregate::invokeSingleUpdate(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    std::string_view entrypoint) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  SCOPE_FAIL {
    if (instance_)
      instance_->invalidate();
  };

  if (entrypoint == manifest_.entrypoints.updateSingleGroup)
    validateRawConstants();
  SelectivityVector filtered =
      entrypoint == manifest_.entrypoints.updateSingleGroup
      ? filterRawRows(rows, args)
      : filterIntermediateRows(rows, args[0]);
  if (!filtered.hasSelections()) {
    return;
  }
  auto batch = gatherToArrowIpc(
      filtered,
      args,
      allocator_->pool(),
      manifest_.rowApi ? manifest_.returnType.type : nullptr,
      std::nullopt,
      manifest_.rowApi && entrypoint == manifest_.entrypoints.mergeSingleGroup
          ? manifest_.intermediateType.type
          : nullptr);
  ensureInstance();
  auto output =
      instance_->invokeSingleGroup(entrypoint, handle(group), batch.input);
  if (reportsStateSize())
    applyUsage({{handle(group), group}}, output);
  else
    requireNoOutput(entrypoint, std::move(output));
  if (manifest_.rowApi && manifest_.defaultNullBehavior)
    clearNull(group);
}

void WasmAggregate::addSingleGroupRawInput(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  invokeSingleUpdate(
      group, rows, args, manifest_.entrypoints.updateSingleGroup);
}

void WasmAggregate::addSingleGroupIntermediateResults(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  VELOX_CHECK_EQ(args.size(), 1, "Wasm UDAF merge expects one argument");
  invokeSingleUpdate(group, rows, args, manifest_.entrypoints.mergeSingleGroup);
}

void WasmAggregate::extract(
    char** groups,
    int32_t numGroups,
    const TypePtr& type,
    std::string_view entrypoint,
    VectorPtr* result) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  SCOPE_FAIL {
    if (instance_)
      instance_->invalidate();
  };

  VELOX_CHECK_GE(numGroups, 0, "Negative Wasm UDAF group count");
  SelectivityVector rows(numGroups, false);
  std::vector<uint32_t> handles(numGroups);
  for (vector_size_t row = 0; row < numGroups; ++row) {
    if (manifest_.rowApi && manifest_.defaultNullBehavior &&
        isNull(groups[row]))
      continue;
    handles[row] = handle(groups[row]);
    rows.setValid(row, true);
  }
  rows.updateBounds();
  if (!rows.hasSelections()) {
    *result = BaseVector::create(type, numGroups, allocator_->pool());
    for (vector_size_t row = 0; row < numGroups; ++row)
      (*result)->setNull(row, true);
    return;
  }
  auto batch =
      gatherToArrowIpcWithStateHandles(rows, handles, {}, allocator_->pool());
  ensureInstance();
  auto output = instance_->invoke(entrypoint, batch.input);
  if (reportsStateSize()) {
    std::unordered_map<uint32_t, char*> affected;
    rows.applyToSelected([&](vector_size_t row) {
      affected.emplace(handles[row], groups[row]);
    });
    const auto reportBytes =
        affected.size() * (sizeof(uint32_t) + sizeof(uint64_t));
    VELOX_CHECK_GE(
        output.size(), reportBytes, "Truncated Wasm extraction state report");
    applyUsage(affected, std::string_view(output).substr(0, reportBytes));
    output.erase(0, reportBytes);
  }
  auto compact = decodeOwnedArrowIpcResult(
      std::move(output), type, rows.countSelected(), allocator_->pool());
  if (rows.isAllSelected()) {
    *result = std::move(compact);
    return;
  }
  *result = BaseVector::create(type, numGroups, allocator_->pool());
  for (vector_size_t row = 0; row < numGroups; ++row)
    (*result)->setNull(row, true);
  scatterArrowResult(rows, compact, type, allocator_->pool(), *result);
}

void WasmAggregate::extractValues(
    char** groups,
    int32_t numGroups,
    VectorPtr* result) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  extract(
      groups,
      numGroups,
      manifest_.returnType.type,
      manifest_.entrypoints.finalize,
      result);
}

void WasmAggregate::extractAccumulators(
    char** groups,
    int32_t numGroups,
    VectorPtr* result) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  extract(
      groups,
      numGroups,
      manifest_.intermediateType.type,
      manifest_.entrypoints.serialize,
      result);
}

void WasmAggregate::destroyInternal(folly::Range<char**> groups) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (groups.empty()) {
    return;
  }
  try {
    SelectivityVector rows(groups.size(), false);
    std::vector<uint32_t> handles(groups.size());
    for (vector_size_t row = 0; row < groups.size(); ++row) {
      const auto stateHandle =
          folly::loadUnaligned<uint32_t>(groups[row] + offset_);
      if (stateHandle != 0) {
        handles[row] = stateHandle;
        rows.setValid(row, true);
      }
    }
    rows.updateBounds();
    if (!rows.hasSelections()) {
      return;
    }
    auto batch =
        gatherToArrowIpcWithStateHandles(rows, handles, {}, allocator_->pool());
    VELOX_CHECK_NOT_NULL(instance_, "Wasm Store already discarded");
    requireNoOutput(
        manifest_.entrypoints.destroy,
        instance_->invoke(manifest_.entrypoints.destroy, batch.input));
  } catch (const std::exception& error) {
    try {
      LOG(WARNING) << "Discarding Wasm aggregate Store after cleanup failure: "
                   << error.what();
    } catch (...) {
      // Diagnostic allocation must not make native destructor cleanup throw.
    }
    if (instance_)
      instance_->invalidate();
  } catch (...) {
    try {
      LOG(WARNING) << "Discarding Wasm aggregate Store after cleanup failure";
    } catch (...) {
    }
    if (instance_)
      instance_->invalidate();
  }
  for (auto* group : groups) {
    activeHandles_.erase(folly::loadUnaligned<uint32_t>(group + offset_));
    setHandle(group, 0);
    if (reportsStateSize()) {
      stateBytes_ -= stateSize(group);
      setStateSize(group, 0);
    }
  }
  // Once the last state is retired, no guest accumulator needs the Store.
  // Native spill/flush can now release actual linear pages before rebuilding
  // groups. Keep an invalid instance poisoned after any cleanup failure.
  if (activeHandles_.empty() && instance_ && instance_->linearMemoryBytes() > 0)
    instance_.reset();
}

} // namespace facebook::velox::functions::wasm
