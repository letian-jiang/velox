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

#include <gtest/gtest.h>
#include "velox/expression/FunctionSignature.h"
#include "velox/expression/VectorFunction.h"
#include "velox/type/TypeCoercer.h"

namespace facebook::velox::exec::test {
namespace {

class MetadataFunction final : public VectorFunction {
 public:
  void apply(
      const SelectivityVector&,
      std::vector<VectorPtr>&,
      const TypePtr&,
      EvalCtx&,
      VectorPtr&) const override {}
};

class VectorFunctionMetadataTest : public testing::Test {
 protected:
  void TearDown() override {
    vectorFunctionFactories().withWLock([](auto& map) { map.erase(kName); });
  }
  static constexpr const char* kName = "test_signature_metadata";
  static std::vector<FunctionSignaturePtr> signatures() {
    return {
        FunctionSignatureBuilder()
            .returnType("bigint")
            .argumentType("bigint")
            .build(),
        FunctionSignatureBuilder()
            .returnType("varchar")
            .argumentType("varchar")
            .build()};
  }
  static VectorFunctionFactory factory() {
    return [](const auto&, const auto&, const auto&) {
      return std::make_shared<MetadataFunction>();
    };
  }
};

TEST_F(VectorFunctionMetadataTest, uniformMetadataRemainsCompatible) {
  VectorFunctionMetadata metadata;
  metadata.deterministic = false;
  metadata.defaultNullBehavior = false;
  ASSERT_TRUE(
      registerStatefulVectorFunction(kName, signatures(), factory(), metadata));
  for (const auto& type : std::vector<TypePtr>{BIGINT(), VARCHAR()}) {
    auto resolved = resolveVectorFunctionWithMetadata(kName, {type});
    ASSERT_TRUE(resolved);
    EXPECT_FALSE(resolved->second.deterministic);
    EXPECT_FALSE(resolved->second.defaultNullBehavior);
    auto instance =
        getVectorFunctionWithMetadata(kName, {type}, {}, core::QueryConfig({}));
    ASSERT_TRUE(instance && instance->first);
    EXPECT_FALSE(instance->second.defaultNullBehavior);
  }
}

TEST_F(VectorFunctionMetadataTest, boundAndUnboundProperties) {
  VectorFunctionMetadata integers;
  VectorFunctionMetadata strings;
  strings.deterministic = false;
  strings.defaultNullBehavior = false;
  ASSERT_TRUE(registerStatefulVectorFunction(
      kName, signatures(), factory(), {}, true, {integers, strings}));
  auto unbound = getVectorFunctionMetadata(kName);
  ASSERT_TRUE(unbound);
  EXPECT_FALSE(unbound->deterministic);
  EXPECT_FALSE(unbound->defaultNullBehavior);
  auto integer = getVectorFunctionWithMetadata(
      kName, {BIGINT()}, {}, core::QueryConfig({}));
  auto string = getVectorFunctionWithMetadata(
      kName, {VARCHAR()}, {}, core::QueryConfig({}));
  ASSERT_TRUE(integer && string);
  EXPECT_TRUE(integer->second.defaultNullBehavior);
  EXPECT_TRUE(integer->second.deterministic);
  EXPECT_FALSE(string->second.defaultNullBehavior);
  EXPECT_FALSE(string->second.deterministic);
}

TEST_F(VectorFunctionMetadataTest, coercionRetainsSelectedProperties) {
  VectorFunctionMetadata integers;
  VectorFunctionMetadata strings;
  strings.deterministic = false;
  strings.defaultNullBehavior = false;
  ASSERT_TRUE(registerStatefulVectorFunction(
      kName, signatures(), factory(), {}, true, {integers, strings}));
  std::vector<TypePtr> coercions;
  auto result = resolveVectorFunctionWithMetadataWithCoercions(
      kName, {SMALLINT()}, coercions, TypeCoercer::defaults());
  ASSERT_TRUE(result);
  ASSERT_EQ(coercions.size(), 1);
  ASSERT_TRUE(coercions[0]);
  EXPECT_TRUE(coercions[0]->isBigint());
  EXPECT_TRUE(result->second.deterministic);
  EXPECT_TRUE(result->second.defaultNullBehavior);
}

TEST_F(VectorFunctionMetadataTest, invalidMetadataDoesNotOverwrite) {
  ASSERT_TRUE(registerStatefulVectorFunction(kName, signatures(), factory()));
  EXPECT_THROW(
      registerStatefulVectorFunction(
          kName, signatures(), factory(), {}, true, {VectorFunctionMetadata{}}),
      VeloxRuntimeError);
  EXPECT_EQ(getVectorFunctionSignatures(kName)->size(), 2);
  EXPECT_TRUE(getVectorFunctionMetadata(kName)->deterministic);
}

} // namespace
} // namespace facebook::velox::exec::test
