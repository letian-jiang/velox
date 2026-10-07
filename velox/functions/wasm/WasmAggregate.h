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

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "velox/exec/Aggregate.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Runtime.h"

namespace facebook::velox::functions::wasm {

class WasmAggregate final : public exec::Aggregate {
 public:
  WasmAggregate(
      TypePtr resultType,
      AggregateManifest manifest,
      std::shared_ptr<WasmModule> module,
      const WasmOptions& options = {},
      std::vector<TypePtr> inputTypes = {},
      std::unordered_map<std::string, std::string> config = {},
      std::vector<TypePtr> lambdaTypes = {},
      bool rawInputTypes = true);

  void setConstantInputs(const std::vector<VectorPtr>& inputs) override;
  bool supportsToIntermediate() const override;
  void toIntermediate(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      VectorPtr& result) const override;

  int32_t accumulatorFixedWidthSize() const override;
  bool accumulatorUsesExternalMemory() const override;
  bool isFixedSize() const override;
  bool supportsCompact() const override;
  uint64_t compact(folly::Range<char**> groups) override;

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override;
  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override;
  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override;
  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override;
  void extractValues(char** groups, int32_t numGroups, VectorPtr* result)
      override;
  void extractAccumulators(char** groups, int32_t numGroups, VectorPtr* result)
      override;

 protected:
  void initializeNewGroupsInternal(
      char** groups,
      folly::Range<const vector_size_t*> indices) override;
  void destroyInternal(folly::Range<char**> groups) override;

 private:
  ArrowIpcInput
  evaluateLambda(uint32_t index, std::string_view input, uint32_t rows) const;
  bool reportsStateSize() const;
  void applyUsage(
      const std::unordered_map<uint32_t, char*>& groups,
      std::string_view report);
  uint64_t stateSize(char* group) const;
  void setStateSize(char* group, uint64_t bytes);
  void ensureInstance() const;
  void validateRawConstants() const;
  uint32_t handle(char* group) const;
  void setHandle(char* group, uint32_t handle) const;
  SelectivityVector filterRawRows(
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args) const;
  SelectivityVector filterIntermediateRows(
      const SelectivityVector& rows,
      const VectorPtr& intermediate) const;
  void invokeUpdate(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args);
  void invokeSingleUpdate(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      std::string_view entrypoint);
  void extract(
      char** groups,
      int32_t numGroups,
      const TypePtr& type,
      std::string_view entrypoint,
      VectorPtr* result);
  void requireNoOutput(std::string_view entrypoint, std::string output) const;

  AggregateManifest manifest_;
  std::shared_ptr<WasmModule> module_;
  WasmOptions options_;
  mutable std::unique_ptr<WasmInstance> instance_;
  std::vector<TypePtr> inputTypes_;
  const bool rawInputTypes_;
  std::vector<bool> rawConstantFlags_;
  std::vector<TypePtr> lambdaTypes_;
  mutable std::vector<std::shared_ptr<exec::ExprSet>> lambdaExprSets_;
  std::vector<VectorPtr> constantInputs_;
  std::unordered_map<std::string, std::string> config_;
  std::unordered_map<uint32_t, char*> activeHandles_;
  mutable uint64_t baseMemoryBytes_{0};
  bool failed_{false};
  uint64_t stateBytes_{0};
  // extractAccumulators may run concurrently for spill. Serialize the entire
  // bridge operation, including lazy instance creation and failure handling.
  mutable std::recursive_mutex mutex_;
};

} // namespace facebook::velox::functions::wasm
