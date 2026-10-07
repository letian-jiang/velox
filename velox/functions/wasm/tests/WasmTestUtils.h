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

#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include "velox/common/memory/Memory.h"
#include "velox/core/Expressions.h"
#include "velox/core/QueryCtx.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/RowContainer.h"
#include "velox/expression/Expr.h"
#include "velox/expression/SpecialFormRegistry.h"
#include "velox/expression/TryExpr.h"
#include "velox/functions/wasm/ArrowIpc.h"
#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Registration.h"
#include "velox/functions/wasm/Runtime.h"
#include "velox/functions/wasm/TypeBridge.h"
#include "velox/type/tests/utils/CustomTypesForTesting.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::functions::wasm::test {
namespace {
void check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

const ScalarManifest& scalarManifest(const std::string& name) {
  static const auto manifests = loadEmbeddedManifests(WASM_MODULE_PATH);
  for (const auto& manifest : manifests.scalars) {
    if (manifest.name == name) {
      return manifest;
    }
  }
  throw std::runtime_error("Missing embedded Wasm scalar function: " + name);
}

std::string materialize(const ArrowIpcInput& input) {
  std::string ipc(input.size(), '\0');
  input.write(reinterpret_cast<uint8_t*>(ipc.data()), input.size());
  return ipc;
}

template <typename T>
std::shared_ptr<FlatVector<T>> makeFlat(
    const TypePtr& type,
    const std::vector<std::optional<T>>& values,
    memory::MemoryPool* pool) {
  auto vector = BaseVector::create<FlatVector<T>>(type, values.size(), pool);
  for (vector_size_t row = 0; row < values.size(); ++row) {
    if (values[row].has_value()) {
      vector->set(row, values[row].value());
    } else {
      vector->setNull(row, true);
    }
  }
  return vector;
}

RowVectorPtr makeRow(
    std::vector<VectorPtr> children,
    memory::MemoryPool* pool) {
  std::vector<std::string> names;
  std::vector<TypePtr> types;
  names.reserve(children.size());
  types.reserve(children.size());
  for (size_t index = 0; index < children.size(); ++index) {
    names.push_back("c" + std::to_string(index));
    types.push_back(children[index]->type());
  }
  return std::make_shared<RowVector>(
      pool,
      ROW(std::move(names), std::move(types)),
      nullptr,
      children.empty() ? 0 : children.front()->size(),
      std::move(children));
}

VectorPtr evaluateCall(
    const std::string& name,
    const TypePtr& returnType,
    const RowVectorPtr& input,
    core::ExecCtx& execCtx,
    bool wrapTry = false) {
  std::vector<core::TypedExprPtr> inputs;
  for (size_t index = 0; index < input->childrenSize(); ++index) {
    inputs.push_back(
        std::make_shared<core::FieldAccessTypedExpr>(
            input->childAt(index)->type(), "c" + std::to_string(index)));
  }
  core::TypedExprPtr call = std::make_shared<core::CallTypedExpr>(
      returnType, std::move(inputs), name);
  if (wrapTry) {
    call = std::make_shared<core::CallTypedExpr>(
        returnType, std::vector<core::TypedExprPtr>{call}, "try");
  }
  exec::ExprSet expression({call}, &execCtx);
  exec::EvalCtx context(&execCtx, &expression, input.get());
  SelectivityVector rows(input->size());
  std::vector<VectorPtr> result(1);
  expression.eval(rows, context, result);
  return result.front();
}

} // namespace
} // namespace facebook::velox::functions::wasm::test
