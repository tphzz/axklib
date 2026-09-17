#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/filesystem_edit_operations.hpp"
#include "axklib/application/filesystem_export.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

template <typename T> T require(axk::app::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

Json io_counts() {
    Json result = Json::object();
#if defined(__linux__)
    std::ifstream input{"/proc/self/io"};
    std::string key;
    std::uint64_t value{};
    while (input >> key >> value) {
        key.pop_back();
        result[key] = value;
    }
#endif
    return result;
}

struct Progress final : axk::ProgressSink {
    Clock::time_point started{Clock::now()};
    Json events = Json::array();
    void report(const axk::Progress &event) noexcept override {
        events.push_back({{"label", event.label},
                          {"elapsedMs", std::chrono::duration<double, std::milli>(Clock::now() - started).count()}});
    }
};
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: axk_files_edit_benchmark SOURCE_IMAGE NEW_SCRATCH_DIRECTORY\n";
        return 2;
    }
    try {
        const std::filesystem::path source{argv[1]}, scratch{argv[2]};
        if (!std::filesystem::create_directory(scratch))
            throw std::runtime_error("Scratch directory must not exist");
        std::filesystem::copy_file(source, scratch / "image.hda");
        auto sandbox = require(axk::app::Sandbox::create({{"bench", "Benchmark", scratch, true}}));
        axk::app::PathReservationCoordinator reservations;
        axk::app::ImageSessionManager images{sandbox, 32U, 500U, std::chrono::minutes{15}, Clock::now, &reservations};
        axk::app::AlterationJournalStore journals{scratch / "journals"};
        Json results{{"source", source.string()},
                     {"sourceBytes", std::filesystem::file_size(source)},
                     {"operations", Json::array()}};
        auto measure = [&](const std::string &name, auto operation) {
            const auto before = io_counts();
            Progress progress;
            operation(progress);
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - progress.started).count();
            auto after = io_counts();
            for (auto &[key, value] : after.items())
                value = value.get<std::uint64_t>() - before.at(key).get<std::uint64_t>();
            results["operations"].push_back(
                {{"name", name}, {"elapsedMs", elapsed}, {"io", after}, {"progress", progress.events}});
            std::cerr << name << ": " << elapsed << " ms\n";
        };
        axk::app::ImageSessionSummary session;
        measure("open", [&](Progress &progress) {
            session = require(images.open({"bench", "image.hda"}, "bench", {}, &progress));
        });
        auto edit = [&](std::vector<axk::FilesystemEdit> edits, Progress &progress) {
            session =
                require(axk::app::apply_filesystem_edits(images, journals, session.image_id, "bench", session.revision,
                                                         axk::PartitionIndex{0}, edits, {}, &progress));
            require(images.filesystem(session.image_id, "bench", session.revision));
        };
        measure("create", [&](Progress &progress) { edit({axk::CreateFilesystemDirectory{{"AXKBENCH"}}}, progress); });
        measure("rename",
                [&](Progress &progress) { edit({axk::RenameFilesystemEntry{{"AXKBENCH"}, "AXKBEN2"}}, progress); });
        measure("import", [&](Progress &progress) {
            edit({axk::PutFilesystemFile{
                     {"AXKBEN2", "DATA.BIN"},
                     std::make_shared<axk::MemoryReader>(std::vector<std::byte>(131071U, std::byte{0x57}))}},
                 progress);
        });
        measure("export", [&](Progress &progress) {
            const auto roots = require(images.filesystem(session.image_id, "bench", session.revision));
            const auto entries = require(
                images.filesystem(session.image_id, "bench", session.revision, {.parent_id = roots.items.front().id}));
            const auto entry = std::ranges::find(entries.items, "AXKBEN2", &axk::app::ImageFilesystemEntry::name);
            if (entry == entries.items.end())
                throw std::runtime_error("Benchmark directory missing");
            const std::vector<std::string> ids{entry->id};
            require(axk::app::export_filesystem_entries(images, sandbox, reservations, session.image_id, "bench",
                                                        session.revision, ids, {"bench", "export"}, {}, &progress));
        });
        measure("delete", [&](Progress &progress) { edit({axk::RemoveFilesystemEntry{{"AXKBEN2"}, true}}, progress); });
        std::cout << results.dump(2) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
