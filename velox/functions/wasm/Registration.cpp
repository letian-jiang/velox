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

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_set>
#include "velox/exec/WindowFunction.h"
#include "velox/exec/window/AggregateWindow.h"
#include "velox/expression/SimpleFunctionRegistry.h"
#include "velox/expression/SpecialFormRegistry.h"

#include "velox/exec/Aggregate.h"
#include "velox/exec/AggregateCompanionAdapter.h"
#include "velox/exec/AggregateCompanionSignatures.h"
#include "velox/expression/FunctionSignature.h"
#include "velox/expression/SignatureBinder.h"
#include "velox/expression/VectorFunction.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/TypeBridge.h"
#include "velox/functions/wasm/WasmAggregate.h"
#include "velox/functions/wasm/WasmVectorFunction.h"

namespace facebook::velox::functions::wasm {
namespace {

void validateAggregateValueType(const TypePtr& type) {
  if (findWasmTypeCodec(type))
    return;
  VELOX_USER_CHECK(
      !type->isFunction(),
      "Wasm UDAF lambda parameters require a host callback ABI");
  if (type->isOpaque()) {
    VELOX_USER_CHECK(
        findWasmTypeCodec(type),
        "Wasm UDAF OPAQUE state needs a serialization codec; invocation handles cannot be retained");
  }
  for (size_t i = 0; i < type->size(); ++i)
    validateAggregateValueType(type->childAt(i));
}

struct ScalarImplementation {
  ScalarManifest manifest;
  std::shared_ptr<WasmModule> module;
  WasmOptions options;
};
using ScalarCatalog = std::map<std::string, std::vector<ScalarImplementation>>;
ScalarCatalog& scalarCatalog() {
  static ScalarCatalog catalog;
  return catalog;
}
struct AggregateImplementation {
  AggregateManifest manifest;
  std::shared_ptr<WasmModule> module;
  WasmOptions options;
};
using AggregateCatalog =
    std::map<std::string, std::vector<AggregateImplementation>>;
AggregateCatalog& aggregateCatalog() {
  static AggregateCatalog catalog;
  return catalog;
}

// Companion names belong to the source aggregate, not to an independent module
// dispatch slot. Refresh the whole family when its overload set changes.
struct CompanionOwners {
  std::map<std::string, std::string> aggregates;
  std::map<std::string, std::string> vectors;
};
CompanionOwners& companionOwners() {
  static CompanionOwners owners;
  return owners;
}

class WasmCompanionExtract final : public exec::VectorFunction {
 public:
  explicit WasmCompanionExtract(
      std::function<std::shared_ptr<exec::VectorFunction>()> create)
      : create_(std::move(create)) {}

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& args,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const override {
    try {
      if (!function_)
        function_ = create_();
      function_->apply(rows, args, outputType, context, result);
    } catch (const VeloxUserError&) {
      // Native extract companions propagate whole-call exceptions. Dispose
      // failed temporary groups, and recreate if an API caller retries later.
      function_.reset();
      throw;
    } catch (const VeloxRuntimeError&) {
      function_.reset();
      throw;
    } catch (const std::exception& error) {
      function_.reset();
      VELOX_FAIL("Wasm companion extraction failure: {}", error.what());
    } catch (...) {
      function_.reset();
      VELOX_FAIL("Unknown Wasm companion extraction failure");
    }
  }

