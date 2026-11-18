#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "axklib/conversion/profiles.hpp"

namespace CLI {
class App;
}
namespace axk::cli {
struct ConversionCommand {
    CLI::App *command{};
    std::string profile;
    std::vector<std::filesystem::path> sources;
    std::filesystem::path destination;
    std::filesystem::path output;
    std::string selection;
    std::string options;
    bool strict{};
};
void register_conversion_command(CLI::App &app, ConversionCommand &state);
int run_conversion(const ConversionCommand &state,
                   const conversion::Registry &profiles = conversion::builtin_profiles());
} // namespace axk::cli
