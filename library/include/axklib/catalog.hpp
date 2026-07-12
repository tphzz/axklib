#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"

namespace axk {

struct ObjectPlacement {
  PartitionIndex partition;
  std::string partition_name;
  SfsId volume_directory;
  std::string volume_name;
  std::string category_name;
  std::string entry_name;
  std::string container_directory;
};

struct ObjectSnapshot {
  std::string key;
  PartitionIndex partition;
  SfsId sfs_id;
  std::string scope_key;
  DecodedObject object;
  std::optional<ObjectPlacement> placement;
};

struct CatalogIssue {
  std::string code;
  std::string message;
  PartitionIndex partition;
  std::optional<SfsId> sfs_id;
};

struct ObjectCatalog {
  std::vector<ObjectSnapshot> objects;
  std::vector<CatalogIssue> issues;
};

AXK_API Result<ObjectCatalog> build_object_catalog(const Container &container,
                                                   std::size_t maximum_object_bytes = 64U * 1024U *
                                                                                      1024U,
                                                   const CancellationToken &cancellation = {});

} // namespace axk
