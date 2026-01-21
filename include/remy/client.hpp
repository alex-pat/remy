#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <filesystem>
#include <memory>

#include "remy/common.hpp"
#include "remy/logger.hpp"
#include "remy/view-client.hpp"

namespace remy {

class ClientUi;

/** Network-related part of client logic */
class ClientNet final : public std::enable_shared_from_this<ClientNet> {
 public:
  explicit ClientNet(const Config &conf);

  void run(std::weak_ptr<ClientUi> ui);
  void stop();

  void request_clients_list();
  void set_new_name(const ClientName &name);

  void request_browser_connection(UiBrowserId browser_id, ClientId host_id);
  void cd(UiBrowserId id, const std::string &dir);
  void disconnect_browser(UiBrowserId id);

  size_t browser_hosts_count() const { return m_browser_hosts_count; }

  void request_copy(RemoteDentry from, RemoteDentry to);

  const Config &m_conf;

 private:
  void set_default_hostname();
  boost::asio::awaitable<void> run_control_coro();
  boost::asio::awaitable<void> incoming_control_msg();
  boost::asio::awaitable<void> outcoming_control_msg();

  boost::asio::io_context m_io_context;
  boost::asio::ip::tcp::endpoint m_endpoint;
  boost::asio::ip::tcp::socket m_control_sock;
  std::jthread m_net_thread;  // Thread where async executor runs
  std::weak_ptr<ClientUi> m_ui;

  std::mutex m_mtx;
  ClientId m_id;
  /** Map between local Browser connection id and UI pane id */
  std::unordered_map<ClientBrowserToken, UiBrowserId> m_browser_ids;

  SendsChannel m_send_requests;

  boost::asio::awaitable<void> run_browser_reader_coro(ClientBrowserToken client_token, ServerBrowserToken serv_token);
  class BrowserReader {
   public:
    explicit BrowserReader(UiBrowserId browser, boost::asio::io_context &ctx, std::weak_ptr<ClientUi> ui,
                           ServerBrowserToken token)
        : m_browser_id(browser)
        , m_ui(ui)
        , m_token(token)
        , m_socket(ctx)
        , m_send_requests(ctx) {}
    boost::asio::awaitable<void> connect(const boost::asio::ip::tcp::endpoint &endpoint);
    void stop();
    void cd(const std::string &dir);

    boost::asio::awaitable<void> outcoming_msg();
    boost::asio::awaitable<void> incoming_msg();

   private:
    UiBrowserId m_browser_id;
    std::weak_ptr<ClientUi> m_ui;
    ServerBrowserToken m_token;
    boost::asio::ip::tcp::socket m_socket;
    SendsChannel m_send_requests;
  };
  /** Which UI pane corresponds to which Browser connection */
  std::unordered_map<UiBrowserId, BrowserReader> m_browser_readers;

  std::atomic_size_t m_browser_hosts_count = 0;  // Info just for curiosity of BrowserHost
  boost::asio::awaitable<void> run_browser_host_coro(ServerBrowserToken serv_token);
  class BrowserHost {
   public:
    explicit BrowserHost(boost::asio::io_context &ctx, std::weak_ptr<ClientUi> ui, ServerBrowserToken token)
        : m_ui(ui)
        , m_token(token)
        , m_socket(ctx)
        , m_send_requests(ctx) {}
    boost::asio::awaitable<void> connect(const boost::asio::ip::tcp::endpoint &endpoint);

    boost::asio::awaitable<void> outcoming_msg();
    boost::asio::awaitable<void> incoming_msg();

   private:
    std::weak_ptr<ClientUi> m_ui;
    ServerBrowserToken m_token;
    boost::asio::ip::tcp::socket m_socket;
    SendsChannel m_send_requests;
  };

  class ClientCopy {
   public:
    ClientCopy(boost::asio::io_context &ctx, std::string &&path)
        : m_socket(ctx)
        , m_base_path(std::move(path)) {}
    virtual ~ClientCopy() = default;
    boost::asio::awaitable<void> connect(const boost::asio::ip::tcp::endpoint &, CopyToken);
    virtual boost::asio::awaitable<void> process() = 0;

   protected:
    boost::asio::ip::tcp::socket m_socket;
    std::string m_base_path;
  };

  template <class Copier>
    requires std::is_base_of_v<ClientCopy, Copier>
  boost::asio::awaitable<void> run_copy(CopyToken token, std::string &&path) {
    try {
      Copier sender{m_io_context, std::move(path)};
      co_await sender.connect(m_endpoint, token);
      co_await sender.process();
    } catch (const std::exception &e) {
      Log.err("{}: {}", std::source_location::current().function_name(), e.what());
    }
  }
  friend class CopySender;
  friend class CopyReceiver;
};

class CopySender : public ClientNet::ClientCopy {
 public:
  using ClientNet::ClientCopy::ClientCopy;

  boost::asio::awaitable<void> process() override;

 private:
  void collect_metadata();

  size_t m_base_len = 0;
  struct Dentry {
    std::filesystem::path path;
    FileMetainfo meta = {};
  };
  std::vector<Dentry> m_dentries;
};

class CopyReceiver : public ClientNet::ClientCopy {
 public:
  using ClientNet::ClientCopy::ClientCopy;

  boost::asio::awaitable<void> process() override;
};

}  // namespace remy
