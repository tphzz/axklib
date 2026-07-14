#include <iostream>

#include <axklib/media.hpp>
#include <axklib/relationship.hpp>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: inventory <image>\n";
        return 2;
    }
    const auto media = axk::open_media(argv[1]);
    if (!media) {
        std::cerr << axk::render_error(media.error()) << '\n';
        return 1;
    }
    const auto catalog = axk::build_object_catalog(*media);
    if (!catalog) {
        std::cerr << axk::render_error(catalog.error()) << '\n';
        return 1;
    }
    const auto graph = axk::build_relationship_graph(*catalog);
    std::cout << "objects=" << catalog->objects.size()
              << " relationships=" << graph.relationships.size() << '\n';
    return 0;
}
