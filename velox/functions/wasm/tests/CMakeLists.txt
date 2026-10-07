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
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/scalar.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/sql_traits.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/aggregate.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/lambda.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/state.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/checkpoint.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/variadic.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/views.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/writers.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/value.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/comparison.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/src/custom.rs
    ${WASM_SDK_ROOT}/velox-wasm-sdk/Cargo.toml
    ${WASM_SDK_ROOT}/velox-wasm-macros/src/lib.rs
    ${WASM_SDK_ROOT}/velox-wasm-macros/src/generics.rs
    ${WASM_SDK_ROOT}/velox-wasm-macros/src/aggregate.rs
    ${WASM_SDK_ROOT}/examples/add/src/lib.rs
  VERBATIM
)
add_custom_target(velox_wasm_test_module DEPENDS ${WASM_EXAMPLE_PATH})

set(
  WASM_TEST_MODULE_DEFINITION
  WASM_MODULE_PATH="${WASM_EXAMPLE_PATH}"
)

if(VELOX_BUILD_TESTING)
  set(WASM_GTEST_MAIN ${PROJECT_SOURCE_DIR}/velox/expression/tests/Main.cpp)
  add_executable(velox_wasm_udf_test WasmScalarTest.cpp RuntimeTest.cpp ${WASM_GTEST_MAIN})
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
  )
  add_test(NAME velox_wasm_udf_test COMMAND velox_wasm_udf_test)

  add_executable(velox_wasm_aggregate_test WasmAggregateTest.cpp ${WASM_GTEST_MAIN})
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
  )
  add_test(NAME velox_wasm_aggregate_test COMMAND velox_wasm_aggregate_test)
endif()

if(VELOX_BUILD_UDF_BENCHMARKS)
  add_executable(velox_wasm_native_udf_benchmark WasmNativeUdfBenchmark.cpp)
  if(NOT VELOX_ENABLE_AGGREGATES)
    # Use the actual native implementations even in a minimal build.
    target_sources(
      velox_wasm_native_udf_benchmark
      PRIVATE
        ../../prestosql/aggregates/AverageAggregate.cpp
        ../../prestosql/aggregates/ReduceAgg.cpp
        ../../prestosql/aggregates/SumAggregate.cpp
    )
  else()
    target_link_libraries(velox_wasm_native_udf_benchmark velox_aggregates)
  endif()
  if(NOT VELOX_ENABLE_EXEC)
    target_sources(
      velox_wasm_native_udf_benchmark
      PRIVATE WasmAggregateTestSupport.cpp ../../../exec/ContainerRowSerde.cpp
    )
  endif()
  add_dependencies(velox_wasm_native_udf_benchmark velox_wasm_test_module)
  target_compile_definitions(
    velox_wasm_native_udf_benchmark
    PRIVATE ${WASM_TEST_MODULE_DEFINITION}
  )
  target_link_libraries(
    velox_wasm_native_udf_benchmark
    velox_wasm_udf
    velox_functions_prestosql
    velox_functions_aggregates
    velox_expression
    velox_core
    velox_memory
    velox_vector
    Folly::follybenchmark
    gflags::gflags
    glog::glog
  )
  add_test(
    NAME velox_wasm_native_udf_benchmark_check
    COMMAND velox_wasm_native_udf_benchmark
      --bm_profile=true --bm_profile_iters=1 --udf_warmup_batches=0
  )
endif()

if(
  VELOX_BUILD_WASM_UDF_TESTING OR
  (VELOX_BUILD_UDF_BENCHMARKS AND NOT VELOX_ENABLE_EXEC)
)
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
endif()

if(VELOX_BUILD_WASM_UDF_TESTING)
  foreach(test_target IN ITEMS velox_wasm_udf_smoke_test velox_wasm_safety_test)
    if(test_target STREQUAL "velox_wasm_udf_smoke_test")
      set(test_source WasmScalarSmokeTest.cpp)
    else()
      set(test_source WasmSafetyTest.cpp)
    endif()
    add_executable(${test_target} ${test_source})
    if(NOT VELOX_ENABLE_EXEC)
      target_sources(
        ${test_target}
        PRIVATE WasmAggregateTestSupport.cpp
      )
    endif()
    add_dependencies(${test_target} velox_wasm_test_module)
    target_compile_definitions(
      ${test_target}
      PRIVATE
        ${WASM_TEST_MODULE_DEFINITION}
    )
    target_link_libraries(
      ${test_target}
      velox_wasm_udf
      velox_expression
      velox_core
      velox_memory
      velox_vector
    )
    add_test(NAME ${test_target} COMMAND ${test_target})
  endforeach()
endif()
