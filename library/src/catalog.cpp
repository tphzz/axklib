#include "axklib/catalog.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace axk {
namespace {

bool is_category(std::string_view name) {
  static const std::unordered_set<std::string_view> categories{"SMPL", "SBNK", "SBAC",
                                                               "PROG", "SEQU", "PRF3"};
  return categories.contains(name);
}

std::string object_key(PartitionIndex partition, SfsId sfs_id) {
  return std::format("p{}:sfs{}", partition.value, sfs_id.value);
}

} // namespace

Result<ObjectCatalog> build_object_catalog(const Container &container,
                                           std::size_t maximum_object_bytes,
                                           const CancellationToken &cancellation) {
  ObjectCatalog result;
  for (const auto &partition : container.partitions()) {
    if (const auto check = cancellation.check(); !check) {
      return std::unexpected{check.error()};
    }

    // Build placement candidates directly so directory ambiguity cannot be
    // hidden by a last-write-wins map.
    struct Candidate {
      SfsId target;
      ObjectPlacement placement;
    };
    std::vector<Candidate> candidates;
    std::unordered_map<std::uint32_t, const IndexRecord *> directories;
    for (const auto &record : partition.records) {
      if (record.directory_id)
        directories.emplace(record.directory_id->value, &record);
    }
    const IndexRecord *root{};
    for (const auto &[id, directory] : directories) {
      if (directory->parent_directory_id && directory->parent_directory_id->value == id) {
        root = directory;
        break;
      }
    }
    if (root != nullptr && root->directory_id) {
      for (const auto &volume_entry : root->directory_entries) {
        const auto volume_found = directories.find(volume_entry.link_id.value);
        if (volume_entry.name == "." || volume_entry.name == ".." ||
            volume_found == directories.end()) {
          continue;
        }
        const auto *volume = volume_found->second;
        if (!volume->parent_directory_id ||
            volume->parent_directory_id->value != root->directory_id->value) {
          continue;
        }
        for (const auto &category_entry : volume->directory_entries) {
          if (!is_category(category_entry.name))
            continue;
          const auto category_found = directories.find(category_entry.link_id.value);
          if (category_found == directories.end())
            continue;
          const auto *category = category_found->second;
          if (!category->parent_directory_id || !volume->directory_id ||
              category->parent_directory_id->value != volume->directory_id->value) {
            continue;
          }
          for (const auto &entry : category->directory_entries) {
            if (entry.name == "." || entry.name == "..")
              continue;
            candidates.push_back({
                SfsId{entry.link_id.value},
                {partition.index,
                 partition.name,
                 volume->sfs_id,
                 volume_entry.name,
                 category_entry.name,
                 entry.name,
                 {}},
            });
          }
        }
      }
    }

    for (const auto &record : partition.records) {
      if (record.payload_kind != PayloadKind::object)
        continue;
      const auto bytes = container.read_record_data(partition.index, record.sfs_id,
                                                    maximum_object_bytes, cancellation);
      if (!bytes)
        return std::unexpected{bytes.error()};
      const auto decoded = decode_object(*bytes);
      if (!decoded) {
        result.issues.push_back({
            "CATALOG_OBJECT_DECODE_FAILED",
            decoded.error().message,
            partition.index,
            record.sfs_id,
        });
        continue;
      }
      std::vector<ObjectPlacement> matching;
      for (const auto &candidate : candidates) {
        if (candidate.target.value == record.sfs_id.value)
          matching.push_back(candidate.placement);
      }
      std::optional<ObjectPlacement> placement;
      if (matching.size() == 1) {
        placement = std::move(matching.front());
      } else {
        result.issues.push_back({
            matching.empty() ? "CATALOG_OBJECT_PLACEMENT_MISSING"
                             : "CATALOG_OBJECT_PLACEMENT_AMBIGUOUS",
            matching.empty() ? "object has no exact volume/category directory placement"
                             : "object has multiple volume/category directory placements",
            partition.index,
            record.sfs_id,
        });
      }
      result.objects.push_back({
          object_key(partition.index, record.sfs_id),
          partition.index,
          record.sfs_id,
          std::format("partition:{}", partition.index.value),
          std::move(*decoded),
          std::move(placement),
          std::move(*bytes),
      });
    }
  }
  std::ranges::sort(result.objects, {}, [](const ObjectSnapshot &item) {
    return std::pair{item.partition.value, item.sfs_id.value};
  });
  return result;
}

} // namespace axk
