#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"

namespace axk::cli::schema::info_v1 {

inline constexpr std::string_view schema_version{"compat-v1"};

struct NodeOutput {
  std::string node_id;
  std::string node_type;
  std::string display_name;
  std::string object_key;
  std::string object_type;
  std::optional<std::uint64_t> count;
  std::vector<std::string> details;
  std::string quality;
  std::string basis;
  std::string notes;
  std::string selector_path;
  std::vector<NodeOutput> children;
};

struct IssueOutput {
  std::string code;
  std::string severity;
  std::string message;
  std::string source_path_utf8;
  std::string sampler_path;
  std::string object_key;
};

struct TreeOutput {
  std::string source_path_utf8;
  std::string container_kind;
  std::string detected_format;
  std::vector<NodeOutput> roots;
  std::vector<IssueOutput> issues;
};

struct LoadErrorOutput {
  std::string path_utf8;
  std::uint64_t error_code{};
  std::string message;
  std::string original_exception;
};

struct InfoOutput {
  std::vector<TreeOutput> trees;
  std::vector<LoadErrorOutput> load_errors;
};

Result<std::string> serialize(const InfoOutput &output);

} // namespace axk::cli::schema::info_v1
