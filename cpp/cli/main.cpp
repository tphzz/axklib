#include "app.hpp"

#ifdef _WIN32
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> utf8;
  utf8.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, nullptr, 0,
                                          nullptr, nullptr);
    if (size <= 0) {
      std::cerr << "error: command-line argument is not valid UTF-16\n";
      return 2;
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, value.data(), size,
                            nullptr, nullptr) <= 0) {
      std::cerr << "error: command-line argument could not be converted to UTF-8\n";
      return 2;
    }
    value.pop_back();
    utf8.push_back(std::move(value));
  }
  std::vector<char *> pointers;
  pointers.reserve(utf8.size());
  for (auto &value : utf8)
    pointers.push_back(value.data());
  return axk::cli::run(static_cast<int>(pointers.size()), pointers.data());
}
#else
#include <csignal>

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  return axk::cli::run(argc, argv);
}
#endif
