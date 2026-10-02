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

#include "velox/functions/java/Manifest.h"

#include <fstream>
#include <sstream>

#include <folly/json.h>

#include "velox/common/base/Exceptions.h"

namespace facebook::velox::functions::java {
namespace {

constexpr uint32_t kAbiVersion = 1;
constexpr uint32_t kMaxTypeDepth = 32;

const folly::dynamic& required(
    const folly::dynamic& object,
    const char* field,
    const std::filesystem::path& path) {
  VELOX_USER_CHECK(
      object.isObject() && object.count(field) != 0,
      "Java UDF manifest '{}' is missing required field '{}'",
      path.string(),
      field);
  return object[field];
}

std::string requiredString(
    const folly::dynamic& object,
    const char* field,
    const std::filesystem::path& path) {
  const auto& value = required(object, field, path);
  VELOX_USER_CHECK(
      value.isString() && !value.asString().empty(),
      "Java UDF '{}' must be a non-empty string",
      field);
  return value.asString();
}

bool optionalBool(
    const folly::dynamic& object,
    const char* field,
    bool defaultValue) {
  if (object.count(field) == 0) {
    return defaultValue;
  }
  VELOX_USER_CHECK(
      object[field].isBool(), "Java UDF '{}' must be a boolean", field);
  return object[field].asBool();
}

std::filesystem::path canonicalFile(
    const std::filesystem::path& input,
    std::string_view description) {
  std::error_code error;
  auto path = std::filesystem::canonical(input, error);
  VELOX_USER_CHECK(
      !error,
      "Cannot resolve {} '{}': {}",
      description,
      input.string(),
      error.message());
  VELOX_USER_CHECK(
      std::filesystem::is_regular_file(path),
      "{} '{}' is not a regular file",
      description,
      path.string());
  return path;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  VELOX_USER_CHECK(input, "Cannot open Java UDF manifest '{}'", path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

folly::dynamic loadRoot(const std::filesystem::path& path) {
  try {
    return folly::parseJson(readFile(path));
  } catch (const folly::json::parse_error& error) {
    VELOX_USER_FAIL(
        "Invalid JSON in Java UDF manifest '{}': {}",
        path.string(),
        error.what());
  }
}

void validateVersionAndKind(
    const folly::dynamic& root,
    const std::filesystem::path& path,
    std::string_view expectedKind) {
  VELOX_USER_CHECK(root.isObject(), "Java UDF manifest root must be an object");
  const auto& version = required(root, "abi_version", path);
  const auto& kind = required(root, "kind", path);
  VELOX_USER_CHECK(version.isInt(), "Java UDF abi_version must be an integer");
  VELOX_USER_CHECK_EQ(
      version.asInt(),
      kAbiVersion,
      "Unsupported Java UDF ABI version in '{}'",
      path.string());
  VELOX_USER_CHECK(
      kind.isString() && kind.asString() == expectedKind,
      "Expected a '{}' Java UDF manifest",
      expectedKind);
}

int64_t requiredInt(
    const folly::dynamic& object,
    const char* field,
    const std::filesystem::path& path) {
  const auto& value = required(object, field, path);
  VELOX_USER_CHECK(value.isInt(), "Java UDF '{}' must be an integer", field);
  return value.asInt();
}

ManifestType parseManifestType(
    const folly::dynamic& value,
    const std::filesystem::path& path,
    uint32_t depth) {
  VELOX_USER_CHECK_LE(
      depth, kMaxTypeDepth, "Java UDF type nesting exceeds {}", kMaxTypeDepth);
  VELOX_USER_CHECK(
      value.isObject(), "Java UDF type declaration must be an object");
  const auto name = requiredString(value, "type", path);
  const auto& nullable = required(value, "nullable", path);
  VELOX_USER_CHECK(nullable.isBool(), "Java UDF nullable must be a boolean");

  TypePtr type;
  if (name == "boolean") {
    type = BOOLEAN();
  } else if (name == "tinyint") {
    type = TINYINT();
  } else if (name == "smallint") {
    type = SMALLINT();
  } else if (name == "integer") {
    type = INTEGER();
  } else if (name == "bigint") {
    type = BIGINT();
  } else if (name == "real") {
    type = REAL();
  } else if (name == "double") {
    type = DOUBLE();
  } else if (name == "varchar") {
    type = VARCHAR();
  } else if (name == "varbinary") {
    type = VARBINARY();
  } else if (name == "date") {
    type = DATE();
  } else if (name == "timestamp") {
    type = TIMESTAMP();
  } else if (name == "decimal") {
    const auto precision = requiredInt(value, "precision", path);
    const auto scale = requiredInt(value, "scale", path);
    VELOX_USER_CHECK(
        precision >= 1 && precision <= 38,
        "Java UDF decimal precision must be between 1 and 38");
    VELOX_USER_CHECK(
        scale >= 0 && scale <= precision,
        "Java UDF decimal scale must be between 0 and precision");
    type = DECIMAL(precision, scale);
  } else if (name == "array") {
    type = ARRAY(
        parseManifestType(required(value, "element", path), path, depth + 1)
            .type);
  } else if (name == "map") {
    auto key = parseManifestType(required(value, "key", path), path, depth + 1);
    auto mapValue =
        parseManifestType(required(value, "value", path), path, depth + 1);
    VELOX_USER_CHECK(!key.nullable, "Java UDF map keys must be non-nullable");
    type = MAP(std::move(key.type), std::move(mapValue.type));
  } else if (name == "row") {
    const auto& fields = required(value, "fields", path);
    VELOX_USER_CHECK(fields.isArray(), "Java UDF row fields must be an array");
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    names.reserve(fields.size());
    types.reserve(fields.size());
    std::unordered_set<std::string> uniqueNames;
    for (const auto& field : fields) {
      VELOX_USER_CHECK(
          field.isObject(), "Java UDF row field must be an object");
      auto fieldName = requiredString(field, "name", path);
      VELOX_USER_CHECK(
          uniqueNames.insert(fieldName).second,
          "Java UDF row contains duplicate field '{}'",
          fieldName);
      names.push_back(std::move(fieldName));
      types.push_back(
          parseManifestType(required(field, "type", path), path, depth + 1)
              .type);
    }
    type = ROW(std::move(names), std::move(types));
  } else {
    VELOX_USER_FAIL(
        "Unsupported type '{}' in Java UDF manifest '{}'", name, path.string());
  }
  return {std::move(type), nullable.asBool()};
}

std::vector<ManifestType> parseArguments(
    const folly::dynamic& root,
    const std::filesystem::path& path) {
  const auto& arguments = required(root, "arguments", path);
  VELOX_USER_CHECK(arguments.isArray(), "Java UDF arguments must be an array");
  std::vector<ManifestType> parsed;
  parsed.reserve(arguments.size());
  for (const auto& argument : arguments) {
    parsed.push_back(parseManifestType(argument, path, 0));
  }
  return parsed;
}

std::filesystem::path parseJarPath(
    const folly::dynamic& root,
    const std::filesystem::path& manifestPath) {
  auto jarPath =
      std::filesystem::path(requiredString(root, "jar", manifestPath));
  if (jarPath.is_relative()) {
    jarPath = manifestPath.parent_path() / jarPath;
  }
  return canonicalFile(jarPath, "Java UDF JAR");
}

std::string quoteRowName(std::string_view name) {
  std::string quoted = "\"";
  for (char c : name) {
    if (c == '"') {
      quoted += "\"\"";
    } else {
      quoted += c;
    }
  }
  return quoted + "\"";
}

} // namespace

ScalarManifest loadScalarManifest(const std::filesystem::path& inputPath) {
  const auto path = canonicalFile(inputPath, "Java UDF manifest");
  auto root = loadRoot(path);
  validateVersionAndKind(root, path, "scalar");
  return {
      .abiVersion = kAbiVersion,
      .manifestPath = path,
      .jarPath = parseJarPath(root, path),
      .name = requiredString(root, "name", path),
      .implementationClass = requiredString(root, "class", path),
      .arguments = parseArguments(root, path),
      .returnType = parseManifestType(required(root, "return", path), path, 0),
      .deterministic = optionalBool(root, "deterministic", true),
      .defaultNullBehavior = optionalBool(root, "default_null_behavior", true),
  };
}

AggregateManifest loadAggregateManifest(
    const std::filesystem::path& inputPath) {
  const auto path = canonicalFile(inputPath, "Java UDF manifest");
  auto root = loadRoot(path);
  validateVersionAndKind(root, path, "aggregate");
  auto intermediate =
      parseManifestType(required(root, "intermediate", path), path, 0);
  VELOX_USER_CHECK(
      intermediate.type->kind() == TypeKind::VARBINARY,
      "Java UDAF intermediate type must be varbinary");
  return {
      .abiVersion = kAbiVersion,
      .manifestPath = path,
      .jarPath = parseJarPath(root, path),
      .name = requiredString(root, "name", path),
      .implementationClass = requiredString(root, "class", path),
      .arguments = parseArguments(root, path),
      .intermediateType = std::move(intermediate),
      .returnType = parseManifestType(required(root, "return", path), path, 0),
      .orderSensitive = optionalBool(root, "order_sensitive", false),
      .ignoreDuplicates = optionalBool(root, "ignore_duplicates", false),
      .defaultNullBehavior = optionalBool(root, "default_null_behavior", true),
  };
}

std::string functionSignatureType(const TypePtr& type) {
  if (type->isDate()) {
    return "date";
  }
  if (type->isDecimal()) {
    const auto [precision, scale] = getDecimalPrecisionScale(*type);
    return fmt::format("decimal({},{})", precision, scale);
  }
  switch (type->kind()) {
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
      return fmt::format("array({})", functionSignatureType(type->childAt(0)));
    case TypeKind::MAP:
      return fmt::format(
          "map({},{})",
          functionSignatureType(type->childAt(0)),
          functionSignatureType(type->childAt(1)));
    case TypeKind::ROW: {
      const auto& row = type->asRow();
      std::string signature = "row(";
      for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) {
          signature += ',';
        }
        signature += quoteRowName(row.nameOf(i));
        signature += ' ';
        signature += functionSignatureType(row.childAt(i));
      }
      return signature + ')';
    }
    default:
      VELOX_UNREACHABLE(
          "Unsupported Java UDF signature type {}", type->toString());
  }
}

} // namespace facebook::velox::functions::java
