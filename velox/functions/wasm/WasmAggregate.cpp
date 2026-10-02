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

#include <folly/lang/Bits.h>

#include <limits>
#include <unordered_set>

#include "velox/common/base/Exceptions.h"
#include "velox/functions/wasm/ArrowIpc.h"

namespace facebook::velox::functions::wasm {

WasmAggregate::WasmAggregate(
    TypePtr resultType,
    AggregateManifest manifest,
    std::shared_ptr<WasmModule> module)
    : Aggregate(std::move(resultType)),
      manifest_(std::move(manifest)),
      instance_(
          std::move(module),
          manifest_.entrypoints.create,
          {manifest_.entrypoints.destroy,
           manifest_.entrypoints.update,
           manifest_.entrypoints.serialize,
           manifest_.entrypoints.merge,
           manifest_.entrypoints.finalize},
          {manifest_.entrypoints.updateSingleGroup,
           manifest_.entrypoints.mergeSingleGroup}) {}

int32_t WasmAggregate::accumulatorFixedWidthSize() const {
  return sizeof(uint32_t);
}

bool WasmAggregate::accumulatorUsesExternalMemory() const {
  return true;
}

bool WasmAggregate::isFixedSize() const {
  return true;
}

uint32_t WasmAggregate::handle(char* group) const {
  return folly::loadUnaligned<uint32_t>(group + offset_);
}

void WasmAggregate::setHandle(char* group, uint32_t stateHandle) const {
  folly::storeUnaligned<uint32_t>(group + offset_, stateHandle);
}

void WasmAggregate::initializeNewGroupsInternal(
    char** groups,
    folly::Range<const vector_size_t*> indices) {
  VELOX_USER_CHECK_LE(
      indices.size(),
      std::numeric_limits<uint32_t>::max(),
      "Too many Wasm UDAF groups in one create batch");
  auto bytes = instance_.invokeCount(
      manifest_.entrypoints.create, static_cast<uint32_t>(indices.size()));
  VELOX_USER_CHECK_EQ(
      bytes.size(),
      indices.size() * sizeof(uint32_t),
      "Wasm UDAF create returned the wrong number of state handles");
  std::unordered_set<uint32_t> unique;
  for (size_t i = 0; i < indices.size(); ++i) {
    const auto stateHandle = folly::loadUnaligned<uint32_t>(
        reinterpret_cast<const uint8_t*>(bytes.data()) + i * sizeof(uint32_t));
    VELOX_USER_CHECK_NE(
        stateHandle, 0, "Wasm UDAF create returned invalid state handle 0");
    VELOX_USER_CHECK(
        unique.insert(stateHandle).second,
        "Wasm UDAF create returned duplicate state handle {}",
        stateHandle);
    setHandle(groups[indices[i]], stateHandle);
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
  VELOX_USER_CHECK(
      output.empty(),
      "Wasm UDAF '{}' returned {} unexpected output bytes",
      entrypoint,
      output.size());
}

void WasmAggregate::invokeUpdate(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args) {
  auto filtered = filterRawRows(rows, args);
  if (!filtered.hasSelections()) {
    return;
  }
  std::vector<uint32_t> handles(filtered.end());
  filtered.applyToSelected(
      [&](vector_size_t row) { handles[row] = handle(groups[row]); });
  auto batch = gatherToArrowIpcWithStateHandles(
      filtered, handles, args, allocator_->pool());
  VELOX_USER_CHECK_EQ(
      batch.rowCount,
      filtered.countSelected(),
      "Wasm UDAF update unexpectedly changed row selection");
  requireNoOutput(
      manifest_.entrypoints.update,
      instance_.invoke(manifest_.entrypoints.update, batch.input));
}

void WasmAggregate::addRawInput(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  invokeUpdate(groups, rows, args);
}

void WasmAggregate::addIntermediateResults(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  VELOX_USER_CHECK_EQ(args.size(), 1, "Wasm UDAF merge expects one argument");
  auto filtered = filterIntermediateRows(rows, args[0]);
  if (!filtered.hasSelections()) {
    return;
  }
  std::vector<uint32_t> handles(filtered.end());
  filtered.applyToSelected(
      [&](vector_size_t row) { handles[row] = handle(groups[row]); });
  auto batch = gatherToArrowIpcWithStateHandles(
      filtered, handles, args, allocator_->pool());
  requireNoOutput(
      manifest_.entrypoints.merge,
      instance_.invoke(manifest_.entrypoints.merge, batch.input));
}

void WasmAggregate::invokeSingleUpdate(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    std::string_view entrypoint) {
  SelectivityVector filtered =
      entrypoint == manifest_.entrypoints.updateSingleGroup
      ? filterRawRows(rows, args)
      : filterIntermediateRows(rows, args[0]);
  if (!filtered.hasSelections()) {
    return;
  }
  auto batch = gatherToArrowIpc(filtered, args, allocator_->pool());
  requireNoOutput(
      entrypoint,
      instance_.invokeSingleGroup(entrypoint, handle(group), batch.input));
}

void WasmAggregate::addSingleGroupRawInput(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  invokeSingleUpdate(
      group, rows, args, manifest_.entrypoints.updateSingleGroup);
}

void WasmAggregate::addSingleGroupIntermediateResults(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  VELOX_USER_CHECK_EQ(args.size(), 1, "Wasm UDAF merge expects one argument");
  invokeSingleUpdate(group, rows, args, manifest_.entrypoints.mergeSingleGroup);
}

void WasmAggregate::extract(
    char** groups,
    int32_t numGroups,
    const TypePtr& type,
    std::string_view entrypoint,
    VectorPtr* result) {
  SelectivityVector rows(numGroups);
  std::vector<uint32_t> handles(numGroups);
  for (vector_size_t row = 0; row < numGroups; ++row) {
    handles[row] = handle(groups[row]);
  }
  auto batch =
      gatherToArrowIpcWithStateHandles(rows, handles, {}, allocator_->pool());
  *result = decodeArrowIpcResult(
      instance_.invoke(entrypoint, batch.input),
      type,
      numGroups,
      allocator_->pool());
}

void WasmAggregate::extractValues(
    char** groups,
    int32_t numGroups,
    VectorPtr* result) {
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
  extract(
      groups, numGroups, VARBINARY(), manifest_.entrypoints.serialize, result);
}

void WasmAggregate::destroyInternal(folly::Range<char**> groups) {
  if (groups.empty()) {
    return;
  }
  SelectivityVector rows(groups.size(), false);
  std::vector<uint32_t> handles(groups.size());
  for (vector_size_t row = 0; row < groups.size(); ++row) {
    const auto stateHandle = handle(groups[row]);
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
  requireNoOutput(
      manifest_.entrypoints.destroy,
      instance_.invoke(manifest_.entrypoints.destroy, batch.input));
  for (auto* group : groups) {
    setHandle(group, 0);
  }
}

} // namespace facebook::velox::functions::wasm
