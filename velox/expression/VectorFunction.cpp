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
#include "velox/expression/VectorFunction.h"
#include "folly/Synchronized.h"
#include "velox/expression/SignatureBinder.h"

namespace facebook::velox::exec {

namespace {
template <typename TResult, typename TFunc>
std::optional<TResult> applyToVectorFunctionEntry(
    const std::string& name,
    TFunc applyFunc) {
  auto sanitizedName = sanitizeName(name);

  return vectorFunctionFactories().withRLock(
      [&](auto& functions) -> std::optional<TResult> {
        auto it = functions.find(sanitizedName);
        if (it == functions.end()) {
          return std::nullopt;
        }
        return applyFunc(sanitizedName, it->second);
      });
}

// Zip `inputTypes` and `constantInputs` vectors into a single
// vector of `VectorFunctionArg`.
std::vector<VectorFunctionArg> toVectorFunctionArgs(
    const std::vector<TypePtr>& inputTypes,
    const std::vector<VectorPtr>& constantInputs) {
  std::vector<VectorFunctionArg> args;
  args.reserve(inputTypes.size());

  for (vector_size_t i = 0; i < inputTypes.size(); ++i) {
    args.push_back({
        inputTypes[i],
        constantInputs.size() > i ? constantInputs[i] : nullptr,
    });
  }

  return args;
}
} // namespace

VectorFunctionMap& vectorFunctionFactories() {
  static VectorFunctionMap factories;
  return factories;
}

std::optional<VectorFunctionMetadata> getVectorFunctionMetadata(
    const std::string& name) {
  return applyToVectorFunctionEntry<VectorFunctionMetadata>(
      name,
      [&](const auto& /*name*/, const auto& entry) { return entry.metadata; });
}

std::optional<std::vector<FunctionSignaturePtr>> getVectorFunctionSignatures(
    const std::string& name) {
  return applyToVectorFunctionEntry<std::vector<FunctionSignaturePtr>>(
      name, [&](const auto& /*name*/, const auto& entry) {
        return entry.signatures;
      });
}

TypePtr resolveVectorFunction(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes) {
  if (auto outputTypeWithMetadata =
          resolveVectorFunctionWithMetadata(functionName, argTypes)) {
    return outputTypeWithMetadata->first;
  }

  return nullptr;
}

namespace {
bool hasCoercion(const std::vector<Coercion>& coercions) {
  for (const auto& coercion : coercions) {
    if (coercion.type != nullptr) {
      return true;
    }
  }

  return false;
}
} // namespace

TypePtr resolveVectorFunctionWithCoercions(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes,
    std::vector<TypePtr>& coercions,
    const TypeCoercer& coercer) {
  if (auto result = resolveVectorFunctionWithMetadataWithCoercions(
          functionName, argTypes, coercions, coercer)) {
    return result->first;
  }

  return nullptr;
}

std::optional<std::pair<TypePtr, VectorFunctionMetadata>>
resolveVectorFunctionWithMetadata(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes) {
  return applyToVectorFunctionEntry<std::pair<TypePtr, VectorFunctionMetadata>>(
      functionName,
      [&](const auto& /*name*/, const auto& entry)
          -> std::optional<std::pair<TypePtr, VectorFunctionMetadata>> {
        for (size_t signatureIndex = 0;
             signatureIndex < entry.signatures.size();
             ++signatureIndex) {
          const auto& signature = entry.signatures[signatureIndex];
          exec::SignatureBinder binder(
              *signature, argTypes, TypeCoercer::defaults());
          if (binder.tryBind()) {
            return {
                {binder.tryResolveReturnType(),
                 entry.metadataAt(signatureIndex)}};
          }
        }
        return std::nullopt;
      });
}

std::optional<std::pair<TypePtr, VectorFunctionMetadata>>
resolveVectorFunctionWithMetadataWithCoercions(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes,
    std::vector<TypePtr>& coercions,
    const TypeCoercer& coercer) {
  coercions.clear();

  return applyToVectorFunctionEntry<std::pair<TypePtr, VectorFunctionMetadata>>(
      functionName,
      [&](const auto& /*name*/, const auto& entry)
          -> std::optional<std::pair<TypePtr, VectorFunctionMetadata>> {
        std::vector<std::pair<std::vector<Coercion>, TypePtr>> candidates;
        std::vector<size_t> candidateSignatures;
        for (size_t signatureIndex = 0;
             signatureIndex < entry.signatures.size();
             ++signatureIndex) {
          const auto& signature = entry.signatures[signatureIndex];
          exec::SignatureBinder binder(*signature, argTypes, coercer);
          std::vector<Coercion> requiredCoercions;
          if (binder.tryBindWithCoercions(requiredCoercions)) {
            auto type = binder.tryResolveReturnType();
            VELOX_CHECK_NOT_NULL(type);
            if (!hasCoercion(requiredCoercions)) {
              coercions.resize(argTypes.size(), nullptr);
              return {{type, entry.metadataAt(signatureIndex)}};
            }

            candidates.emplace_back(requiredCoercions, type);
            candidateSignatures.push_back(signatureIndex);
          }
        }

        auto index = Coercion::pickLowestCost(
            candidates, argTypes, [&](size_t candidateIndex) {
              return Coercion::CandidateMetadata{
                  .returnType = candidates[candidateIndex].second,
                  .nullOnNull =
                      entry.metadataAt(candidateSignatures[candidateIndex])
                          .defaultNullBehavior};
            });

        if (index) {
          const auto& requiredCoercions = candidates[index.value()].first;
          coercions.reserve(requiredCoercions.size());
          for (const auto& coercion : requiredCoercions) {
            coercions.push_back(coercion.type);
          }

          return {
              {candidates[index.value()].second,
               entry.metadataAt(candidateSignatures[index.value()])}};
        }

        return std::nullopt;
      });
}

std::shared_ptr<VectorFunction> getVectorFunction(
    const std::string& name,
    const std::vector<TypePtr>& inputTypes,
    const std::vector<VectorPtr>& constantInputs,
    const core::QueryConfig& config) {
  auto functionWithMetadata =
      getVectorFunctionWithMetadata(name, inputTypes, constantInputs, config);
  if (!functionWithMetadata.has_value()) {
    return nullptr;
  }
  return functionWithMetadata->first;
}

std::optional<
    std::pair<std::shared_ptr<VectorFunction>, VectorFunctionMetadata>>
getVectorFunctionWithMetadata(
    const std::string& name,
    const std::vector<TypePtr>& inputTypes,
    const std::vector<VectorPtr>& constantInputs,
    const core::QueryConfig& config) {
  if (!constantInputs.empty()) {
    VELOX_CHECK_EQ(inputTypes.size(), constantInputs.size());
  }

  return applyToVectorFunctionEntry<
      std::pair<std::shared_ptr<VectorFunction>, VectorFunctionMetadata>>(
      name,
      [&](const auto& sanitizedName, const auto& entry)
          -> std::optional<std::pair<
              std::shared_ptr<VectorFunction>,
              VectorFunctionMetadata>> {
        for (size_t signatureIndex = 0;
             signatureIndex < entry.signatures.size();
             ++signatureIndex) {
          const auto& signature = entry.signatures[signatureIndex];
          exec::SignatureBinder binder(
              *signature, inputTypes, TypeCoercer::defaults());
          if (binder.tryBind()) {
            auto inputArgs = toVectorFunctionArgs(inputTypes, constantInputs);

            return {
                {entry.factory(sanitizedName, inputArgs, config),
                 entry.metadataAt(signatureIndex)}};
          }
        }
        return std::nullopt;
      });
}

/// Registers a new vector function. When overwrite = true, previous functions
/// with the given name will be replaced.
/// Returns true iff an insertion actually happened
bool registerStatefulVectorFunction(
    std::string_view name,
    std::vector<FunctionSignaturePtr> signatures,
    VectorFunctionFactory factory,
    VectorFunctionMetadata metadata,
    bool overwrite,
    std::vector<VectorFunctionMetadata> signatureMetadata) {
  VELOX_CHECK(
      signatureMetadata.empty() ||
          signatureMetadata.size() == signatures.size(),
      "Signature metadata count must match signatures");
  if (!signatureMetadata.empty()) {
    metadata = signatureMetadata.front();
    for (const auto& properties : signatureMetadata) {
      metadata.deterministic &= properties.deterministic;
      metadata.defaultNullBehavior &= properties.defaultNullBehavior;
      metadata.supportsFlattening &= properties.supportsFlattening;
      metadata.companionFunction &= properties.companionFunction;
      if (metadata.owner != properties.owner) {
        metadata.owner = "";
      }
    }
  }
  auto sanitizedName = sanitizeName(name);

  if (overwrite) {
    vectorFunctionFactories().withWLock([&](auto& functionMap) {
      // Insert/overwrite.
      functionMap[sanitizedName] = {
          std::move(signatures),
          std::move(factory),
          std::move(metadata),
          std::move(signatureMetadata)};
    });
    return true;
  }

  return vectorFunctionFactories().withWLock([&](auto& functionMap) {
    auto [iterator, inserted] = functionMap.insert(
        {sanitizedName,
         {std::move(signatures),
          std::move(factory),
          std::move(metadata),
          std::move(signatureMetadata)}});
    return inserted;
  });
}

// Returns true iff an insertion actually happened
bool registerVectorFunction(
    std::string_view name,
    std::vector<FunctionSignaturePtr> signatures,
    std::unique_ptr<VectorFunction> func,
    VectorFunctionMetadata metadata,
    bool overwrite) {
  std::shared_ptr<VectorFunction> sharedFunc = std::move(func);
  auto factory = [sharedFunc](
                     const auto& /*name*/,
                     const auto& /*vectorArg*/,
                     const auto& /*config*/) { return sharedFunc; };
  return registerStatefulVectorFunction(
      name, signatures, factory, metadata, overwrite);
}

} // namespace facebook::velox::exec
