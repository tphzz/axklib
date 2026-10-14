#include "conversion_command.hpp"

#include <iostream>

#include <CLI/CLI.hpp>

#include "axklib/conversion/file_plan.hpp"
#include "axklib/utf8.hpp"
#include "exit_status.hpp"

namespace axk::cli {
void register_conversion_command(CLI::App &app, ConversionCommand &state) {
    state.command = app.add_subcommand("convert", "prepare, review and optionally publish a directional conversion");
    state.command->add_option("--profile", state.profile, "conversion profile ID")->required();
    state.command->add_option("--source", state.sources, "explicit source image(s) or folder(s)")->required();
    state.command->add_option("--destination", state.destination, "existing destination image template")->required();
    state.command->add_option("--selection", state.selection, "destination selection JSON")->required();
    state.command->add_option("--options", state.options, "profile options JSON")->required();
    state.command->add_flag("--strict", state.strict, "reject meaningful loss or uncertainty");
    state.command->add_option("--output", state.output, "new output image; prompts for review in this process");
}

int run_conversion(const ConversionCommand &state) {
    conversion::FileRequest request;
    request.profile_id = state.profile;
    request.sources = state.sources;
    request.destination = state.destination;
    request.fidelity = state.strict ? conversion::Fidelity::strict : conversion::Fidelity::best;
    try {
        request.selection = nlohmann::json::parse(state.selection);
        request.options = nlohmann::json::parse(state.options);
    } catch (const nlohmann::json::exception &) {
        std::cerr << "Conversion selection and options must be valid JSON\n";
        return exit_code(ExitStatus::invalid_request);
    }
    auto plan = conversion::FilePlan::prepare(request);
    if (!plan) {
        std::cerr << render_error(plan.error()) << '\n';
        return exit_code(core_error_status(plan.error()));
    }
    std::cout << conversion::report_json(plan->report()).dump(2) << '\n';
    if (!plan->report().executable)
        return exit_code(ExitStatus::invalid_request);
    if (state.output.empty())
        return exit_code(ExitStatus::success);
    std::vector<std::string> acknowledged;
    std::string confirmation = "APPLY";
    for (const auto &issue : plan->report().issues) {
        if (issue.impact == conversion::Impact::loss || issue.impact == conversion::Impact::uncertainty) {
            acknowledged.push_back(issue.id);
            confirmation += ' ' + issue.id;
        }
    }
    std::cerr << "New output: " << text::path_to_utf8(state.output) << "\nType exactly " << confirmation << ": "
              << std::flush;
    std::string response;
    if (!std::getline(std::cin, response) || response != confirmation) {
        std::cerr << "Conversion not applied\n";
        return exit_code(ExitStatus::invalid_request);
    }
    const auto result = plan->apply(state.output, acknowledged);
    if (!result) {
        std::cerr << render_error(result.error()) << '\n';
        return exit_code(core_error_status(result.error()));
    }
    for (const auto &warning : result->warnings)
        std::cerr << "warning[" << warning.code << "]: " << warning.message << '\n';
    return exit_code(ExitStatus::success);
}
} // namespace axk::cli