 private:
  std::function<std::shared_ptr<exec::VectorFunction>()> create_;
  mutable std::shared_ptr<exec::VectorFunction> function_;
};

std::mutex& registrationMutex() {
  static std::mutex mutex;
  return mutex;
}

// Prefer concrete overloads, then generic ones, just as Simple Functions do.
std::pair<int, int> overloadPriority(const exec::FunctionSignature& signature) {
  int concrete = 0;
  auto containsVariable = [&](auto&& self,
                              const exec::TypeSignature& type) -> bool {
    if (type.baseName() == "any") {
      return true;
    }
    if (auto it = signature.variables().find(type.baseName());
        it != signature.variables().end()) {
      return it->second.isTypeParameter();
    }
    ++concrete;
    // Precision and scale are integer parameters, not generic SQL types.
    if (type.baseName() == "decimal") {
      return false;
    }
    bool variable = false;
    for (const auto& child : type.parameters()) {
      variable |= self(self, child);
    }
    return variable;
  };
  bool generic = containsVariable(containsVariable, signature.returnType());
  bool variadicGeneric = false;
  for (const auto& type : signature.argumentTypes()) {
    variadicGeneric = containsVariable(containsVariable, type);
    generic |= variadicGeneric;
  }
  const int rank = signature.variableArity() && variadicGeneric ? 4
      : generic                                                 ? 3
      : signature.variableArity()                               ? 2
                                                                : 1;
  return {rank, -concrete};
}

exec::VectorFunctionEntry makeScalarEntry(
    std::vector<ScalarImplementation> implementations) {
  std::stable_sort(
      implementations.begin(),
      implementations.end(),
      [](const auto& a, const auto& b) {
        return overloadPriority(*a.manifest.signature) <
            overloadPriority(*b.manifest.signature);
      });
  std::vector<exec::FunctionSignaturePtr> signatures;
  signatures.reserve(implementations.size());
  std::vector<exec::VectorFunctionMetadata> signatureMetadata;
  for (const auto& implementation : implementations) {
    const auto& manifest = implementation.manifest;
    signatures.push_back(manifest.signature);
    exec::VectorFunctionMetadata metadata;
    metadata.deterministic = manifest.deterministic;
    metadata.defaultNullBehavior = manifest.defaultNullBehavior;
    signatureMetadata.push_back(metadata);
  }
  exec::VectorFunctionMetadata metadata = signatureMetadata.front();
  metadata.owner = "velox.wasm";
  for (const auto& properties : signatureMetadata) {
    metadata.deterministic &= properties.deterministic;
    metadata.defaultNullBehavior &= properties.defaultNullBehavior;
  }
  return {
      std::move(signatures),
      [implementations = std::move(implementations)](
          const std::string&,
          const std::vector<exec::VectorFunctionArg>& inputArgs,
          const core::QueryConfig& queryConfig) {
        std::vector<TypePtr> types;
        for (const auto& arg : inputArgs) {
          types.push_back(arg.type);
        }
        for (const auto& implementation : implementations) {
          const auto& manifest = implementation.manifest;
          exec::SignatureBinder binder(
              *manifest.signature, types, TypeCoercer::defaults());
          if (!binder.tryBind() || !binder.tryResolveReturnType()) {
            continue;
          }
          const auto& constants = manifest.signature->constantArguments();
          for (size_t i = 0; i < inputArgs.size(); ++i) {
            const size_t index = std::min(i, constants.size() - 1);
            VELOX_USER_CHECK(
                !constants[index] || inputArgs[i].constantValue,
                "Wasm scalar '{}' argument {} must be constant",
                manifest.name,
                i);
          }
          ScalarInitialization initialization;
          initialization.entrypoint = manifest.initializeEntrypoint;
          if (!initialization.entrypoint.empty()) {
            initialization.arguments =
                std::vector<exec::VectorFunctionArg>(inputArgs);
            const auto config = queryConfig.rawConfigsCopy();
            for (const auto& key : manifest.configKeys) {
              if (auto it = config.find(key); it != config.end()) {
                initialization.config.emplace(key, it->second);
              } else {
                for (const auto& property :
                     core::QueryConfig::registeredProperties()) {
                  if (property.name == key && property.defaultValue) {
                    initialization.config.emplace(key, *property.defaultValue);
                    break;
                  }
                }
              }
            }
          }
          return std::make_shared<WasmVectorFunction>(
              implementation.module,
              manifest.entrypoint,
              std::move(initialization),
              manifest.rowApi,
              implementation.options,
              manifest.hasAscii);
        }
        VELOX_USER_FAIL("No Wasm scalar overload matches the input types");
      },
      metadata,
      std::move(signatureMetadata)};
}

exec::AggregateFunctionEntry makeAggregateImplementationEntry(
    AggregateManifest manifest,
    std::shared_ptr<WasmModule> module,
    WasmOptions options) {
  const auto signature = manifest.signature;
  exec::AggregateFunctionMetadata metadata;
  metadata.orderSensitive = manifest.orderSensitive;
  metadata.ignoreDuplicates = manifest.ignoreDuplicates;
  return {
      {signature},
      [module = std::move(module),
       manifest = std::move(manifest),
       options,
       signature](
          core::AggregationNode::Step step,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType,
          const core::QueryConfig& queryConfig) {
        const bool rawInput = exec::isRawInput(step);
        const bool partialOutput = exec::isPartialOutput(step);
        // Bind final/intermediate input against the declared intermediate type.
        // A typed intermediate preserves generic variables across aggregation
        // stages, whereas a varbinary payload cannot recover their types.
        std::unique_ptr<exec::FunctionSignature> mergeSignature;
        if (!rawInput) {
          mergeSignature = std::make_unique<exec::FunctionSignature>(
              exec::usedTypeVariables(
                  {signature->intermediateType(), signature->returnType()},
                  signature->variables()),
              signature->returnType(),
              std::vector<exec::TypeSignature>{signature->intermediateType()},
              std::vector<bool>{false},
              false);
        }
        const exec::FunctionSignature& binding =
            rawInput ? *signature : *mergeSignature;
        exec::SignatureBinder binder(
            binding, argTypes, TypeCoercer::defaults());
        VELOX_USER_CHECK(
            binder.tryBind(),
            "Wasm UDAF '{}' received incompatible arguments",
            manifest.name);
        auto finalType = binder.tryResolveReturnType();
        auto intermediateType =
            binder.tryResolveType(signature->intermediateType());
        VELOX_USER_CHECK(
            finalType && intermediateType,
            "Wasm UDAF '{}' cannot resolve result/intermediate types",
            manifest.name);
        const auto expected = partialOutput ? intermediateType : finalType;
        VELOX_USER_CHECK(
            resultType->equivalent(*expected),
            "Wasm UDAF '{}' result has type '{}', expected '{}'",
            manifest.name,
            resultType->toString(),
            expected->toString());
        std::vector<TypePtr> lambdaTypes;
        const auto lambdaStart = signature->argumentTypes().size() -
            manifest.lambdaCount - (signature->variableArity() ? 1 : 0);
        for (size_t i = lambdaStart; i < lambdaStart + manifest.lambdaCount;
             ++i) {
          auto type = binder.tryResolveType(signature->argumentTypes()[i]);
          // Independent merge/extract may erase input-only generic variables.
          // Preserve the lambda position without inventing a type. An author
          // can still initialize neutral merge State, but cannot call this
          // slot.
          if (!type && !rawInput) {
            lambdaTypes.push_back(nullptr);
            continue;
          }
          VELOX_USER_CHECK(
              type && type->isFunction(), "Cannot bind Wasm lambda types");
          for (size_t j = 0; j < type->size(); ++j)
            validateAggregateValueType(type->childAt(j));
          lambdaTypes.push_back(std::move(type));
        }
        const auto actualLambdas = std::count_if(
            argTypes.begin(), argTypes.end(), [](const auto& type) {
              return type->isFunction();
            });
        VELOX_USER_CHECK_EQ(
            actualLambdas,
            rawInput ? manifest.lambdaCount : 0,
            "Wasm aggregate function arguments require declared lambdas");
        for (size_t i = 0; i < argTypes.size(); ++i)
          VELOX_USER_CHECK(
              argTypes[i]->isFunction() ==
                  (rawInput && i >= lambdaStart &&
                   i < lambdaStart + manifest.lambdaCount),
              "Wasm aggregate lambdas must follow fixed values and precede the variadic tail");
        std::vector<TypePtr> valueTypes;
        for (const auto& type : argTypes) {
          if (!type->isFunction())
            valueTypes.push_back(type);
        }
        if (manifest.rowApi) {
          for (const auto& type : valueTypes)
            validateAggregateValueType(type);
          validateAggregateValueType(finalType);
          validateAggregateValueType(intermediateType);
        }
        auto bound = manifest;
        bound.returnType.type = std::move(finalType);
        bound.intermediateType.type = std::move(intermediateType);
        std::unordered_map<std::string, std::string> config;
        const auto raw = queryConfig.rawConfigsCopy();
        for (const auto& key : manifest.configKeys) {
          if (auto it = raw.find(key); it != raw.end())
            config.emplace(key, it->second);
          else
            for (const auto& property :
                 core::QueryConfig::registeredProperties()) {
              if (property.name == key && property.defaultValue) {
                config.emplace(key, *property.defaultValue);
                break;
              }
            }
        }
        return std::make_unique<WasmAggregate>(
            resultType,
            std::move(bound),
            module,
            options,
            std::move(valueTypes),
            std::move(config),
            std::move(lambdaTypes),
            rawInput);
      },
      metadata};
}

exec::AggregateFunctionEntry makeAggregateEntry(
    std::vector<AggregateImplementation> implementations) {
  std::stable_sort(
      implementations.begin(),
      implementations.end(),
      [](const auto& a, const auto& b) {
        return overloadPriority(*a.manifest.signature) <
            overloadPriority(*b.manifest.signature);
      });
  std::vector<exec::AggregateFunctionEntry> entries;
  std::vector<exec::AggregateFunctionSignaturePtr> signatures;
  exec::AggregateFunctionMetadata metadata;
  metadata.ignoreDuplicates = true;
  metadata.orderSensitive = false;
  for (const auto& implementation : implementations) {
    auto entry = makeAggregateImplementationEntry(
        implementation.manifest, implementation.module, implementation.options);
    signatures.push_back(implementation.manifest.signature);
    metadata.orderSensitive |= entry.metadata.orderSensitive;
    metadata.ignoreDuplicates &= entry.metadata.ignoreDuplicates;
    entries.push_back(std::move(entry));
  }
  return {
      signatures,
      [entries = std::move(entries)](
          core::AggregationNode::Step step,
          const std::vector<TypePtr>& types,
          const TypePtr& result,
          const core::QueryConfig& config) {
        for (const auto& entry : entries) {
          const auto& signature = *entry.signatures[0];
          std::unique_ptr<exec::FunctionSignature> mergeSignature;
          if (!exec::isRawInput(step)) {
            mergeSignature = std::make_unique<exec::FunctionSignature>(
                exec::usedTypeVariables(
                    {signature.intermediateType(), signature.returnType()},
                    signature.variables()),
                signature.returnType(),
                std::vector<exec::TypeSignature>{signature.intermediateType()},
                std::vector<bool>{false},
                false);
          }
          const exec::FunctionSignature& binding = exec::isRawInput(step)
              ? static_cast<const exec::FunctionSignature&>(signature)
              : *mergeSignature;
          exec::SignatureBinder binder(binding, types, TypeCoercer::defaults());
          if (!binder.tryBind())
            continue;
          auto expected = binder.tryResolveType(
              exec::isPartialOutput(step) ? signature.intermediateType()
                                          : signature.returnType());
          if (expected && result->equivalent(*expected))
            return entry.factory(step, types, result, config);
        }
        VELOX_USER_FAIL(
            "No Wasm aggregate overload matches the input and result types");
      },
      metadata};
}

} // namespace

size_t registerWasmModule(
    const std::filesystem::path& wasmPath,
    bool overwrite,
    const WasmOptions& options) {
  const auto bytes = WasmModule::read(wasmPath, options.maxModuleBytes);
  auto manifests = loadEmbeddedManifests(wasmPath, bytes);
  auto module = WasmModule::compile(wasmPath, bytes);
  // Validate every export before changing Velox's function registry.
  for (const auto& manifest : manifests.scalars) {
    WasmInstance validationInstance(module, manifest.entrypoint, options);
    if (!manifest.initializeEntrypoint.empty()) {
      validationInstance.addBatchEntrypoint(manifest.initializeEntrypoint);
    }
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
        {exports.updateSingleGroup, exports.mergeSingleGroup},
        options);
    if (!manifest.initializeEntrypoint.empty()) {
      validationInstance.addBatchEntrypoint(manifest.initializeEntrypoint);
    }
    if (!exports.toIntermediate.empty())
      validationInstance.addBatchEntrypoint(exports.toIntermediate);
    if (!exports.compact.empty())
      validationInstance.addBatchEntrypoint(exports.compact);
  }
  // Window registries in Velox are startup-only. This API shares that
  // contract; all potentially throwing construction happens before publication.
  std::lock_guard<std::mutex> registrationLock(registrationMutex());
  auto nextCatalog = scalarCatalog();
  auto nextAggregateCatalog = aggregateCatalog();
  auto nextCompanionOwners = companionOwners();
  std::map<std::string, std::vector<ScalarManifest>> scalarGroups;
  for (auto& manifest : manifests.scalars) {
    scalarGroups[manifest.name].push_back(std::move(manifest));
  }
  const auto nativeNames = exec::simpleFunctions().getFunctionNames();
  for (const auto& [name, overloads] : scalarGroups) {
    VELOX_USER_CHECK(
        !exec::isFunctionCallToSpecialFormRegistered(name),
        "Wasm function '{}' conflicts with a special form",
        name);
    VELOX_USER_CHECK(
        std::find(nativeNames.begin(), nativeNames.end(), name) ==
            nativeNames.end(),
        "Wasm function '{}' conflicts with a native Simple Function",
        name);
  }
  auto vectorLock = exec::vectorFunctionFactories().wlock();
  auto aggregateLock = exec::aggregateFunctions().wlock();
  auto nextVectors = *vectorLock;
  auto nextAggregates = *aggregateLock;
  auto nextWindows = exec::windowFunctions();
  for (auto& [name, overloads] : scalarGroups) {
    if (auto it = nextVectors.find(name); it != nextVectors.end()) {
      VELOX_USER_CHECK(
          nextCatalog.count(name) && it->second.metadata.owner == "velox.wasm",
          "Wasm function '{}' conflicts with a native VectorFunction",
          name);
    }
    if (!nextVectors.count(name))
      nextCatalog.erase(name);
    auto& implementations = nextCatalog[name];
    for (auto& manifest : overloads) {
      auto existing = std::find_if(
          implementations.begin(), implementations.end(), [&](const auto& old) {
            return scalarDispatchKey(*old.manifest.signature) ==
                scalarDispatchKey(*manifest.signature);
          });
      VELOX_USER_CHECK(
          existing == implementations.end() || overwrite,
          "Wasm scalar signature collision for '{}'",
          name);
      ScalarImplementation implementation{std::move(manifest), module, options};
      if (existing == implementations.end())
        implementations.push_back(std::move(implementation));
      else
        *existing = std::move(implementation);
    }
    nextVectors[name] = makeScalarEntry(implementations);
  }
  std::map<std::string, std::vector<AggregateManifest>> aggregateGroups;
  for (auto& manifest : manifests.aggregates)
    aggregateGroups[manifest.name].push_back(std::move(manifest));
  for (auto& [name, overloads] : aggregateGroups) {
    VELOX_USER_CHECK(
        (!nextAggregates.count(name) && !nextWindows.count(name)) ||
            nextAggregateCatalog.count(name),
        "Wasm aggregate/window name collision for '{}'",
        name);
    if (!nextAggregates.count(name))
      nextAggregateCatalog.erase(name);
    auto& implementations = nextAggregateCatalog[name];
    for (auto& manifest : overloads) {
      auto existing = std::find_if(
          implementations.begin(), implementations.end(), [&](const auto& old) {
            return scalarDispatchKey(*old.manifest.signature) ==
                scalarDispatchKey(*manifest.signature);
          });
      VELOX_USER_CHECK(
          existing == implementations.end() || overwrite,
          "Wasm aggregate signature collision for '{}'",
          name);
      AggregateImplementation implementation{
          std::move(manifest), module, options};
      if (existing == implementations.end())
        implementations.push_back(std::move(implementation));
      else
        *existing = std::move(implementation);
    }
    auto entry = makeAggregateEntry(implementations);
    nextWindows[name] =
        exec::window::makeAggregateWindowFunctionEntry(name, entry.signatures);
    nextAggregates[name] = std::move(entry);
  }
  for (const auto& [name, unused] : aggregateGroups) {
    auto removeOwned = [&](auto& owners, auto& registry) {
      for (auto it = owners.begin(); it != owners.end();) {
        if (it->second != name) {
          ++it;
          continue;
        }
        registry.erase(it->first);
        it = owners.erase(it);
      }
    };
    removeOwned(nextCompanionOwners.aggregates, nextAggregates);
    removeOwned(nextCompanionOwners.vectors, nextVectors);
    const auto& source = nextAggregates.at(name);
    auto entries = exec::CompanionFunctionsRegistrar::prepareEntries(
        name, source.signatures, source.metadata);
    for (auto& [companion, entry] : entries.aggregates) {
      VELOX_USER_CHECK(
          !nextAggregates.count(companion) && !nextWindows.count(companion),
          "Wasm aggregate companion name collision for '{}'",
          companion);
      if (companion == exec::CompanionSignatures::partialFunctionName(name)) {
        auto factory = std::move(entry.factory);
        entry.factory = [factory = std::move(factory)](
                            core::AggregationNode::Step step,
                            const std::vector<TypePtr>& args,
                            const TypePtr& result,
                            const core::QueryConfig& config) {
          // A Spark-style partial companion on a single-step operator still
          // constructs the original UDAF with an intermediate result contract.
          return factory(
              exec::isRawInput(step)
                  ? core::AggregationNode::Step::kPartial
                  : core::AggregationNode::Step::kIntermediate,
              args,
              result,
              config);
        };
      }
      nextCompanionOwners.aggregates.emplace(companion, name);
      nextAggregates.emplace(companion, std::move(entry));
    }
    for (auto& [companion, entry] : entries.vectors) {
      VELOX_USER_CHECK(
          !nextVectors.count(companion) &&
              std::find(nativeNames.begin(), nativeNames.end(), companion) ==
                  nativeNames.end() &&
              !exec::isFunctionCallToSpecialFormRegistered(companion),
          "Wasm extract companion name collision for '{}'",
          companion);
      // Metadata owner is a string_view; its storage must outlive the registry.
      entry.metadata.owner = "velox.wasm.companion";
      auto factory = std::move(entry.factory);
      entry.factory = [factory = std::move(factory)](
                          const std::string& functionName,
                          const std::vector<exec::VectorFunctionArg>& args,
                          const core::QueryConfig& config) {
        return std::make_shared<WasmCompanionExtract>(
            [factory, functionName, args, config] {
              return factory(functionName, args, config);
            });
      };
      nextCompanionOwners.vectors.emplace(companion, name);
      nextVectors.emplace(companion, std::move(entry));
    }
  }
  // std::unordered_map/map swap with the default allocator is nonthrowing.
  vectorLock->swap(nextVectors);
  aggregateLock->swap(nextAggregates);
  exec::windowFunctions().swap(nextWindows);
  scalarCatalog().swap(nextCatalog);
  aggregateCatalog().swap(nextAggregateCatalog);
  companionOwners().aggregates.swap(nextCompanionOwners.aggregates);
  companionOwners().vectors.swap(nextCompanionOwners.vectors);
  return scalarGroups.size() + aggregateGroups.size();
}

} // namespace facebook::velox::functions::wasm
