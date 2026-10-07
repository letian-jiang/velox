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

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include <arrow/array/builder_binary.h>
#include <arrow/util/compression.h>
#include <folly/json.h>

#include "velox/exec/AggregateCompanionSignatures.h"
#include "velox/exec/WindowFunction.h"
#include "velox/functions/Macros.h"
#include "velox/functions/Registerer.h"
#include "velox/functions/wasm/WasmAggregate.h"
#include "velox/functions/wasm/WasmVectorFunction.h"
#include "velox/functions/wasm/tests/WasmTestUtils.h"
#include "velox/vector/LazyVector.h"

namespace facebook::velox::functions::wasm::test {
namespace {

template <typename F>
void expectFatal(F&& operation, std::string_view message) {
  try {
    operation();
  } catch (const VeloxRuntimeError& error) {
    check(
        std::string(error.what()).find(message) != std::string::npos,
        "unexpected fatal error: " + std::string(error.what()));
    return;
  }
  throw std::runtime_error("expected fatal error: " + std::string(message));
}

template <typename F>
void expectCollision(F&& operation) {
  try {
    operation();
  } catch (const VeloxUserError&) {
    return;
  }
  throw std::runtime_error("expected registration collision");
}

class ModuleFile {
 public:
  explicit ModuleFile(std::string_view bytes) {
    static std::atomic<uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
        ("velox-wasm-safety-" + std::to_string(getpid()) + "-" +
         std::to_string(sequence++) + ".wasm");
    write(bytes);
  }
  ~ModuleFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  void write(std::string_view bytes) {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), bytes.size());
    check(stream.good(), "cannot write safety test module");
  }
  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

std::string watBytes(const std::string& wat) {
  wasm_byte_vec_t bytes;
  auto* error = wasmtime_wat2wasm(wat.data(), wat.size(), &bytes);
  if (error != nullptr) {
    wasmtime_error_delete(error);
    throw std::runtime_error("invalid safety test WAT");
  }
  std::string output(bytes.data, bytes.size);
  wasm_byte_vec_delete(&bytes);
  return output;
}

std::string tinyModule(
    const std::string& run = "(v128.const i32x4 0 4096 1 0)",
    const std::string& extra = "",
    const std::string& alloc = "i32.const 1024",
    const std::string& free = "") {
  return watBytes(
      "(module (memory (export \"memory\") 1) " + extra +
      " (data (i32.const 4096) \"x\")"
      " (func (export \"velox_wasm_alloc\") (param i32) (result i32) " +
      alloc +
      ")"
      " (func (export \"velox_wasm_free\") (param i32 i32) " +
      free +
      ")"
      " (func (export \"run\") (param i32 i32) (result v128) " +
      run + "))");
}

uint32_t readUleb(std::string_view bytes, size_t& offset) {
  uint32_t value = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    const auto byte = static_cast<uint8_t>(bytes.at(offset++));
    value |= static_cast<uint32_t>(byte & 127) << shift;
    if (!(byte & 128))
      return value;
  }
  throw std::runtime_error("invalid test module length");
}
void appendUleb(std::string& bytes, uint32_t value) {
  do {
    auto byte = value & 127;
    value >>= 7;
    bytes.push_back(static_cast<char>(byte | (value ? 128 : 0)));
  } while (value);
}

// Keep executable code intact and rewrite only embedded declarations. This
// exercises real guest exports with collisions and multi-module overloads.
std::string rewriteDeclarations(
    const std::function<bool(folly::dynamic&)>& edit) {
  const auto bytes = WasmModule::read(WASM_MODULE_PATH);
  std::string output = bytes.substr(0, 8);
  size_t offset = 8;
  while (offset < bytes.size()) {
    const auto sectionStart = offset;
    const auto id = bytes[offset++];
    const auto length = readUleb(bytes, offset);
    const auto end = offset + length;
    if (id == 0) {
      const auto nameStart = offset;
      const auto nameLength = readUleb(bytes, offset);
      const auto name = bytes.substr(offset, nameLength);
      offset += nameLength;
      if (name == "velox.udf.v1") {
        std::string payload = bytes.substr(nameStart, offset - nameStart);
        while (offset < end) {
          const auto terminator = bytes.find('\0', offset);
          check(terminator < end, "unterminated test declaration");
          if (terminator > offset) {
            auto declaration =
                folly::parseJson(bytes.substr(offset, terminator - offset));
            if (edit(declaration)) {
              payload += folly::toJson(declaration);
              payload.push_back('\0');
            }
          }
          offset = terminator + 1;
        }
        output.push_back(0);
        appendUleb(output, payload.size());
        output += payload;
        continue;
      }
    }
    output += bytes.substr(sectionStart, end - sectionStart);
    offset = end;
  }
  return output;
}

std::string constantResultModule(std::string_view ipc) {
  std::string escaped;
  for (uint8_t byte : ipc) {
    escaped += fmt::format("\\{:02x}", byte);
  }
  return tinyModule(
      fmt::format("(v128.const i32x4 0 8192 {} 0)", ipc.size()),
      "(data (i32.const 8192) \"" + escaped + "\")");
}

void testNativeStatusAbi(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto declaration = [](int version, bool rowApi) {
    return rewriteDeclarations([&](folly::dynamic& root) {
      if (root["name"].asString() != "strict_sum_wasm")
        return false;
      root["abi_version"] = version;
      root["row_api"] = rowApi;
      // This fixture tests legacy profile parsing, independently of the
      // additive ABI-3-only checkpoint capability on the source declaration.
      root.erase("state_checkpoint");
      root["entrypoints"].erase("checkpoint");
      root["entrypoints"].erase("restore");
      return true;
    });
  };
  for (int version : {1, 2, 3}) {
    ModuleFile module(declaration(version, true));
    auto manifests = loadEmbeddedManifests(module.path());
    check(
        manifests.aggregates.size() == 1 &&
            manifests.aggregates[0].abiVersion == version,
        "manifest ABI version was not preserved");
  }
  for (int version : {0, 4}) {
    ModuleFile module(declaration(version, true));
    expectCollision([&] { registerWasmModule(module.path()); });
  }
  ModuleFile invalidProfile(declaration(2, false));
  expectCollision([&] { registerWasmModule(invalidProfile.path()); });

  // A whole-export UserError remains a scalar bridge failure, even under TRY.
  // Only the validated row-error column can make a scalar error recoverable.
  auto bytes = tinyModule("(v128.const i32x4 257 4096 1 0)");
  const std::string metadata = R"({"abi_version":2,"kind":"scalar",
    "name":"safety_typed_export_error","entrypoint":"run","row_api":true,
    "arguments":[{"type":"bigint","nullable":true}],
    "return":{"type":"bigint","nullable":true}})";
  std::string section;
  appendUleb(section, std::string_view("velox.udf.v1").size());
  section += "velox.udf.v1";
  section += metadata;
  section.push_back('\0');
  bytes.push_back(0);
  appendUleb(bytes, section.size());
  bytes += section;
  ModuleFile module(bytes);
  registerWasmModule(module.path());
  auto input = makeRow({makeFlat<int64_t>(BIGINT(), {42}, pool)}, pool);
  expectFatal(
      [&] {
        evaluateCall(
            "safety_typed_export_error", BIGINT(), input, execCtx, true);
      },
      "execution or protocol failure");
}

