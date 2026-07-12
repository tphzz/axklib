#include <string_view>

#include "axklib/version.hpp"

int main() { return axk::version() == std::string_view{"0.1.0"} ? 0 : 1; }
