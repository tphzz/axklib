#pragma once

#if defined(_WIN32)
#if defined(AXK_SDK_EXPORTS)
#define AXK_SDK_API __declspec(dllexport)
#else
#define AXK_SDK_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define AXK_SDK_API __attribute__((visibility("default")))
#define AXK_SDK_HIDDEN __attribute__((visibility("hidden")))
#else
#define AXK_SDK_API
#define AXK_SDK_HIDDEN
#endif

#if defined(_WIN32)
#define AXK_SDK_HIDDEN
#endif
