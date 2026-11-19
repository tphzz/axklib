#pragma once

#include <string>
#include <string_view>

namespace axk::typhoon {
// Typhoon's item namespace admits punctuation that FAT short names cannot store.
// Translate the reference, not the directory entry: Files retains the actual DOS name.
inline std::string reference_stem(std::string_view item_name) {
    std::string stem(item_name);
    for (char &ch : stem) {
        switch (ch) {
        case ' ':
            ch = '_';
            break;
        case '.':
            ch = '^';
            break;
        case '/':
            ch = '@';
            break;
        case '+':
            ch = '$';
            break;
        case ',':
            ch = '~';
            break;
        default:
            break;
        }
    }
    return stem;
}
} // namespace axk::typhoon