void testCompanionRegistrationAndRecovery(
    memory::MemoryPool* pool,
    core::ExecCtx& execCtx) {
  const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  for (const auto& manifest : declarations.aggregates) {
    for (const auto& suffix : {"_partial", "_merge", "_merge_extract"}) {
      const auto name = manifest.name + suffix;
      const auto* entry = exec::getAggregateFunctionEntry(name);
      check(
          entry && entry->metadata.companionFunction,
          "missing native aggregate companion: " + name);
      check(
          !exec::windowFunctions().count(name),
          "companion was registered as a window function");
    }
    const auto name = manifest.name + "_extract";
    auto signatures = exec::getVectorFunctionSignatures(name);
    check(
        signatures && !signatures->empty(),
        "missing extract companion: " + name);
  }
  // Partial has a distinct output contract on a single-step operator.
  auto partial = exec::Aggregate::create(
      "avg_f64_partial",
      core::AggregationNode::Step::kSingle,
      {DOUBLE()},
      VARBINARY(),
      core::QueryConfig({}));
  check(
      partial->resultType()->isVarbinary(),
      "partial companion lost intermediate type");

  const auto before =
      exec::getAggregateFunctionEntry("checked_sum_wasm")->signatures;
  ModuleFile collision(rewriteDeclarations([](auto& root) {
    if (root["name"].asString() == "checked_sum_wasm")
      return true;
    if (root["name"].asString() == "add_i64") {
      root["name"] = "checked_sum_wasm_extract";
      return true;
    }
    return false;
  }));
  expectCollision([&] { registerWasmModule(collision.path(), true); });
  check(
      exec::getAggregateFunctionEntry("checked_sum_wasm")->signatures == before,
      "failed companion transaction modified the source aggregate");

  // The native family changes to return-type suffixes after an erased-state
  // overload is added. Verify stale names disappear and replacements remain.
  ModuleFile first(rewriteDeclarations([](auto& root) {
    if (root["name"].asString() != "sum_i64")
      return false;
    root["name"] = "safety_companion_refresh";
    return true;
  }));
  registerWasmModule(first.path());
  check(
      exec::getVectorFunctionSignatures("safety_companion_refresh_extract")
          .has_value(),
      "initial extract missing");
  ModuleFile second(rewriteDeclarations([](auto& root) {
    if (root["name"].asString() != "avg_f64")
      return false;
    root["name"] = "safety_companion_refresh";
    return true;
  }));
  registerWasmModule(second.path());
  check(
      !exec::getVectorFunctionSignatures("safety_companion_refresh_extract"),
      "stale unsuffixed extract retained");
  check(
      exec::getVectorFunctionSignatures(
          "safety_companion_refresh_extract_bigint")
              .has_value() &&
          exec::getVectorFunctionSignatures(
              "safety_companion_refresh_extract_double")
              .has_value(),
      "suffixed extracts missing");
  check(
      !exec::getAggregateFunctionEntry(
          "safety_companion_refresh_merge_extract") &&
          exec::getAggregateFunctionEntry(
              "safety_companion_refresh_merge_extract_bigint") &&
          exec::getAggregateFunctionEntry(
              "safety_companion_refresh_merge_extract_double"),
      "merge-extract family was not refreshed");
  registerWasmModule(second.path(), true);

  ModuleFile newCollision(rewriteDeclarations([](auto& root) {
    if (root["name"].asString() == "checked_sum_wasm") {
      root["name"] = "safety_failed_companion";
      return true;
    }
    if (root["name"].asString() == "add_i64") {
      root["name"] = "safety_failed_companion_extract";
      return true;
    }
    return false;
  }));
  expectCollision([&] { registerWasmModule(newCollision.path()); });
  check(
      !exec::getAggregateFunctionEntry("safety_failed_companion") &&
          !exec::getAggregateFunctionEntry("safety_failed_companion_partial") &&
          !exec::getVectorFunctionSignatures(
              "safety_failed_companion_extract") &&
          !exec::windowFunctions().count("safety_failed_companion"),
      "failed companion construction published part of a module");

  // Identical names in distinct scalar/aggregate namespaces must not cause
  // companion refresh to delete an independently registered scalar function.
  ModuleFile scalarHomonym(rewriteDeclarations([](auto& root) {
    if (root["name"].asString() != "add_i64")
      return false;
    root["name"] = "safety_companion_refresh_partial";
    return true;
  }));
  registerWasmModule(scalarHomonym.path());
  registerWasmModule(second.path(), true);
  check(
      exec::getVectorFunctionSignatures("safety_companion_refresh_partial")
          .has_value(),
      "refresh deleted a scalar homonym of an aggregate companion");

  // Native extract companions propagate whole-call UserError even under TRY.
  // A caller retrying the same expression later must not reuse failed state.
  auto value = makeFlat<int128_t>(HUGEINT(), {HugeInt::build(1, 0)}, pool);
  auto input = makeRow({value}, pool);
  core::TypedExprPtr call = std::make_shared<core::CallTypedExpr>(
      BIGINT(),
      std::vector<core::TypedExprPtr>{
          std::make_shared<core::FieldAccessTypedExpr>(HUGEINT(), "c0")},
      "checked_sum_wasm_extract");
  call = std::make_shared<core::CallTypedExpr>(
      BIGINT(), std::vector<core::TypedExprPtr>{call}, "try");
  exec::ExprSet expression({call}, &execCtx);
  std::vector<VectorPtr> result(1);
  for (auto number : {HugeInt::build(1, 0), int128_t{42}, int128_t{7}}) {
    value->set(0, number);
    exec::EvalCtx context(&execCtx, &expression, input.get());
    if (number == HugeInt::build(1, 0)) {
      bool failed = false;
      try {
        expression.eval(SelectivityVector(1), context, result);
      } catch (const VeloxUserError&) {
        failed = true;
      }
      check(
          failed,
          "extract incorrectly converted a whole-call error into row errors");
    } else {
      expression.eval(SelectivityVector(1), context, result);
      check(
          !result[0]->isNullAt(0) &&
              result[0]->as<SimpleVector<int64_t>>()->valueAt(0) == number,
          "extract retained failed/previous group state");
    }
  }
}

void testUntrustedAsciiOutput(
    memory::MemoryPool* pool,
    core::ExecCtx& execCtx) {
  auto unicode = makeFlat<StringView>(VARCHAR(), {StringView("é")}, pool);
  auto output =
      gatherToArrowIpc(SelectivityVector(1), {unicode}, pool, VARCHAR());
  auto bytes = constantResultModule(materialize(output.input));
  const std::string declaration = R"({"abi_version":1,"kind":"scalar",
    "name":"safety_nonascii_output","entrypoint":"run","row_api":true,
    "has_ascii":true,"arguments":[{"type":"varchar","nullable":true}],
    "return":{"type":"varchar","nullable":true}})";
  std::string payload;
  appendUleb(payload, std::string_view("velox.udf.v1").size());
  payload += "velox.udf.v1";
  payload += declaration;
  payload.push_back('\0');
  bytes.push_back(0);
  appendUleb(bytes, payload.size());
  bytes += payload;
  ModuleFile module(bytes);
  registerWasmModule(module.path());
  auto input = makeFlat<StringView>(VARCHAR(), {StringView("ascii")}, pool);
  auto result = evaluateCall(
      "safety_nonascii_output", VARCHAR(), makeRow({input}, pool), execCtx);
  auto strings = result->as<SimpleVector<StringView>>();
  check(
      strings->valueAt(0) == StringView("é"), "guest constant output changed");
  check(
      input->isAscii(SelectivityVector(1)) == true,
      "input ASCII was not cached");
  check(
      strings->isAscii(SelectivityVector(1)) == false,
      "guest ASCII entrypoint declaration must not imply ASCII output");
}

void testIntermediateRowAliases(memory::MemoryPool* pool) {
  auto leaf = makeRow(
      {makeFlat<int128_t>(HUGEINT(), {10, 20}, pool),
       makeFlat<StringView>(VARCHAR(), {StringView("a"), std::nullopt}, pool)},
      pool);
  leaf->setNull(1, true);
  auto offsets = allocateOffsets(2, pool);
  auto sizes = allocateSizes(2, pool);
  for (vector_size_t i = 0; i < 2; ++i) {
    offsets->asMutable<vector_size_t>()[i] = i;
    sizes->asMutable<vector_size_t>()[i] = 1;
  }
  auto list = std::make_shared<ArrayVector>(
      pool, ARRAY(leaf->type()), nullptr, 2, offsets, sizes, leaf);
  auto map = std::make_shared<MapVector>(
      pool,
      MAP(BIGINT(), leaf->type()),
      nullptr,
      2,
      offsets,
      sizes,
      makeFlat<int64_t>(BIGINT(), {1, 2}, pool),
      leaf);
  auto source = makeRow({leaf, list, map}, pool);
  auto expectedLeaf = ROW({"total", "label"}, {HUGEINT(), VARCHAR()});
  auto expected =
      ROW({"value", "items", "lookup"},
          {expectedLeaf, ARRAY(expectedLeaf), MAP(BIGINT(), expectedLeaf)});
  auto originalType = source->type();
  auto indices = allocateIndices(2, pool);
  indices->asMutable<vector_size_t>()[0] = 1;
  indices->asMutable<vector_size_t>()[1] = 0;
  std::vector<VectorPtr> encodings{
      source,
      BaseVector::wrapInDictionary(nullptr, indices, 2, source),
      BaseVector::wrapInConstant(2, 0, source)};
  for (const auto& input : encodings) {
    SelectivityVector rows(2);
    auto batch =
        gatherToArrowIpc(rows, {input}, pool, BIGINT(), std::nullopt, expected);
    auto bytes = materialize(batch.input);
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(
                      std::make_shared<arrow::io::BufferReader>(bytes))
                      .ValueOrDie();
    auto exported = reader->Next().ValueOrDie();
    const auto& fields = exported->column(0)->type();
    check(fields->field(0)->name() == "value", "intermediate outer ROW name");
    check(
        fields->field(0)->type()->field(0)->name() == "total",
        "intermediate nested ROW name");
    auto listType =
        std::static_pointer_cast<arrow::ListType>(fields->field(1)->type());
    check(
        listType->value_type()->field(1)->name() == "label",
        "intermediate ARRAY ROW name");
    auto mapType =
        std::static_pointer_cast<arrow::MapType>(fields->field(2)->type());
    check(
        mapType->item_type()->field(0)->name() == "total",
        "intermediate MAP ROW name");
    auto decoded = decodeArrowIpcResult(bytes, expected, 2, pool);
    for (vector_size_t row = 0; row < 2; ++row) {
      check(
          decoded->equalValueAt(input.get(), row, row), "renaming lost value");
    }
    auto handled = gatherToArrowIpcWithStateHandles(
        rows, {1, 2}, {input}, pool, true, expected);
    auto handledBytes = materialize(handled.input);
    auto handleReader =
        arrow::ipc::RecordBatchStreamReader::Open(
            std::make_shared<arrow::io::BufferReader>(handledBytes))
            .ValueOrDie();
    auto handleBatch = handleReader->Next().ValueOrDie();
    check(handleBatch->num_columns() == 2, "missing state handle column");
    check(
        handleBatch->column(1)->type()->Equals(fields),
        "grouped/single intermediate schemas differ");
    auto initialized = makeScalarInitializationInput(
        {{input->type(), BaseVector::wrapInConstant(1, 0, input)}},
        BIGINT(),
        {},
        pool,
        true,
        expected,
        {},
        65'536,
        false);
    auto initBytes = materialize(initialized);
    auto initReader = arrow::ipc::RecordBatchStreamReader::Open(
                          std::make_shared<arrow::io::BufferReader>(initBytes))
                          .ValueOrDie();
    check(
        initReader->Next().ValueOrDie()->column(0)->type()->Equals(fields),
        "constant initialization intermediate schema differs");
  }
  check(source->type() == originalType, "caller ROW type was mutated");
  check(
      leaf->type()->asRow().nameOf(0) == "c0", "shared child type was mutated");
  auto wrongLeaf = ROW({"total", "label"}, {BIGINT(), VARCHAR()});
  auto incompatible =
      ROW({"value", "items", "lookup"},
          {wrongLeaf, ARRAY(wrongLeaf), MAP(BIGINT(), wrongLeaf)});
  expectFatal(
      [&] {
        gatherToArrowIpc(
            SelectivityVector(2),
            {source},
            pool,
            BIGINT(),
            std::nullopt,
            incompatible);
      },
      "intermediate type mismatch");
  auto decimal = makeRow({makeFlat<int64_t>(DECIMAL(6, 2), {123}, pool)}, pool);
  expectFatal(
      [&] {
        gatherToArrowIpc(
            SelectivityVector(1),
            {decimal},
            pool,
            BIGINT(),
            std::nullopt,
            ROW({"value"}, {DECIMAL(6, 3)}));
      },
      "intermediate type mismatch");
}

