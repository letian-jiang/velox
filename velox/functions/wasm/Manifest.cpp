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

#include "velox/functions/wasm/Manifest.h"
#include "velox/functions/wasm/Runtime.h"

#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <folly/json.h>

#include "velox/common/base/Exceptions.h"
#include "velox/functions/wasm/Abi.h"
#include "velox/functions/wasm/TypeBridge.h"
#include "velox/type/fbhive/HiveTypeParser.h"

namespace facebook::velox::functions::wasm {
namespace {

std::string quoteRowName(std::string_view name) {
  std::string quoted = "\"";
  for (const auto character : name) {
    if (character == '"') {
      quoted += '"';
    }
    quoted += character;
  }
  return quoted + '"';
}

} // namespace

std::string signatureType(const TypePtr& type) {
  if (isBridgedType(type)) {
    if (type->kind() == TypeKind::OPAQUE) {
      return "opaque";
    }
    std::string name = type->name();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
      return std::tolower(c);
    });
    return name;
  }
  if (type->isDecimal()) {
    auto [precision, scale] = getDecimalPrecisionScale(*type);
    return fmt::format("decimal({},{})", precision, scale);
  }
  if (type->isDate()) {
    return "date";
  }
  switch (type->kind()) {
    case TypeKind::UNKNOWN:
      return "unknown";
    case TypeKind::BOOLEAN:
      return "boolean";
    case TypeKind::TINYINT:
      return "tinyint";
    case TypeKind::SMALLINT:
      return "smallint";
    case TypeKind::INTEGER:
      return "integer";
    case TypeKind::BIGINT:
      return "bigint";
    case TypeKind::HUGEINT:
      return "hugeint";
    case TypeKind::REAL:
      return "real";
    case TypeKind::DOUBLE:
      return "double";
    case TypeKind::VARCHAR:
      return "varchar";
    case TypeKind::VARBINARY:
      return "varbinary";
    case TypeKind::TIMESTAMP:
      return "timestamp";
    case TypeKind::ARRAY:
      return "array(" + signatureType(type->childAt(0)) + ")";
    case TypeKind::MAP:
      return "map(" + signatureType(type->childAt(0)) + "," +
          signatureType(type->childAt(1)) + ")";
    case TypeKind::ROW: {
      std::string result = "row(";
      for (size_t index = 0; index < type->size(); ++index) {
        if (index != 0) {
          result += ",";
        }
        result += quoteRowName(type->asRow().nameOf(index)) + " " +
            signatureType(type->childAt(index));
      }
      return result + ")";
    }
    default:
      VELOX_UNREACHABLE("Unsupported Wasm UDF signature type");
  }
}

