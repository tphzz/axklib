#include <filesystem>
#include <iostream>
#include <string_view>

#include <CLI/CLI.hpp>

#include "axklib/conversion/file_plan.hpp"
#include "conversion_command.hpp"
#include "fixture_conversion_profile.hpp"

int measure_sfs_window(const std::filesystem::path &path);

int main(int argc, char **argv) {
    if (argc == 3 && std::string_view{argv[1]} == "measure-sfs")
        return measure_sfs_window(argv[2]);
    if (argc > 1 && std::string_view{argv[1]} == "convert") {
        CLI::App app;
        axk::cli::ConversionCommand state;
        axk::cli::register_conversion_command(app, state);
        CLI11_PARSE(app, argc, argv);
        return axk::cli::run_conversion(state, fixture::conversion_profiles(true));
    }
    if (argc != 5)
        return 2;
    const auto profiles = fixture::conversion_profiles(std::string_view{argv[1]} == "on");
    const axk::conversion::FileRequest request{
        fixture::profile_id,        {argv[2]}, argv[3], {{"partitionIndex", 0}, {"volumeName", "New Volume"}},
        {{"reviewLabel", argv[4]}}, {},        {}};
    const auto result = axk::conversion::FilePlan::prepare(request, {}, nullptr, profiles);
    auto json = axk::conversion::profiles_json(profiles);
    if (result)
        json["report"] = result->report_json();
    else
        json["error"] = result.error().message;
    std::cout << json.dump() << '\n';
}
