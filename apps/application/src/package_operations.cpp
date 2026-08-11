#include "axklib/application/package_operations.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/secure_random.hpp"
#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/package_import_planning.hpp"
#include "package_operations_internal.hpp"

using namespace axk::app::package_operations_internal;

axk::app::Result<void> axk::app::bind_package_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                         UploadStore &uploads) {
    auto state = std::make_shared<PackagePlanStore>();

    if (!registry.is_implemented("package.inspect")) {
        auto bound =
            registry.bind("package.inspect", [&sandbox, &uploads](const Json &input, const OperationContext &context) {
                try {
                    return read_operation(input, context, sandbox, uploads, false);
                } catch (const Json::exception &) {
                    return Result<Json>{std::unexpected(operation_error("invalid_request", "package is required"))};
                }
            });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.verify")) {
        auto bound =
            registry.bind("package.verify", [&sandbox, &uploads](const Json &input, const OperationContext &context) {
                try {
                    return read_operation(input, context, sandbox, uploads, true);
                } catch (const Json::exception &) {
                    return Result<Json>{std::unexpected(operation_error("invalid_request", "package is required"))};
                }
            });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.export")) {
        auto bound = registry.bind("package.export", [&sandbox](const Json &input, const OperationContext &context) {
            auto source = parse_file_ref(input, "source");
            auto output = parse_file_ref(input, "output");
            auto roots = parse_roots(input);
            if (!source)
                return Result<Json>{std::unexpected(source.error())};
            if (!output)
                return Result<Json>{std::unexpected(output.error())};
            if (!roots)
                return Result<Json>{std::unexpected(roots.error())};
            const auto overwrite = input.value("overwrite", false);
            if (const auto distinct = sandbox.require_distinct(*source, *output); !distinct)
                return Result<Json>{std::unexpected(distinct.error())};
            auto source_file = sandbox.open_file(*source);
            if (!source_file)
                return Result<Json>{std::unexpected(source_file.error())};
            auto media = axk::open_media(source_file->reader, std::filesystem::path{source_file->filename},
                                         context.cancellation);
            if (!media)
                return Result<Json>{std::unexpected(core_error(media.error(), source->relative_path))};
            auto build = axk::build_portable_package(*media, *roots, context.cancellation);
            if (!build)
                return Result<Json>{std::unexpected(core_error(build.error(), source->relative_path))};
            auto effective_output = *output;
            const auto required = std::string{build->required_extension};
            auto filename = package_internal::resolve_filename(effective_output.relative_path, required);
            if (!filename)
                return Result<Json>{std::unexpected(filename.error())};
            effective_output.relative_path = std::move(*filename);
            if (context.progress)
                context.progress->report(
                    {axk::ProgressPhase::exporting, 0U, 1U, "Publishing portable package", std::nullopt});
            const auto size_bytes = build->archive.size();
            const axk::MemoryReader archive{std::move(build->archive)};
            if (auto publication = sandbox.publish_file(effective_output, overwrite, archive); !publication)
                return Result<Json>{std::unexpected(publication.error())};
            if (context.progress)
                context.progress->report(
                    {axk::ProgressPhase::exporting, 1U, 1U, "Portable package published", std::nullopt});
            auto result = package_json(build->package);
            result["output"] = file_ref_json(effective_output);
            result["sizeBytes"] = size_bytes;
            return Result<Json>{std::move(result)};
        });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.plan_import")) {
        auto bound = registry.bind("package.plan_import", [state, &sandbox, &uploads](const Json &input,
                                                                                      const OperationContext &context) {
            auto target = parse_file_ref(input, "target");
            auto output = parse_file_ref(input, "output");
            if (!target)
                return Result<Json>{std::unexpected(target.error())};
            if (!output)
                return Result<Json>{std::unexpected(output.error())};
            if (const auto distinct = sandbox.require_distinct(*target, *output); !distinct)
                return Result<Json>{std::unexpected(distinct.error())};
            auto target_file = sandbox.open_file(*target);
            if (!target_file)
                return Result<Json>{std::unexpected(target_file.error())};
            auto target_staging = sandbox.create_staging_directory("axklib-package-plan");
            if (!target_staging)
                return Result<Json>{std::unexpected(target_staging.error())};
            TemporaryDirectoryCleanup target_cleanup{*target_staging};
            const auto target_path = *target_staging / "target.img";
            if (auto staged = write_reader(target_path, *target_file->reader); !staged)
                return Result<Json>{std::unexpected(staged.error())};
            const auto overwrite = input.value("overwrite", false);
            auto output_path = sandbox.resolve_output_file(*output, overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};

            std::vector<PackageInput> inputs;
            try {
                if (!input.contains("packages") || !input.at("packages").is_array() || input.at("packages").empty() ||
                    input.at("packages").size() > 256U) {
                    return Result<Json>{std::unexpected(
                        operation_error("invalid_request", "packages must contain 1 to 256 references"))};
                }
                for (const auto &value : input.at("packages")) {
                    auto parsed = parse_package_input(value);
                    if (!parsed)
                        return Result<Json>{std::unexpected(parsed.error())};
                    inputs.push_back(std::move(*parsed));
                }
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "packages are malformed"))};
            }
            auto import_request = parse_import_request(input);
            if (!import_request)
                return Result<Json>{std::unexpected(import_request.error())};

            auto retained_sources = package_plan_internal::retain_sources(inputs, context.owner_id, sandbox, uploads);
            if (!retained_sources)
                return Result<Json>{std::unexpected(retained_sources.error())};
            auto admission = package_plan_internal::admit(state, retained_sources->source_bytes);
            if (!admission)
                return Result<Json>{std::unexpected(admission.error())};

            std::vector<axk::PortablePackage> packages;
            packages.reserve(inputs.size());
            for (const auto &source : inputs) {
                auto item = resolve_package(source, context.owner_id, sandbox, uploads);
                if (!item)
                    return Result<Json>{std::unexpected(item.error())};
                auto package = read_package(*item, true, context);
                if (!package)
                    return Result<Json>{std::unexpected(package.error())};
                packages.push_back(std::move(*package));
            }
            auto plan = axk::plan_package_import(target_path, packages, *import_request, context.cancellation);
            if (!plan)
                return Result<Json>{std::unexpected(core_error(plan.error(), target->relative_path))};

            const auto now = Clock::now();
            auto token = axk::app::secure_random_hex(24U);
            if (!token)
                return Result<Json>{std::unexpected(token.error())};
            auto record = std::make_shared<PackagePlanRecord>(
                PackagePlanRecord{*token, context.owner_id, now + state->retention, *target, *output, *output_path,
                                  overwrite, std::move(inputs), std::move(retained_sources->upload_leases),
                                  retained_sources->source_bytes, std::move(*plan), false});
            if (auto stored = admission->commit(record); !stored)
                return Result<Json>{std::unexpected(stored.error())};
            return Result<Json>{
                plan_json(record->plan, *token, static_cast<std::uint64_t>(state->retention.count() * 60))};
        });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.import")) {
        auto bound = registry.bind("package.import", [state, &sandbox, &uploads](const Json &input,
                                                                                 const OperationContext &context) {
            std::string token;
            try {
                token = input.at("planToken").get<std::string>();
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "planToken is required"))};
            }
            auto claim = package_plan_internal::claim(state, token, context.owner_id);
            if (!claim)
                return Result<Json>{std::unexpected(claim.error())};
            const auto record = claim->record();
            if (!record->plan.valid()) {
                claim->consume();
                return Result<Json>{std::unexpected(
                    operation_error("package_plan_conflicts", "package import plan contains unresolved conflicts"))};
            }

            std::vector<axk::PortablePackage> packages;
            packages.reserve(record->inputs.size());
            std::vector<ResolvedPackage> current_inputs;
            current_inputs.reserve(record->inputs.size());
            for (std::size_t index = 0; index < record->inputs.size(); ++index) {
                auto resolved = resolve_package(record->inputs[index], context.owner_id, sandbox, uploads);
                if (!resolved)
                    return Result<Json>{std::unexpected(resolved.error())};
                auto package = read_package(*resolved, true, context);
                if (!package)
                    return Result<Json>{std::unexpected(package.error())};
                if (package->package_id != record->plan.package_ids[index]) {
                    claim->consume();
                    return Result<Json>{std::unexpected(
                        operation_error("package_plan_stale", "a package changed after import planning"))};
                }
                current_inputs.push_back(std::move(*resolved));
                packages.push_back(std::move(*package));
            }
            auto target_file = sandbox.open_file(record->target);
            if (!target_file)
                return Result<Json>{std::unexpected(target_file.error())};
            auto output_path = sandbox.resolve_output_file(record->output, record->overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};
            if (normalized_path(*output_path) != normalized_path(record->output_path)) {
                claim->consume();
                return Result<Json>{std::unexpected(
                    operation_error("package_plan_stale", "package import destination changed after planning"))};
            }
            auto staging_directory = sandbox.create_staging_directory("axklib-package-import");
            if (!staging_directory)
                return Result<Json>{std::unexpected(staging_directory.error())};
            TemporaryDirectoryCleanup staging_cleanup{*staging_directory};
            const auto target_path = *staging_directory / "target.img";
            if (auto staged = write_reader(target_path, *target_file->reader); !staged)
                return Result<Json>{std::unexpected(staged.error())};
            const auto staged_output = *staging_directory / output_path->filename();
            auto report = axk::apply_package_import(target_path, packages, record->plan, staged_output, false,
                                                    context.cancellation, context.progress);
            if (!report)
                return Result<Json>{std::unexpected(core_error(report.error(), record->output.relative_path))};
            auto staged_reader = axk::FileReader::open(staged_output);
            if (!staged_reader)
                return Result<Json>{std::unexpected(core_error(staged_reader.error(), record->output.relative_path))};
            if (auto published = sandbox.publish_file(record->output, record->overwrite, **staged_reader); !published)
                return Result<Json>{std::unexpected(published.error())};
            claim->consume();
            return Result<Json>{{{"schemaVersion", "1.0"},
                                 {"planId", report->plan_id},
                                 {"output", file_ref_json(record->output)},
                                 {"sourceSnapshotId", report->source_snapshot_id},
                                 {"outputSnapshotId", report->output_snapshot_id},
                                 {"programAssignmentAdjustments",
                                  program_assignment_adjustments_json(report->program_assignment_adjustments)},
                                 {"applied", report->applied}}};
        });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.plan_import.release")) {
        auto bound = registry.bind(
            "package.plan_import.release", [state](const Json &input, const OperationContext &context) -> Result<Json> {
                std::string token;
                try {
                    token = input.at("planToken").get<std::string>();
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error("invalid_request", "planToken is required"));
                }
                if (auto released = package_plan_internal::release(state, token, context.owner_id); !released) {
                    return std::unexpected(released.error());
                }
                return Json{{"released", true}};
            });
        if (!bound)
            return bound;
    }
    auto accesses_bound = registry.bind_path_accesses(
        "package.import",
        [state](const Json &input, const OperationContext &context) -> Result<std::vector<PathAccess>> {
            std::string token;
            try {
                token = input.at("planToken").get<std::string>();
            } catch (const Json::exception &) {
                return std::unexpected(operation_error("invalid_request", "planToken is required"));
            }
            return package_plan_internal::path_accesses(state, token, context.owner_id);
        });
    if (!accesses_bound)
        return accesses_bound;
    return {};
}
