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

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/testutil/TempDirectoryPath.h"
#include "velox/exec/Driver.h"
#include "velox/exec/Task.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"
#include "velox/functions/wasm/ArrowIpc.h"
#include "velox/functions/wasm/Runtime.h"

namespace facebook::velox::functions::wasm::test {
namespace {

constexpr uint64_t kMemoryLimit = 64 << 10;
constexpr uint64_t kFuel = 1'000;
const std::string kLoop = "(loop $forever (br $forever))";
const std::string kResult = "(v128.const i32x4 0 4096 1 0)";

class RuntimeTest : public functions::test::FunctionBaseTest {
 protected:
  std::shared_ptr<WasmModule> module(
      const std::string& run = kResult,
      const std::string& alloc = "i32.const 1024",
      const std::string& free = "",
      const std::string& start = "",
      const std::string& create = kResult,
      const std::string& payload = "x",
      const std::string& singleBody = "",
      const std::string& imports = "") {
    auto directory = common::testutil::TempDirectoryPath::create();
    const auto path = directory->getPath() + "/fuel.wasm";
    std::string escaped;
    for (uint8_t byte : payload) {
      escaped += fmt::format("\\{:02x}", byte);
    }
    const auto wat = "(module" + imports +
        " (memory (export \"memory\") 1)"
        " (data (i32.const 4096) \"" +
        escaped +
        "\")"
        " (func (export \"velox_wasm_alloc\") (param i32) (result i32) " +
        alloc +
        ")"
        " (func (export \"velox_wasm_free\") (param i32 i32) " +
        free +
        ")"
        " (func (export \"run\") (param i32 i32) (result v128) " +
        run +
        ")"
        " (func (export \"single\") (param i32 i32 i32) (result v128) " +
        (singleBody.empty() ? run : singleBody) +
        ")"
        " (func (export \"create\") (param i32) (result v128) " +
        create +
        ")"
        " (func $start " +
        start + ") (start $start))";
    wasm_byte_vec_t bytes;
    auto* error = wasmtime_wat2wasm(wat.data(), wat.size(), &bytes);
    if (error != nullptr) {
      wasmtime_error_delete(error);
      throw std::runtime_error("Cannot compile runtime test WAT");
    }
    {
      std::ofstream output(path, std::ios::binary);
      output.write(bytes.data, bytes.size);
    }
    wasm_byte_vec_delete(&bytes);
    return WasmModule::compile(path);
  }

