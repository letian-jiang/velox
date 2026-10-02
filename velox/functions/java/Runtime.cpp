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

#include "velox/functions/java/Runtime.h"

#include <cstdlib>
#include <limits>

#include "velox/common/base/Exceptions.h"

namespace facebook::velox::functions::java {
namespace {

struct JvmState {
  JavaVM* vm{nullptr};
  bool created{false};
  std::mutex mutex;
};

JvmState& state() {
  static JvmState instance;
  return instance;
}

class LocalFrame final {
 public:
  LocalFrame(JNIEnv* env, jint capacity) : env_(env) {
    VELOX_USER_CHECK_EQ(
        env_->PushLocalFrame(capacity), 0, "Cannot create JNI local frame");
  }

  ~LocalFrame() {
    env_->PopLocalFrame(nullptr);
  }

 private:
  JNIEnv* env_;
};

JNIEnv* environment() {
  auto& runtime = state();
  VELOX_USER_CHECK_NOT_NULL(
      runtime.vm, "Java UDF runtime has not been initialized");
  JNIEnv* env = nullptr;
  const auto status =
      runtime.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
  if (status == JNI_EDETACHED) {
    VELOX_USER_CHECK_EQ(
        runtime.vm->AttachCurrentThread(
            reinterpret_cast<void**>(&env), nullptr),
        JNI_OK,
        "Cannot attach Velox thread to the JVM");
  } else {
    VELOX_USER_CHECK_EQ(status, JNI_OK, "Cannot access the process JVM");
  }
  return env;
}

std::string javaString(JNIEnv* env, jstring value) {
  if (value == nullptr) {
    return {};
  }
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) {
    return {};
  }
  std::string result(utf);
  env->ReleaseStringUTFChars(value, utf);
  return result;
}

[[noreturn]] void throwJavaException(JNIEnv* env, std::string_view operation) {
  auto throwable = env->ExceptionOccurred();
  env->ExceptionClear();
  std::string detail = "unknown Java exception";
  if (throwable != nullptr) {
    auto bootstrap = env->FindClass("io/velox/udf/runtime/Bootstrap");
    if (bootstrap != nullptr) {
      auto format = env->GetStaticMethodID(
          bootstrap,
          "formatThrowable",
          "(Ljava/lang/Throwable;)Ljava/lang/String;");
      if (format != nullptr) {
        auto formatted = static_cast<jstring>(
            env->CallStaticObjectMethod(bootstrap, format, throwable));
        if (!env->ExceptionCheck() && formatted != nullptr) {
          detail = javaString(env, formatted);
        } else {
          env->ExceptionClear();
        }
      } else {
        env->ExceptionClear();
      }
    } else {
      env->ExceptionClear();
    }
    if (detail == "unknown Java exception") {
      auto throwableClass = env->GetObjectClass(throwable);
      auto toString =
          env->GetMethodID(throwableClass, "toString", "()Ljava/lang/String;");
      if (toString != nullptr) {
        auto message =
            static_cast<jstring>(env->CallObjectMethod(throwable, toString));
        if (!env->ExceptionCheck() && message != nullptr) {
          detail = javaString(env, message);
        } else {
          env->ExceptionClear();
        }
      }
    }
  }
  VELOX_USER_FAIL("Java UDF {} failed: {}", operation, detail);
}

void checkException(JNIEnv* env, std::string_view operation) {
  if (env->ExceptionCheck()) {
    throwJavaException(env, operation);
  }
}

jbyteArray makeByteArray(JNIEnv* env, std::string_view input) {
  VELOX_USER_CHECK_LE(
      input.size(),
      std::numeric_limits<jsize>::max(),
      "Java UDF input exceeds the JNI byte-array limit");
  auto bytes = env->NewByteArray(static_cast<jsize>(input.size()));
  checkException(env, "input byte-array allocation");
  VELOX_USER_CHECK_NOT_NULL(bytes, "Cannot allocate Java UDF input byte array");
  if (!input.empty()) {
    env->SetByteArrayRegion(
        bytes,
        0,
        static_cast<jsize>(input.size()),
        reinterpret_cast<const jbyte*>(input.data()));
    checkException(env, "input byte-array copy");
  }
  return bytes;
}

std::string copyByteArray(JNIEnv* env, jbyteArray bytes) {
  VELOX_USER_CHECK_NOT_NULL(bytes, "Java UDF returned a null byte array");
  const auto size = env->GetArrayLength(bytes);
  std::string result(size, '\0');
  if (size != 0) {
    env->GetByteArrayRegion(
        bytes, 0, size, reinterpret_cast<jbyte*>(result.data()));
    checkException(env, "output byte-array copy");
  }
  return result;
}

jmethodID requiredMethod(
    JNIEnv* env,
    jclass clazz,
    const char* name,
    const char* signature) {
  auto method = env->GetMethodID(clazz, name, signature);
  checkException(env, fmt::format("method linkage for {}{}", name, signature));
  VELOX_USER_CHECK_NOT_NULL(
      method, "Java UDF adapter is missing method '{}{}'", name, signature);
  return method;
}

} // namespace

