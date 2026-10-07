# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

velox_add_library(
  velox_wasm_udf
  ArrowIpc.cpp
  Manifest.cpp
  Registration.cpp
  Runtime.cpp
  LinearMemory.cpp
  TypeBridge.cpp
  WasmAggregate.cpp
  WasmVectorFunction.cpp
  HEADERS
  Abi.h
  ArrowIpc.h
  Manifest.h
  Registration.h
  Runtime.h
  LinearMemory.h
  TypeBridge.h
  WasmAggregate.h
  WasmVectorFunction.h
)

velox_link_libraries(
  velox_wasm_udf
  arrow
  Wasmtime::wasmtime
  velox_arrow_bridge
  velox_expression
  velox_type
  velox_type_fbhive
  velox_vector
  velox_exception
  Folly::folly
)

if(VELOX_ENABLE_EXEC)
  get_target_property(wasm_target velox_wasm_udf ALIASED_TARGET)
  if(NOT wasm_target)
    set(wasm_target velox_wasm_udf)
  endif()
  target_compile_definitions(${wasm_target} PRIVATE VELOX_WASM_HAS_EXEC)
  velox_link_libraries(velox_wasm_udf velox_exec)
endif()

if(NOT VELOX_ENABLE_EXEC)
  # Standalone bridge tests still exercise the real native companion adapters
  # and signature rules, using only the small Aggregate registry test shim.
  get_target_property(wasm_target velox_wasm_udf ALIASED_TARGET)
  if(NOT wasm_target)
    set(wasm_target velox_wasm_udf)
  endif()
  target_sources(${wasm_target} PRIVATE
    ${PROJECT_SOURCE_DIR}/velox/exec/AggregateCompanionAdapter.cpp
    ${PROJECT_SOURCE_DIR}/velox/exec/AggregateCompanionSignatures.cpp
    ${PROJECT_SOURCE_DIR}/velox/exec/AggregateFunctionRegistry.cpp
  )
endif()

if(
  VELOX_BUILD_TESTING
  OR VELOX_BUILD_WASM_UDF_TESTING
  OR VELOX_BUILD_UDF_BENCHMARKS
)
  add_subdirectory(tests)
endif()
