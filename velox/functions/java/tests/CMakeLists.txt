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

find_program(MAVEN_EXECUTABLE mvn REQUIRED)

set(JAVA_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../sdk)
set(JAVA_RUNTIME_JAR ${JAVA_SDK_ROOT}/velox-java-runtime/target/velox-java-runtime-1.0-SNAPSHOT.jar)
set(JAVA_EXAMPLE_JAR ${JAVA_SDK_ROOT}/examples/target/velox-java-examples-1.0-SNAPSHOT.jar)
set(JAVA_DEPENDENCY_CLASSPATH ${CMAKE_CURRENT_BINARY_DIR}/java-classpath.txt)
file(
  GLOB_RECURSE JAVA_SDK_SOURCES
  CONFIGURE_DEPENDS
  ${JAVA_SDK_ROOT}/*/src/main/java/*.java
  ${JAVA_SDK_ROOT}/*/src/main/resources/*
)

add_custom_command(
  OUTPUT ${JAVA_RUNTIME_JAR} ${JAVA_EXAMPLE_JAR} ${JAVA_DEPENDENCY_CLASSPATH}
  COMMAND
    ${MAVEN_EXECUTABLE} -q -DskipTests -pl examples -am package
    dependency:build-classpath
    -Dmdep.outputFile=${JAVA_DEPENDENCY_CLASSPATH}
  WORKING_DIRECTORY ${JAVA_SDK_ROOT}
  DEPENDS
    ${JAVA_SDK_ROOT}/pom.xml
    ${JAVA_SDK_ROOT}/velox-java-runtime/pom.xml
    ${JAVA_SDK_ROOT}/velox-java-processor/pom.xml
    ${JAVA_SDK_ROOT}/examples/pom.xml
    ${JAVA_SDK_SOURCES}
  VERBATIM
)
add_custom_target(
  velox_java_test_jar
  DEPENDS ${JAVA_RUNTIME_JAR} ${JAVA_EXAMPLE_JAR} ${JAVA_DEPENDENCY_CLASSPATH}
)

if(VELOX_BUILD_UDF_BENCHMARKS)
  set(WASM_TEST_BINARY_DIR ${PROJECT_BINARY_DIR}/velox/functions/wasm/tests)
  add_executable(velox_java_wasm_udf_benchmark JavaWasmUdfBenchmark.cpp)
  if(NOT VELOX_ENABLE_EXEC)
    target_sources(
      velox_java_wasm_udf_benchmark
      PRIVATE ../../wasm/tests/WasmAggregateTestSupport.cpp
    )
  endif()
  add_dependencies(
    velox_java_wasm_udf_benchmark
    velox_java_test_jar
    velox_wasm_test_module
  )
  target_compile_definitions(
    velox_java_wasm_udf_benchmark
    PRIVATE
      JAVA_RUNTIME_JAR="${JAVA_RUNTIME_JAR}"
      JAVA_DEPENDENCY_CLASSPATH="${JAVA_DEPENDENCY_CLASSPATH}"
      JAVA_ADD_MANIFEST="${JAVA_SDK_ROOT}/examples/add.json"
      JAVA_PREFIX_MANIFEST="${JAVA_SDK_ROOT}/examples/prefix.json"
      JAVA_AVERAGE_MANIFEST="${JAVA_SDK_ROOT}/examples/average.json"
      JAVA_SUM_LONG_MANIFEST="${JAVA_SDK_ROOT}/examples/sum_long.json"
      JAVA_LONGEST_STRING_MANIFEST="${JAVA_SDK_ROOT}/examples/longest_string.json"
      JAVA_FUNNEL_STEPS_MANIFEST="${JAVA_SDK_ROOT}/examples/funnel_steps.json"
      JAVA_WINDOW_FUNNEL_MANIFEST="${JAVA_SDK_ROOT}/examples/window_funnel.json"
      JAVA_BITMAP_UNION_MANIFEST="${JAVA_SDK_ROOT}/examples/bitmap_union.json"
      JAVA_BITMAP_INTERSECTION_MANIFEST="${JAVA_SDK_ROOT}/examples/bitmap_intersection.json"
      JAVA_BITMAP_DIFFERENCE_MANIFEST="${JAVA_SDK_ROOT}/examples/bitmap_difference.json"
      WASM_ADD_MANIFEST="${WASM_TEST_BINARY_DIR}/add.json"
      WASM_PREFIX_MANIFEST="${WASM_TEST_BINARY_DIR}/prefix.json"
      WASM_AVG_F64_MANIFEST="${WASM_TEST_BINARY_DIR}/avg_f64.json"
      WASM_SUM_I64_MANIFEST="${WASM_TEST_BINARY_DIR}/sum_i64.json"
      WASM_LONGEST_STRING_MANIFEST="${WASM_TEST_BINARY_DIR}/longest_string.json"
      WASM_FUNNEL_STEPS_MANIFEST="${WASM_TEST_BINARY_DIR}/funnel_steps.json"
      WASM_WINDOW_FUNNEL_MANIFEST="${WASM_TEST_BINARY_DIR}/window_funnel.json"
      WASM_BITMAP_UNION_MANIFEST="${WASM_TEST_BINARY_DIR}/bitmap_union.json"
      WASM_BITMAP_INTERSECTION_MANIFEST="${WASM_TEST_BINARY_DIR}/bitmap_intersection.json"
      WASM_BITMAP_DIFFERENCE_MANIFEST="${WASM_TEST_BINARY_DIR}/bitmap_difference.json"
  )
  target_link_libraries(
    velox_java_wasm_udf_benchmark
    velox_java_udf
    velox_wasm_udf
    velox_expression
    velox_core
    velox_memory
    velox_vector
    Folly::folly
    Folly::follybenchmark
    gflags::gflags
    glog::glog
  )
endif()

if(VELOX_BUILD_JAVA_UDF_TESTING)
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
  add_executable(velox_java_udf_smoke_test JavaUdfSmokeTest.cpp)
  if(NOT VELOX_ENABLE_EXEC)
    target_sources(
      velox_java_udf_smoke_test
      PRIVATE ../../wasm/tests/WasmAggregateTestSupport.cpp
    )
  endif()
  add_dependencies(velox_java_udf_smoke_test velox_java_test_jar)
  target_compile_definitions(
    velox_java_udf_smoke_test
    PRIVATE
      JAVA_RUNTIME_JAR="${JAVA_RUNTIME_JAR}"
      JAVA_DEPENDENCY_CLASSPATH="${JAVA_DEPENDENCY_CLASSPATH}"
      JAVA_ADD_MANIFEST="${JAVA_SDK_ROOT}/examples/add.json"
      JAVA_PREFIX_MANIFEST="${JAVA_SDK_ROOT}/examples/prefix.json"
      JAVA_CHECKED_DIVIDE_MANIFEST="${JAVA_SDK_ROOT}/examples/checked_divide.json"
      JAVA_PROFILE_TRANSFORM_MANIFEST="${JAVA_SDK_ROOT}/examples/profile_transform.json"
      JAVA_AVERAGE_MANIFEST="${JAVA_SDK_ROOT}/examples/average.json"
      JAVA_SUM_LONG_MANIFEST="${JAVA_SDK_ROOT}/examples/sum_long.json"
  )
  target_link_libraries(
    velox_java_udf_smoke_test
    velox_java_udf
    velox_expression
    velox_core
    velox_memory
    velox_vector
  )
  if(APPLE)
    # CTest/CMake uses posix_spawn on macOS. A Zulu 8 JVM embedded directly in
    # that child is killed by the OS; launching it from a small shell child
    # avoids that JDK 8 launcher limitation while preserving the exit status.
    add_test(
      NAME velox_java_udf_smoke_test
      COMMAND
        /bin/sh -c
        "\"$<TARGET_FILE:velox_java_udf_smoke_test>\"; status=$?; exit $status"
    )
  else()
    add_test(NAME velox_java_udf_smoke_test COMMAND velox_java_udf_smoke_test)
  endif()
endif()
