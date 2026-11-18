#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "axklib/sdk.hpp"
#include "axklib/sdk/source_set.hpp"

// One installed C++17 handle serves every requested page; this client has no JSON dependency.
int main(int argc, char **argv) {
    axk::operation_context context;
    const auto session = axk::source_set::open(std::vector<std::string>{argv + 1, argv + argc}, context);
    if (!session)
        return 1;
    const auto summary = session->summary_json();
    if (!summary)
        return 2;
    std::cout << *summary << '\n';
    std::string collection, document;
    std::size_t offset{}, limit{};
    while (std::cin >> collection >> document >> offset >> limit) {
        if (collection != "objects" && collection != "relationships")
            return 3;
        const auto page = collection == "objects" ? session->objects_json(document, offset, limit, context)
                                                  : session->relationships_json(document, offset, limit, context);
        if (!page)
            return 4;
        std::cout << *page << '\n';
    }
    return std::cin.eof() ? 0 : 5;
}
