#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "axklib/io.hpp"

namespace axk::filesystems {

struct Entry {
    std::string id;
    std::string path;
    bool directory{};
    std::uint32_t attributes{};
    std::shared_ptr<const RandomAccessReader> reader;
};

struct Root {
    std::string id;
    std::string kind;
    std::vector<Entry> entries;
};

} // namespace axk::filesystems
