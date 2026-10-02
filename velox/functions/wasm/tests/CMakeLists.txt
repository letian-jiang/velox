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

find_program(CARGO_EXECUTABLE cargo REQUIRED)

set(WASM_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../sdk)
set(WASM_CARGO_TARGET ${CMAKE_CURRENT_BINARY_DIR}/cargo-target)
set(
  WASM_EXAMPLE_PATH
  ${WASM_CARGO_TARGET}/wasm32-unknown-unknown/release/velox_wasm_example_add.wasm
)
add_custom_command(
  OUTPUT ${WASM_EXAMPLE_PATH}
  COMMAND
    ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${WASM_CARGO_TARGET}"
    "RUSTFLAGS=-C target-feature=+simd128" ${CARGO_EXECUTABLE} build --release
    --target wasm32-unknown-unknown -p velox-wasm-example-add
  WORKING_DIRECTORY ${WASM_SDK_ROOT}
  DEPENDS
    ${WASM_SDK_ROOT}/Cargo.lock
    ${WASM_SDK_ROOT}/Cargo.toml
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/lib.rs
    ${WASM_SDK_ROOT}/velox-wasm-macros/src/lib.rs
    ${WASM_SDK_ROOT}/examples/add/src/lib.rs
  VERBATIM
)
add_custom_target(velox_wasm_test_module DEPENDS ${WASM_EXAMPLE_PATH})

set(
  WASM_TEST_MODULE_DEFINITION
  WASM_MODULE_PATH="${WASM_EXAMPLE_PATH}"
)

if(VELOX_BUILD_TESTING)
  add_executable(velox_wasm_udf_test WasmScalarTest.cpp)
  add_dependencies(velox_wasm_udf_test velox_wasm_test_module)
  target_compile_definitions(
    velox_wasm_udf_test
    PRIVATE
      ${WASM_TEST_MODULE_DEFINITION}
  )
  target_link_libraries(
    velox_wasm_udf_test
    velox_wasm_udf
    velox_functions_test_lib
    velox_exec_test_lib
    GTest::gtest
    GTest::gtest_main
  )
  add_test(NAME velox_wasm_udf_test COMMAND velox_wasm_udf_test)

  add_executable(velox_wasm_aggregate_test WasmAggregateTest.cpp)
  add_dependencies(velox_wasm_aggregate_test velox_wasm_test_module)
  target_compile_definitions(
    velox_wasm_aggregate_test
    PRIVATE
      ${WASM_TEST_MODULE_DEFINITION}
  )
  target_link_libraries(
    velox_wasm_aggregate_test
    velox_wasm_udf
    velox_functions_test_lib
    velox_exec_test_lib
    GTest::gtest
    GTest::gtest_main
  )
  add_test(NAME velox_wasm_aggregate_test COMMAND velox_wasm_aggregate_test)
endif()

if(VELOX_BUILD_WASM_UDF_TESTING)
  enable_testing()
  # The minimal build keeps references to these lightweight libraries in the
  # expression/core link interfaces, but does not enter velox/exec where their
  # targets are normally declared.
  if(NOT TARGET velox_exec_spill_stats)
    velox_add_library(
      velox_exec_spill_stats
      ${PROJECT_SOURCE_DIR}/velox/exec/SpillStats.cpp
      HEADERS
      ${PROJECT_SOURCE_DIR}/velox/exec/SpillStats.h
    )
    velox_link_libraries(
      velox_exec_spill_stats
      velox_common_base
      velox_file
      Folly::folly
    )
  endif()
  if(NOT TARGET velox_trace)
    velox_add_library(
      velox_trace
      ${PROJECT_SOURCE_DIR}/velox/exec/trace/Trace.cpp
      ${PROJECT_SOURCE_DIR}/velox/exec/trace/TraceUtil.cpp
      HEADERS
      ${PROJECT_SOURCE_DIR}/velox/exec/trace/Trace.h
      ${PROJECT_SOURCE_DIR}/velox/exec/trace/TraceCtx.h
      ${PROJECT_SOURCE_DIR}/velox/exec/trace/TraceUtil.h
      ${PROJECT_SOURCE_DIR}/velox/exec/trace/TraceWriter.h
    )
    velox_link_libraries(
      velox_trace
      velox_common_base
      velox_exception
      velox_file
      velox_core
      velox_type
      velox_vector
    )
  endif()
  add_executable(velox_wasm_udf_smoke_test WasmScalarSmokeTest.cpp)
  if(NOT VELOX_ENABLE_EXEC)
    target_sources(
      velox_wasm_udf_smoke_test
      PRIVATE WasmAggregateTestSupport.cpp
    )
  endif()
  add_dependencies(velox_wasm_udf_smoke_test velox_wasm_test_module)
  target_compile_definitions(
    velox_wasm_udf_smoke_test
    PRIVATE
      ${WASM_TEST_MODULE_DEFINITION}
  )
  target_link_libraries(
    velox_wasm_udf_smoke_test
    velox_wasm_udf
    velox_expression
    velox_core
    velox_memory
    velox_vector
  )
  add_test(NAME velox_wasm_udf_smoke_test COMMAND velox_wasm_udf_smoke_test)
endif()
