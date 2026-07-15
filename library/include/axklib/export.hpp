#pragma once

#if defined(_WIN32) && defined(AXK_SHARED_LIBRARY)
#if defined(AXK_CORE_EXPORTS)
#define AXK_API __declspec(dllexport)
#else
#define AXK_API __declspec(dllimport)
#endif
#if defined(AXK_AUDIO_EXPORTS)
#define AXK_AUDIO_API __declspec(dllexport)
#else
#define AXK_AUDIO_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(AXK_SHARED_LIBRARY)
#define AXK_API __attribute__((visibility("default")))
#define AXK_AUDIO_API __attribute__((visibility("default")))
#else
#define AXK_API
#define AXK_AUDIO_API
#endif
