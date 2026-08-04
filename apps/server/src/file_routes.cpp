#include "route_registration.hpp"

#include <utility>

namespace axk::server::detail {

void register_file_routes(ServerCrowApp &app, FileRoutes routes) {
    app.route_dynamic("/api/v1/files/list").methods(crow::HTTPMethod::Post)(std::move(routes.directory_list));
    app.route_dynamic("/api/v1/files/media-source/inspect")
        .methods(crow::HTTPMethod::Post)(std::move(routes.media_source_inspect));
    app.route_dynamic("/api/v1/files/metadata").methods(crow::HTTPMethod::Post)(std::move(routes.metadata));
    app.route_dynamic("/api/v1/filesystem/directories")
        .methods(crow::HTTPMethod::Post)(std::move(routes.create_directory));
    app.route_dynamic("/api/v1/filesystem/entries")
        .methods(crow::HTTPMethod::Patch, crow::HTTPMethod::Delete)(std::move(routes.mutate_entry));
    app.route_dynamic("/api/v1/files/content")
        .methods(crow::HTTPMethod::GET, crow::HTTPMethod::HEAD)(std::move(routes.file_content));
    app.route_dynamic("/api/v1/download-archives/<string>/content")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete)(std::move(routes.download_archive_content));
    app.route_dynamic("/api/v1/images").methods(crow::HTTPMethod::Post)(std::move(routes.create_image));
    app.route_dynamic("/api/v1/images/<string>")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete)(std::move(routes.image));
    app.route_dynamic("/api/v1/images/<string>/companions")
        .methods(crow::HTTPMethod::Post)(std::move(routes.attach_companions));
    app.route_dynamic("/api/v1/images/<string>/content")(std::move(routes.image_content));
    app.route_dynamic("/api/v1/images/<string>/objects")(std::move(routes.image_objects));
    app.route_dynamic("/api/v1/images/<string>/relationships")(std::move(routes.image_relationships));
    app.route_dynamic("/api/v1/images/<string>/validation/issues")(std::move(routes.image_validation));
    app.route_dynamic("/api/v1/images/<string>/preview")(std::move(routes.image_preview));
    app.route_dynamic("/api/v1/auditions/<string>/content")
        .methods(crow::HTTPMethod::Get)(std::move(routes.audition_content));
    app.route_dynamic("/api/v1/auditions/<string>")
        .methods(crow::HTTPMethod::Delete)(std::move(routes.delete_audition));
    app.route_dynamic("/api/v1/uploads")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)(std::move(routes.uploads));
    app.route_dynamic("/api/v1/uploads/<string>")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put, crow::HTTPMethod::Delete)(std::move(routes.upload));
    app.route_dynamic("/api/v1/uploads/<string>/complete")
        .methods(crow::HTTPMethod::Post)(std::move(routes.complete_upload));
    app.route_dynamic("/api/v1/uploads/<string>/materialize")
        .methods(crow::HTTPMethod::Post)(std::move(routes.materialize_upload));
}

} // namespace axk::server::detail