namespace {

constexpr std::string_view kMetadataSection = "velox.udf.v1";
constexpr size_t kMaxMetadataBytes = 1UL << 20;
constexpr size_t kMaxFunctions = 256;

const folly::dynamic& required(
    const folly::dynamic& object,
    const char* field,
    const std::filesystem::path& path) {
  VELOX_USER_CHECK(
      object.isObject() && object.count(field) != 0,
      "Wasm UDF metadata in '{}' is missing required field '{}'",
      path.string(),
      field);
  return object[field];
}

void validateTypeDepth(std::string_view type) {
  size_t depth = 0;
  for (char byte : type) {
    if (byte == '(' || byte == '<') {
      VELOX_USER_CHECK_LE(++depth, 64, "Wasm type nesting exceeds 64");
    } else if (byte == ')' || byte == '>') {
      if (depth > 0)
        --depth;
    }
  }
}

TypePtr parseTypeName(
    const std::string& name,
    const std::filesystem::path& path) {
  validateTypeDepth(name);
  if (name.find('<') != std::string::npos) {
    auto type = type::fbhive::HiveTypeParser().parse(name);
    VELOX_USER_CHECK(
        type->kind() == TypeKind::ARRAY || type->kind() == TypeKind::MAP ||
            type->kind() == TypeKind::ROW,
        "Unsupported Wasm UDF complex type '{}' in '{}'",
        name,
        path.string());
    return type;
  }
  if (name == "boolean")
    return BOOLEAN();
  if (name == "tinyint")
    return TINYINT();
  if (name == "smallint")
    return SMALLINT();
  if (name == "integer")
    return INTEGER();
  if (name == "bigint")
    return BIGINT();
  if (name == "real")
    return REAL();
  if (name == "double")
    return DOUBLE();
  if (name == "varchar")
    return VARCHAR();
  if (name == "varbinary")
    return VARBINARY();
  if (name == "date")
    return DATE();
  if (name == "timestamp")
    return TIMESTAMP();
  VELOX_USER_FAIL(
      "Unsupported Wasm UDF type '{}' in '{}'", name, path.string());
}

TypePtr parseTypeDeclaration(
    const folly::dynamic& declaration,
    const std::filesystem::path& path) {
  VELOX_USER_CHECK(
      declaration.isObject(), "Wasm UDF type declaration must be an object");
  const auto& name = required(declaration, "type", path);
  const auto& nullable = required(declaration, "nullable", path);
  VELOX_USER_CHECK(name.isString(), "Wasm UDF type must be a string");
  VELOX_USER_CHECK(nullable.isBool(), "Wasm UDF nullable must be a boolean");
  if (name.asString() == "array") {
    return ARRAY(
        parseTypeDeclaration(required(declaration, "element", path), path));
  }
  if (name.asString() == "map") {
    auto key = parseTypeDeclaration(required(declaration, "key", path), path);
    auto value =
        parseTypeDeclaration(required(declaration, "value", path), path);
    VELOX_USER_CHECK(
        !required(declaration, "key", path)["nullable"].asBool(),
        "Wasm UDF map keys must be non-null");
    return MAP(std::move(key), std::move(value));
  }
  if (name.asString() == "row") {
    const auto& fields = required(declaration, "fields", path);
    VELOX_USER_CHECK(fields.isArray(), "Wasm UDF row fields must be an array");
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    for (const auto& field : fields) {
      const auto& fieldName = required(field, "name", path);
      VELOX_USER_CHECK(
          fieldName.isString() && !fieldName.asString().empty(),
          "Wasm UDF row field name must be a non-empty string");
      names.push_back(fieldName.asString());
      types.push_back(
          parseTypeDeclaration(required(field, "type", path), path));
    }
    return ROW(std::move(names), std::move(types));
  }
  return parseTypeName(name.asString(), path);
}

ManifestType parseManifestType(
    const folly::dynamic& value,
    const std::filesystem::path& path) {
  VELOX_USER_CHECK(
      value.isObject(), "Wasm UDF type declaration must be an object");
  const auto& type = required(value, "type", path);
  const auto& nullable = required(value, "nullable", path);
  VELOX_USER_CHECK(type.isString(), "Wasm UDF type must be a string");
  VELOX_USER_CHECK(nullable.isBool(), "Wasm UDF nullable must be a boolean");
  return {parseTypeDeclaration(value, path), nullable.asBool()};
}

bool optionalBool(
    const folly::dynamic& object,
    const char* field,
    bool defaultValue) {
  if (object.count(field) == 0)
    return defaultValue;
  VELOX_USER_CHECK(
      object[field].isBool(), "Wasm UDF '{}' must be a boolean", field);
  return object[field].asBool();
}

std::string requiredString(
    const folly::dynamic& object,
    const char* field,
    const std::filesystem::path& path) {
  const auto& value = required(object, field, path);
  VELOX_USER_CHECK(
      value.isString() && !value.asString().empty(),
      "Wasm UDF '{}' must be a non-empty string",
      field);
  return value.asString();
}

std::vector<ManifestType> parseArguments(
    const folly::dynamic& root,
    const std::filesystem::path& path) {
  const auto& arguments = required(root, "arguments", path);
  VELOX_USER_CHECK(arguments.isArray(), "Wasm UDF arguments must be an array");
  std::vector<ManifestType> parsed;
  parsed.reserve(arguments.size());
  for (const auto& argument : arguments) {
    parsed.push_back(parseManifestType(argument, path));
  }
  return parsed;
}

void validateVersionAndKind(
    const folly::dynamic& root,
    const std::filesystem::path& path,
    std::string_view kind) {
  VELOX_USER_CHECK(root.isObject(), "Wasm UDF metadata must be an object");
  const auto& version = required(root, "abi_version", path);
  const auto& actualKind = required(root, "kind", path);
  VELOX_USER_CHECK(version.isInt(), "Wasm UDF abi_version must be an integer");
  VELOX_USER_CHECK(
      version.asInt() == kAbiVersion ||
          version.asInt() == kNativeStatusAbiVersion ||
          version.asInt() == kStateAccountingAbiVersion,
      "Unsupported Wasm UDF ABI version in '{}'",
      path.string());
  VELOX_USER_CHECK(
      version.asInt() == kAbiVersion || optionalBool(root, "row_api", false),
      "Wasm ABI 2/3 requires the row API transport profile");
  VELOX_USER_CHECK(
      version.asInt() != kStateAccountingAbiVersion || kind == "aggregate",
      "Wasm ABI 3 requires an aggregate declaration");
  VELOX_USER_CHECK(
      actualKind.isString() && actualKind.asString() == kind,
      "Expected a '{}' Wasm UDF declaration in '{}'",
      kind,
      path.string());
}

// Scalar signatures use Velox's SQL signature syntax, including nested
// variables. Older SDKs emit Hive syntax or structured concrete declarations.
std::string scalarSignatureType(
    const folly::dynamic& declaration,
    const std::filesystem::path& path) {
  const auto name = requiredString(declaration, "type", path);
  validateTypeDepth(name);
  VELOX_USER_CHECK(
      required(declaration, "nullable", path).isBool(),
      "Wasm UDF nullable must be a boolean");
  if (name.find('<') != std::string::npos || name == "array" || name == "map" ||
      name == "row") {
    return signatureType(parseTypeDeclaration(declaration, path));
  }
  return name;
}

ScalarManifest parseScalar(
    const folly::dynamic& root,
    const std::filesystem::path& path,
    bool validateDeclaration = true) {
  if (validateDeclaration)
    validateVersionAndKind(root, path, "scalar");
  exec::FunctionSignatureBuilder builder;
  if (root.count("type_variables")) {
    const auto& variables = root["type_variables"];
    VELOX_USER_CHECK(variables.isObject(), "type_variables must be an object");
    for (const auto& item : variables.items()) {
      VELOX_USER_CHECK(
          item.second.isString(), "type variable constraint must be a string");
      const auto name = item.first.asString();
      const auto constraint = item.second.asString();
      if (constraint == "any") {
        builder.typeVariable(name);
      } else if (constraint == "known") {
        builder.knownTypeVariable(name);
      } else if (constraint == "comparable") {
        builder.comparableTypeVariable(name);
      } else if (constraint == "orderable") {
        builder.orderableTypeVariable(name);
      } else if (
          constraint == "known&comparable" || constraint == "known&orderable") {
        builder.variable(
            exec::SignatureVariable(
                name,
                std::nullopt,
                exec::ParameterType::kTypeParameter,
                true,
                constraint == "known&orderable",
                true));
      } else {
        VELOX_USER_FAIL(
            "Unsupported type variable constraint '{}'", constraint);
      }
    }
  }
  if (root.count("integer_variables")) {
    const auto& variables = root["integer_variables"];
    VELOX_USER_CHECK(
        variables.isObject(), "integer_variables must be an object");
    for (const auto& item : variables.items()) {
      VELOX_USER_CHECK(
          item.second.isString(),
          "integer variable constraint must be a string");
      VELOX_USER_CHECK_LE(
          item.second.asString().size(),
          512,
          "Wasm integer constraint exceeds 512 bytes");
      validateTypeDepth(item.second.asString());
      builder.integerVariable(item.first.asString(), item.second.asString());
    }
  }
  const auto& arguments = required(root, "arguments", path);
  VELOX_USER_CHECK(arguments.isArray(), "Wasm UDF arguments must be an array");
  for (const auto& argument : arguments) {
    auto type = scalarSignatureType(argument, path);
    if (optionalBool(argument, "constant", false)) {
      builder.constantArgumentType(type);
    } else {
      builder.argumentType(type);
    }
  }
  if (optionalBool(root, "variable_arity", false)) {
    VELOX_USER_CHECK(
        !arguments.empty(), "Variadic signature needs a tail argument");
    builder.variableArity();
  }
  builder.returnType(scalarSignatureType(required(root, "return", path), path));
  std::string initialize;
  if (root.count("initialize")) {
    initialize = requiredString(root, "initialize", path);
  }
  std::vector<std::string> configKeys;
  if (root.count("config_keys")) {
    VELOX_USER_CHECK(!initialize.empty(), "config_keys requires initialize");
    VELOX_USER_CHECK(
        root["config_keys"].isArray(), "config_keys must be an array");
    for (const auto& key : root["config_keys"]) {
      VELOX_USER_CHECK(
          key.isString() && !key.asString().empty(),
          "config_keys must contain non-empty strings");
      configKeys.push_back(key.asString());
    }
  }
  return {
      .abiVersion = static_cast<uint32_t>(root["abi_version"].asInt()),
      .wasmPath = path,
      .name = requiredString(root, "name", path),
      .entrypoint = requiredString(root, "entrypoint", path),
      .signature = builder.build(),
      .initializeEntrypoint = std::move(initialize),
      .configKeys = std::move(configKeys),
      .rowApi = optionalBool(root, "row_api", false),
      .hasAscii = optionalBool(root, "has_ascii", false),
      .deterministic = optionalBool(root, "deterministic", true),
      .defaultNullBehavior = optionalBool(root, "default_null_behavior", true),
  };
}

AggregateManifest parseAggregate(
    const folly::dynamic& root,
    const std::filesystem::path& path) {
  validateVersionAndKind(root, path, "aggregate");
  const auto& exports = required(root, "entrypoints", path);
  VELOX_USER_CHECK(
      exports.isObject(), "Wasm UDAF entrypoints must be an object");
  const bool rowApi = optionalBool(root, "row_api", false);
  exec::AggregateFunctionSignaturePtr signature;
  std::vector<ManifestType> arguments;
  ManifestType intermediate{nullptr, true};
  ManifestType result{nullptr, true};
  std::string initialize;
  std::vector<std::string> configKeys;
  uint32_t lambdaCount = 0;
  if (root.count("lambdas")) {
    const auto& lambdas = root["lambdas"];
    VELOX_USER_CHECK(
        lambdas.isArray() && !lambdas.empty() && lambdas.size() <= 64,
        "Wasm UDAF lambdas must contain 1..64 function signatures");
    VELOX_USER_CHECK(
        rowApi && root["abi_version"].asInt() == kStateAccountingAbiVersion,
        "Wasm UDAF lambdas require row ABI 3");
    VELOX_USER_CHECK_EQ(
        requiredString(root, "lambda_callback", path),
        "ipc-v1",
        "Unsupported Wasm lambda callback protocol");
    lambdaCount = lambdas.size();
  } else {
    VELOX_USER_CHECK(
        !root.count("lambda_callback"),
        "Wasm lambda protocol requires lambda signatures");
  }
  if (rowApi) {
    auto scalarRoot = root;
    if (lambdaCount) {
      const auto& values = required(root, "arguments", path);
      VELOX_USER_CHECK(values.isArray(), "Wasm UDF arguments must be an array");
      const bool variadic = optionalBool(root, "variable_arity", false);
      VELOX_USER_CHECK(
          !variadic || !values.empty(),
          "Variadic signature needs a tail argument");
      // Functions are fixed arguments: insert them between the fixed value
      // prefix and the final variadic value type, or after all fixed values.
      folly::dynamic combined = folly::dynamic::array;
      const auto fixed = values.size() - (variadic ? 1 : 0);
      for (size_t i = 0; i < fixed; ++i)
        combined.push_back(values[i]);
      for (const auto& lambda : root["lambdas"]) {
        VELOX_USER_CHECK(
            lambda.isString(), "Wasm lambda signature must be a string");
        validateTypeDepth(lambda.asString());
        auto type = exec::parseTypeSignature(lambda.asString());
        VELOX_USER_CHECK(
            type.baseName() == "function" && !type.parameters().empty(),
            "Wasm lambda needs a function signature");
        combined.push_back(
            folly::dynamic::object("type", lambda)("nullable", false));
      }
      if (variadic)
        combined.push_back(values[values.size() - 1]);
      scalarRoot["arguments"] = std::move(combined);
    }
    scalarRoot["kind"] = "scalar";
    scalarRoot["entrypoint"] = requiredString(exports, "update", path);
    // Version/kind were validated as an aggregate before reusing the scalar
    // signature parser. ABI 3 cannot be loaded as a scalar declaration.
    auto scalar = parseScalar(scalarRoot, path, false);
    initialize = std::move(scalar.initializeEntrypoint);
    configKeys = std::move(scalar.configKeys);
    VELOX_USER_CHECK(!initialize.empty(), "Row UDAF requires initialize");
    signature = std::make_shared<exec::AggregateFunctionSignature>(
        scalar.signature->variables(),
        scalar.signature->returnType(),
        exec::parseTypeSignature(
            scalarSignatureType(required(root, "intermediate", path), path)),
        scalar.signature->argumentTypes(),
        scalar.signature->constantArguments(),
        scalar.signature->variableArity());
  } else {
    arguments = parseArguments(root, path);
    intermediate =
        parseManifestType(required(root, "intermediate", path), path);
    result = parseManifestType(required(root, "return", path), path);
    VELOX_USER_CHECK(
        intermediate.type->kind() == TypeKind::VARBINARY,
        "Legacy Wasm UDAF intermediate type must be varbinary");
    exec::AggregateFunctionSignatureBuilder builder;
    builder.returnType(signatureType(result.type));
    builder.intermediateType(signatureType(intermediate.type));
    for (const auto& arg : arguments)
      builder.argumentType(signatureType(arg.type));
    signature = builder.build();
  }
  const bool checkpoint = root.count("state_checkpoint") != 0;
  if (checkpoint) {
    VELOX_USER_CHECK_EQ(
        requiredString(root, "state_checkpoint", path),
        "owned-v1",
        "Unsupported Wasm state checkpoint protocol");
    VELOX_USER_CHECK(
        rowApi && root["abi_version"].asInt() == kStateAccountingAbiVersion,
        "Wasm checkpoint requires row aggregate ABI 3");
  } else {
    VELOX_USER_CHECK(
        !exports.count("checkpoint") && !exports.count("restore"),
        "Wasm checkpoint exports require protocol metadata");
  }
  return {
      .abiVersion = static_cast<uint32_t>(root["abi_version"].asInt()),
      .wasmPath = path,
      .name = requiredString(root, "name", path),
      .entrypoints =
          {
              .create = requiredString(exports, "create", path),
              .destroy = requiredString(exports, "destroy", path),
              .update = requiredString(exports, "update", path),
              .updateSingleGroup =
                  requiredString(exports, "update_single_group", path),
              .serialize = requiredString(exports, "serialize", path),
              .merge = requiredString(exports, "merge", path),
              .mergeSingleGroup =
                  requiredString(exports, "merge_single_group", path),
              .finalize = requiredString(exports, "finalize", path),
              .toIntermediate = exports.count("to_intermediate")
                  ? requiredString(exports, "to_intermediate", path)
                  : "",
              .compact =
                  root["abi_version"].asInt() == kStateAccountingAbiVersion
                  ? requiredString(exports, "compact", path)
                  : "",
              .checkpoint =
                  checkpoint ? requiredString(exports, "checkpoint", path) : "",
              .restore =
                  checkpoint ? requiredString(exports, "restore", path) : "",
          },
      .arguments = std::move(arguments),
      .intermediateType = std::move(intermediate),
      .returnType = std::move(result),
      .orderSensitive = optionalBool(root, "order_sensitive", false),
      .ignoreDuplicates = optionalBool(root, "ignore_duplicates", false),
      .defaultNullBehavior = optionalBool(root, "default_null_behavior", true),
      .signature = std::move(signature),
      .initializeEntrypoint = std::move(initialize),
      .configKeys = std::move(configKeys),
      .rowApi = rowApi,
      .lambdaCount = lambdaCount,
  };
}

uint32_t readU32(
    std::string_view bytes,
    size_t& offset,
    size_t end,
    const std::filesystem::path& path) {
  uint32_t value = 0;
  for (int shift = 0; shift <= 28; shift += 7) {
    VELOX_USER_CHECK_LT(
        offset, end, "Truncated Wasm section in '{}'", path.string());
    const auto byte = static_cast<uint8_t>(bytes[offset++]);
    VELOX_USER_CHECK(
        shift != 28 || (byte & 0xf0) == 0,
        "Invalid Wasm section length in '{}'",
        path.string());
    value |= static_cast<uint32_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0)
      return value;
  }
  VELOX_USER_FAIL("Invalid Wasm section length in '{}'", path.string());
}

} // namespace

