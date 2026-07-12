#include "axklib/error.hpp"

#include <string_view>

namespace axk {
namespace {

std::string_view category_name(ErrorCategory category) {
  switch (category) {
  case ErrorCategory::io:
    return "io";
  case ErrorCategory::container:
    return "container";
  case ErrorCategory::allocation:
    return "allocation";
  case ErrorCategory::object:
    return "object";
  case ErrorCategory::relationship:
    return "relationship";
  case ErrorCategory::audio:
    return "audio";
  case ErrorCategory::manifest:
    return "manifest";
  case ErrorCategory::transaction:
    return "transaction";
  case ErrorCategory::unsupported:
    return "unsupported";
  case ErrorCategory::cancelled:
    return "cancelled";
  case ErrorCategory::internal:
    return "internal";
  }
  return "internal";
}

void append_label(std::string &target, std::string_view label, std::string_view value) {
  target += target.ends_with('[') ? "" : ", ";
  target += label;
  target += '=';
  target += value;
}

} // namespace

std::string render_error(const Error &error, bool include_trace) {
  std::string result{category_name(error.category)};
  result += '[' + std::to_string(static_cast<std::uint32_t>(error.code)) + "]: ";
  result += error.message;

  const auto &context = error.context;
  const bool has_context = context.partition_index || context.volume_name || context.object_type ||
                           context.object_name ||
                           (include_trace && (context.source_path || context.raw_offset));
  if (!has_context) {
    return result;
  }
  result += " [";
  if (context.partition_index) {
    append_label(result, "partition", std::to_string(*context.partition_index));
  }
  if (context.volume_name) {
    append_label(result, "volume", *context.volume_name);
  }
  if (context.object_type) {
    append_label(result, "object_type", *context.object_type);
  }
  if (context.object_name) {
    append_label(result, "object", *context.object_name);
  }
  if (include_trace && context.source_path) {
    append_label(result, "source", *context.source_path);
  }
  if (include_trace && context.raw_offset) {
    append_label(result, "offset", std::to_string(*context.raw_offset));
  }
  result += ']';
  return result;
}

} // namespace axk
