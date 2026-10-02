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

#include <filesystem>
#include <string>
#include <vector>

#include "velox/type/Type.h"

namespace facebook::velox::functions::wasm {

struct ManifestType {
  TypePtr type;
  bool nullable;
};

struct ScalarManifest {
  uint32_t abiVersion;
  std::filesystem::path wasmPath;
  std::string name;
  std::string entrypoint;
  std::vector<ManifestType> arguments;
  ManifestType returnType;
  bool deterministic;
  bool defaultNullBehavior;
};

struct AggregateEntrypoints {
  std::string create;
  std::string destroy;
  std::string update;
  std::string updateSingleGroup;
  std::string serialize;
  std::string merge;
  std::string mergeSingleGroup;
  std::string finalize;
};

struct AggregateManifest {
  uint32_t abiVersion;
  std::filesystem::path wasmPath;
  std::string name;
  AggregateEntrypoints entrypoints;
  std::vector<ManifestType> arguments;
  ManifestType intermediateType;
  ManifestType returnType;
  bool orderSensitive;
  bool ignoreDuplicates;
  bool defaultNullBehavior;
};

struct EmbeddedManifests {
  std::vector<ScalarManifest> scalars;
  std::vector<AggregateManifest> aggregates;
};

/// Reads all Velox UDF declarations from the velox.udf.v1 custom section.
EmbeddedManifests loadEmbeddedManifests(const std::filesystem::path& wasmPath);

} // namespace facebook::velox::functions::wasm
