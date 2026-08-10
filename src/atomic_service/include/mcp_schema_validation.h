#pragma once

/**
 * @file mcp_schema_validation.h
 * @brief Private MCP schema and bounded policy-value validation helpers.
 *
 * This header is private to Atomic Service and is not part of the installed API.
 */

#include "master_agent/atomic_service/atomic_service.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace master_agent::atomic_service {
namespace {

bool jsonMatchesType(const nlohmann::json& value,
                     const std::string& type) {
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    return false;
}

bool safeSchemaIdentifier(const std::string& value,
                          std::size_t max_bytes) {
    return !value.empty() && value.size() <= max_bytes &&
           std::all_of(
               value.begin(), value.end(),
               [](unsigned char byte) {
                   return std::isalnum(byte) != 0 ||
                          byte == '_' || byte == '-' ||
                          byte == '.';
               });
}

bool safeSchemaText(const std::string& value,
                    std::size_t max_bytes,
                    bool allow_empty = true) {
    if ((!allow_empty && value.empty()) ||
        value.size() > max_bytes) {
        return false;
    }
    return std::none_of(
        value.begin(), value.end(), [](unsigned char byte) {
            return byte == 0 ||
                   (byte < 0x20U && byte != '\t' &&
                    byte != '\n' && byte != '\r');
        });
}

// Freeze the JSON Schema subset actually enforced by validateArguments.
// Rejecting unsupported keywords prevents a catalog from promising
// constraints that the executor silently ignores.
Status validateObjectSchemaSubset(
    const nlohmann::json& schema) noexcept {
    try {
        if (!schema.is_object() || schema.size() > 8 ||
            !schema.contains("type") ||
            !schema.at("type").is_string() ||
            schema.at("type").get<std::string>() != "object" ||
            schema.dump().size() > 64U * 1024U) {
            return Status::Error(
                "atomic_service", "ATOMIC_SCHEMA_INVALID",
                "schema must be a bounded object schema");
        }
        static const std::set<std::string> root_keys{
            "type", "properties", "required",
            "additionalProperties", "description", "title"};
        for (const auto& item : schema.items()) {
            if (root_keys.count(item.key()) == 0) {
                return Status::Error(
                    "atomic_service", "ATOMIC_SCHEMA_INVALID",
                    "schema contains an unsupported root keyword");
            }
        }
        if (schema.contains("description") &&
            (!schema.at("description").is_string() ||
             !safeSchemaText(
                 schema.at("description").get<std::string>(),
                 8192))) {
            return Status::Error(
                "atomic_service", "ATOMIC_SCHEMA_INVALID",
                "schema description is invalid");
        }
        if (schema.contains("title") &&
            (!schema.at("title").is_string() ||
             !safeSchemaText(
                 schema.at("title").get<std::string>(),
                 512))) {
            return Status::Error(
                "atomic_service", "ATOMIC_SCHEMA_INVALID",
                "schema title is invalid");
        }
        if (schema.contains("additionalProperties") &&
            !schema.at("additionalProperties").is_boolean()) {
            return Status::Error(
                "atomic_service", "ATOMIC_SCHEMA_INVALID",
                "additionalProperties must be boolean");
        }
        const nlohmann::json empty_properties =
            nlohmann::json::object();
        const auto& properties =
            schema.contains("properties")
                ? schema.at("properties")
                : empty_properties;
        if (!properties.is_object() ||
            properties.size() > 256) {
            return Status::Error(
                "atomic_service", "ATOMIC_SCHEMA_INVALID",
                "properties must be a bounded object");
        }
        static const std::set<std::string> property_keys{
            "type", "enum", "description", "title"};
        static const std::set<std::string> scalar_types{
            "string", "boolean", "integer", "number"};
        for (const auto& item : properties.items()) {
            if (!safeSchemaIdentifier(item.key(), 128) ||
                !item.value().is_object() ||
                !item.value().contains("type") ||
                !item.value().at("type").is_string()) {
                return Status::Error(
                    "atomic_service", "ATOMIC_SCHEMA_INVALID",
                    "property name and schema are invalid");
            }
            for (const auto& keyword : item.value().items()) {
                if (property_keys.count(keyword.key()) == 0) {
                    return Status::Error(
                        "atomic_service", "ATOMIC_SCHEMA_INVALID",
                        "property contains an unsupported keyword");
                }
            }
            const auto type =
                item.value().at("type").get<std::string>();
            if (scalar_types.count(type) == 0) {
                return Status::Error(
                    "atomic_service", "ATOMIC_SCHEMA_INVALID",
                    "only the frozen scalar property types are supported");
            }
            for (const auto* text_key : {"description", "title"}) {
                if (item.value().contains(text_key) &&
                    (!item.value().at(text_key).is_string() ||
                     !safeSchemaText(
                         item.value().at(text_key)
                             .get<std::string>(),
                         text_key == std::string("description")
                             ? 8192
                             : 512))) {
                    return Status::Error(
                        "atomic_service", "ATOMIC_SCHEMA_INVALID",
                        "property text metadata is invalid");
                }
            }
            if (item.value().contains("enum")) {
                const auto& values =
                    item.value().at("enum");
                if (!values.is_array() || values.empty() ||
                    values.size() > 128) {
                    return Status::Error(
                        "atomic_service", "ATOMIC_SCHEMA_INVALID",
                        "enum must be a bounded non-empty array");
                }
                std::set<std::string> canonical_values;
                for (const auto& value : values) {
                    if (!jsonMatchesType(value, type) ||
                        (value.is_string() &&
                         !safeSchemaText(
                             value.get<std::string>(),
                             16U * 1024U)) ||
                        !canonical_values.insert(value.dump()).second) {
                        return Status::Error(
                            "atomic_service", "ATOMIC_SCHEMA_INVALID",
                            "enum values must be unique and match type");
                    }
                }
            }
        }
        if (schema.contains("required")) {
            const auto& required = schema.at("required");
            if (!required.is_array() ||
                required.size() > properties.size()) {
                return Status::Error(
                    "atomic_service", "ATOMIC_SCHEMA_INVALID",
                    "required must be a bounded array");
            }
            std::set<std::string> unique_required;
            for (const auto& value : required) {
                if (!value.is_string()) {
                    return Status::Error(
                        "atomic_service", "ATOMIC_SCHEMA_INVALID",
                        "required entries must be strings");
                }
                const auto name = value.get<std::string>();
                if (!properties.contains(name) ||
                    !unique_required.insert(name).second) {
                    return Status::Error(
                        "atomic_service", "ATOMIC_SCHEMA_INVALID",
                        "required must uniquely reference properties");
                }
            }
        }
        return Status::Ok();
    } catch (...) {
        return Status::Error(
            "atomic_service", "ATOMIC_SCHEMA_INVALID",
            "schema validation failed safely");
    }
}

bool validBoundedUniquePolicyValues(
    const std::vector<std::string>& values,
    std::size_t max_count = 64) {
    if (values.size() > max_count) return false;
    std::set<std::string> unique;
    return std::all_of(
        values.begin(), values.end(),
        [&unique](const std::string& value) {
            return safeSchemaIdentifier(value, 256) &&
                   unique.insert(value).second;
        });
}

}  // namespace
}  // namespace master_agent::atomic_service

