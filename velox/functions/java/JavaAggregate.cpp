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

#include "velox/functions/java/JavaAggregate.h"

#include <folly/lang/Bits.h>

#include <unordered_set>

#include "velox/functions/java/ArrowIpc.h"

namespace facebook::velox::functions::java {

JavaAggregate::JavaAggregate(TypePtr resultType, AggregateManifest manifest)
    : Aggregate(std::move(resultType)),
      manifest_(std::move(manifest)),
      instance_(manifest_.jarPath, manifest_.implementationClass, true) {}

int32_t JavaAggregate::accumulatorFixedWidthSize() const {
  return sizeof(uint32_t);
}

bool JavaAggregate::accumulatorUsesExternalMemory() const {
  return true;
}

bool JavaAggregate::isFixedSize() const {
  return true;
}

uint32_t JavaAggregate::handle(char* group) const {
  return folly::loadUnaligned<uint32_t>(group + offset_);
}

void JavaAggregate::setHandle(char* group, uint32_t stateHandle) const {
  folly::storeUnaligned<uint32_t>(group + offset_, stateHandle);
}

void JavaAggregate::initializeNewGroupsInternal(
    char** groups,
    folly::Range<const vector_size_t*> indices) {
  auto handles = instance_.create(indices.size());
  VELOX_USER_CHECK_EQ(
      handles.size(),
      indices.size(),
      "Java UDAF create returned the wrong number of state handles");
  std::unordered_set<uint32_t> unique;
  for (size_t i = 0; i < indices.size(); ++i) {
    const auto stateHandle = handles[i];
    VELOX_USER_CHECK_NE(
        stateHandle, 0, "Java UDAF create returned invalid state handle 0");
    VELOX_USER_CHECK(
        unique.insert(stateHandle).second,
        "Java UDAF create returned duplicate state handle {}",
        stateHandle);
    setHandle(groups[indices[i]], stateHandle);
  }
}

SelectivityVector JavaAggregate::filterRawRows(
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

SelectivityVector JavaAggregate::filterIntermediateRows(
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

void JavaAggregate::invokeUpdate(
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
  instance_.update(batch.ipc);
}

void JavaAggregate::addRawInput(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  invokeUpdate(groups, rows, args);
}

void JavaAggregate::addIntermediateResults(
    char** groups,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  VELOX_USER_CHECK_EQ(args.size(), 1, "Java UDAF merge expects one argument");
  auto filtered = filterIntermediateRows(rows, args[0]);
  if (!filtered.hasSelections()) {
    return;
  }
  std::vector<uint32_t> handles(filtered.end());
  filtered.applyToSelected(
      [&](vector_size_t row) { handles[row] = handle(groups[row]); });
  auto batch = gatherToArrowIpcWithStateHandles(
      filtered, handles, args, allocator_->pool());
  instance_.merge(batch.ipc);
}

void JavaAggregate::invokeSingleUpdate(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool intermediate) {
  auto filtered = intermediate ? filterIntermediateRows(rows, args[0])
                               : filterRawRows(rows, args);
  if (!filtered.hasSelections()) {
    return;
  }
  auto batch = gatherToArrowIpc(filtered, args, allocator_->pool());
  if (intermediate) {
    instance_.mergeSingleGroup(handle(group), batch.ipc);
  } else {
    instance_.updateSingleGroup(handle(group), batch.ipc);
  }
}

void JavaAggregate::addSingleGroupRawInput(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  invokeSingleUpdate(group, rows, args, false);
}

void JavaAggregate::addSingleGroupIntermediateResults(
    char* group,
    const SelectivityVector& rows,
    const std::vector<VectorPtr>& args,
    bool /*mayPushdown*/) {
  VELOX_USER_CHECK_EQ(args.size(), 1, "Java UDAF merge expects one argument");
  invokeSingleUpdate(group, rows, args, true);
}

void JavaAggregate::extract(
    char** groups,
    int32_t numGroups,
    const TypePtr& type,
    bool accumulators,
    VectorPtr* result) {
  SelectivityVector rows(numGroups);
  std::vector<uint32_t> handles(numGroups);
  for (vector_size_t row = 0; row < numGroups; ++row) {
    handles[row] = handle(groups[row]);
  }
  auto batch =
      gatherToArrowIpcWithStateHandles(rows, handles, {}, allocator_->pool());
  auto output = accumulators ? instance_.serialize(batch.ipc)
                             : instance_.finish(batch.ipc);
  *result = decodeArrowIpcResult(output, type, numGroups, allocator_->pool());
}

void JavaAggregate::extractValues(
    char** groups,
    int32_t numGroups,
    VectorPtr* result) {
  extract(groups, numGroups, manifest_.returnType.type, false, result);
}

void JavaAggregate::extractAccumulators(
    char** groups,
    int32_t numGroups,
    VectorPtr* result) {
  extract(groups, numGroups, VARBINARY(), true, result);
}

void JavaAggregate::destroyInternal(folly::Range<char**> groups) {
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
  instance_.destroy(batch.ipc);
  for (auto* group : groups) {
    setHandle(group, 0);
  }
}

} // namespace facebook::velox::functions::java
