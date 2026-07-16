#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "axklib/application/operation_registry.hpp"
#include "axklib/server/contract.hpp"

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: axk-server-contract-generator BASE OUTPUT\n";
        return 2;
    }
    try {
        std::ifstream input{std::filesystem::path{argv[1]}, std::ios::binary};
        if (!input)
            throw std::runtime_error{"could not open base OpenAPI document"};
        const std::string base{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        const auto document = axk::server::build_openapi_document(base, axk::app::make_operation_registry());
        std::ofstream output{std::filesystem::path{argv[2]}, std::ios::binary | std::ios::trunc};
        output << document.dump(2) << '\n';
        if (!output)
            throw std::runtime_error{"could not write generated OpenAPI document"};
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "OpenAPI generation failed: " << error.what() << '\n';
        return 1;
    }
}