class SelectedTagLoader final : public VectorLoader {
 public:
  SelectedTagLoader(memory::MemoryPool* pool, std::shared_ptr<size_t> loaded)
      : pool_(pool), loaded_(std::move(loaded)) {}

 protected:
  void loadInternal(
      RowSet rows,
      ValueHook* hook,
      vector_size_t resultSize,
      VectorPtr* result) override {
    check(hook == nullptr, "unexpected gather value hook");
    auto values =
        BaseVector::create<FlatVector<int64_t>>(BIGINT(), resultSize, pool_);
    for (auto row : rows) {
      check(
          row == 5 || row == 7,
          "gather eagerly loaded an unused lazy base row");
      values->set(row, row * 10);
      ++*loaded_;
    }
    *result = std::move(values);
  }

 private:
  memory::MemoryPool* pool_;
  std::shared_ptr<size_t> loaded_;
};

void testGatherEncodedLeaves(memory::MemoryPool* pool) {
  auto payloadValues =
      makeFlat<int64_t>(BIGINT(), {10, 11, 20, 21, 30, 31, 40, 41}, pool);
  auto offsets = allocateOffsets(4, pool);
  auto sizes = allocateSizes(4, pool);
  for (vector_size_t i = 0; i < 4; ++i) {
    offsets->asMutable<vector_size_t>()[i] = i * 2;
    sizes->asMutable<vector_size_t>()[i] = 2;
  }
  auto payload = std::make_shared<ArrayVector>(
      pool, ARRAY(BIGINT()), nullptr, 4, offsets, sizes, payloadValues);
  auto tags = makeFlat<int64_t>(BIGINT(), {7, std::nullopt, 9, 10}, pool);
  auto tagIndices = allocateIndices(4, pool);
  for (vector_size_t i = 0; i < 4; ++i)
    tagIndices->asMutable<vector_size_t>()[i] = 3 - i;
  auto dictionaryTags =
      BaseVector::wrapInDictionary(nullptr, tagIndices, 4, tags);
  auto itemIndices = allocateIndices(8, pool);
  for (vector_size_t i = 0; i < 8; ++i)
    itemIndices->asMutable<vector_size_t>()[i] = i % 4;
  auto encodedItems =
      BaseVector::wrapInDictionary(nullptr, itemIndices, 8, dictionaryTags);
  auto encodedArray = std::make_shared<ArrayVector>(
      pool, ARRAY(BIGINT()), nullptr, 4, offsets, sizes, encodedItems);
  auto map = std::make_shared<MapVector>(
      pool,
      MAP(BIGINT(), BIGINT()),
      nullptr,
      4,
      offsets,
      sizes,
      BaseVector::wrapInConstant(8, 0, tags),
      encodedItems);
  for (auto tag : std::vector<VectorPtr>{
           dictionaryTags,
           BaseVector::wrapInConstant(4, 0, tags),
           BaseVector::wrapInConstant(4, 1, tags)}) {
    auto source = makeRow({payload, tag, encodedArray, map}, pool);
    source->setNull(2, true);
    const auto originalChildren = source->children();
    auto values = payloadValues->values();
    const auto references = values->refCount();
    {
      auto gathered = gatherToArrowIpc(
          SelectivityVector(4), {source}, pool, source->type());
      check(
          values->refCount() > references,
          "encoded scalar leaf copied its plain sibling payload");
      auto result = decodeOwnedArrowIpcResult(
          materialize(gathered.input), source->type(), 4, pool);
      for (vector_size_t row = 0; row < 4; ++row)
        check(
            result->equalValueAt(source.get(), row, row), "mixed gather value");
    }
    check(values->refCount() == references, "gather retained released payload");
    check(
        source->children() == originalChildren,
        "gather mutated input children");
    auto rootIndices = allocateIndices(4, pool);
    for (vector_size_t row = 0; row < 4; ++row)
      rootIndices->asMutable<vector_size_t>()[row] = 3 - row;
    for (auto input : std::vector<VectorPtr>{
             source,
             BaseVector::wrapInDictionary(nullptr, rootIndices, 4, source),
             BaseVector::wrapInConstant(4, 1, source)}) {
      for (bool sparse : {false, true}) {
        SelectivityVector selected(4, !sparse);
        if (sparse) {
          selected.setValid(1, true);
          selected.setValid(3, true);
          selected.updateBounds();
        }
        auto gathered =
            gatherToArrowIpc(selected, {input}, pool, source->type());
        auto result = decodeOwnedArrowIpcResult(
            materialize(gathered.input), source->type(), sparse ? 2 : 4, pool);
        vector_size_t compact = 0;
        selected.applyToSelected([&](vector_size_t row) {
          check(
              result->equalValueAt(input.get(), compact++, row),
              "encoded/sparse complex gather value");
        });
      }
    }
  }
  auto encodedComplex = BaseVector::wrapInConstant(4, 1, payload);
  auto unknown = BaseVector::createNullConstant(UNKNOWN(), 4, pool);
  auto fallback = makeRow({encodedComplex, unknown}, pool);
  for (bool empty : {false, true}) {
    SelectivityVector selected(4, !empty);
    auto gathered =
        gatherToArrowIpc(selected, {fallback}, pool, fallback->type());
    auto result = decodeOwnedArrowIpcResult(
        materialize(gathered.input), fallback->type(), empty ? 0 : 4, pool);
    for (vector_size_t row = 0; row < result->size(); ++row)
      check(
          result->equalValueAt(fallback.get(), row, row),
          "UNKNOWN gather fallback");
  }
  // Borrowed payloads must remain alive after the caller releases its vectors.
  auto makeDetached = [&] {
    auto sibling = makeFlat<int64_t>(BIGINT(), {101, 202}, pool);
    auto tag = BaseVector::wrapInConstant(2, 0, tags);
    auto source = makeRow({sibling, tag}, pool);
    return gatherToArrowIpc(
        SelectivityVector(2), {source}, pool, source->type());
  };
  auto detached = makeDetached();
  auto result = decodeOwnedArrowIpcResult(
      materialize(detached.input), ROW({BIGINT(), BIGINT()}), 2, pool);
  check(
      result->as<RowVector>()->childAt(0)->as<SimpleVector<int64_t>>()->valueAt(
          1) == 202,
      "gather payload did not retain its caller storage");
  auto loaded = std::make_shared<size_t>(0);
  auto lazy = std::make_shared<LazyVector>(
      pool, BIGINT(), 100, std::make_unique<SelectedTagLoader>(pool, loaded));
  auto lazyIndices = allocateIndices(2, pool);
  lazyIndices->asMutable<vector_size_t>()[0] = 5;
  lazyIndices->asMutable<vector_size_t>()[1] = 7;
  auto lazyTags = BaseVector::wrapInDictionary(nullptr, lazyIndices, 2, lazy);
  auto lazySource =
      makeRow({makeFlat<int64_t>(BIGINT(), {1, 2}, pool), lazyTags}, pool);
  auto lazyGather = gatherToArrowIpc(
      SelectivityVector(2), {lazySource}, pool, lazySource->type());
  auto lazyResult = decodeOwnedArrowIpcResult(
      materialize(lazyGather.input), lazySource->type(), 2, pool);
  check(*loaded == 2, "gather changed selective loading of a lazy base");
  for (vector_size_t row = 0; row < 2; ++row)
    check(
        lazyResult->equalValueAt(lazySource.get(), row, row),
        "lazy gather value");
}

