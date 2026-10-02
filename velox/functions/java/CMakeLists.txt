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
  velox_java_udf
  ArrowIpc.cpp
  JavaAggregate.cpp
  JavaVectorFunction.cpp
  Manifest.cpp
  Registration.cpp
  Runtime.cpp
  HEADERS
  ArrowIpc.h
  JavaAggregate.h
  JavaVectorFunction.h
  Manifest.h
  Registration.h
  Runtime.h
)

if(VELOX_MONO_LIBRARY)
  target_include_directories(velox PRIVATE ${JNI_INCLUDE_DIRS})
else()
  target_include_directories(velox_java_udf PRIVATE ${JNI_INCLUDE_DIRS})
endif()

velox_link_libraries(
  velox_java_udf
  arrow
  JNI::JVM
  velox_arrow_bridge
  velox_expression
  velox_type
  velox_vector
  velox_exception
  Folly::folly
)

if(VELOX_ENABLE_EXEC)
  velox_link_libraries(velox_java_udf velox_exec)
endif()

if(
  VELOX_BUILD_TESTING
  OR VELOX_BUILD_JAVA_UDF_TESTING
  OR VELOX_BUILD_UDF_BENCHMARKS
)
  add_subdirectory(tests)
endif()
