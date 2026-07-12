#include <filesystem>
#include <fstream>
#include <string>

#include "axklib/sdk.hpp"

int main() {
  axk::result<int> value{42};
  if (axk::sdk_version() != std::string{"0.1.0"} || !value || *value != 42)
    return 1;

  axk::operation_context context;
  auto image = axk::image::open(AXK_TEST_FIXTURE, context);
  if (!image)
    return 2;
  auto validation = image->validation(context);
  auto relationships = image->relationships(0U, 64U, context);
  if (!validation || !relationships || validation->object_count == 0U ||
      relationships->total_count != validation->relationship_count)
    return 3;

  const auto root = std::filesystem::temp_directory_path() / "axklib-installed-consumer";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  const auto manifest = root / "build.json";
  const auto output = root / "output.hds";
  std::ofstream{manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Volume","waveforms":[],"sample_banks":[]}]}]})";
  auto plan = axk::build_plan::from_manifest(manifest.string(), context);
  if (!plan || !plan->apply(output.string(), {}, context) || !std::filesystem::exists(output))
    return 4;
  std::filesystem::remove_all(root, error);
  return 0;
}
