#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "axklib/application/contracts.hpp"
#include "axklib/application/image_documents.hpp"
#include "axklib/native/document.hpp"

namespace axk::app {

struct NativeDocumentSnapshot {
    ImageDocumentRef reference;
    std::uint64_t revision{};
    std::shared_ptr<const native::Document> document;
    // Retains source admission, but never the workspace's access mutex.
    std::function<Result<void>(const CancellationToken &)> verify_current;
    std::shared_ptr<const std::unordered_map<std::string, std::string>> native_keys_by_id{};
};

} // namespace axk::app