void testHostileIpc(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto source = makeFlat<StringView>(VARBINARY(), {StringView("abc")}, pool);
  auto input = gatherToArrowIpc(SelectivityVector(1), {source}, pool);
  auto ipc = materialize(input.input);
  const std::string offsets("\0\0\0\0\3\0\0\0", 8);
  const auto position = ipc.rfind(offsets);
  check(position != std::string::npos, "missing binary offsets");
  ipc[position + 4] = 64;
  expectFatal(
      [&] { decodeOwnedArrowIpcResult(ipc, VARBINARY(), 1, pool); },
      "validation");
  expectFatal(
      [&] {
        decodeOwnedArrowIpcResult(materialize(input.input), BIGINT(), 1, pool);
      },
      "schema");

  auto decimal = makeFlat<int64_t>(DECIMAL(2, 0), {999}, pool);
  auto decimalIpc = gatherToArrowIpc(SelectivityVector(1), {decimal}, pool);
  expectFatal(
      [&] {
        decodeOwnedArrowIpcResult(
            materialize(decimalIpc.input), decimal->type(), 1, pool);
      },
      "precision");

  // Serialize the MAP physical layout using a LIST array and a MAP schema.
  // This creates hostile IPC without calling Arrow's aborting MAP constructor.
  auto malformedMap = [&](bool nullKey, bool nullEntry) {
    arrow::Int64Builder keys;
    check(keys.AppendValues({1, 2}).ok(), "test key append");
    auto keyArray = keys.Finish().ValueOrDie();
    if (nullKey) {
      arrow::Int64Builder nullKeys;
      check(nullKeys.AppendNull().ok(), "test NULL key append");
      check(nullKeys.Append(2).ok(), "test second key append");
      keyArray = nullKeys.Finish().ValueOrDie();
    }
    arrow::Int64Builder values;
    check(values.AppendValues({10, 20}).ok(), "test value append");
    auto entries = arrow::StructArray::Make(
                       {keyArray, values.Finish().ValueOrDie()},
                       {arrow::field("key", arrow::int64(), false),
                        arrow::field("value", arrow::int64())})
                       .ValueOrDie();
    if (nullEntry) {
      auto data = entries->data()->Copy();
      data->buffers[0] = arrow::Buffer::FromString(std::string(1, '\x02'));
      data->null_count = 1;
      entries =
          std::static_pointer_cast<arrow::StructArray>(arrow::MakeArray(data));
    }
    arrow::Int32Builder offsets;
    check(offsets.AppendValues({0, 2}).ok(), "test map offsets append");
    auto list =
        arrow::ListArray::FromArrays(*offsets.Finish().ValueOrDie(), *entries)
            .ValueOrDie();
    ArrowIpcInput hostile(
        arrow::RecordBatch::Make(
            arrow::schema({arrow::field(
                "result", arrow::map(arrow::int64(), arrow::int64()))}),
            1,
            {list}));
    return materialize(hostile);
  };
  for (bool nullEntry : {false, true}) {
    auto bytes = malformedMap(!nullEntry, nullEntry);
    expectFatal(
        [&] {
          decodeOwnedArrowIpcResult(bytes, MAP(BIGINT(), BIGINT()), 1, pool);
        },
        nullEntry ? "MAP entry contains NULL" : "MAP key contains NULL");
    WasmVectorFunction hostile(
        WasmModule::compile(
            nullEntry ? "hostile-map-entry" : "hostile-map-key",
            constantResultModule(bytes)),
        "run",
        {},
        true);
    std::vector<VectorPtr> arguments{makeFlat<int64_t>(BIGINT(), {1}, pool)};
    auto input = makeRow(arguments, pool);
    exec::ExprSet expression({}, &execCtx);
    exec::EvalCtx context(&execCtx, &expression, input.get());
    VectorPtr result;
    expectFatal(
        [&] {
          hostile.apply(
              SelectivityVector(1),
              arguments,
              MAP(BIGINT(), BIGINT()),
              context,
              result);
        },
        nullEntry ? "MAP entry contains NULL" : "MAP key contains NULL");
    expectFatal(
        [&] {
          hostile.apply(
              SelectivityVector(1),
              arguments,
              MAP(BIGINT(), BIGINT()),
              context,
              result);
        },
        "invalidated");
    expectFatal(
        [&] { decodeOwnedArrowIpcResult(bytes, BIGINT(), 1, pool); }, "schema");
  }
  auto mapBytes = malformedMap(false, false);
  auto valid =
      decodeOwnedArrowIpcResult(mapBytes, MAP(BIGINT(), BIGINT()), 1, pool);
  check(valid->as<MapVector>()->sizeAt(0) == 2, "valid MAP IPC failed");
  expectFatal(
      [&] {
        decodeOwnedArrowIpcResult(
            mapBytes + "junk", MAP(BIGINT(), BIGINT()), 1, pool);
      },
      "trailing bytes");

  auto largeString = makeFlat<StringView>(
      VARBINARY(),
      {StringView("a string longer than an inline StringView")},
      pool);
  auto ownedInput = gatherToArrowIpc(SelectivityVector(1), {largeString}, pool);
  const auto before = pool->usedBytes();
  auto value = decodeOwnedArrowIpcResult(
      materialize(ownedInput.input), VARBINARY(), 1, pool);
  check(pool->usedBytes() > before, "owned IPC storage was not charged");
  value.reset();
  check(pool->usedBytes() == before, "IPC charge survived its owner");

  arrow::BinaryBuilder builder;
  check(
      builder.Append(std::string(80UL << 20, 'x')).ok(),
      "cannot build compressed test data");
  auto array = builder.Finish().ValueOrDie();
  auto batch = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("value", arrow::binary())}), 1, {array});
  auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
  auto options = arrow::ipc::IpcWriteOptions::Defaults();
  options.codec =
      arrow::util::Codec::Create(arrow::Compression::ZSTD).ValueOrDie();
  auto writer =
      arrow::ipc::MakeStreamWriter(sink, batch->schema(), options).ValueOrDie();
  check(
      writer->WriteRecordBatch(*batch).ok() && writer->Close().ok(),
      "cannot write compressed test data");
  auto compressed = sink->Finish().ValueOrDie();
  expectFatal(
      [&] {
        decodeArrowIpcResult(compressed->ToString(), VARBINARY(), 1, pool);
      },
      "allocation exceeds");
  check(
      pool->usedBytes() == before,
      "failed decompression leaked memory charges");
}

void testRuntimeLimits(memory::MemoryPool* pool) {
  WasmOptions options;
  options.memoryLimitBytes = 64 << 10;
  options.fuelPerCall = 1'000;
  auto input = gatherToArrowIpc(
      SelectivityVector(1), {makeFlat<int64_t>(BIGINT(), {1}, pool)}, pool);
  const std::string loop = "(loop $forever (br $forever))";
  auto hugeTable = WasmModule::compile(
      "huge-table",
      tinyModule("(v128.const i32x4 0 4096 1 0)", "(table 200000 funcref)"));
  expectFatal(
      [&] { WasmInstance instance(hugeTable, "run", options); }, "table");
  auto manyTables = WasmModule::compile(
      "many-tables",
      tinyModule(
          "(v128.const i32x4 0 4096 1 0)",
          "(table 1 funcref) (table 1 funcref)"));
  expectFatal(
      [&] { WasmInstance instance(manyTables, "run", options); }, "table");
  auto finite = WasmModule::compile("finite", tinyModule());
  auto smallInput = options;
  smallInput.maxInputBytes = 1;
  WasmInstance inputLimit(finite, "run", smallInput);
  expectFatal([&] { inputLimit.invoke(input.input); }, "input exceeds");
  expectFatal([&] { inputLimit.invoke(input.input); }, "invalidated");
  auto smallOutput = options;
  smallOutput.maxOutputBytes = 0;
  WasmInstance outputLimit(finite, "run", smallOutput);
  expectFatal([&] { outputLimit.invoke(input.input); }, "output exceeds");
  auto endless = WasmModule::compile(
      "loop", tinyModule(loop + " (v128.const i32x4 0 4096 1 0)"));
  WasmInstance fuel(endless, "run", options);
  expectFatal([&] { fuel.invoke(input.input); }, "fuel");
  expectFatal([&] { fuel.invoke(input.input); }, "invalidated");

  options.fuelPerCall = UINT64_MAX;
  options.maxCallMillis = 30;
  WasmInstance deadline(endless, "run", options);
  auto start = std::chrono::steady_clock::now();
  expectFatal([&] { deadline.invoke(input.input); }, "deadline");
  check(
      std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
      "deadline interrupt exceeded one second");

  options.maxCallMillis = 1'000;
  WasmInstance cancellable(endless, "run", options);
  std::atomic<bool> cancelled{false};
  cancellable.setCancellationCheck([&] { return cancelled.load(); });
  std::jthread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancelled = true;
  });
  start = std::chrono::steady_clock::now();
  expectFatal([&] { cancellable.invoke(input.input); }, "cancelled");
  check(
      std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
      "cancellation interrupt exceeded one second");

  const auto before = pool->usedBytes();
  {
    auto capacity = options;
    capacity.memoryLimitBytes = 4 * 65536;
    WasmInstance charged(
        WasmModule::compile(
            "charged",
            tinyModule(
                "i32.const 1 memory.grow drop (v128.const i32x4 0 4096 1 0)")),
        "run",
        capacity,
        pool);
    check(
        pool->usedBytes() == before + 65536,
        "Store accessible pages were not charged at creation");
    check(charged.invoke(input.input) == "x", "tiny module changed output");
    check(
        pool->usedBytes() == before + 2 * 65536,
        "Store growth was not charged");
    charged.invalidate();
    check(pool->usedBytes() == before, "invalidated Store retained its charge");
  }

  {
    auto root = memory::memoryManager()->addRootPool(
        "safety-wasm-native-growth", 1UL << 20);
    auto limited = root->addLeafChild("safety-wasm-native-growth-leaf");
    auto capacity = options;
    capacity.memoryLimitBytes = 2UL << 20;
    auto compiled = WasmModule::compile(
        "native-growth-cap",
        tinyModule(
            "i32.const 16 memory.grow drop (v128.const i32x4 0 4096 1 0)"));
    WasmInstance limitedStore(compiled, "run", capacity, limited.get());
    expectFatal(
        [&] { limitedStore.invoke(input.input); }, "memory pool capacity");
    check(
        limited->usedBytes() == 0,
        "guest ignored growth failure and retained native charge");
    expectFatal([&] { limitedStore.invoke(input.input); }, "invalidated");
  }
  options.tableElements = 8;
  auto growing = WasmModule::compile(
      "table-grow",
      tinyModule(
          "(if (i32.ne (table.grow (ref.null func) (i32.const 8)) (i32.const -1)) (then unreachable)) (v128.const i32x4 0 4096 1 0)",
          "(table 1 funcref)"));
  WasmInstance growth(growing, "run", options);
  check(growth.invoke(input.input) == "x", "table.grow escaped element budget");
}

template <typename T>
struct NativeIdentity {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(int64_t& out, const int64_t& in) {
    out = in;
    return true;
  }
};

