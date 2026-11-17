#pragma once

#include <memory>
#include <optional>
#include <string>

#include "remy/common.hpp"

namespace remy {

/**
 * Base interface for Remy Client User Interfaces (TUI, Headless / NoUI, etc.)
 */
class IClientUI : public std::enable_shared_from_this<IClientUI> {
 public:
  virtual ~IClientUI() = default;

  virtual void run() = 0;

  // Callbacks invoked by ClientNet
  virtual void update_clients(const ClientsListPayload &clients) {}
  virtual void update_name(ClientName &&name) {}
  virtual void show_dents(UiBrowserId id, PathDentsPayload &&path) {}
  virtual void show_warning(std::string &&err_msg) = 0;
  virtual void critical_error(std::string &&err_msg) = 0;
  virtual void browser_error(UiBrowserId id) {}
  virtual void update_watcher_info(std::optional<WatcherInfo> &&info) {}
  virtual void show_delete_result(std::string &&msg) {}
  virtual void show_mkdir_result(std::string &&msg) {}
};

}  // namespace remy
