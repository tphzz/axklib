#include <filesystem>
#include <iostream>
#include <utility>

#include "schema/objects_v1.hpp"

#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"
#include "axklib/utf8.hpp"

namespace object_schema = axk::cli::schema::objects_v1;

namespace {

std::string label_status(axk::LabelStatus status) {
    switch (status) {
    case axk::LabelStatus::confirmed:
        return "confirmed";
    case axk::LabelStatus::navigation_aid:
        return "navigation-aid";
    case axk::LabelStatus::raw_identifier:
        return "raw-identifier";
    }
    return "raw-identifier";
}

std::string media_kind(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::fat12_floppy:
        return "fat12_floppy";
    case axk::MediaKind::iso9660:
        return "iso9660";
    case axk::MediaKind::standalone_object:
        return "standalone_object";
    case axk::MediaKind::sfs:
        return "sfs";
    }
    return "unknown";
}

int dump_file_media(const axk::MediaContainer &media) {
    const auto objects = media.objects();
    if (!objects) {
        std::cerr << axk::render_error(objects.error()) << '\n';
        return 1;
    }
    object_schema::ObjectsOutput output{
        .shape = object_schema::ContainerShape::media,
        .container_kind = media_kind(media.kind()),
        .objects = {},
    };
    for (const auto &object : *objects) {
        const auto structured = axk::structured_object_path(object);
        output.objects.push_back({
            .partition_index = std::nullopt,
            .sfs_id = std::nullopt,
            .key = object.key,
            .logical_path = object.logical_path,
            .scope_key = object.scope_key,
            .raw_group = object.raw_group,
            .raw_volume = object.raw_volume,
            .group_label = object.group_label.value,
            .group_label_status = label_status(object.group_label.status),
            .group_label_basis = object.group_label.basis,
            .volume_label = object.volume_label.value,
            .volume_label_status = label_status(object.volume_label.status),
            .volume_label_basis = object.volume_label.basis,
            .data_offset = object.data_offset,
            .size = object.size,
            .structured_path_utf8 = axk::text::path_to_utf8(structured.relative_path),
            .header = object.decoded.header,
            .decoded = object.decoded,
        });
    }
    const auto serialized = object_schema::serialize(output, false);
    if (!serialized) {
        std::cerr << axk::render_error(serialized.error()) << '\n';
        return 1;
    }
    std::cout << *serialized << '\n';
    return 0;
}

int dump_sfs(const std::filesystem::path &path) {
    const auto container = axk::open_image(path);
    if (!container) {
        std::cerr << axk::render_error(container.error()) << '\n';
        return 1;
    }
    object_schema::ObjectsOutput output{
        .shape = object_schema::ContainerShape::sfs,
        .container_kind = {},
        .objects = {},
    };
    for (const auto &partition : container->partitions()) {
        for (const auto &record : partition.records) {
            if (record.payload_kind != axk::PayloadKind::object)
                continue;
            const auto payload = container->read_record_data(partition.index, record.sfs_id, 64U * 1024U * 1024U);
            if (!payload) {
                std::cerr << axk::render_error(payload.error()) << '\n';
                return 1;
            }
            auto decoded = axk::decode_object(*payload);
            if (!decoded) {
                std::cerr << axk::render_error(decoded.error()) << '\n';
                return 1;
            }
            output.objects.push_back({
                .partition_index = partition.index.value,
                .sfs_id = record.sfs_id.value,
                .key = {},
                .logical_path = {},
                .scope_key = {},
                .raw_group = {},
                .raw_volume = {},
                .group_label = {},
                .group_label_status = {},
                .group_label_basis = {},
                .volume_label = {},
                .volume_label_status = {},
                .volume_label_basis = {},
                .data_offset = 0U,
                .size = 0U,
                .structured_path_utf8 = {},
                .header = decoded->header,
                .decoded = std::move(*decoded),
            });
        }
    }
    const auto serialized = object_schema::serialize(output, false);
    if (!serialized) {
        std::cerr << axk::render_error(serialized.error()) << '\n';
        return 1;
    }
    std::cout << *serialized << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path path{argv[1]};
    const auto media = axk::open_media(path);
    if (!media) {
        std::cerr << axk::render_error(media.error()) << '\n';
        return 1;
    }
    return media->kind() == axk::MediaKind::sfs ? dump_sfs(path) : dump_file_media(*media);
}
