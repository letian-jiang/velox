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

#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <folly/json.h>

#include "velox/common/base/Exceptions.h"
#include "velox/functions/wasm/Abi.h"
#include "velox/type/fbhive/HiveTypeParser.h"

namespace facebook::velox::functions::wasm {
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

TypePtr parseTypeName(
    const std::string& name,
    const std::filesystem::path& path) {
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
  VELOX_USER_CHECK_EQ(
      version.asInt(),
      kAbiVersion,
      "Unsupported Wasm UDF ABI version in '{}'",
      path.string());
  VELOX_USER_CHECK(
      actualKind.isString() && actualKind.asString() == kind,
      "Expected a '{}' Wasm UDF declaration in '{}'",
      kind,
      path.string());
}

ScalarManifest parseScalar(
    const folly::dynamic& root,
    const std::filesystem::path& path) {
  validateVersionAndKind(root, path, "scalar");
  return {
      .abiVersion = kAbiVersion,
      .wasmPath = path,
      .name = requiredString(root, "name", path),
      .entrypoint = requiredString(root, "entrypoint", path),
      .arguments = parseArguments(root, path),
      .returnType = parseManifestType(required(root, "return", path), path),
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
  auto intermediate =
      parseManifestType(required(root, "intermediate", path), path);
  VELOX_USER_CHECK(
      intermediate.type->kind() == TypeKind::VARBINARY,
      "Wasm UDAF intermediate type must be varbinary");
  return {
      .abiVersion = kAbiVersion,
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
          },
      .arguments = parseArguments(root, path),
      .intermediateType = std::move(intermediate),
      .returnType = parseManifestType(required(root, "return", path), path),
      .orderSensitive = optionalBool(root, "order_sensitive", false),
      .ignoreDuplicates = optionalBool(root, "ignore_duplicates", false),
      .defaultNullBehavior = optionalBool(root, "default_null_behavior", true),
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

std::string readWasm(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  VELOX_USER_CHECK(input, "Cannot open Wasm UDF module '{}'", path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  VELOX_USER_CHECK(
      !input.bad(), "Cannot read Wasm UDF module '{}'", path.string());
  return contents.str();
}

} // namespace

EmbeddedManifests loadEmbeddedManifests(
    const std::filesystem::path& inputPath) {
  std::error_code error;
  const auto path = std::filesystem::canonical(inputPath, error);
  VELOX_USER_CHECK(
      !error,
      "Cannot resolve Wasm UDF module '{}': {}",
      inputPath.string(),
      error.message());
  const auto bytes = readWasm(path);
  VELOX_USER_CHECK(
      bytes.size() >= 8 &&
          bytes.compare(0, 8, std::string_view("\0asm\x01\0\0\0", 8)) == 0,
      "Invalid Wasm module header in '{}'",
      path.string());

  EmbeddedManifests manifests;
  std::unordered_set<std::string> scalarSignatures;
  std::unordered_set<std::string> aggregateNames;
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
            root = folly::parseJson(bytes.substr(offset, end - offset));
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
            auto signatureKey = declaration.name + "(";
            for (const auto& argument : declaration.arguments) {
              signatureKey += argument.type->toString() + ",";
            }
            signatureKey += ")";
            VELOX_USER_CHECK(
                scalarSignatures.insert(signatureKey).second,
                "Duplicate Wasm scalar signature '{}' in '{}'",
                signatureKey,
                path.string());
            manifests.scalars.push_back(std::move(declaration));
          } else if (root["kind"].asString() == "aggregate") {
            auto declaration = parseAggregate(root, path);
            VELOX_USER_CHECK(
                aggregateNames.insert(declaration.name).second,
                "Duplicate Wasm aggregate function '{}' in '{}'",
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
