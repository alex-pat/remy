#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <memory>

#include "remy/client.hpp"
#include "remy/common.hpp"
#include "remy/view-client-iface.hpp"

namespace remy {

/**
 * Headless client implementation without TUI.
 * Connects, logs messages, and handles SIGINT / SIGTERM cleanly.
 */
class ClientNoUI final : public IClientUI {
 public:
  explicit ClientNoUI(std::shared_ptr<ClientNet> client);
  ~ClientNoUI() override;

  void run() override;

  void update_name(ClientName &&name) override;
  void show_warning(std::string &&err_msg) override;
  void critical_error(std::string &&err_msg) override;

 private:
  std::shared_ptr<ClientNet> m_client;
  boost::asio::io_context m_io_context;
  boost::asio::signal_set m_signals;
};

}  // namespace remy
