# C++ And CLI Usage

## CMake Consumer

```cmake
cmake_minimum_required(VERSION 3.28)
project(example LANGUAGES CXX)

find_package(axklib CONFIG REQUIRED COMPONENTS axklib)
add_executable(example main.cpp)
target_compile_features(example PRIVATE cxx_std_17)
target_link_libraries(example PRIVATE axklib::axklib)
```

The shared library contains the complete supported facade and embeds its codec,
resampling, and engine dependencies. Consumers do not find those packages.

```cpp
#include <iostream>

#include <axklib/sdk.hpp>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  axk::operation_context context;
  auto media = axk::image::open(argv[1], context);
  if (!media) {
    std::cerr << axk::render_error(media.error()) << '\n';
    return 1;
  }
  auto objects = media->objects(0, 256, context);
  if (!objects) return 1;
  std::cout << objects->total_count << " object(s)\n";
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
axklib create manifest hds --output image.json
axklib create hds image.json --output HD00_512_generated.hds
axklib alter hds source.hds transaction.json --output altered.hds
```

Use `axklib <command> --help` for the exact selectors and overwrite options of
each command. See [Writer And Alteration](write.md#generate-a-starter-manifest)
for the generated HDS schema and the floppy and ISO starter variants.