void testRegistration() {
  ModuleFile partial(rewriteDeclarations([](auto& declaration) {
    if (declaration["name"].asString() == "add_i64")
      declaration["name"] = "aaa_safety_new";
    return true;
  }));
  const auto windows = exec::windowFunctions().size();
  expectCollision([&] { registerWasmModule(partial.path()); });
  check(
      !exec::getVectorFunctionSignatures("aaa_safety_new"),
      "failed module left a scalar registered");
  check(
      exec::windowFunctions().size() == windows,
      "failed module changed window registrations");

  facebook::velox::registerFunction<NativeIdentity, int64_t, int64_t>(
      {"native_safety_identity"});
  ModuleFile shadow(rewriteDeclarations([](auto& declaration) {
    if (declaration["name"].asString() != "add_i64")
      return false;
    declaration["name"] = "NATIVE_SAFETY_IDENTITY";
    return true;
  }));
  expectCollision([&] { registerWasmModule(shadow.path(), true); });
  check(
      !exec::getVectorFunctionSignatures("native_safety_identity"),
      "Wasm shadowed native SFI");

  ModuleFile special(rewriteDeclarations([](auto& declaration) {
    if (declaration["name"].asString() != "add_i64")
      return false;
    declaration["name"] = "TRY";
    return true;
  }));
  expectCollision([&] { registerWasmModule(special.path(), true); });

  ModuleFile aliases(rewriteDeclarations([](auto& declaration) {
    if (declaration["name"].asString() == "add_i64")
      return true;
    if (declaration["name"].asString() == "overloaded_add" &&
        declaration["arguments"][0]["type"].asString() == "bigint") {
      declaration["name"] = "ADD_I64";
      return true;
    }
    return false;
  }));
  expectCollision([&] { registerWasmModule(aliases.path()); });

  const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  for (const auto& scalar : declarations.scalars) {
    if (scalar.name != "overloaded_add")
      continue;
    ModuleFile overload(rewriteDeclarations([&](auto& declaration) {
      if (declaration["kind"].asString() != "scalar" ||
          declaration["entrypoint"].asString() != scalar.entrypoint)
        return false;
      declaration["name"] = "safety_overload";
      return true;
    }));
    check(
        registerWasmModule(overload.path()) == 1,
        "cannot add disjoint overload");
    expectCollision([&] { registerWasmModule(overload.path()); });
    check(
        registerWasmModule(overload.path(), true) == 1,
        "cannot replace one overload");
  }
  const auto overloads = exec::getVectorFunctionSignatures("safety_overload");
  check(
      overloads && overloads->size() == 2,
      "overload replacement lost another module's signature");
  check(
      exec::resolveVectorFunction("safety_overload", {BIGINT(), BIGINT()})
          ->isBigint(),
      "integer overload lost");
  check(
      exec::resolveVectorFunction("safety_overload", {DOUBLE(), DOUBLE()})
          ->isDouble(),
      "double overload lost");

  ModuleFile generic(rewriteDeclarations([](auto& declaration) {
    if (declaration["name"].asString() != "generic_identity")
      return false;
    declaration["name"] = "safety_generic";
    return true;
  }));
  ModuleFile renamedGeneric(rewriteDeclarations([](auto& declaration) {
    if (declaration["name"].asString() != "generic_identity")
      return false;
    declaration["name"] = "SAFETY_GENERIC";
    declaration["arguments"][0]["type"] = "U";
    declaration["return"]["type"] = "U";
    declaration["type_variables"] = folly::dynamic::object("U", "any");
    return true;
  }));
  check(registerWasmModule(generic.path()) == 1, "cannot register generic");
  expectCollision([&] { registerWasmModule(renamedGeneric.path()); });
  check(
      exec::getVectorFunctionSignatures("safety_generic")->size() == 1,
      "alpha-equivalent generic was registered twice");

  ModuleFile concurrent(rewriteDeclarations([](auto& declaration) {
    if (declaration["name"].asString() != "add_i64")
      return false;
    declaration["name"] = "safety_concurrent";
    return true;
  }));
  std::atomic<int> success{0}, collisions{0};
  auto load = [&]() {
    try {
      registerWasmModule(concurrent.path());
      ++success;
    } catch (const VeloxUserError&) {
      ++collisions;
    }
  };
  std::thread firstLoad(load), secondLoad(load);
  firstLoad.join();
  secondLoad.join();
  check(
      success == 1 && collisions == 1,
      "concurrent registration was not serialized");

  for (const auto& original : declarations.aggregates) {
    if (original.name != "sum_i64" && original.name != "avg_f64")
      continue;
    ModuleFile overload(rewriteDeclarations([&](auto& declaration) {
      if (declaration["name"].asString() != original.name)
        return false;
      declaration["name"] = "safety_aggregate_overload";
      return true;
    }));
    check(
        registerWasmModule(overload.path()) == 1,
        "cannot add aggregate overload");
    expectCollision([&] { registerWasmModule(overload.path()); });
    check(
        registerWasmModule(overload.path(), true) == 1,
        "cannot replace aggregate overload");
  }
  const auto* entry =
      exec::getAggregateFunctionEntry("safety_aggregate_overload");
  check(
      entry && entry->signatures.size() == 2,
      "aggregate replacement lost another overload");
  check(
      exec::Aggregate::create(
          "safety_aggregate_overload",
          core::AggregationNode::Step::kSingle,
          {BIGINT()},
          BIGINT(),
          core::QueryConfig({}))
          ->resultType()
          ->isBigint(),
      "aggregate BIGINT overload lost");
  check(
      exec::Aggregate::create(
          "safety_aggregate_overload",
          core::AggregationNode::Step::kSingle,
          {DOUBLE()},
          DOUBLE(),
          core::QueryConfig({}))
          ->resultType()
          ->isDouble(),
      "aggregate DOUBLE overload lost");

  auto bytes = tinyModule();
  ModuleFile replace(bytes);
  auto first = WasmModule::compile(replace.path());
  const auto time = std::filesystem::last_write_time(replace.path());
  const auto marker = bytes.rfind('x');
  check(marker != std::string::npos, "missing module data marker");
  bytes[marker] = 'y';
  replace.write(bytes);
  std::filesystem::last_write_time(replace.path(), time);
  auto second = WasmModule::compile(replace.path());
  check(first != second, "same size/mtime reused stale module contents");
}

void testRowErrors(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto module = WasmModule::compile(WASM_MODULE_PATH);
  exec::ExprSet emptyExpression({}, &execCtx);
  for (const auto& entrypoint : {"checked_divide", "status_result"}) {
    const bool typed = std::string_view(entrypoint) == "status_result";
    std::vector<VectorPtr> arguments{
        makeFlat<int64_t>(BIGINT(), {typed ? 1 : 84}, pool)};
    if (!typed) {
      arguments.push_back(makeFlat<int64_t>(BIGINT(), {0}, pool));
    }
    auto input = makeRow(arguments, pool);
    exec::EvalCtx context(&execCtx, &emptyExpression, input.get());
    WasmVectorFunction function(module, entrypoint, {}, true);
    VectorPtr result;
    bool userError = false;
    try {
      function.apply(
          SelectivityVector(1), arguments, BIGINT(), context, result);
    } catch (const VeloxUserError&) {
      userError = true;
    }
    check(userError, "valid business error lost its native UserError category");
    arguments.back()->as<FlatVector<int64_t>>()->set(0, typed ? 0 : 2);
    function.apply(SelectivityVector(1), arguments, BIGINT(), context, result);
    check(
        !result->isNullAt(0) &&
            result->as<SimpleVector<int64_t>>()->valueAt(0) == (typed ? 0 : 42),
        "valid business error invalidated its Store");
  }

  // Validate the whole result before reporting the first business error.
  arrow::Int64Builder values;
  check(values.AppendNulls(2).ok(), "cannot build NULL status results");
  arrow::StringBuilder errors;
  check(errors.Append("first user error").ok(), "cannot build first error");
  check(
      errors
          .Append(
              "\x1e"
              "velox.status.v1:99\nbad status")
          .ok(),
      "cannot build malformed status");
  ArrowIpcInput output(
      arrow::RecordBatch::Make(
          arrow::schema(
              {arrow::field("value", arrow::int64()),
               arrow::field("__velox_wasm_error", arrow::utf8())}),
          2,
          {values.Finish().ValueOrDie(), errors.Finish().ValueOrDie()}));
  WasmVectorFunction malformed(
      WasmModule::compile(
          "malformed-later-status", constantResultModule(materialize(output))),
      "run",
      {},
      true);
  std::vector<VectorPtr> arguments{makeFlat<int64_t>(BIGINT(), {1, 2}, pool)};
  auto input = makeRow(arguments, pool);
  exec::EvalCtx context(&execCtx, &emptyExpression, input.get());
  VectorPtr statusResult;
  expectFatal(
      [&] {
        malformed.apply(
            SelectivityVector(2), arguments, BIGINT(), context, statusResult);
      },
      "Invalid scalar status code");
  expectFatal(
      [&] {
        malformed.apply(
            SelectivityVector(2), arguments, BIGINT(), context, statusResult);
      },
      "invalidated");

  auto left = makeFlat<int64_t>(BIGINT(), {INT64_MIN, 10}, pool);
  auto right = makeFlat<int64_t>(BIGINT(), {-1, 2}, pool);
  auto result = evaluateCall(
      "checked_divide", BIGINT(), makeRow({left, right}, pool), execCtx, true);
  check(
      result->isNullAt(0) &&
          result->as<SimpleVector<int64_t>>()->valueAt(1) == 5,
      "overflow poisoned a successful division row");
  left = makeFlat<int64_t>(BIGINT(), {INT64_MAX, 3}, pool);
  right = makeFlat<int64_t>(BIGINT(), {1, 2}, pool);
  result = evaluateCall(
      "add_i64", BIGINT(), makeRow({left, right}, pool), execCtx, true);
  check(
      result->isNullAt(0) &&
          result->as<SimpleVector<int64_t>>()->valueAt(1) == 5,
      "addition overflow contract changed");
  registerOpaqueType<int64_t>("wasm_safety_handle");
  auto opaque = makeFlat<std::shared_ptr<void>>(
      OPAQUE<int64_t>(), {std::make_shared<int64_t>(1)}, pool);
  expectFatal(
      [&] {
        evaluateCall(
            "opaque_forge",
            opaque->type(),
            makeRow({opaque}, pool),
            execCtx,
            true);
      },
      "Invalid or expired");
}