  ArrowIpcInput input() {
    return gatherToArrowIpc(
               SelectivityVector(1), {makeFlatVector<int64_t>({42})}, pool())
        .input;
  }
};

TEST_F(RuntimeTest, interruptsStartForScalarAndAggregate) {
  auto compiled = module(kResult, "i32.const 1024", "", kLoop);
  VELOX_ASSERT_THROW(
      WasmInstance(compiled, "run", kMemoryLimit, kFuel), "fuel");
  VELOX_ASSERT_THROW(
      WasmInstance(
          compiled, "create", {"run"}, {"single"}, kMemoryLimit, kFuel),
      "fuel");
}

TEST_F(RuntimeTest, interruptsScalarAndInvalidatesStore) {
  WasmInstance instance(module(kLoop + kResult), "run", kMemoryLimit, kFuel);
  auto batch = input();
  VELOX_ASSERT_THROW(instance.invoke(batch), "fuel");
  VELOX_ASSERT_THROW(instance.invoke(batch), "invalidated");
}

TEST_F(RuntimeTest, interruptsAggregateEntrypoints) {
  WasmInstance instance(
      module(kLoop + kResult, "i32.const 1024", "", "", kLoop + kResult),
      "create",
      {"run"},
      {"single"},
      kMemoryLimit,
      kFuel);
  VELOX_ASSERT_THROW(instance.invokeCount("create", 1), "fuel");
  auto batch = input();
  VELOX_ASSERT_THROW(instance.invoke("run", batch), "invalidated");
  VELOX_ASSERT_THROW(
      instance.invokeSingleGroup("single", 1, batch), "invalidated");
}

TEST_F(RuntimeTest, typedStatusesFailAggregateEntrypointsAndInvalidateStore) {
  const std::string message = "aggregate error: {}\nUTF-8: \xe4\xb8\xad";
  auto batch = input();
  for (uint32_t code = 1; code <= 11; ++code) {
    const auto result = fmt::format(
        "(v128.const i32x4 {} 4096 {} 0)", 0x100 + code, message.size());
    auto compiled = module(result, "i32.const 1024", "", "", result, message);
    for (int entry = 0; entry < 3; ++entry) {
      SCOPED_TRACE(fmt::format("status {} entry {}", code, entry));
      WasmInstance instance(
          compiled, "create", {"run"}, {"single"}, kMemoryLimit, kFuel);
      auto invoke = [&] {
        if (entry == 0)
          return instance.invokeCount("create", 1);
        if (entry == 1)
          return instance.invoke("run", batch);
        return instance.invokeSingleGroup("single", 1, batch);
      };
      if (code == 1) {
        EXPECT_THROW(invoke(), VeloxUserError);
      } else {
        EXPECT_THROW(invoke(), VeloxRuntimeError);
      }
      VELOX_ASSERT_THROW(instance.invokeCount("create", 1), "invalidated");
      VELOX_ASSERT_THROW(instance.invoke("run", batch), "invalidated");
      VELOX_ASSERT_THROW(
          instance.invokeSingleGroup("single", 1, batch), "invalidated");
    }
  }
}

TEST_F(RuntimeTest, malformedStatusLanesAndLegacyStatusRemainFatal) {
  auto batch = input();
  for (uint32_t code : {1u, 2u, 0x100u, 0x10cu, 0xffffffffu}) {
    const auto result = fmt::format("(v128.const i32x4 {} 4096 1 0)", code);
    WasmInstance instance(module(result), "run", kMemoryLimit, kFuel);
    EXPECT_THROW(instance.invoke(batch), VeloxRuntimeError);
    VELOX_ASSERT_THROW(instance.invoke(batch), "invalidated");
  }
  // Reserved lane validation takes precedence over a valid typed UserError.
  WasmInstance reserved(
      module("(v128.const i32x4 257 4096 1 1)"), "run", kMemoryLimit, kFuel);
  VELOX_ASSERT_THROW(reserved.invoke(batch), "reserved");
  VELOX_ASSERT_THROW(reserved.invoke(batch), "invalidated");
}

TEST_F(RuntimeTest, interruptsAllocatorAndDeallocator) {
  auto batch = input();
  WasmInstance allocator(
      module(kResult, kLoop + "i32.const 1024"), "run", kMemoryLimit, kFuel);
  VELOX_ASSERT_THROW(allocator.invoke(batch), "fuel");
  WasmInstance deallocator(
      module(kResult, "i32.const 1024", kLoop), "run", kMemoryLimit, kFuel);
  VELOX_ASSERT_THROW(deallocator.invoke(batch), "fuel");
}

TEST_F(RuntimeTest, refreshesBudgetForEveryCall) {
  const std::string finite =
      "(local $n i32) (local.set $n (i32.const 80))"
      " (loop $work (local.set $n (i32.sub (local.get $n) (i32.const 1)))"
      " (br_if $work (local.get $n))) " +
      kResult;
  WasmInstance instance(module(finite), "run", kMemoryLimit, kFuel);
  auto batch = input();
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(instance.invoke(batch), "x");
  }
}

TEST_F(RuntimeTest, rejectsUnlimitedBudget) {
  VELOX_ASSERT_THROW(
      WasmInstance(module(), "run", kMemoryLimit, 0), "positive");
}

TEST_F(RuntimeTest, driverTaskCancellationInterruptsGuest) {
  auto plan = exec::test::PlanBuilder()
                  .values({makeRowVector({makeFlatVector<int64_t>({42})})})
                  .planFragment();
  auto task = exec::Task::create(
      "wasm-cancellation",
      std::move(plan),
      0,
      core::QueryCtx::create(),
      exec::Task::ExecutionMode::kSerial);
  exec::DriverCtx driver(task, 0, 0, exec::kUngroupedGroupId, 0);
  exec::ScopedDriverThreadContext scopedDriver(&driver);
  auto cancelled = currentWasmCancellationCheck();
  ASSERT_TRUE(cancelled);
  EXPECT_FALSE(cancelled());
  WasmOptions options;
  options.fuelPerCall = std::numeric_limits<uint64_t>::max();
  options.maxCallMillis = 5'000;
  WasmInstance instance(
      module(kLoop + kResult), "run", options, pool(), cancelled);
  auto batch = input();
  const auto started = std::chrono::steady_clock::now();
  std::jthread canceller([task] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    task->requestCancel();
  });
  VELOX_ASSERT_THROW(instance.invoke(batch), "cancelled");
  EXPECT_LT(
      std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  EXPECT_TRUE(cancelled());
}

TEST_F(RuntimeTest, chargesAccessiblePagesAndTracksCrossThreadGrowth) {
  auto data = input();
  const auto before = pool()->usedBytes();
  WasmOptions policy;
  policy.memoryLimitBytes = 4 * 65536;
  auto compiled = module("i32.const 1 memory.grow drop " + kResult);
  {
    WasmInstance instance(compiled, "run", policy, pool());
    EXPECT_EQ(instance.linearMemoryBytes(), 65536);
    EXPECT_EQ(pool()->usedBytes(), before + 65536);
    std::exception_ptr error;
    std::thread other([&] {
      try {
        EXPECT_EQ(instance.invoke(data), "x");
      } catch (...) {
        error = std::current_exception();
      }
    });
    other.join();
    if (error)
      std::rethrow_exception(error);
    EXPECT_EQ(instance.linearMemoryBytes(), 2 * 65536);
    EXPECT_EQ(pool()->usedBytes(), before + 2 * 65536);
    EXPECT_EQ(instance.invoke(data), "x");
    EXPECT_EQ(instance.linearMemoryBytes(), 3 * 65536);
    instance.invalidate();
    EXPECT_EQ(instance.linearMemoryBytes(), 0);
    EXPECT_EQ(pool()->usedBytes(), before);
  }
  EXPECT_EQ(pool()->usedBytes(), before);
}

TEST_F(RuntimeTest, latePoolAttachmentChargesCurrentPagesAndGrowth) {
  auto data = input();
  WasmOptions policy;
  policy.memoryLimitBytes = 4 * 65536;
  WasmInstance instance(
      module("i32.const 1 memory.grow drop " + kResult), "run", policy);
  EXPECT_EQ(instance.invoke(data), "x");
  const auto before = pool()->usedBytes();
  instance.setMemoryPool(pool());
  EXPECT_EQ(pool()->usedBytes(), before + 2 * 65536);
  instance.setMemoryPool(pool());
  EXPECT_EQ(pool()->usedBytes(), before + 2 * 65536);
  EXPECT_EQ(instance.invoke(data), "x");
  EXPECT_EQ(pool()->usedBytes(), before + 3 * 65536);
  instance.invalidate();
  instance.invalidate();
  EXPECT_EQ(pool()->usedBytes(), before);
}

TEST_F(RuntimeTest, queryPoolFailureCannotBeHiddenByGuestMemoryGrow) {
  auto data = input();
  auto root =
      memory::memoryManager()->addRootPool("wasm-grow-limit", 16 * 65536);
  auto leaf = root->addLeafChild("wasm-grow-limit-leaf");
  WasmOptions policy;
  policy.memoryLimitBytes = 32 * 65536;
  auto compiled = module("i32.const 16 memory.grow drop " + kResult);
  WasmInstance instance(compiled, "run", policy, leaf.get());
  EXPECT_EQ(leaf->usedBytes(), 65536);
  EXPECT_THROW(instance.invoke(data), VeloxRuntimeError);
  EXPECT_EQ(instance.linearMemoryBytes(), 0);
  EXPECT_EQ(leaf->usedBytes(), 0);
  VELOX_ASSERT_THROW(instance.invoke(data), "invalidated");
  auto duringStart =
      module(kResult, "i32.const 1024", "", "i32.const 16 memory.grow drop");
  EXPECT_THROW(
      WasmInstance(duringStart, "run", policy, leaf.get()), VeloxRuntimeError);
  EXPECT_EQ(leaf->usedBytes(), 0);
}

TEST_F(
    RuntimeTest,
    allocationAndFreeGrowthEnforceNativePoolAndReleaseOnFailure) {
  auto data = input();
  auto root = memory::memoryManager()->addRootPool(
      "wasm-helper-grow-limit", 16 * 65536);
  auto leaf = root->addLeafChild("wasm-helper-grow-limit-leaf");
  WasmOptions policy;
  policy.memoryLimitBytes = 32 * 65536;
  for (bool allocating : {false, true}) {
    SCOPED_TRACE(allocating);
    auto compiled = module(
        kResult,
        allocating ? "i32.const 16 memory.grow drop i32.const 1024"
                   : "i32.const 1024",
        allocating ? "" : "i32.const 16 memory.grow drop");
    WasmInstance instance(compiled, "run", policy, leaf.get());
    EXPECT_THROW(instance.invoke(data), VeloxRuntimeError);
    EXPECT_EQ(leaf->usedBytes(), 0);
    VELOX_ASSERT_THROW(instance.invoke(data), "invalidated");
  }
}

TEST_F(
    RuntimeTest,
    uncommittedPagesRemainGuardedAndSuccessfulGrowthIsZeroFilled) {
  auto data = input();
  WasmOptions policy;
  policy.memoryLimitBytes = 4 * 65536;
  const auto before = pool()->usedBytes();
  {
    WasmInstance invalid(
        module("i32.const 65535 i32.load drop " + kResult),
        "run",
        policy,
        pool());
    VELOX_ASSERT_THROW(invalid.invoke(data), "out of bounds");
    EXPECT_EQ(pool()->usedBytes(), before);
  }
  {
    auto run =
        "i32.const 1 memory.grow drop (if (i32.load (i32.const 65536)) (then unreachable)) " +
        kResult;
    WasmInstance valid(module(run), "run", policy, pool());
    EXPECT_EQ(valid.invoke(data), "x");
    EXPECT_EQ(valid.linearMemoryBytes(), 2 * 65536);
  }
  EXPECT_EQ(pool()->usedBytes(), before);
}

TEST_F(RuntimeTest, checkpointBufferIsQueryOwnedAndSurvivesStoreRetirement) {
  const auto before = pool()->usedBytes();
  WasmInstance instance(module(), "run", WasmOptions{}, pool());
  instance.addCountEntrypoint("create");
  auto saved = instance.invokeCountBuffer("create", 1024);
  ASSERT_EQ(saved->size(), 1);
  EXPECT_EQ(saved->as<uint8_t>()[0], 'x');
  EXPECT_GT(pool()->usedBytes(), before + 65536);
  instance.invalidate();
  EXPECT_EQ(saved->as<uint8_t>()[0], 'x');
  EXPECT_GT(pool()->usedBytes(), before);
  saved.reset();
  EXPECT_EQ(pool()->usedBytes(), before);
}

TEST_F(RuntimeTest, checkpointCopyHonorsNativePoolAndPoisonsFailedStore) {
  auto root =
      memory::memoryManager()->addRootPool("wasm-checkpoint-limit", 16 * 65536);
  auto leaf = root->addLeafChild("wasm-checkpoint-limit-leaf");
  WasmOptions policy;
  policy.memoryLimitBytes = 32 * 65536;
  WasmInstance instance(
      module(
          kResult,
          "i32.const 1024",
          "",
          "",
          "i32.const 15 memory.grow drop " + kResult),
      "run",
      policy,
      leaf.get());
  instance.addCountEntrypoint("create");
  EXPECT_THROW(instance.invokeCountBuffer("create", 1024), VeloxRuntimeError);
  EXPECT_EQ(instance.linearMemoryBytes(), 0);
  EXPECT_EQ(leaf->usedBytes(), 0);
  VELOX_ASSERT_THROW(instance.invokeCountBuffer("create", 1024), "invalidated");
}

TEST_F(RuntimeTest, rawRestoreBytesForwardBudgetPointerAndLength) {
  const auto single =
      "(if (i32.ne (local.get 0) (i32.const 9)) (then unreachable)) "
      "(if (i32.ne (local.get 2) (i32.const 3)) (then unreachable)) "
      "(if (i32.ne (i32.load8_u (local.get 1)) (i32.const 97)) (then unreachable)) " +
      kResult;
  WasmInstance instance(
      module(kResult, "i32.const 1024", "", "", kResult, "x", single),
      "run",
      WasmOptions{},
      pool());
  instance.addSingleGroupEntrypoint("single");
  EXPECT_EQ(instance.invokeBytes("single", "abc", 9), "x");
  VELOX_ASSERT_THROW(
      instance.invokeBytes("single", "abc", 8), "invocation failed");
  VELOX_ASSERT_THROW(instance.invokeBytes("single", "abc", 9), "invalidated");
}

const std::string kLambdaImports =
    "(import \"velox_udf_v1\" \"lambda_call\" (func $lc (param i32 i32 i32) (result i64)))"
    "(import \"velox_udf_v1\" \"lambda_result\" (func $lr (param i32 i32 i32) (result i32)))";
const std::string kLambdaRequest =
    "i32.const 0 i32.const 4096 i32.const 1 call $lc local.set $reply ";
const std::string kLambdaRead =
    "local.get $reply i64.const 32 i64.shr_u i32.wrap_i64 i32.const 8192 local.get $reply i32.wrap_i64 call $lr drop ";
const std::string kLambdaBatchImports = kLambdaImports +
    "(import \"velox_udf_v1\" \"lambda_call_batch\" (func $lcb (param i32 i32 i32 i32) (result i64)))";
std::string lambdaBatchRequest(int64_t rows) {
  return "i32.const 0 i32.const 4096 i32.const 1 i32.const " +
      std::to_string(rows) + " call $lcb local.set $reply ";
}

TEST_F(RuntimeTest, nativeLambdaBatchesHaveRowQuotasAndResetAcrossExports) {
  auto reply = input();
  WasmOptions options;
  options.maxLambdaRowsPerCall = 4;
  options.maxLambdaEvaluations = 4;
  auto compiled = module(
      "(local $reply i64) " + lambdaBatchRequest(4) + kLambdaRead + kResult,
      "i32.const 1024",
      "",
      "",
      kResult,
      "x",
      "",
      kLambdaBatchImports);
  WasmInstance instance(compiled, "run", options, pool());
  int calls = 0;
  instance.setLambdaCallback(
      [&](uint32_t index, std::string_view request, uint32_t rows) {
        EXPECT_EQ(index, 0);
        EXPECT_EQ(request, "x");
        EXPECT_EQ(rows, 4);
        ++calls;
        return reply;
      });
  EXPECT_EQ(instance.invoke(reply), "x");
  EXPECT_EQ(instance.invoke(reply), "x");
  EXPECT_EQ(calls, 2);
  instance.setLambdaCallback(WasmInstance::LambdaCallback{});
  VELOX_ASSERT_THROW(instance.invoke(reply), "capability is not bound");
  for (const int64_t rows : {0, -1, 5}) {
    SCOPED_TRACE(rows);
    WasmInstance invalid(
        module(
            "(local $reply i64) " + lambdaBatchRequest(rows) + kLambdaRead +
                kResult,
            "i32.const 1024",
            "",
            "",
            kResult,
            "x",
            "",
            kLambdaBatchImports),
        "run",
        options,
        pool());
    invalid.setLambdaCallback([&](uint32_t, std::string_view, uint32_t) {
      ++calls;
      return reply;
    });
    VELOX_ASSERT_THROW(
        invalid.invoke(reply),
        rows == 0 ? "must contain rows" : "exceeds row limit");
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(invalid.linearMemoryBytes(), 0);
  }
  options.maxLambdaEvaluations = 3;
  WasmInstance overBudget(
      module(
          "(local $reply i64) " + lambdaBatchRequest(2) + kLambdaRead +
              lambdaBatchRequest(2) + kLambdaRead + kResult,
          "i32.const 1024",
          "",
          "",
          kResult,
          "x",
          "",
          kLambdaBatchImports),
      "run",
      options,
      pool());
  calls = 0;
  overBudget.setLambdaCallback([&](uint32_t, std::string_view, uint32_t) {
    ++calls;
    return reply;
  });
  VELOX_ASSERT_THROW(overBudget.invoke(reply), "evaluation limit exceeded");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(overBudget.linearMemoryBytes(), 0);
}

TEST_F(RuntimeTest, nativeLambdaBatchImportRequiresExactTypeAndAuthority) {
  auto reply = input();
  VELOX_ASSERT_THROW(
      module(
          kResult,
          "i32.const 1024",
          "",
          "",
          kResult,
          "x",
          "",
          "(import \"velox_udf_v1\" \"lambda_call_batch\" (func (param i32 i32 i32) (result i64)))"),
      "unsupported imports");
  auto compiled = module(
      "(local $reply i64) " + lambdaBatchRequest(2) + kLambdaRead + kResult,
      "i32.const 1024",
      "",
      "",
      kResult,
      "x",
      "",
      kLambdaBatchImports);
  WasmInstance unbound(compiled, "run", WasmOptions{}, pool());
  VELOX_ASSERT_THROW(unbound.invoke(reply), "capability is not bound");
  WasmInstance singleProvider(compiled, "run", WasmOptions{}, pool());
  int calls = 0;
  singleProvider.setLambdaCallback([&](uint32_t, std::string_view) {
    ++calls;
    return reply;
  });
  VELOX_ASSERT_THROW(
      singleProvider.invoke(reply),
      "Single-row lambda provider cannot evaluate a batch");
  EXPECT_EQ(calls, 0);
}
TEST_F(RuntimeTest, nativeLambdaResultsAreChargedConsumedOnceAndBoundToStore) {
  auto reply = input();
  auto compiled = module(
      "(local $reply i64) " + kLambdaRequest + kLambdaRead + kResult,
      "i32.const 1024",
      "",
      "",
      kResult,
      "x",
      "",
      kLambdaImports);
  WasmInstance unbound(compiled, "run", WasmOptions{}, pool());
  VELOX_ASSERT_THROW(unbound.invoke(reply), "capability is not bound");
  int calls = 0;
  const auto before = pool()->usedBytes();
  WasmInstance bound(compiled, "run", WasmOptions{}, pool());
  bound.setLambdaCallback([&](uint32_t index, std::string_view request) {
    EXPECT_EQ(index, 0);
    EXPECT_EQ(request, "x");
    ++calls;
    return reply;
  });
  EXPECT_EQ(bound.invoke(reply), "x");
  EXPECT_EQ(bound.invoke(reply), "x");
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(pool()->usedBytes(), before + 65536);
  bound.invalidate();
  EXPECT_EQ(pool()->usedBytes(), before);
  WasmInstance replay(
      module(
          "(local $reply i64) " + kLambdaRequest + kLambdaRead + kLambdaRead +
              kResult,
          "i32.const 1024",
          "",
          "",
          kResult,
          "x",
          "",
          kLambdaImports),
      "run",
      WasmOptions{},
      pool());
  replay.setLambdaCallback([&](uint32_t, std::string_view) { return reply; });
  VELOX_ASSERT_THROW(replay.invoke(reply), "expired Wasm lambda result");
  EXPECT_EQ(pool()->usedBytes(), before);
}
TEST_F(RuntimeTest, nativeLambdaFailuresNeverUnwindThroughWasmtime) {
  auto reply = input();
  auto compiled = module(
      "(local $reply i64) " + kLambdaRequest + kLambdaRead + kResult,
      "i32.const 1024",
      "",
      "",
      kResult,
      "x",
      "",
      kLambdaImports);
  WasmInstance error(compiled, "run", WasmOptions{}, pool());
  error.setLambdaCallback([](uint32_t, std::string_view) -> ArrowIpcInput {
    VELOX_USER_FAIL("native lambda user error");
  });
  EXPECT_THROW(error.invoke(reply), VeloxUserError);
  VELOX_ASSERT_THROW(error.invoke(reply), "invalidated");
  WasmOptions policy;
  policy.maxLambdaCalls = 1;
  WasmInstance tooMany(
      module(
          "(local $reply i64) " + kLambdaRequest + kLambdaRead +
              kLambdaRequest + kLambdaRead + kResult,
          "i32.const 1024",
          "",
          "",
          kResult,
          "x",
          "",
          kLambdaImports),
      "run",
      policy,
      pool());
  int calls = 0;
  tooMany.setLambdaCallback([&](uint32_t, std::string_view) {
    ++calls;
    return reply;
  });
  VELOX_ASSERT_THROW(tooMany.invoke(reply), "lambda call limit");
  EXPECT_EQ(calls, 1);
  WasmInstance pending(
      module(
          "(local $reply i64) " + kLambdaRequest + kResult,
          "i32.const 1024",
          "",
          "",
          kResult,
          "x",
          "",
          kLambdaImports),
      "run",
      WasmOptions{},
      pool());
  pending.setLambdaCallback([&](uint32_t, std::string_view) { return reply; });
  VELOX_ASSERT_THROW(pending.invoke(reply), "result was not consumed");
  EXPECT_EQ(pending.linearMemoryBytes(), 0);
  bool cancelled = false;
  WasmInstance cancellation(
      compiled, "run", WasmOptions{}, pool(), [&] { return cancelled; });
  cancellation.setLambdaCallback([&](uint32_t, std::string_view) {
    cancelled = true;
    return reply;
  });
  VELOX_ASSERT_THROW(cancellation.invoke(reply), "cancelled");
  EXPECT_EQ(cancellation.linearMemoryBytes(), 0);
}

TEST_F(RuntimeTest, nativeLambdaRejectsForgedMemoryAndResponseRequests) {
  auto reply = input();
  struct Case {
    std::string body;
    std::string error;
    int calls;
  };
  const std::vector<Case> cases{
      {"i32.const 0 i32.const -1 i32.const 1 call $lc drop " + kResult,
       "request is out of bounds",
       0},
      {"(local $reply i64) " + kLambdaRequest +
           "local.get $reply i64.const 32 i64.shr_u i32.wrap_i64 "
           "i32.const -1 local.get $reply i32.wrap_i64 call $lr drop " +
           kResult,
       "response is out of bounds",
       1},
      {"(local $reply i64) " + kLambdaRequest +
           "local.get $reply i64.const 32 i64.shr_u i32.wrap_i64 "
           "i32.const 8192 i32.const 0 call $lr drop " +
           kResult,
       "response length mismatch",
       1},
      {"(local $reply i64) " + kLambdaRequest + kLambdaRequest + kResult,
       "result was not consumed",
       1}};
  const auto before = pool()->usedBytes();
  for (const auto& test : cases) {
    SCOPED_TRACE(test.error);
    WasmInstance instance(
        module(
            test.body,
            "i32.const 1024",
            "",
            "",
            kResult,
            "x",
            "",
            kLambdaImports),
        "run",
        WasmOptions{},
        pool());
    int calls = 0;
    instance.setLambdaCallback([&](uint32_t, std::string_view) {
      ++calls;
      return reply;
    });
    VELOX_ASSERT_THROW(instance.invoke(reply), test.error);
    EXPECT_EQ(calls, test.calls);
    EXPECT_EQ(pool()->usedBytes(), before);
    VELOX_ASSERT_THROW(instance.invoke(reply), "invalidated");
  }
}

TEST_F(RuntimeTest, nativeLambdaEnforcesImportTypesAndStartAuthority) {
  for (
      const std::string imports :
      {"(import \"velox_udf_v1\" \"lambda_call\" (func (param i32 i32 i32) (result i32)))",
       "(import \"velox_udf_v1\" \"lambda_result\" (func (param i32 i64 i32) (result i32)))",
       "(import \"velox_udf_v1\" \"lambda_call\" (global i32))",
       "(import \"other\" \"lambda_call\" (func (param i32 i32 i32) (result i64)))"}) {
    SCOPED_TRACE(imports);
    VELOX_ASSERT_THROW(
        module(kResult, "i32.const 1024", "", "", kResult, "x", "", imports),
        "unsupported imports");
  }
  auto compiled = module(
      kResult,
      "i32.const 1024",
      "",
      "i32.const 0 i32.const 4096 i32.const 1 call $lc drop",
      kResult,
      "x",
      "",
      kLambdaImports);
  const auto before = pool()->usedBytes();
  VELOX_ASSERT_THROW(
      WasmInstance(compiled, "run", WasmOptions{}, pool()),
      "capability is not bound");
  EXPECT_EQ(pool()->usedBytes(), before);
}

TEST_F(RuntimeTest, nativeLambdaResponseObeysSizeAndQueryPoolLimits) {
  auto reply = input();
  auto compiled = module(
      "(local $reply i64) " + kLambdaRequest + kLambdaRead + kResult,
      "i32.const 1024",
      "",
      "",
      kResult,
      "x",
      "",
      kLambdaImports);
  WasmOptions policy;
  policy.maxOutputBytes = 1;
  WasmInstance limited(compiled, "run", policy, pool());
  limited.setLambdaCallback([&](uint32_t, std::string_view) { return reply; });
  VELOX_ASSERT_THROW(limited.invoke(reply), "response exceeds size limit");
  EXPECT_EQ(limited.linearMemoryBytes(), 0);
  auto big = gatherToArrowIpc(
                 SelectivityVector(1),
                 {makeFlatVector<std::string>({std::string(2UL << 20, 'x')})},
                 pool())
                 .input;
  auto root =
      memory::memoryManager()->addRootPool("lambda-pool-limit", 1UL << 20);
  auto leaf = root->addLeafChild("lambda-pool-limit-leaf");
  WasmInstance oom(compiled, "run", WasmOptions{}, leaf.get());
  oom.setLambdaCallback([&](uint32_t, std::string_view) { return big; });
  EXPECT_THROW(oom.invoke(reply), VeloxRuntimeError);
  EXPECT_EQ(leaf->usedBytes(), 0);
  VELOX_ASSERT_THROW(oom.invoke(reply), "invalidated");
}
} // namespace
} // namespace facebook::velox::functions::wasm::test
