#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

namespace axk::native {

inline bool ascii_digit(char value) { return value >= '0' && value <= '9'; }

inline int natural_compare(std::string_view left, std::string_view right) {
    std::size_t left_index{};
    std::size_t right_index{};
    while (left_index < left.size() && right_index < right.size()) {
        if (ascii_digit(left[left_index]) && ascii_digit(right[right_index])) {
            const auto left_begin = left_index;
            const auto right_begin = right_index;
            while (left_index < left.size() && ascii_digit(left[left_index]))
                ++left_index;
            while (right_index < right.size() && ascii_digit(right[right_index]))
                ++right_index;
            auto left_significant = left_begin;
            auto right_significant = right_begin;
            while (left_significant < left_index && left[left_significant] == '0')
                ++left_significant;
            while (right_significant < right_index && right[right_significant] == '0')
                ++right_significant;
            const auto left_digits = left_index - left_significant;
            const auto right_digits = right_index - right_significant;
            if (left_digits != right_digits)
                return left_digits < right_digits ? -1 : 1;
            const auto left_number = left.substr(left_significant, left_digits);
            const auto right_number = right.substr(right_significant, right_digits);
            if (left_number != right_number)
                return left_number < right_number ? -1 : 1;
            const auto left_width = left_index - left_begin;
            const auto right_width = right_index - right_begin;
            if (left_width != right_width)
                return left_width < right_width ? -1 : 1;
            continue;
        }
        const auto left_folded = static_cast<char>(std::tolower(static_cast<unsigned char>(left[left_index])));
        const auto right_folded = static_cast<char>(std::tolower(static_cast<unsigned char>(right[right_index])));
        if (left_folded != right_folded)
            return left_folded < right_folded ? -1 : 1;
        ++left_index;
        ++right_index;
    }
    if (left_index != left.size() || right_index != right.size())
        return left_index == left.size() ? -1 : 1;
    if (left == right)
        return 0;
    return left < right ? -1 : 1;
}

} // namespace axk::native