void testDeferredCodecs(memory::MemoryPool* pool, core::ExecCtx& execCtx) {
  auto type = facebook::velox::test::BIGINT_TYPE_WITH_CUSTOM_COMPARISON();
  auto count = std::make_shared<size_t>(0);
  registerWasmTypeCodec(
      {type,
       "safety_lazy_i64",
       1,
       [](const BaseVector& source, vector_size_t row) {
         const auto value = source.as<SimpleVector<int64_t>>()->valueAt(row);
         return std::string(
             reinterpret_cast<const char*>(&value), sizeof(value));
       },
       [count](std::string_view bytes, BaseVector& target, vector_size_t row) {
         ++*count;
         VELOX_USER_CHECK_EQ(
             bytes.size(), sizeof(int64_t), "invalid deferred codec bytes");
         int64_t value;
         std::memcpy(&value, bytes.data(), sizeof(value));
         VELOX_USER_CHECK_NE(value, 999, "injected deferred codec failure");
         target.as<FlatVector<int64_t>>()->set(row, value);
       }});
  const ArrowIpcDecodeOptions lazy{.lazyCodecs = true};
  auto values = makeFlat<int64_t>(type, {1, std::nullopt, 3, 4}, pool);
  auto ipc = materialize(
      gatherToArrowIpc(SelectivityVector(4), {values}, pool, type).input);
  auto decoded =
      decodeOwnedArrowIpcResult(ipc, type, 4, pool, nullptr, nullptr, lazy);
  check(
      decoded->isLazy() && *count == 0,
      "codec was decoded before first access");
  const vector_size_t first = 0;
  decoded->as<LazyVector>()->load(RowSet(&first, 1), nullptr);
  auto loaded = decoded->loadedVector()->as<SimpleVector<int64_t>>();
  check(
      *count == 3 && loaded->valueAt(0) == 1 && loaded->isNullAt(1) &&
          loaded->valueAt(3) == 4,
      "single-use codec load lost values for later row consumers");
  check(
      decoded->loadedVector()->hashValueAt(2) == values->hashValueAt(2) &&
          *count == 3,
      "later access repeated decoding or changed custom semantics");
  *count = 0;
  auto eager = decodeArrowIpcResult(ipc, type, 4, pool);
  check(
      !eager->isLazy() && *count == 3,
      "borrowed IPC changed its eager lifetime contract");

  auto offsets = AlignedBuffer::allocate<vector_size_t>(2, pool);
  auto sizes = AlignedBuffer::allocate<vector_size_t>(2, pool);
  offsets->asMutable<vector_size_t>()[0] = 0;
  offsets->asMutable<vector_size_t>()[1] = 2;
  sizes->asMutable<vector_size_t>()[0] = sizes->asMutable<vector_size_t>()[1] =
      2;
  auto array = std::make_shared<ArrayVector>(
      pool, ARRAY(type), nullptr, 2, offsets, sizes, values);
  *count = 0;
  auto arrayResult = decodeOwnedArrowIpcResult(
      materialize(
          gatherToArrowIpc(SelectivityVector(2), {array}, pool, array->type())
              .input),
      array->type(),
      2,
      pool,
      nullptr,
      nullptr,
      lazy);
  check(
      *count == 0 && arrayResult->as<ArrayVector>()->elements()->isLazy(),
      "ARRAY codec elements were not deferred");
  check(
      arrayResult->as<ArrayVector>()
                  ->elements()
                  ->loadedVector()
                  ->as<SimpleVector<int64_t>>()
                  ->valueAt(3) == 4 &&
          *count == 3,
      "ARRAY deferred codec values changed");
  auto keys = makeFlat<int64_t>(type, {7, 8, 9, 10}, pool);
  auto map = std::make_shared<MapVector>(
      pool, MAP(type, type), nullptr, 2, offsets, sizes, keys, values);
  *count = 0;
  auto mapResult = decodeOwnedArrowIpcResult(
      materialize(
          gatherToArrowIpc(SelectivityVector(2), {map}, pool, map->type())
              .input),
      map->type(),
      2,
      pool,
      nullptr,
      nullptr,
      lazy);
  check(
      *count == 4 && !mapResult->as<MapVector>()->mapKeys()->isLazy() &&
          mapResult->as<MapVector>()->mapValues()->isLazy(),
      "MAP keys must be eager while values may be deferred");
  check(
      mapResult->as<MapVector>()
                  ->mapValues()
                  ->loadedVector()
                  ->as<SimpleVector<int64_t>>()
                  ->valueAt(3) == 4 &&
          *count == 7,
      "MAP deferred codec values changed");

  // Owned IPC survives the input, temporary string, function and caller pool
  // reference until the deferred load. The vector follows native pool lifetime
  // rules after materialization.
  *count = 0;
  VectorPtr retained;
  {
    auto temporaryPool =
        memory::memoryManager()->addLeafPool("deferred-codec-owner");
    auto source =
        makeFlat<int64_t>(type, {7, std::nullopt, 9}, temporaryPool.get());
    auto output = gatherToArrowIpc(
        SelectivityVector(3), {source}, temporaryPool.get(), type);
    retained = decodeOwnedArrowIpcResult(
        materialize(output.input),
        type,
        3,
        temporaryPool.get(),
        nullptr,
        nullptr,
        lazy);
  }
  check(*count == 0, "retained codec result was eagerly decoded");
  check(
      retained->loadedVector()->as<SimpleVector<int64_t>>()->valueAt(2) == 9 &&
          *count == 2,
      "deferred codec lost its IPC or memory pool owner");
  retained.reset();

  // Unused columns do not hide wire corruption. NULL payload under a non-NULL
  // codec value is rejected before any lazy vector is published.
  auto one = makeFlat<int64_t>(type, {1}, pool);
  auto valid = materialize(
      gatherToArrowIpc(SelectivityVector(1), {one}, pool, type).input);
  auto reader = arrow::ipc::RecordBatchStreamReader::Open(
                    std::make_shared<arrow::io::BufferReader>(valid))
                    .ValueOrDie();
  auto batch = reader->Next().ValueOrDie();
  arrow::BinaryBuilder nullBuilder;
  check(nullBuilder.AppendNull().ok(), "cannot construct NULL codec payload");
  auto invalid =
      arrow::StructArray::Make(
          {nullBuilder.Finish().ValueOrDie()},
          std::static_pointer_cast<arrow::StructType>(batch->column(0)->type())
              ->fields())
          .ValueOrDie();
  ArrowIpcInput invalidInput(
      arrow::RecordBatch::Make(batch->schema(), 1, {invalid}));
  *count = 0;
  expectFatal(
      [&] {
        decodeOwnedArrowIpcResult(
            materialize(invalidInput), type, 1, pool, nullptr, nullptr, lazy);
      },
      "payload must be non-null");
  check(*count == 0, "wire corruption invoked the deferred decoder");

  // Runtime failures are not row errors and never return a partial vector.
  auto poison = makeFlat<int64_t>(type, {1, 999}, pool);
  auto poisonIpc = materialize(
      gatherToArrowIpc(SelectivityVector(2), {poison}, pool, type).input);
  size_t invalidations = 0;
  auto failed = decodeOwnedArrowIpcResult(
      poisonIpc,
      type,
      2,
      pool,
      nullptr,
      nullptr,
      {.lazyCodecs = true, .onDeferredCodecFailure = [&] { ++invalidations; }});
  expectFatal(
      [&] { failed->loadedVector(); }, "injected deferred codec failure");
  const auto failedCount = *count;
  expectFatal(
      [&] { failed->loadedVector(); }, "injected deferred codec failure");
  check(
      invalidations == 1 && *count == failedCount,
      "failed codec was retried or exposed partial values");

  // A deferred result outlives its producer without retaining a Wasm Store.
  // A failure while the same producer is alive invalidates its next invocation.
  auto module = WasmModule::compile(WASM_MODULE_PATH);
  const auto rowType = ROW({"plain", "encoded"}, {BIGINT(), type});
  auto plain = makeFlat<int64_t>(BIGINT(), {10, 20}, pool);
  auto source = std::make_shared<RowVector>(
      pool, rowType, nullptr, 2, std::vector<VectorPtr>{plain, poison});
  std::vector<VectorPtr> arguments{source};
  auto input = makeRow(arguments, pool);
  exec::ExprSet empty({}, &execCtx);
  exec::EvalCtx context(&execCtx, &empty, input.get());
  WasmVectorFunction function(module, "generic_identity", {}, true);
  VectorPtr result;
  *count = 0;
  function.apply(SelectivityVector(2), arguments, rowType, context, result);
  check(
      *count == 0 && result->as<RowVector>()->childAt(1)->isLazy(),
      "nested codec was eagerly decoded");
  expectFatal(
      [&] { result->as<RowVector>()->childAt(1)->loadedVector(); },
      "injected deferred codec failure");
  expectFatal(
      [&] {
        function.apply(
            SelectivityVector(2), arguments, rowType, context, result);
      },
      "invalidated");
  auto good = makeFlat<int64_t>(type, {7, 8}, pool);
  source->childAt(1) = good;
  {
    WasmVectorFunction temporary(module, "generic_identity", {}, true);
    temporary.apply(SelectivityVector(2), arguments, rowType, context, result);
  }
  check(
      result->as<RowVector>()
              ->childAt(1)
              ->loadedVector()
              ->as<SimpleVector<int64_t>>()
              ->valueAt(1) == 8,
      "deferred result retained a dangling producer pointer");

  auto handle = makeFlat<std::shared_ptr<void>>(
      OPAQUE<int64_t>(), {std::make_shared<int64_t>(42)}, pool);
  auto handleRow = makeRow({handle}, pool);
  auto handleOutput = gatherToArrowIpc(
      SelectivityVector(1), {handleRow}, pool, handleRow->type());
  WasmVectorFunction expiredHandle(
      WasmModule::compile(
          "deferred-expired-handle",
          constantResultModule(materialize(handleOutput.input))),
      "run",
      {},
      true);
  std::vector<VectorPtr> handleArguments{handleRow};
  expectFatal(
      [&] {
        expiredHandle.apply(
            SelectivityVector(1),
            handleArguments,
            handleRow->type(),
            context,
            result);
      },
      "Invalid or expired");
  expectFatal(
      [&] {
        expiredHandle.apply(
            SelectivityVector(1),
            handleArguments,
            handleRow->type(),
            context,
            result);
      },
      "invalidated");

  // Sparse scatter preserves prior values. It may materialize a codec column
  // while copying, but must not evaluate unselected input rows.
  WasmVectorFunction sparseFunction(module, "generic_identity", {}, true);
  SelectivityVector sparseRows(2, false);
  sparseRows.setValid(1, true);
  sparseRows.updateBounds();
  result = BaseVector::copy(*source, pool);
  result->as<RowVector>()->childAt(0)->as<FlatVector<int64_t>>()->set(0, 111);
  *count = 0;
  sparseFunction.apply(sparseRows, arguments, rowType, context, result);
  check(
      result->as<RowVector>()->childAt(0)->as<SimpleVector<int64_t>>()->valueAt(
          0) == 111 &&
          result->as<RowVector>()
                  ->childAt(1)
                  ->loadedVector()
                  ->as<SimpleVector<int64_t>>()
                  ->valueAt(1) == 8 &&
          *count == 1,
      "sparse codec scatter changed unselected rows");
}

