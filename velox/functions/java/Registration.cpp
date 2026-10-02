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

#include "velox/functions/java/Registration.h"

#include "velox/exec/Aggregate.h"
#include "velox/expression/FunctionSignature.h"
#include "velox/expression/VectorFunction.h"
#include "velox/functions/java/JavaAggregate.h"
#include "velox/functions/java/JavaVectorFunction.h"
#include "velox/functions/java/Manifest.h"

namespace facebook::velox::functions::java {

bool registerJavaScalarFunction(
    const std::filesystem::path& manifestPath,
    bool overwrite) {
  auto manifest = loadScalarManifest(manifestPath);

  // Load and link the generated adapter during startup.
  JavaInstance validationInstance(
      manifest.jarPath, manifest.implementationClass, false);

  exec::FunctionSignatureBuilder signature;
  signature.returnType(functionSignatureType(manifest.returnType.type));
  for (const auto& argument : manifest.arguments) {
    signature.argumentType(functionSignatureType(argument.type));
  }

  exec::VectorFunctionMetadata metadata;
  metadata.deterministic = manifest.deterministic;
  metadata.defaultNullBehavior = manifest.defaultNullBehavior;
  const auto jarPath = manifest.jarPath;
  const auto implementationClass = manifest.implementationClass;
  return exec::registerStatefulVectorFunction(
      manifest.name,
      {signature.build()},
      [jarPath, implementationClass](
          const std::string&,
          const std::vector<exec::VectorFunctionArg>&,
          const core::QueryConfig&) {
        return std::make_shared<JavaVectorFunction>(
            jarPath, implementationClass);
      },
      metadata,
      overwrite);
}

bool registerJavaAggregateFunction(
    const std::filesystem::path& manifestPath,
    bool overwrite) {
  auto manifest = loadAggregateManifest(manifestPath);

  // Load and link the generated adapter during startup.
  JavaInstance validationInstance(
      manifest.jarPath, manifest.implementationClass, true);

  exec::AggregateFunctionSignatureBuilder signature;
  signature.returnType(functionSignatureType(manifest.returnType.type));
  signature.intermediateType("varbinary");
  for (const auto& argument : manifest.arguments) {
    signature.argumentType(functionSignatureType(argument.type));
  }

  exec::AggregateFunctionMetadata metadata;
  metadata.orderSensitive = manifest.orderSensitive;
  metadata.ignoreDuplicates = manifest.ignoreDuplicates;
  auto name = manifest.name;
  auto registered = exec::registerAggregateFunction(
      name,
      {signature.build()},
      [manifest = std::move(manifest)](
          core::AggregationNode::Step step,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType,
          const core::QueryConfig&) {
        const bool rawInput = step == core::AggregationNode::Step::kPartial ||
            step == core::AggregationNode::Step::kSingle;
        if (rawInput) {
          VELOX_USER_CHECK_EQ(
              argTypes.size(),
              manifest.arguments.size(),
              "Java UDAF '{}' received the wrong argument count",
              manifest.name);
          for (size_t i = 0; i < argTypes.size(); ++i) {
            VELOX_USER_CHECK(
                argTypes[i]->equivalent(*manifest.arguments[i].type),
                "Java UDAF '{}' argument {} has type '{}', expected '{}'",
                manifest.name,
                i,
                argTypes[i]->toString(),
                manifest.arguments[i].type->toString());
          }
        } else {
          VELOX_USER_CHECK_EQ(
              argTypes.size(), 1, "Java UDAF final step expects one argument");
          VELOX_USER_CHECK(
              argTypes[0]->equivalent(*VARBINARY()),
              "Java UDAF final input must be varbinary");
        }
        const auto& expectedResult =
            step == core::AggregationNode::Step::kPartial
            ? manifest.intermediateType.type
            : manifest.returnType.type;
        VELOX_USER_CHECK(
            resultType->equivalent(*expectedResult),
            "Java UDAF '{}' result has type '{}', expected '{}'",
            manifest.name,
            resultType->toString(),
            expectedResult->toString());
        return std::make_unique<JavaAggregate>(resultType, manifest);
      },
      metadata,
      false,
      overwrite);
  return registered.mainFunction;
}

} // namespace facebook::velox::functions::java
