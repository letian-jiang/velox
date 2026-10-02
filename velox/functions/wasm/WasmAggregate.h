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

#include "velox/exec/Aggregate.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Runtime.h"

namespace facebook::velox::functions::wasm {

class WasmAggregate final : public exec::Aggregate {
 public:
  WasmAggregate(
      TypePtr resultType,
      AggregateManifest manifest,
      std::shared_ptr<WasmModule> module);

  int32_t accumulatorFixedWidthSize() const override;
  bool accumulatorUsesExternalMemory() const override;
  bool isFixedSize() const override;

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
  WasmInstance instance_;
};

} // namespace facebook::velox::functions::wasm
