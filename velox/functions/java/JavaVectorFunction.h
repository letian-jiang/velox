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

#include "velox/expression/VectorFunction.h"
#include "velox/functions/java/Runtime.h"

namespace facebook::velox::functions::java {

class JavaVectorFunction final : public exec::VectorFunction {
 public:
  JavaVectorFunction(
      std::filesystem::path jarPath,
      std::string implementationClass);

  void apply(
      const SelectivityVector& rows,
      std::vector<VectorPtr>& arguments,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const override;

 private:
  void applyInternal(
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& arguments,
      const TypePtr& outputType,
      exec::EvalCtx& context,
      VectorPtr& result) const;

  mutable JavaInstance instance_;
};

} // namespace facebook::velox::functions::java
