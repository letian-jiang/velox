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

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <jni.h>

namespace facebook::velox::functions::java {

/// Initializes the process JVM. If a JVM already exists, it is reused and the
/// options are ignored. If options do not contain java.class.path, the value of
/// VELOX_JAVA_UDF_CLASSPATH is used when present.
void initializeJavaUdfRuntime(const std::vector<std::string>& jvmOptions = {});

class JavaInstance final {
 public:
  JavaInstance(
      const std::filesystem::path& jarPath,
      std::string implementationClass,
      bool aggregate);
  ~JavaInstance();

  JavaInstance(const JavaInstance&) = delete;
  JavaInstance& operator=(const JavaInstance&) = delete;

  std::string invokeScalar(std::string_view input);

  std::vector<uint32_t> create(uint32_t count);
  void destroy(std::string_view input);
  void update(std::string_view input);
  void updateSingleGroup(uint32_t handle, std::string_view input);
  std::string serialize(std::string_view input);
  void merge(std::string_view input);
  void mergeSingleGroup(uint32_t handle, std::string_view input);
  std::string finish(std::string_view input);

 private:
  std::string invokeBytes(jmethodID method, std::string_view input);
  void invokeVoid(jmethodID method, std::string_view input);
  void invokeSingleGroupVoid(
      jmethodID method,
      uint32_t handle,
      std::string_view input);

  jobject object_{nullptr};
  jmethodID close_{nullptr};
  jmethodID scalarApply_{nullptr};
  jmethodID create_{nullptr};
  jmethodID destroy_{nullptr};
  jmethodID update_{nullptr};
  jmethodID updateSingleGroup_{nullptr};
  jmethodID serialize_{nullptr};
  jmethodID merge_{nullptr};
  jmethodID mergeSingleGroup_{nullptr};
  jmethodID finish_{nullptr};
  bool aggregate_;
  std::mutex mutex_;
};

} // namespace facebook::velox::functions::java
