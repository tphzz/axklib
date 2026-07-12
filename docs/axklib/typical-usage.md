# C++ And CLI Usage

## CMake Consumer

```cmake
cmake_minimum_required(VERSION 3.28)
project(example LANGUAGES CXX)

find_package(axklib CONFIG REQUIRED COMPONENTS core)
add_executable(example main.cpp)
target_compile_features(example PRIVATE cxx_std_23)
target_link_libraries(example PRIVATE axklib::core)
```

Read-only inventory, validation, and exact export through `axklib::core` do not
link libsndfile or libsoxr. Applications that import WAV/AIFF/FLAC audio or use
the fresh-image and alteration writers link `axklib::audio` instead; that target
includes the core transitively.

For writer consumers, request the audio component and link its target:

```cmake
find_package(axklib CONFIG REQUIRED COMPONENTS audio)
target_link_libraries(example PRIVATE axklib::audio)
```

```cpp
#include <iostream>

#include <axklib/media.hpp>
#include <axklib/relationship.hpp>
#include <axklib/semantic.hpp>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  auto media = axk::open_media(argv[1]);
  if (!media) {
    std::cerr << axk::render_error(media.error()) << '\n';
    return 1;
  }
  auto catalog = axk::build_object_catalog(*media);
  if (!catalog) return 1;
  auto graph = axk::build_relationship_graph(*catalog);
  auto tree = axk::build_content_tree(*media, *catalog, graph);
  std::cout << tree.roots.size() << " root scope(s)\n";
}
```

## Common CLI Flows

```bash
axklib info source.hds
axklib objects source.hds --output-dir reports/objects
axklib relationships source.hds --output-dir reports/relationships
axklib validate source.hds --output-dir reports/validation
axklib extract wav file source.hds --output-dir exports/wav
axklib extract sfz file source.hds --output-dir exports/sfz
axklib create hds image.json --output HD00_512_generated.hds
axklib alter hds source.hds transaction.json --output altered.hds
```

Use `axklib <command> --help` for the exact selectors and overwrite options of
each command.