void testStateAccountingProtocol(memory::MemoryPool* pool) {
  const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  auto original = *std::find_if(
      declarations.aggregates.begin(),
      declarations.aggregates.end(),
      [](const auto& entry) { return entry.name == "strict_sum_wasm"; });
  check(
      original.abiVersion == 3,
      "new row module must declare state-accounting ABI");
  auto record = [](uint32_t handle, uint64_t size) {
    std::string bytes;
    for (size_t i = 0; i < 4; ++i)
      bytes += static_cast<char>(handle >> (i * 8));
    for (size_t i = 0; i < 8; ++i)
      bytes += static_cast<char>(size >> (i * 8));
    return bytes;
  };
  auto data = [](int offset, std::string_view bytes) {
    std::string escaped;
    for (uint8_t byte : bytes)
      escaped += fmt::format("\\{:02x}", byte);
    return fmt::format("(data (i32.const {}) \"{}\")", offset, escaped);
  };
  const auto created = record(1, 16) + record(2, 16);
  std::vector<std::pair<std::string, std::string>> invalid{
      {record(1, 16), "count mismatch"},
      {record(1, 16) + record(1, 16), "duplicate Wasm state"},
      {record(1, 16) + record(3, 16), "Unexpected"},
      {record(1, std::numeric_limits<uint64_t>::max()) + record(2, 16),
       "exceeds Store memory limit"},
      {record(1, 64UL << 20) + record(2, 16), "live state reports exceed"}};
  for (const auto& [report, error] : invalid) {
    auto exports = data(8192, created) + data(9000, report);
    exports +=
        " (func (export \"create\") (param i32) (result v128) (v128.const i32x4 0 8192 24 0))";
    for (const auto* name :
         {"destroy", "update", "serialize", "merge", "finalize", "compact"}) {
      exports += " (func (export \"" + std::string(name) +
          "\") (param i32 i32) (result v128) " +
          (std::string_view(name) == "destroy"
               ? "(v128.const i32x4 0 0 0 0)"
               : fmt::format("(v128.const i32x4 0 9000 {} 0)", report.size())) +
          ")";
    }
    for (const auto* name : {"update_single", "merge_single"})
      exports += " (func (export \"" + std::string(name) +
          "\") (param i32 i32 i32) (result v128) (v128.const i32x4 0 0 0 0))";
    auto manifest = original;
    manifest.initializeEntrypoint.clear();
    manifest.entrypoints = {
        "create",
        "destroy",
        "update",
        "update_single",
        "serialize",
        "merge",
        "merge_single",
        "finalize",
        "",
        "compact"};
    WasmAggregate aggregate(
        HUGEINT(),
        manifest,
        WasmModule::compile(
            "state-report-" + error,
            tinyModule("(v128.const i32x4 0 0 0 0)", exports)));
    HashStringAllocator allocator(pool);
    aggregate.setAllocator(&allocator);
    constexpr int32_t offset = alignof(std::max_align_t);
    aggregate.setOffsets(
        offset,
        exec::RowContainer::nullByte(0),
        exec::RowContainer::nullMask(0),
        exec::RowContainer::initializedByte(0),
        exec::RowContainer::initializedMask(0),
        4);
    auto first = AlignedBuffer::allocate<char>(
        offset + aggregate.accumulatorFixedWidthSize(), pool, 0);
    auto second = AlignedBuffer::allocate<char>(
        offset + aggregate.accumulatorFixedWidthSize(), pool, 0);
    char* groups[] = {first->asMutable<char>(), second->asMutable<char>()};
    const vector_size_t indices[] = {0, 1};
    aggregate.initializeNewGroups(
        groups, folly::Range<const vector_size_t*>(indices, 2));
    check(
        folly::loadUnaligned<uint32_t>(groups[0] + 4) == 16,
        "initial size report was not tracked");
    const auto before = pool->usedBytes();
    expectFatal([&] { aggregate.compact(folly::Range(groups, 2)); }, error);
    check(
        folly::loadUnaligned<uint32_t>(groups[0] + 4) == 16 &&
            folly::loadUnaligned<uint32_t>(groups[1] + 4) == 16,
        "malformed report partially changed row sizes");
    check(
        pool->usedBytes() < before,
        "malformed state report retained Store charge");
    expectFatal(
        [&] { aggregate.compact(folly::Range(groups, 2)); }, "invalidated");
    aggregate.destroy(folly::Range(groups, 2));
    check(
        folly::loadUnaligned<uint32_t>(groups[0] + 4) == 0 &&
            folly::loadUnaligned<uint32_t>(groups[1] + 4) == 0,
        "cleanup did not remove guest state sizes");
  }
  ModuleFile noCompact(rewriteDeclarations([](folly::dynamic& root) {
    if (root["name"].asString() != "strict_sum_wasm")
      return false;
    root["entrypoints"].erase("compact");
    return true;
  }));
  expectCollision([&] { registerWasmModule(noCompact.path()); });
  ModuleFile wrongKind(rewriteDeclarations([](folly::dynamic& root) {
    if (root["name"].asString() != "add_i64")
      return false;
    root["abi_version"] = 3;
    return true;
  }));
  expectCollision([&] { registerWasmModule(wrongKind.path()); });
}

// Exercise full-Store migration when native compacts only one of two groups.
// The synthetic module grows only in create, so replaying create during restore
// would both lose the page reduction and reinitialize its private state.
void testOwnedCheckpointProtocol(memory::MemoryPool* pool) {
  const auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  auto manifest = *std::find_if(
      declarations.aggregates.begin(),
      declarations.aggregates.end(),
      [](const auto& entry) { return entry.name == "strict_sum_wasm"; });
  manifest.initializeEntrypoint.clear();
  manifest.entrypoints = {
      "create",
      "destroy",
      "update",
      "update_single",
      "serialize",
      "merge",
      "merge_single",
      "finalize",
      "",
      "compact",
      "checkpoint",
      "restore"};
  auto word = [](uint32_t value) {
    std::string bytes;
    for (size_t i = 0; i < 4; ++i)
      bytes += static_cast<char>(value >> (8 * i));
    return bytes;
  };
  auto record = [&](uint32_t handle, uint64_t bytes) {
    std::string result = word(handle);
    for (size_t i = 0; i < 8; ++i)
      result += static_cast<char>(bytes >> (8 * i));
    return result;
  };
  auto data = [](int offset, std::string_view bytes) {
    std::string escaped;
    for (uint8_t byte : bytes)
      escaped += fmt::format("\\{:02x}", byte);
    return fmt::format("(data (i32.const {}) \"{}\")", offset, escaped);
  };
  const auto saved = std::string("VWS1") + word(3) + word(2) + word(1) +
      word(0) + word(2) + word(0);
  const auto restored = record(1, 8) + record(2, 8);
  struct Case {
    std::string name;
    std::string snapshot;
    std::string restoreReport;
    std::string error;
    bool trap{false};
    bool noHeadroom{false};
  };
  std::vector<Case> cases{
      {"success", saved, restored, ""},
      {"no-headroom", saved, restored, "", false, true}};
  auto corrupt = saved;
  corrupt[0] = 'X';
  cases.push_back({"magic", corrupt, restored, "format"});
  corrupt = saved;
  corrupt[8] = 1;
  cases.push_back({"count", corrupt, restored, "count mismatch"});
  corrupt = saved;
  corrupt[20] = 1;
  cases.push_back(
      {"duplicate", corrupt, restored, "duplicate Wasm checkpoint handle"});
  corrupt = saved;
  corrupt[20] = 4;
  cases.push_back({"unknown", corrupt, restored, "checkpoint handle"});
  corrupt = saved;
  corrupt[12] = 0;
  cases.push_back({"zero", corrupt, restored, "checkpoint handle"});
  corrupt = saved;
  corrupt[4] = 2;
  cases.push_back({"high-water", corrupt, restored, "checkpoint handle"});
  cases.push_back(
      {"truncated", saved.substr(0, saved.size() - 1), restored, "Truncated"});
  cases.push_back({"trailing", saved + 'x', restored, "Trailing"});
  cases.push_back(
      {"restore-report",
       saved,
       record(1, 8) + record(1, 8),
       "duplicate Wasm state"});
  cases.push_back({"restore-trap", saved, restored, "invocation failed", true});
  for (const auto& test : cases) {
    auto limitedRoot = test.noHeadroom
        ? memory::memoryManager()->addRootPool(
              "wasm-checkpoint-no-headroom", 16 * 65536)
        : nullptr;
    auto limitedLeaf = limitedRoot
        ? limitedRoot->addLeafChild("wasm-checkpoint-no-headroom-leaf")
        : nullptr;
    auto* casePool = limitedLeaf ? limitedLeaf.get() : pool;
    auto exports = data(8192, record(1, 16) + record(2, 16)) +
        data(9000, record(1, 16)) + data(10000, test.snapshot) +
        data(12000, test.restoreReport);
    exports += fmt::format(
        " (func (export \"create\") (param i32) (result v128) i32.const {} memory.grow drop (v128.const i32x4 0 8192 24 0))",
        test.noHeadroom ? 14 : 32);
    exports +=
        " (func (export \"compact\") (param i32 i32) (result v128) (v128.const i32x4 0 9000 12 0))";
    exports += " (func (export \"checkpoint\") (param i32) (result v128) " +
        (test.noHeadroom
             ? std::string("unreachable")
             : fmt::format(
                   "(v128.const i32x4 0 10000 {} 0)", test.snapshot.size())) +
        ")";
    exports +=
        " (func (export \"restore\") (param i32 i32 i32) (result v128) " +
        (test.trap ? std::string("unreachable")
                   : fmt::format(
                         "(v128.const i32x4 0 12000 {} 0)",
                         test.restoreReport.size())) +
        ")";
    for (const auto* name :
         {"destroy", "update", "serialize", "merge", "finalize"})
      exports += " (func (export \"" + std::string(name) +
          "\") (param i32 i32) (result v128) (v128.const i32x4 0 0 0 0))";
    for (const auto* name : {"update_single", "merge_single"})
      exports += " (func (export \"" + std::string(name) +
          "\") (param i32 i32 i32) (result v128) (v128.const i32x4 0 0 0 0))";
    WasmAggregate aggregate(
        HUGEINT(),
        manifest,
        WasmModule::compile(
            "checkpoint-" + test.name,
            tinyModule("(v128.const i32x4 0 0 0 0)", exports)));
    HashStringAllocator allocator(casePool);
    aggregate.setAllocator(&allocator);
    constexpr int32_t offset = alignof(std::max_align_t);
    aggregate.setOffsets(
        offset,
        exec::RowContainer::nullByte(0),
        exec::RowContainer::nullMask(0),
        exec::RowContainer::initializedByte(0),
        exec::RowContainer::initializedMask(0),
        4);
    auto first = AlignedBuffer::allocate<char>(
        offset + aggregate.accumulatorFixedWidthSize(), casePool, 0);
    auto second = AlignedBuffer::allocate<char>(
        offset + aggregate.accumulatorFixedWidthSize(), casePool, 0);
    char* groups[] = {first->asMutable<char>(), second->asMutable<char>()};
    folly::storeUnaligned<uint32_t>(groups[0] + 4, 50);
    folly::storeUnaligned<uint32_t>(groups[1] + 4, 70);
    const vector_size_t indices[] = {0, 1};
    aggregate.initializeNewGroups(
        groups, folly::Range<const vector_size_t*>(indices, 2));
    const auto firstNull = groups[0][exec::RowContainer::nullByte(0)];
    const auto secondNull = groups[1][exec::RowContainer::nullByte(0)];
    const auto before = casePool->usedBytes();
    if (test.noHeadroom) {
      check(
          aggregate.compact(folly::Range(groups, 1)) == 0,
          "logical compaction reported physical reclaim");
      check(
          casePool->usedBytes() == before,
          "headroom check allocated a new Store");
      check(
          folly::loadUnaligned<uint32_t>(groups[0] + 4) == 66 &&
              folly::loadUnaligned<uint32_t>(groups[1] + 4) == 86,
          "headroom check changed live State");
    } else if (test.error.empty()) {
      const auto freed = aggregate.compact(folly::Range(groups, 1));
      check(
          freed == (32UL << 16),
          "Store compaction did not free original grown pages");
      check(
          before - casePool->usedBytes() >= (1UL << 20),
          "Store page charges were not returned");
      check(
          folly::loadUnaligned<uint32_t>(groups[0] + 4) == 58 &&
              folly::loadUnaligned<uint32_t>(groups[1] + 4) == 78,
          "full-Store restore did not track every group or preserve shared sizes");
      check(
          folly::loadUnaligned<uint32_t>(groups[0] + offset) == 1 &&
              folly::loadUnaligned<uint32_t>(groups[1] + offset) == 2,
          "restored handles changed");
      check(
          groups[0][exec::RowContainer::nullByte(0)] == firstNull &&
              groups[1][exec::RowContainer::nullByte(0)] == secondNull,
          "compaction changed NULL flags");
    } else {
      expectFatal(
          [&] { aggregate.compact(folly::Range(groups, 1)); }, test.error);
      check(
          folly::loadUnaligned<uint32_t>(groups[0] + 4) == 66 &&
              folly::loadUnaligned<uint32_t>(groups[1] + 4) == 86,
          "checkpoint failure partially published restore sizes");
      expectFatal(
          [&] { aggregate.compact(folly::Range(groups, 1)); }, "invalidated");
      check(
          before - casePool->usedBytes() >= (1UL << 20),
          "failed migration retained Store charges");
    }
    aggregate.destroy(folly::Range(groups, 2));
    check(
        folly::loadUnaligned<uint32_t>(groups[0] + 4) == 50 &&
            folly::loadUnaligned<uint32_t>(groups[1] + 4) == 70,
        "migration cleanup erased shared row sizes");
  }
  for (const auto* problem : {"protocol", "missing", "undeclared", "version"}) {
    ModuleFile invalid(rewriteDeclarations([&](folly::dynamic& root) {
      if (root["name"].asString() != "collect_strings_wasm")
        return false;
      const std::string_view issue(problem);
      if (issue == "protocol")
        root["state_checkpoint"] = "unknown";
      if (issue == "missing")
        root["entrypoints"].erase("restore");
      if (issue == "undeclared")
        root.erase("state_checkpoint");
      if (issue == "version")
        root["abi_version"] = 2;
      return true;
    }));
    expectCollision([&] { registerWasmModule(invalid.path()); });
  }
}

