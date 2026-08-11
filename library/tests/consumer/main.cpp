#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "axklib/sdk.hpp"
#include "axklib/sdk/version.hpp"

int main() {
    axk::result<int> value{42};
    const auto build = axk::sdk_build_info();
    if (axk::sdk_version() != std::string{axk::version_string} || build.source_identity == nullptr ||
        std::string{build.package_basename}.rfind("axklib-", 0) != 0 || !value || *value != 42)
        return 1;

    axk::operation_context context;
    auto image = axk::image::open(AXK_TEST_FIXTURE, context);
    if (!image)
        return 2;
    auto validation = image->validation(context);
    auto relationships = image->relationships(0U, 64U, context);
    auto objects = image->objects(0U, 64U, context);
    if (!validation || !relationships || validation->object_count == 0U ||
        relationships->total_count != validation->relationship_count || !objects)
        return 3;

    const auto root = std::filesystem::temp_directory_path() / "axklib-installed-consumer";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    const auto manifest = root / "build.json";
    const auto output = root / "output.hds";
    const auto imported = root / "imported.hds";
    const auto package_stem = root / "waveform";
    const auto waveform = std::find_if(objects->items.begin(), objects->items.end(),
                                       [](const axk::object_info &item) { return item.type == "SMPL"; });
    if (waveform == objects->items.end())
        return 4;

    axk::package_root_selector selector;
    selector.kind = axk::package_root_kind::wave_data;
    selector.partition_index = waveform->partition_index;
    selector.group_name = waveform->partition_name;
    selector.volume_name = waveform->volume_name;
    selector.object_name = waveform->name;
    selector.object_key = waveform->key;
    auto exported =
        axk::portable_package::export_from(AXK_TEST_FIXTURE, {selector}, package_stem.string(), {}, context);
    if (!exported || exported->package_kind != "smpl" ||
        std::filesystem::path{exported->output_path}.extension() != ".axksmpl")
        return 5;
    auto package = axk::portable_package::open(exported->output_path, context);
    if (!package || !package->verify(context))
        return 6;
    auto package_summary = package->summary();
    if (!package_summary || package_summary->package_id != exported->package_id || package_summary->object_count != 1U)
        return 7;

    std::ofstream{manifest}
        << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Volume","waveforms":[],"samples":[]}]}]})";
    auto plan = axk::build_plan::from_manifest(manifest.string(), context);
    if (!plan || !plan->apply(output.string(), {}, context) || !std::filesystem::exists(output))
        return 8;

    axk::package_import_request import_request;
    axk::package_root_destination destination;
    destination.package_index = 0U;
    destination.root_index = 0U;
    destination.partition_index = 0U;
    destination.volume_name = "Volume";
    import_request.root_destinations.push_back(std::move(destination));
    auto import_plan =
        axk::package_import_plan::create(output.string(), {exported->output_path}, import_request, context);
    if (!import_plan)
        return 9;
    auto import_summary = import_plan->summary();
    auto actions = import_plan->actions();
    if (!import_summary || !import_summary->valid || import_summary->object_count != 1U || !actions ||
        actions->size() != 1U)
        return 10;
    auto applied = import_plan->apply(imported.string(), {}, context);
    if (!applied || !applied->applied || !std::filesystem::exists(imported))
        return 11;
    auto reopened = axk::image::open(imported.string(), context);
    if (!reopened)
        return 12;
    auto imported_objects = reopened->objects(0U, 64U, context);
    if (!imported_objects ||
        std::none_of(imported_objects->items.begin(), imported_objects->items.end(), [&](const axk::object_info &item) {
            return item.type == "SMPL" && item.name == waveform->name && item.volume_name == "Volume";
        }))
        return 13;

    std::filesystem::remove_all(root, error);
    return 0;
}
