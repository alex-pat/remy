#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>

#if defined(__linux__) && defined(__has_include) && __has_include(<linux/stat.h>)
#include <linux/stat.h>
#endif

// POSIX file mode bits and test macros for platforms where they are not provided
#ifndef S_IFMT
#define S_IFMT 00170000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0140000
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_IFREG
#define S_IFREG 0100000
#endif
#ifndef S_IFBLK
#define S_IFBLK 0060000
#endif
#ifndef S_IFDIR
#define S_IFDIR 0040000
#endif
#ifndef S_IFCHR
#define S_IFCHR 0020000
#endif
#ifndef S_IFIFO
#define S_IFIFO 0010000
#endif
#ifndef S_ISUID
#define S_ISUID 0004000
#endif
#ifndef S_ISGID
#define S_ISGID 0002000
#endif
#ifndef S_ISVTX
#define S_ISVTX 0001000
#endif

#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISCHR
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif

namespace remy {

struct RemyStatxTimestamp {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

#if defined(__linux__) && defined(STATX_TYPE)
static_assert(sizeof(RemyStatxTimestamp) == sizeof(struct statx_timestamp),
              "RemyStatxTimestamp size mismatch with Linux struct statx_timestamp");
static_assert(offsetof(RemyStatxTimestamp, tv_sec) == offsetof(struct statx_timestamp, tv_sec),
              "RemyStatxTimestamp tv_sec offset mismatch");
static_assert(offsetof(RemyStatxTimestamp, tv_nsec) == offsetof(struct statx_timestamp, tv_nsec),
              "RemyStatxTimestamp tv_nsec offset mismatch");
static_assert(offsetof(RemyStatxTimestamp, __reserved) == offsetof(struct statx_timestamp, __reserved),
              "RemyStatxTimestamp __reserved offset mismatch");
#endif

}  // namespace remy

namespace remy::utils {

inline std::string pretty_size(uint64_t size) {
  static const std::array SIZE_NAMES = {"", "K", "M", "G", "T"};
  size_t div = 0;
  size_t rem = 0;

  while (size >= 1024 && div < SIZE_NAMES.size()) {
    rem = (size % 1024);
    div++;
    size /= 1024;
  }

  return std::format("{:.{}f}{}", size + rem / 1024.f, rem == 0 ? 0 : 1, SIZE_NAMES[div]);
}

inline std::string pretty_duration(uint64_t seconds) {
  if (seconds >= 3600) {
    return std::format("{}h {}m {}s", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
  } else if (seconds >= 60) {
    return std::format("{}m {}s", seconds / 60, seconds % 60);
  } else {
    return std::format("{}s", seconds);
  }
}

inline void trim_end(std::string &str) {
  str.erase(std::find_if(str.rbegin(), str.rend(), [](auto ch) {
    return !std::isspace(ch);
  }).base(), str.end());
}

}  // namespace remy::utils