void initializeJavaUdfRuntime(const std::vector<std::string>& jvmOptions) {
  auto& runtime = state();
  std::lock_guard<std::mutex> guard(runtime.mutex);
  if (runtime.vm != nullptr) {
    return;
  }

  jsize count = 0;
  JavaVM* existing = nullptr;
  VELOX_USER_CHECK_EQ(
      JNI_GetCreatedJavaVMs(&existing, 1, &count),
      JNI_OK,
      "Cannot inspect existing JVMs");
  if (count != 0) {
    runtime.vm = existing;
    return;
  }

  std::vector<std::string> options = jvmOptions;
  bool hasClasspath = false;
  for (const auto& option : options) {
    hasClasspath = hasClasspath || option.rfind("-Djava.class.path=", 0) == 0;
  }
  if (!hasClasspath) {
    if (const char* classpath = std::getenv("VELOX_JAVA_UDF_CLASSPATH")) {
      options.push_back(std::string("-Djava.class.path=") + classpath);
    }
  }

  std::vector<JavaVMOption> rawOptions(options.size());
  for (size_t i = 0; i < options.size(); ++i) {
    rawOptions[i].optionString = options[i].data();
    rawOptions[i].extraInfo = nullptr;
  }
  JavaVMInitArgs arguments{};
  arguments.version = JNI_VERSION_1_8;
  arguments.nOptions = static_cast<jint>(rawOptions.size());
  arguments.options = rawOptions.data();
  arguments.ignoreUnrecognized = JNI_FALSE;
  JNIEnv* env = nullptr;
  JavaVM* vm = nullptr;
  const auto result =
      JNI_CreateJavaVM(&vm, reinterpret_cast<void**>(&env), &arguments);
  VELOX_USER_CHECK_EQ(result, JNI_OK, "Cannot create Java UDF JVM");
  runtime.vm = vm;
  runtime.created = true;
}

JavaInstance::JavaInstance(
    const std::filesystem::path& jarPath,
    std::string implementationClass,
    bool aggregate)
    : aggregate_(aggregate) {
  initializeJavaUdfRuntime();
  auto* env = environment();
  LocalFrame frame(env, 32);
  auto bootstrap = env->FindClass("io/velox/udf/runtime/Bootstrap");
  checkException(env, "bootstrap class loading");
  VELOX_USER_CHECK_NOT_NULL(
      bootstrap,
      "Cannot load io.velox.udf.runtime.Bootstrap; configure the Java UDF runtime classpath");
  const char* methodName = aggregate ? "loadAggregate" : "loadScalar";
  const char* returnType = aggregate
      ? "Lio/velox/udf/runtime/BatchAggregateAdapter;"
      : "Lio/velox/udf/runtime/BatchScalarAdapter;";
  const auto signature =
      fmt::format("(Ljava/lang/String;Ljava/lang/String;){}", returnType);
  auto load = env->GetStaticMethodID(bootstrap, methodName, signature.c_str());
  checkException(env, "bootstrap method linkage");
  VELOX_USER_CHECK_NOT_NULL(load, "Java UDF bootstrap method is missing");
  auto jar = env->NewStringUTF(jarPath.string().c_str());
  auto clazzName = env->NewStringUTF(implementationClass.c_str());
  checkException(env, "adapter name allocation");
  auto localObject =
      env->CallStaticObjectMethod(bootstrap, load, jar, clazzName);
  checkException(env, "adapter loading");
  VELOX_USER_CHECK_NOT_NULL(localObject, "Java UDF bootstrap returned null");
  object_ = env->NewGlobalRef(localObject);
  checkException(env, "adapter global reference creation");
  VELOX_USER_CHECK_NOT_NULL(object_, "Cannot retain Java UDF adapter");

  auto objectClass = env->GetObjectClass(localObject);
  close_ = requiredMethod(env, objectClass, "close", "()V");
  if (!aggregate) {
    scalarApply_ = requiredMethod(env, objectClass, "apply", "([B)[B");
  } else {
    create_ = requiredMethod(env, objectClass, "create", "(I)[I");
    destroy_ = requiredMethod(env, objectClass, "destroy", "([B)V");
    update_ = requiredMethod(env, objectClass, "update", "([B)V");
    updateSingleGroup_ =
        requiredMethod(env, objectClass, "updateSingleGroup", "(I[B)V");
    serialize_ = requiredMethod(env, objectClass, "serialize", "([B)[B");
    merge_ = requiredMethod(env, objectClass, "merge", "([B)V");
    mergeSingleGroup_ =
        requiredMethod(env, objectClass, "mergeSingleGroup", "(I[B)V");
    finish_ = requiredMethod(env, objectClass, "finish", "([B)[B");
  }
}

