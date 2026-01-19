#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/detail/error_code.hpp>
#include <memory>

#include "remy/common.hpp"

namespace remy {

class ClientCtrl;

class Server final : public std::enable_shared_from_this<Server> {
 public:
  explicit Server(boost::asio::any_io_executor &ctx);

  static boost::asio::awaitable<void> run(const Config &conf, boost::asio::signal_set &signals);

  boost::asio::awaitable<void> process_new_connection(boost::asio::ip::tcp::socket &&socket);

  boost::asio::awaitable<void> process_control_conn(boost::asio::ip::tcp::socket &&socket);
  boost::asio::awaitable<void> process_browser_conn(ClientId host, ClientId reader, ClientBrowserToken);
  boost::asio::awaitable<void> process_copy(RemoteDentry &&src, RemoteDentry &&dst, std::weak_ptr<ClientCtrl> watcher);

  /** Collects clients list for sending */
  ClientsListPayload get_clients_descs();
  /** Send clients list to all clients */
  boost::asio::awaitable<void> clients_list_notify_all();

  /**
   * Channel for sending socket. During establishing connecting two clients in `Browser` or `Copy` connections, new
   * connections of these types are sent to corresponding coroutines.
   */
  using NewSocketsChan = boost::asio::experimental::concurrent_channel<void(
      boost::system::error_code, boost::asio::ip::tcp::socket::native_handle_type &&)>;

 private:
  boost::asio::any_io_executor &m_io_context;
  ClientId m_next_allocated_client_id = 0;  // Used to assign new ids to clients

  /** Map of all objects assotiated to Control connections */
  std::unordered_map<ClientId, std::shared_ptr<ClientCtrl>> m_clients;

  /** Channels for each new `Browser` connections which are expected to be created. */
  std::unordered_map<ServerBrowserToken, NewSocketsChan> m_pending_browsers;
  /** Channels for each new `Copy` connections which are expected to be created. */
  std::unordered_map<CopyToken, NewSocketsChan> m_pending_copy_ops;
  std::mutex m_mtx;

  /** Generates `CopyToken`, asks clients to create `Copy` connections and returns connected sockets for source and destination */
  boost::asio::awaitable<std::pair<boost::asio::ip::tcp::socket, boost::asio::ip::tcp::socket>> copy_handshake(
      RemoteDentry &&src_info, RemoteDentry &&dst_info);

  // Helper functions to operate with `m_pending_browsers` and `m_pending_copy_ops`
  template <typename Key>
  NewSocketsChan &new_sockets_chan(std::unordered_map<Key, NewSocketsChan> &chan_map, Key token);
  template <typename Key>
  NewSocketsChan &get_sockets_chan(std::unordered_map<Key, NewSocketsChan> &chan_map, Key token);
  template <typename Key>
  void delete_sockets_chan(std::unordered_map<Key, NewSocketsChan> &chan_map, Key token);
};

/** All logic related to `Control` connection */
class ClientCtrl : public std::enable_shared_from_this<ClientCtrl> {
 public:
  explicit ClientCtrl(ClientId id, std::shared_ptr<Server> server, boost::asio::ip::tcp::socket &&socket);
  ~ClientCtrl();
  boost::asio::awaitable<void> incoming_messages();
  boost::asio::awaitable<void> outcoming_messages();

  boost::asio::awaitable<void> send_clients_list(const ClientsListPayload &clients);

  boost::asio::awaitable<void> request_browser_host(ServerBrowserToken);
  boost::asio::awaitable<void> request_browser_reader(ClientBrowserToken, std::optional<ServerBrowserToken>);
  boost::asio::awaitable<void> request_copy(CopyToken, CopyRole, std::string &&path);
  boost::asio::awaitable<void> send_watcher_info(std::optional<WatcherInfo>);

  const ClientName &name() const { return m_name; }

  const ClientId m_id;

 private:
  std::shared_ptr<Server> m_server;
  ClientName m_name;
  boost::asio::ip::tcp::socket m_socket;

  SendsChannel m_send_requests;
};

}  // namespace remy
