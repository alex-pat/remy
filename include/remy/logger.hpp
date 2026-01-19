#pragma once

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <source_location>
#include <string_view>
#include <syncstream>
#include <vector>

namespace remy {

enum class LogLevel {
  Trace,
  Info,
  Warning,
  Error,
  Critical,
};

/**
 * Provides a way to send logs in different places via Observers.
 * Example: UI can show logs on screen, server can write to stdout, both can simultaniously save these logs to file.
 */
class DistLog {
 public:
  class Observer {
   public:
    virtual void log(LogLevel, std::string_view) = 0;
    virtual ~Observer() = default;
  };

  void addObserver(std::weak_ptr<Observer> observer) {
    std::unique_lock l(m_mtx);
    m_observers.push_back(observer);
  }

  template <class... Args>
  std::string trace(std::format_string<Args...> fmt, Args &&...args) {
    return log(LogLevel::Trace, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  std::string info(std::format_string<Args...> fmt, Args &&...args) {
    return log(LogLevel::Info, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  std::string warn(std::format_string<Args...> fmt, Args &&...args) {
    return log(LogLevel::Warning, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  std::string err(std::format_string<Args...> fmt, Args &&...args) {
    return log(LogLevel::Error, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  std::string crit(std::format_string<Args...> fmt, Args &&...args) {
    return log(LogLevel::Critical, fmt, std::forward<Args>(args)...);
  }

  template <class... Args>
  std::string log(LogLevel level, std::format_string<Args...> fmt, Args &&...args) {
    auto line = std::format(fmt, std::forward<Args>(args)...);
    if (send_to_observers(level, line)) {
      cleanup_observers();
    }
    return line;
  }

 private:
  // Returns true if needs cleanup
  bool send_to_observers(LogLevel level, std::string_view line) {
    std::shared_lock l(m_mtx);

    bool needs_cleanup = false;
    for (const auto &obs_weak : m_observers) {
      if (auto obs = obs_weak.lock(); obs) {
        try {
          obs->log(level, line);
        } catch (...) {
          // Just protect user
        }
      } else {
        needs_cleanup = true;
      }
    }
    return needs_cleanup;
  }
  void cleanup_observers() {
    std::unique_lock l(m_mtx);
    m_observers.erase(std::remove_if(m_observers.begin(), m_observers.end(), [](auto weak) { return weak.expired(); }));
  }

  std::shared_mutex m_mtx;
  std::vector<std::weak_ptr<Observer>> m_observers;

  class Guard {
   public:
    Guard(DistLog &logger, std::string_view func)
        : m_logger(logger)
        , m_func_name(func) {
      m_logger.trace("++{}", m_func_name);
    }
    ~Guard() { m_logger.trace("--{}", m_func_name); }

   private:
    DistLog &m_logger;
    std::string m_func_name;
  };

 public:
  Guard guard(std::string_view name) { return {*this, name}; }
  Guard guard(const std::source_location location = std::source_location::current()) {
    return guard(location.function_name());
  }
};

inline DistLog Log;

static std::string_view to_str(LogLevel level) {
  switch (level) {
    using enum LogLevel;
    case Trace:
      return "Trace";
    case Info:
      return "Info";
    case Warning:
      return "Warning";
    case Error:
      return "Error";
    case Critical:
      return "Critical";
    default:
      return "(Unknown)";
  }
}

static std::string_view ansi_color(LogLevel level) {
  switch (level) {
    using enum LogLevel;
    case Info:
      return "\033[32m";
    case Warning:
      return "\033[33m";
    case Error:
      return "\033[31m";
    case Critical:
      return "\033[31;1;4m";
    default:
      return "\033[0m";
  }
}

class CoutLogWriter final : public DistLog::Observer {
 public:
  void log(LogLevel level, std::string_view line) override {
    std::osyncstream(std::cout) << ansi_color(level) << to_str(level) << " " << line << "\033[0m" << std::endl;
  }
};

class FileLogWriter final : public DistLog::Observer {
 public:
  explicit FileLogWriter(const std::filesystem::path &path,
                         std::ios_base::openmode mode = std::ios::app | std::ios::out)
      : m_file(path, mode) {
    m_file.exceptions(std::ios::goodbit);
  }
  void log(LogLevel level, std::string_view line) override {
    const std::time_t curTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm timeInfo = {};
    (void)localtime_r(&curTime, &timeInfo);
    std::array<char, 128> timeBuf;
    (void)std::strftime(timeBuf.data(), timeBuf.size(), "%c", &timeInfo);

    m_file << timeBuf.data() << " [" << to_str(level) << "]: " << line << std::endl;
  }

 private:
  std::ofstream m_file;
};

#ifdef DEBUG_OUT
#define DBG(x) x
#else
#define DBG(x)
#endif

}  // namespace remy