std::string scalarDispatchKey(const exec::FunctionSignature& signature) {
  std::unordered_map<std::string, std::string> variables;
  auto canonical = [&](auto&& self,
                       const exec::TypeSignature& type) -> std::string {
    const auto& name = type.baseName();
    std::string key = name;
    if (auto it = signature.variables().find(name);
        it != signature.variables().end()) {
      auto [entry, inserted] =
          variables.emplace(name, "v" + std::to_string(variables.size()));
      key = entry->second;
      const auto& variable = it->second;
      if (variable.isTypeParameter()) {
        key += fmt::format(
            "[type:{},{},{}]",
            variable.knownTypesOnly(),
            variable.comparableTypesOnly(),
            variable.orderableTypesOnly());
      } else {
        key += "[integer]";
      }
    }
    key += "(";
    for (const auto& child : type.parameters())
      key += self(self, child) + ",";
    return key + ")";
  };
  std::string key = signature.variableArity() ? "variadic:" : "fixed:";
  for (const auto& argument : signature.argumentTypes())
    key += canonical(canonical, argument) + ";";
  return key;
}

EmbeddedManifests loadEmbeddedManifests(
    const std::filesystem::path& inputPath) {
  std::error_code error;
  const auto path = std::filesystem::canonical(inputPath, error);
  VELOX_USER_CHECK(
      !error,
      "Cannot resolve Wasm UDF module '{}': {}",
      inputPath.string(),
      error.message());
  return loadEmbeddedManifests(path, WasmModule::read(path));
}

