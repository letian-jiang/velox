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

include(FindPackageHandleStandardArgs)

find_path(WASMTIME_INCLUDE_DIR wasmtime.h)
find_library(WASMTIME_LIBRARY NAMES wasmtime)

if(WASMTIME_INCLUDE_DIR)
  file(STRINGS "${WASMTIME_INCLUDE_DIR}/wasmtime.h" WASMTIME_VERSION_LINE
       REGEX "^#define WASMTIME_VERSION ")
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" Wasmtime_VERSION "${WASMTIME_VERSION_LINE}")
endif()

find_package_handle_standard_args(
  Wasmtime
  REQUIRED_VARS WASMTIME_LIBRARY WASMTIME_INCLUDE_DIR
  VERSION_VAR Wasmtime_VERSION
)

if(Wasmtime_FOUND AND NOT TARGET Wasmtime::wasmtime)
  add_library(Wasmtime::wasmtime UNKNOWN IMPORTED)
  set_target_properties(
    Wasmtime::wasmtime
    PROPERTIES
      IMPORTED_LOCATION "${WASMTIME_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${WASMTIME_INCLUDE_DIR}"
  )
endif()
