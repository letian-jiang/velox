/*
 * Minimal Aggregate base and registry definitions for the standalone Wasm UDF
 * test and native comparison benchmark. Full builds use velox_exec.
 */

#include "velox/exec/Aggregate.h"
#include "velox/exec/window/AggregateWindow.h"

namespace facebook::velox::exec {

bool isRawInput(core::AggregationNode::Step step) {
  return step == core::AggregationNode::Step::kPartial ||
      step == core::AggregationNode::Step::kSingle;
}

bool isPartialOutput(core::AggregationNode::Step step) {
  return step == core::AggregationNode::Step::kPartial ||
      step == core::AggregationNode::Step::kIntermediate;
}

AggregateFunctionMap& aggregateFunctions() {
  static AggregateFunctionMap functions;
  return functions;
}

const AggregateFunctionEntry* getAggregateFunctionEntry(
    const std::string& name) {
  const auto sanitizedName = sanitizeName(name);
  return aggregateFunctions().withRLock(
      [&](const auto& functions) -> const AggregateFunctionEntry* {
        auto it = functions.find(sanitizedName);
        return it == functions.end() ? nullptr : &it->second;
      });
}

std::optional<std::vector<AggregateFunctionSignaturePtr>>
getAggregateFunctionSignatures(const std::string& name) {
  if (const auto* entry = getAggregateFunctionEntry(name))
    return entry->signatures;
  return std::nullopt;
}

AggregateRegistrationResult registerAggregateFunction(
    const std::string& name,
    const std::vector<std::shared_ptr<AggregateFunctionSignature>>& signatures,
    const AggregateFunctionFactory& factory,
    const AggregateFunctionMetadata& metadata,
    bool /*registerCompanionFunctions*/,
    bool overwrite) {
  AggregateRegistrationResult result;
  const auto sanitizedName = sanitizeName(name);
  result.mainFunction = aggregateFunctions().withWLock([&](auto& functions) {
    if (!overwrite && functions.count(sanitizedName) != 0) {
      return false;
    }
    functions[sanitizedName] = {signatures, factory, metadata};
    return true;
  });
  return result;
}

std::vector<AggregateRegistrationResult> registerAggregateFunction(
    const std::vector<std::string>& names,
    const std::vector<std::shared_ptr<AggregateFunctionSignature>>& signatures,
    const AggregateFunctionFactory& factory,
    const AggregateFunctionMetadata& metadata,
    bool registerCompanionFunctions,
    bool overwrite) {
  std::vector<AggregateRegistrationResult> results;
  for (const auto& name : names) {
    results.push_back(registerAggregateFunction(
        name,
        signatures,
        factory,
        metadata,
        registerCompanionFunctions,
        overwrite));
  }
  return results;
}

std::unique_ptr<Aggregate> Aggregate::create(
    const std::string& name,
    core::AggregationNode::Step step,
    const std::vector<TypePtr>& argTypes,
    const TypePtr& resultType,
    const core::QueryConfig& config) {
  if (auto* entry = getAggregateFunctionEntry(name)) {
    return entry->factory(step, argTypes, resultType, config);
  }
  VELOX_USER_FAIL("Aggregate function not registered: {}", name);
}

void Aggregate::setAllocatorInternal(HashStringAllocator* allocator) {
  allocator_ = allocator;
}

void Aggregate::setLambdaExpressions(
    std::vector<core::LambdaTypedExprPtr> expressions,
    std::shared_ptr<core::ExpressionEvaluator> evaluator) {
  lambdaExpressions_ = std::move(expressions);
  expressionEvaluator_ = std::move(evaluator);
}

void Aggregate::setOffsetsInternal(
    int32_t offset,
    int32_t nullByte,
    uint8_t nullMask,
    int32_t initializedByte,
    uint8_t initializedMask,
    int32_t rowSizeOffset) {
  offset_ = offset;
  nullByte_ = nullByte;
  nullMask_ = nullMask;
  initializedByte_ = initializedByte;
  initializedMask_ = initializedMask;
  rowSizeOffset_ = rowSizeOffset;
}

void Aggregate::clearInternal() {
  numNulls_ = 0;
}

} // namespace facebook::velox::exec

namespace facebook::velox::exec {
WindowFunctionMap& windowFunctions() {
  static WindowFunctionMap functions;
  return functions;
}
namespace window {
WindowFunctionEntry makeAggregateWindowFunctionEntry(
    const std::string&,
    const std::vector<AggregateFunctionSignaturePtr>& signatures) {
  return {
      std::vector<FunctionSignaturePtr>(signatures.begin(), signatures.end()),
      [](const auto&, const auto&, bool, auto*, auto*, const auto&)
          -> std::unique_ptr<WindowFunction> {
        VELOX_FAIL("Window execution is unavailable in the minimal test build");
      },
      {WindowFunction::ProcessMode::kRows, true}};
}
} // namespace window
} // namespace facebook::velox::exec