EmbeddedManifests loadEmbeddedManifests(
    const std::filesystem::path& path,
    std::string_view bytes) {
  VELOX_USER_CHECK(
      bytes.size() >= 8 &&
          bytes.compare(0, 8, std::string_view("\0asm\x01\0\0\0", 8)) == 0,
      "Invalid Wasm module header in '{}'",
      path.string());

  EmbeddedManifests manifests;
  std::unordered_set<std::string> scalarSignatures;
  std::unordered_set<std::string> aggregateSignatures;
  size_t metadataBytes = 0;
  size_t offset = 8;
  while (offset < bytes.size()) {
    const auto sectionId = static_cast<uint8_t>(bytes[offset++]);
    const auto sectionSize = readU32(bytes, offset, bytes.size(), path);
    VELOX_USER_CHECK_LE(
        sectionSize,
        bytes.size() - offset,
        "Truncated Wasm section in '{}'",
        path.string());
    const auto sectionEnd = offset + sectionSize;
    if (sectionId == 0) {
      const auto nameSize = readU32(bytes, offset, sectionEnd, path);
      VELOX_USER_CHECK_LE(
          nameSize,
          sectionEnd - offset,
          "Truncated Wasm custom section name in '{}'",
          path.string());
      const auto name = std::string_view(bytes.data() + offset, nameSize);
      offset += nameSize;
      if (name == kMetadataSection) {
        VELOX_USER_CHECK_LE(
            sectionEnd - offset,
            kMaxMetadataBytes - metadataBytes,
            "Wasm UDF metadata exceeds 1 MiB in '{}'",
            path.string());
        metadataBytes += sectionEnd - offset;
        while (offset < sectionEnd) {
          const auto end = bytes.find('\0', offset);
          VELOX_USER_CHECK(
              end != std::string::npos && end < sectionEnd,
              "Unterminated Wasm UDF metadata in '{}'",
              path.string());
          if (end == offset) {
            offset = end + 1;
            continue;
          }
          folly::dynamic root;
          try {
            root = folly::parseJson(
                std::string(bytes.substr(offset, end - offset)));
          } catch (const folly::json::parse_error& parseError) {
            VELOX_USER_FAIL(
                "Invalid Wasm UDF metadata in '{}': {}",
                path.string(),
                parseError.what());
          }
          VELOX_USER_CHECK(
              root.isObject() && root.count("kind") && root["kind"].isString(),
              "Wasm UDF metadata in '{}' is missing kind",
              path.string());
          if (root["kind"].asString() == "scalar") {
            auto declaration = parseScalar(root, path);
            declaration.name = exec::sanitizeName(declaration.name);
            auto signatureKey = declaration.name + ":" +
                scalarDispatchKey(*declaration.signature);
            VELOX_USER_CHECK(
                scalarSignatures.insert(signatureKey).second,
                "Duplicate Wasm scalar signature '{}' in '{}'",
                signatureKey,
                path.string());
            manifests.scalars.push_back(std::move(declaration));
          } else if (root["kind"].asString() == "aggregate") {
            auto declaration = parseAggregate(root, path);
            declaration.name = exec::sanitizeName(declaration.name);
            VELOX_USER_CHECK(
                aggregateSignatures
                    .insert(
                        declaration.name + ":" +
                        scalarDispatchKey(*declaration.signature))
                    .second,
                "Duplicate Wasm aggregate signature '{}' in '{}'",
                declaration.name,
                path.string());
            manifests.aggregates.push_back(std::move(declaration));
          } else {
            VELOX_USER_FAIL("Unsupported Wasm UDF kind in '{}'", path.string());
          }
          VELOX_USER_CHECK_LE(
              manifests.scalars.size() + manifests.aggregates.size(),
              kMaxFunctions,
              "Too many Wasm UDF declarations in '{}'",
              path.string());
          offset = end + 1;
        }
      }
    }
    offset = sectionEnd;
  }
  VELOX_USER_CHECK(
      !manifests.scalars.empty() || !manifests.aggregates.empty(),
      "Wasm UDF metadata is missing in '{}'",
      path.string());
  return manifests;
}

} // namespace facebook::velox::functions::wasm
