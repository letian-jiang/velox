/*
 * Minimal Aggregate base and registry definitions for the standalone Wasm UDF
 * test build. Full Velox builds link these definitions from velox_exec.
 */

#include "velox/exec/Aggregate.h"

namespace facebook::velox::exec {

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
