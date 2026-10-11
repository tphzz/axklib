// Shared helpers for the rawdisk test programs.
#pragma once

#include "rawdisk/block_device.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

inline int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (0)

template <class T, class E> std::error_code err(const std::expected<T, E> &r) {
    if (r)
        return {};
    if constexpr (std::is_same_v<E, std::error_code>)
        return r.error();
    else
        return r.error().code;
}

inline std::filesystem::path test_dir() {
    static std::filesystem::path d = [] {
        auto p = std::filesystem::temp_directory_path() / std::format("rawdisk-t-{}", std::random_device{}());
        std::filesystem::create_directories(p);
        return p;
    }();
    return d;
}

// Card image: `sectors` x 512 bytes, byte i of sector s = (s*7 + i) & 0xFF, plus optional bytes
// written at `at_lba`.
inline std::filesystem::path make_card(const std::string &name, std::uint64_t sectors,
                                       const std::vector<unsigned char> &extra = {}, std::uint64_t at_lba = 0) {
    const auto p = test_dir() / name;
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        std::vector<unsigned char> s(512);
        for (std::uint64_t lba = 0; lba < sectors; ++lba) {
            for (std::size_t i = 0; i < 512; ++i)
                s[i] = static_cast<unsigned char>((lba * 7 + i) & 0xFF);
            f.write(reinterpret_cast<const char *>(s.data()), 512);
        }
    }
    if (!extra.empty()) {
        std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(at_lba * 512));
        f.write(reinterpret_cast<const char *>(extra.data()), static_cast<std::streamsize>(extra.size()));
    }
    return p;
}

inline unsigned char pattern_at(std::uint64_t lba, std::size_t i) {
    return static_cast<unsigned char>((lba * 7 + i) & 0xFF);
}

inline std::shared_ptr<rawdisk::BlockDevice>
open_shared(const std::filesystem::path &p, rawdisk::Access a = rawdisk::Access::ReadWrite, std::uint32_t ss = 512) {
    auto d = rawdisk::open_image(p, a, ss);
    if (!d)
        return nullptr;
    return std::shared_ptr<rawdisk::BlockDevice>(std::move(*d));
}

// V6 configuration block builder (firmware layout).
struct TestTarget {
    std::uint8_t id = 0;
    bool enabled = true;
    std::uint32_t start = 0, sectors = 0;
    std::uint16_t bps = 512;
    std::string vendor = "ACME", product = "DISK";
};
inline std::vector<unsigned char> v6_block(const std::vector<TestTarget> &ts) {
    std::vector<unsigned char> b(1024, 0);
    std::memcpy(b.data(), "BCFG", 4);
    b[6] = 255;
    b[7] = 1;
    auto put16 = [&](std::size_t o, std::uint16_t v) {
        b[o] = static_cast<unsigned char>(v & 0xFF);
        b[o + 1] = static_cast<unsigned char>(v >> 8);
    };
    auto put32 = [&](std::size_t o, std::uint32_t v) {
        put16(o, static_cast<std::uint16_t>(v & 0xFFFF));
        put16(o + 2, static_cast<std::uint16_t>(v >> 16));
    };
    for (std::size_t i = 0; i < ts.size() && i < 7; ++i) {
        const std::size_t o = 128 * (i + 1);
        b[o] = static_cast<unsigned char>((ts[i].enabled ? 0x80 : 0) | (ts[i].id & 7));
        put32(o + 4, ts[i].start);
        put32(o + 8, ts[i].sectors);
        put16(o + 12, ts[i].bps);
        for (std::size_t k = 0; k < 8; ++k)
            b[o + 18 + k] = k < ts[i].vendor.size() ? static_cast<unsigned char>(ts[i].vendor[k]) : ' ';
        for (std::size_t k = 0; k < 16; ++k)
            b[o + 26 + k] = k < ts[i].product.size() ? static_cast<unsigned char>(ts[i].product[k]) : ' ';
    }
    return b;
}

inline int finish_tests() {
    std::error_code ec;
    std::filesystem::remove_all(test_dir(), ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