void testCleanup(memory::MemoryPool* pool) {
  auto declarations = loadEmbeddedManifests(WASM_MODULE_PATH);
  auto manifest = *std::find_if(
      declarations.aggregates.begin(),
      declarations.aggregates.end(),
      [](const auto& a) { return a.name == "sum_i64"; });
  manifest.entrypoints.destroy = scalarManifest("checked_divide").entrypoint;
  WasmAggregate aggregate(
      BIGINT(), manifest, WasmModule::compile(WASM_MODULE_PATH));
  HashStringAllocator allocator(pool);
  aggregate.setAllocator(&allocator);
  constexpr int32_t offset = alignof(std::max_align_t);
  aggregate.setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  auto storage = AlignedBuffer::allocate<char>(
      offset + aggregate.accumulatorFixedWidthSize(), pool, 0);
  char* group = storage->asMutable<char>();
  const vector_size_t index = 0;
  aggregate.initializeNewGroups(
      &group, folly::Range<const vector_size_t*>(&index, 1));
  const auto before = pool->usedBytes();
  aggregate.destroy(folly::Range<char**>(&group, 1));
  uint32_t handle;
  std::memcpy(&handle, group + offset, sizeof(handle));
  check(handle == 0, "cleanup failure left a host state handle");
  check(
      pool->usedBytes() < before,
      "cleanup failure retained Store memory budget");

  std::string exports =
      " (data (i32.const 8192) \"\\01\\00\\00\\00\")"
      " (func (export \"create\") (param i32) (result v128)"
      " (v128.const i32x4 0 8192 4 0))";
  for (const auto* name :
       {"destroy", "update", "serialize", "merge", "finalize"}) {
    exports += " (func (export \"" + std::string(name) +
        "\") (param i32 i32) (result v128) " +
        (std::string_view(name) == "finalize" ? "(v128.const i32x4 0 4096 1 0)"
                                              : "(v128.const i32x4 0 0 0 0)") +
        ")";
  }
  for (const auto* name : {"update_single", "merge_single"}) {
    exports += " (func (export \"" + std::string(name) +
        "\") (param i32 i32 i32) (result v128) (v128.const i32x4 0 0 0 0))";
  }
  manifest.entrypoints = {
      "create",
      "destroy",
      "update",
      "update_single",
      "serialize",
      "merge",
      "merge_single",
      "finalize"};
  WasmAggregate protocolFailure(
      BIGINT(),
      manifest,
      WasmModule::compile(
          "aggregate-invalid-ipc",
          tinyModule("(v128.const i32x4 0 4096 1 0)", exports)));
  protocolFailure.setAllocator(&allocator);
  protocolFailure.setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  std::memset(group, 0, storage->size());
  protocolFailure.initializeNewGroups(
      &group, folly::Range<const vector_size_t*>(&index, 1));
  VectorPtr result;
  expectFatal(
      [&] { protocolFailure.extractValues(&group, 1, &result); }, "Arrow IPC");
  expectFatal(
      [&] { protocolFailure.extractValues(&group, 1, &result); },
      "invalidated");
  protocolFailure.destroy(folly::Range<char**>(&group, 1));

  // A create result must not alias a live state from an earlier create batch.
  WasmAggregate duplicate(
      BIGINT(),
      manifest,
      WasmModule::compile(
          "aggregate-duplicate-handles",
          tinyModule("(v128.const i32x4 0 0 0 0)", exports)));
  duplicate.setAllocator(&allocator);
  duplicate.setOffsets(
      offset,
      exec::RowContainer::nullByte(0),
      exec::RowContainer::nullMask(0),
      exec::RowContainer::initializedByte(0),
      exec::RowContainer::initializedMask(0),
      4);
  auto otherStorage =
      AlignedBuffer::allocate<char>(offset + sizeof(uint32_t), pool, 0);
  char* other = otherStorage->asMutable<char>();
  std::memset(group, 0, storage->size());
  duplicate.initializeNewGroups(
      &group, folly::Range<const vector_size_t*>(&index, 1));
  expectFatal(
      [&] {
        duplicate.initializeNewGroups(
            &other, folly::Range<const vector_size_t*>(&index, 1));
      },
      "duplicate state handle");
  check(
      folly::loadUnaligned<uint32_t>(other + offset) == 0,
      "duplicate handle published to another group");
  duplicate.destroy(folly::Range<char**>(&group, 1));
}

} // namespace
} // namespace facebook::velox::functions::wasm::test

int main() {
  using namespace facebook::velox;
  using namespace facebook::velox::functions::wasm;
  using namespace facebook::velox::functions::wasm::test;
  try {
    memory::MemoryManager::Options memoryOptions;
    memoryOptions.trackDefaultUsage = true;
    memory::MemoryManager::initialize(memoryOptions);
    auto pool = memory::memoryManager()->addLeafPool("wasm-safety");
    auto query = core::QueryCtx::create();
    core::ExecCtx execCtx(pool.get(), query.get());
    exec::registerFunctionCallToSpecialForm(
        "try", std::make_unique<exec::TryCallToSpecialForm>());
    registerWasmModule(WASM_MODULE_PATH);
    testHostileIpc(pool.get(), execCtx);
    testIntermediateRowAliases(pool.get());
    testGatherEncodedLeaves(pool.get());
    testRuntimeLimits(pool.get());
    testRegistration();
    testRowErrors(pool.get(), execCtx);
    testNativeStatusAbi(pool.get(), execCtx);
    testCompanionRegistrationAndRecovery(pool.get(), execCtx);
    testCleanup(pool.get());
    testStateAccountingProtocol(pool.get());
    testOwnedCheckpointProtocol(pool.get());
    testDeferredCodecs(pool.get(), execCtx);
    testUntrustedAsciiOutput(pool.get(), execCtx);
    std::cout << "Wasm safety regressions passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Wasm safety regressions failed: " << error.what() << '\n';
    return 1;
  }
}
