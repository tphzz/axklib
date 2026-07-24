#include "axklib/application/application_operations.hpp"

#include "axklib/application/audio_operations.hpp"
#include "axklib/application/extraction_operations.hpp"
#include "axklib/application/file_operations.hpp"
#include "axklib/application/package_operations.hpp"
#include "axklib/application/validation_operations.hpp"
#include "axklib/application/write_operations.hpp"

namespace {

using Json = nlohmann::json;
using axk::app::FileRef;
using axk::app::OperationContext;
using axk::app::PathAccess;
using axk::app::PathAccessMode;
using axk::app::Result;

std::optional<FileRef> file_ref(const Json &value) {
    try {
        return FileRef{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
    } catch (const Json::exception &) {
        return std::nullopt;
    }
}

void append_file_ref(const Json &input, std::string_view member, PathAccessMode mode, std::vector<PathAccess> &result) {
    const auto found = input.find(member);
    if (found != input.end()) {
        if (auto reference = file_ref(*found))
            result.push_back({std::move(*reference), mode});
    }
}

void append_file_refs(const Json &input, std::string_view member, PathAccessMode mode,
                      std::vector<PathAccess> &result) {
    const auto found = input.find(member);
    if (found == input.end() || !found->is_array())
        return;
    for (const auto &value : *found) {
        if (auto reference = file_ref(value))
            result.push_back({std::move(*reference), mode});
    }
}

void append_input_ref(const Json &input, std::string_view member, std::vector<PathAccess> &result) {
    const auto found = input.find(member);
    if (found == input.end() || !found->is_object())
        return;
    const auto file = found->find("fileRef");
    if (file != found->end()) {
        if (auto reference = file_ref(*file))
            result.push_back({std::move(*reference), PathAccessMode::shared});
    }
}

void append_input_refs(const Json &input, std::string_view member, std::vector<PathAccess> &result) {
    const auto found = input.find(member);
    if (found == input.end() || !found->is_array())
        return;
    for (const auto &value : *found) {
        if (value.is_object()) {
            const auto file = value.find("fileRef");
            if (file != value.end()) {
                if (auto reference = file_ref(*file))
                    result.push_back({std::move(*reference), PathAccessMode::shared});
            }
        }
    }
}

void append_manifest_inputs(const Json &input, std::vector<PathAccess> &result) {
    append_input_ref(input, "manifest", result);
    const auto bindings = input.find("inputBindings");
    if (bindings == input.end() || !bindings->is_array())
        return;
    for (const auto &binding : *bindings) {
        if (binding.is_object())
            append_input_ref(binding, "input", result);
    }
}

using Resolver = axk::app::OperationRegistry::PathAccessResolver;

Result<void> bind_standard_path_accesses(axk::app::OperationRegistry &registry) {
    const auto bind = [&](std::string_view id, Resolver resolver) { return registry.bind_path_accesses(id, resolver); };
    const auto reports = [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
        std::vector<PathAccess> result;
        append_file_refs(input, "sources", PathAccessMode::shared, result);
        append_file_ref(input, "destination", PathAccessMode::exclusive, result);
        return result;
    };
    const auto sources = [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
        std::vector<PathAccess> result;
        append_file_refs(input, "sources", PathAccessMode::shared, result);
        return result;
    };
    for (const auto id : {"report.objects", "report.relationships", "report.inventory", "report.coverage",
                          "report.orphans", "corpus.audit", "extract.wav", "extract.sfz"}) {
        if (auto bound = bind(id, reports); !bound)
            return bound;
    }
    if (auto bound = bind("report.validate",
                          [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
                              std::vector<PathAccess> result;
                              append_file_refs(input, "sources", PathAccessMode::shared, result);
                              append_file_ref(input, "exports", PathAccessMode::shared, result);
                              append_file_ref(input, "destination", PathAccessMode::exclusive, result);
                              return result;
                          });
        !bound) {
        return bound;
    }
    if (auto bound = bind("report.info", sources); !bound)
        return bound;
    if (auto bound = bind("package.export",
                          [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
                              std::vector<PathAccess> result;
                              append_file_ref(input, "source", PathAccessMode::shared, result);
                              append_file_ref(input, "output", PathAccessMode::exclusive, result);
                              return result;
                          });
        !bound) {
        return bound;
    }
    for (const auto id : {"package.inspect", "package.verify", "audio.inspect"}) {
        if (auto bound = bind(id,
                              [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
                                  std::vector<PathAccess> result;
                                  append_input_ref(input, input.contains("package") ? "package" : "source", result);
                                  return result;
                              });
            !bound) {
            return bound;
        }
    }
    if (auto bound = bind("package.plan_import",
                          [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
                              std::vector<PathAccess> result;
                              append_input_refs(input, "packages", result);
                              append_file_ref(input, "target", PathAccessMode::shared, result);
                              append_file_ref(input, "output", PathAccessMode::exclusive, result);
                              return result;
                          });
        !bound) {
        return bound;
    }
    for (const auto id : {"create.plan", "create.hds.plan"}) {
        if (auto bound = bind(id,
                              [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
                                  std::vector<PathAccess> result;
                                  append_manifest_inputs(input, result);
                                  append_file_ref(input, "output", PathAccessMode::exclusive, result);
                                  return result;
                              });
            !bound) {
            return bound;
        }
    }
    for (const auto id : {"alter.inspect", "alter.hds"}) {
        if (auto bound = bind(id,
                              [id](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
                                  std::vector<PathAccess> result;
                                  append_manifest_inputs(input, result);
                                  append_file_ref(input, "source", PathAccessMode::shared, result);
                                  if (id == std::string_view{"alter.hds"})
                                      append_file_ref(input, "output", PathAccessMode::exclusive, result);
                                  return result;
                              });
            !bound) {
            return bound;
        }
    }
    return {};
}

} // namespace

axk::app::Result<void> axk::app::bind_application_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                             UploadStore &uploads,
                                                             const axk::MediaBuildLimits &media_limits) {
    if (auto bound = bind_file_operations(registry, sandbox); !bound)
        return bound;
    if (auto bound = bind_audio_operations(registry, sandbox, uploads); !bound)
        return bound;
    if (auto bound = bind_validation_operations(registry, sandbox); !bound)
        return bound;
    if (auto bound = bind_extraction_operations(registry, sandbox); !bound)
        return bound;
    if (auto bound = bind_package_operations(registry, sandbox, uploads); !bound)
        return bound;
    if (auto bound = bind_write_operations(registry, sandbox, uploads, media_limits); !bound)
        return bound;
    return bind_standard_path_accesses(registry);
}

axk::app::Result<axk::app::OperationRegistry>
axk::app::make_application_registry(const Sandbox &sandbox, UploadStore &uploads, OperationRegistry registry,
                                    const axk::MediaBuildLimits &media_limits) {
    if (auto bound = bind_application_operations(registry, sandbox, uploads, media_limits); !bound)
        return std::unexpected(bound.error());
    return registry;
}

axk::app::Result<void> axk::app::bind_session_application_operations(OperationRegistry &registry,
                                                                     const Sandbox &sandbox, UploadStore &uploads,
                                                                     ImageSessionManager &images,
                                                                     AlterationJournalStore &journals) {
    return bind_session_write_operations(registry, sandbox, uploads, images, journals);
}
