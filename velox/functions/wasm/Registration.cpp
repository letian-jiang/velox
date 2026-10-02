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

#include "velox/functions/wasm/Registration.h"

#include <map>

#include "velox/exec/Aggregate.h"
#include "velox/expression/FunctionSignature.h"
#include "velox/expression/VectorFunction.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/WasmAggregate.h"
#include "velox/functions/wasm/WasmVectorFunction.h"

namespace facebook::velox::functions::wasm {
namespace {

std::string signatureType(const TypePtr& type) {
  if (type->isDate()) {
    return "date";
  }
  switch (type->kind()) {
    case TypeKind::BOOLEAN:
      return "boolean";
    case TypeKind::TINYINT:
      return "tinyint";
    case TypeKind::SMALLINT:
      return "smallint";
    case TypeKind::INTEGER:
      return "integer";
    case TypeKind::BIGINT:
      return "bigint";
    case TypeKind::REAL:
      return "real";
    case TypeKind::DOUBLE:
      return "double";
    case TypeKind::VARCHAR:
      return "varchar";
    case TypeKind::VARBINARY:
      return "varbinary";
    case TypeKind::TIMESTAMP:
      return "timestamp";
    case TypeKind::ARRAY:
      return "array(" + signatureType(type->childAt(0)) + ")";
    case TypeKind::MAP:
      return "map(" + signatureType(type->childAt(0)) + "," +
          signatureType(type->childAt(1)) + ")";
    case TypeKind::ROW: {
      std::string result = "row(";
      for (size_t index = 0; index < type->size(); ++index) {
        if (index != 0) {
          result += ",";
        }
        result += signatureType(type->childAt(index));
      }
      return result + ")";
    }
    default:
      VELOX_UNREACHABLE("Unsupported Wasm UDF signature type");
  }
}

} // namespace

namespace {

bool registerScalar(
    std::vector<ScalarManifest> manifests,
    std::shared_ptr<WasmModule> module,
    bool overwrite) {
  const auto& first = manifests.front();
  std::vector<exec::FunctionSignaturePtr> signatures;
  signatures.reserve(manifests.size());
  for (const auto& manifest : manifests) {
    VELOX_USER_CHECK_EQ(
        manifest.deterministic,
        first.deterministic,
        "Wasm scalar overloads of '{}' must agree on determinism",
        first.name);
    VELOX_USER_CHECK_EQ(
        manifest.defaultNullBehavior,
        first.defaultNullBehavior,
        "Wasm scalar overloads of '{}' must agree on null behavior",
        first.name);
    exec::FunctionSignatureBuilder signature;
    signature.returnType(signatureType(manifest.returnType.type));
    for (const auto& argument : manifest.arguments) {
      signature.argumentType(signatureType(argument.type));
    }
    signatures.push_back(signature.build());
  }

  exec::VectorFunctionMetadata metadata;
  metadata.deterministic = first.deterministic;
  metadata.defaultNullBehavior = first.defaultNullBehavior;
  return exec::registerStatefulVectorFunction(
      first.name,
      std::move(signatures),
      [module = std::move(module), manifests = std::move(manifests)](
          const std::string&,
          const std::vector<exec::VectorFunctionArg>& inputArgs,
          const core::QueryConfig&) {
        for (const auto& manifest : manifests) {
          if (inputArgs.size() != manifest.arguments.size()) {
            continue;
          }
          bool matches = true;
          for (size_t i = 0; i < inputArgs.size(); ++i) {
            if (!inputArgs[i].type->equivalent(*manifest.arguments[i].type)) {
              matches = false;
              break;
            }
          }
          if (matches) {
            return std::make_shared<WasmVectorFunction>(
                module, manifest.entrypoint);
          }
        }
        VELOX_USER_FAIL("No Wasm scalar overload matches the input types");
      },
      metadata,
      overwrite);
}

bool registerAggregate(
    AggregateManifest manifest,
    std::shared_ptr<WasmModule> module,
    bool overwrite) {
  exec::AggregateFunctionSignatureBuilder signature;
  signature.returnType(signatureType(manifest.returnType.type));
  signature.intermediateType("varbinary");
  for (const auto& argument : manifest.arguments) {
    signature.argumentType(signatureType(argument.type));
  }

  exec::AggregateFunctionMetadata metadata;
  metadata.orderSensitive = manifest.orderSensitive;
  metadata.ignoreDuplicates = manifest.ignoreDuplicates;
  auto name = manifest.name;
  auto registered = exec::registerAggregateFunction(
      name,
      {signature.build()},
      [module = std::move(module), manifest = std::move(manifest)](
          core::AggregationNode::Step step,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType,
          const core::QueryConfig& /*config*/) {
        const bool rawInput = step == core::AggregationNode::Step::kPartial ||
            step == core::AggregationNode::Step::kSingle;
        const auto expectedArgumentCount =
            rawInput ? manifest.arguments.size() : 1;
        VELOX_USER_CHECK_EQ(
            argTypes.size(),
            expectedArgumentCount,
            "Wasm UDAF '{}' received the wrong argument count",
            manifest.name);
        for (size_t i = 0; i < argTypes.size(); ++i) {
          const auto& expectedType = rawInput ? manifest.arguments[i].type
                                              : manifest.intermediateType.type;
          VELOX_USER_CHECK(
              argTypes[i]->equivalent(*expectedType),
              "Wasm UDAF '{}' argument {} has type '{}', expected '{}'",
              manifest.name,
              i,
              argTypes[i]->toString(),
              expectedType->toString());
        }
        const bool partialOutput =
            step == core::AggregationNode::Step::kPartial ||
            step == core::AggregationNode::Step::kIntermediate;
        const auto& expectedResult = partialOutput
            ? manifest.intermediateType.type
            : manifest.returnType.type;
        VELOX_USER_CHECK(
            resultType->equivalent(*expectedResult),
            "Wasm UDAF '{}' result has type '{}', expected '{}'",
            manifest.name,
            resultType->toString(),
            expectedResult->toString());
        return std::make_unique<WasmAggregate>(resultType, manifest, module);
      },
      metadata,
      false,
      overwrite);
  return registered.mainFunction;
}

} // namespace

size_t registerWasmModule(
    const std::filesystem::path& wasmPath,
    bool overwrite) {
  auto manifests = loadEmbeddedManifests(wasmPath);
  auto module = WasmModule::compile(wasmPath);
  // Validate every export before changing Velox's function registry.
  for (const auto& manifest : manifests.scalars) {
    WasmInstance validationInstance(module, manifest.entrypoint);
  }
  for (const auto& manifest : manifests.aggregates) {
    const auto& exports = manifest.entrypoints;
    WasmInstance validationInstance(
        module,
        exports.create,
        {exports.destroy,
         exports.update,
         exports.serialize,
         exports.merge,
         exports.finalize},
        {exports.updateSingleGroup, exports.mergeSingleGroup});
  }
  size_t registered = 0;
  std::map<std::string, std::vector<ScalarManifest>> scalarGroups;
  for (auto& manifest : manifests.scalars) {
    auto name = manifest.name;
    scalarGroups[name].push_back(std::move(manifest));
  }
  for (auto& [name, overloads] : scalarGroups) {
    registered += registerScalar(std::move(overloads), module, overwrite);
  }
  for (auto& manifest : manifests.aggregates) {
    registered += registerAggregate(std::move(manifest), module, overwrite);
  }
  return registered;
}

} // namespace facebook::velox::functions::wasm