JavaInstance::~JavaInstance() {
  if (object_ == nullptr || state().vm == nullptr) {
    return;
  }
  auto* env = environment();
  env->CallVoidMethod(object_, close_);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteGlobalRef(object_);
}

std::string JavaInstance::invokeBytes(
    jmethodID method,
    std::string_view input) {
  auto* env = environment();
  LocalFrame frame(env, 16);
  auto bytes = makeByteArray(env, input);
  auto output =
      static_cast<jbyteArray>(env->CallObjectMethod(object_, method, bytes));
  checkException(env, "batch invocation");
  return copyByteArray(env, output);
}

void JavaInstance::invokeVoid(jmethodID method, std::string_view input) {
  auto* env = environment();
  LocalFrame frame(env, 16);
  auto bytes = makeByteArray(env, input);
  env->CallVoidMethod(object_, method, bytes);
  checkException(env, "aggregate batch invocation");
}

void JavaInstance::invokeSingleGroupVoid(
    jmethodID method,
    uint32_t handle,
    std::string_view input) {
  VELOX_USER_CHECK_NE(handle, 0, "Java UDAF state handle must be non-zero");
  auto* env = environment();
  LocalFrame frame(env, 16);
  auto bytes = makeByteArray(env, input);
  env->CallVoidMethod(object_, method, static_cast<jint>(handle), bytes);
  checkException(env, "single-group aggregate invocation");
}

std::string JavaInstance::invokeScalar(std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  VELOX_USER_CHECK(!aggregate_, "Aggregate adapter used as a scalar UDF");
  return invokeBytes(scalarApply_, input);
}

std::vector<uint32_t> JavaInstance::create(uint32_t count) {
  std::lock_guard<std::mutex> guard(mutex_);
  VELOX_USER_CHECK(aggregate_, "Scalar adapter used as a Java UDAF");
  VELOX_USER_CHECK_LE(
      count,
      static_cast<uint32_t>(std::numeric_limits<jint>::max()),
      "Too many Java UDAF groups in one create call");
  auto* env = environment();
  LocalFrame frame(env, 16);
  auto handles = static_cast<jintArray>(
      env->CallObjectMethod(object_, create_, static_cast<jint>(count)));
  checkException(env, "aggregate state creation");
  VELOX_USER_CHECK_NOT_NULL(handles, "Java UDAF create returned null");
  const auto size = env->GetArrayLength(handles);
  VELOX_USER_CHECK_EQ(
      size, count, "Java UDAF create returned the wrong handle count");
  std::vector<jint> raw(size);
  if (size != 0) {
    env->GetIntArrayRegion(handles, 0, size, raw.data());
    checkException(env, "aggregate handle copy");
  }
  std::vector<uint32_t> result(size);
  for (size_t i = 0; i < raw.size(); ++i) {
    result[i] = static_cast<uint32_t>(raw[i]);
  }
  return result;
}

void JavaInstance::destroy(std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  invokeVoid(destroy_, input);
}

void JavaInstance::update(std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  invokeVoid(update_, input);
}

void JavaInstance::updateSingleGroup(uint32_t handle, std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  invokeSingleGroupVoid(updateSingleGroup_, handle, input);
}

std::string JavaInstance::serialize(std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  return invokeBytes(serialize_, input);
}

void JavaInstance::merge(std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  invokeVoid(merge_, input);
}

void JavaInstance::mergeSingleGroup(uint32_t handle, std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  invokeSingleGroupVoid(mergeSingleGroup_, handle, input);
}

std::string JavaInstance::finish(std::string_view input) {
  std::lock_guard<std::mutex> guard(mutex_);
  return invokeBytes(finish_, input);
}

} // namespace facebook::velox::functions::java
