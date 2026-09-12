#ifndef MW_EXPORT_H_
#define MW_EXPORT_H_

#if defined(_WIN32)
#define MW_EXPORT __declspec(dllexport)
#define MW_IMPORT __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define MW_EXPORT __attribute__((visibility("default")))
#define MW_IMPORT
#else
#define MW_EXPORT
#define MW_IMPORT
#endif

#if defined(MW_LOG_STATIC_LIBRARY)
#define MW_LOG_API
#elif defined(MW_LOG_BUILDING_LIBRARY)
#define MW_LOG_API MW_EXPORT
#else
#define MW_LOG_API MW_IMPORT
#endif

#if defined(MW_STREAMER_STATIC_LIBRARY)
#define MW_STREAMER_API
#elif defined(MW_STREAMER_BUILDING_LIBRARY)
#define MW_STREAMER_API MW_EXPORT
#else
#define MW_STREAMER_API MW_IMPORT
#endif

#if defined(MW_OPENCV_ADAPTER_STATIC_LIBRARY)
#define MW_OPENCV_ADAPTER_API
#elif defined(MW_OPENCV_ADAPTER_BUILDING_LIBRARY)
#define MW_OPENCV_ADAPTER_API MW_EXPORT
#else
#define MW_OPENCV_ADAPTER_API MW_IMPORT
#endif

#endif  // MW_EXPORT_H_
