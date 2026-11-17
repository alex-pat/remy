#include "remy/view-client-noui.hpp"

#include <iostream>

#include "remy/logger.hpp"

namespace remy {

ClientNoUI::ClientNoUI(std::shared_ptr<ClientNet> client)
    : m_client(std::move(client))
    , m_io_context()
    , m_signals(m_io_context, SIGINT, SIGTERM) {
  m_signals.async_wait([this](const boost::system::error_code &ec, int) {
    if (!ec) {
      Log.info("Received termination signal, stopping...");
      m_io_context.stop();
    }
  });
}

ClientNoUI::~ClientNoUI() {
  if (m_client) {
    m_client->stop();
  }
}

void ClientNoUI::run() {
  m_client->run(weak_from_this());
  m_io_context.run();
}

void ClientNoUI::update_name(ClientName &&name) {
  Log.info("Client registered with name: '{}'", name);
}

void ClientNoUI::show_warning(std::string &&err_msg) {
  Log.warn("Warning: {}", err_msg);
}

void ClientNoUI::critical_error(std::string &&err_msg) {
  Log.crit("Critical error: {}", err_msg);
  m_io_context.stop();
}

}  // namespace remy
