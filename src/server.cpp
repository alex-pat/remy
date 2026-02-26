#include "remy/server.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/completion_condition.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>

#include "remy/common.hpp"
#include "remy/logger.hpp"
#include "remy/serde.hpp"
#include "remy/utils.hpp"

namespace remy {

using namespace boost;
using namespace boost::asio::ip;
using namespace boost::asio::experimental::awaitable_operators;
using namespace std::chrono_literals;

asio::awaitable<void> Server::run(const Config& conf, asio::signal_set& signals) {
  try {
    auto l = Log.guard();
    auto executor = co_await asio::this_coro::executor;
    auto server = std::make_shared<Server>(executor);
    tcp::acceptor acceptor(executor, {make_address(conf.addr), conf.port});

    while (true) {
      tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);

      co_spawn(
          executor,
          [server, sock = std::move(socket)]() mutable -> asio::awaitable<void> {
            co_await server->process_new_connection(std::move(sock));
          },
          asio::detached);
    }
  } catch (const std::exception& e) {
    Log.crit("Server error: {}", e.what());
  }
  signals.cancel();
}

Server::Server(asio::any_io_executor& ctx)
    : m_io_context(ctx) {
  std::srand(std::time({}));
}

asio::awaitable<void> Server::process_new_connection(tcp::socket&& socket) {
  auto ex = co_await asio::this_coro::executor;
  auto endpoint = socket.remote_endpoint();
  auto endpoint_name = std::format("{}:{}", endpoint.address().to_string(), endpoint.port());
  try {
    auto l = Log.guard();
    auto conn_type = co_await serde::recv<ConnectionType>(socket);

    switch (conn_type) {
      using enum ConnectionType;
      case Control:
        Log.info("New Control conn {}", endpoint_name);
        co_await process_control_conn(std::move(socket));
        break;
      case Browser: {
        Log.info("New Browser conn {}", endpoint_name);
        auto token = co_await serde::recv<ServerBrowserToken>(socket);
        auto& channel = get_sockets_chan(m_pending_browsers, token);
        co_await channel.async_send({}, socket.release(), asio::use_awaitable);
      } break;
      case Copy: {
        Log.info("New Copy conn {}", endpoint_name);
        auto token = co_await serde::recv<CopyToken>(socket);
        auto& channel = get_sockets_chan(m_pending_copy_ops, token);
        co_await channel.async_send({}, socket.release(), asio::use_awaitable);
      } break;
    }
  } catch (const std::exception& e) {
    Log.err("Conn {} error: {}", endpoint_name, e.what());
  }
}

asio::awaitable<void> Server::process_control_conn(tcp::socket&& socket) {
  auto l = Log.guard();
  std::shared_ptr<ClientCtrl> client;
  {
    std::lock_guard lock(m_mtx);
    // Assigning a new unique id to client has logic similar to `pid`s in kernel
    while (m_clients.contains(m_next_allocated_client_id)) {
      m_next_allocated_client_id =
          m_next_allocated_client_id != std::numeric_limits<ClientId>::max()  // overflowing increment
              ? (m_next_allocated_client_id + 1)
              : 0;
    }
    client = std::make_shared<ClientCtrl>(m_next_allocated_client_id, shared_from_this(), std::move(socket));
    m_clients[m_next_allocated_client_id] = client;
  }

  co_await clients_list_notify_all();
  try {
    co_await (client->incoming_messages() || client->outcoming_messages());
  } catch (const std::exception& e) {
    Log.err("Client {} err: {}", client->m_id, e.what());
  }
  {
    std::lock_guard l(m_mtx);
    m_clients.erase(client->m_id);
  }
  co_await clients_list_notify_all();
}

ClientsListPayload Server::get_clients_descs() {
  std::lock_guard l(m_mtx);
  ClientsListPayload result;

  for (const auto& [id, client] : m_clients) {
    result.emplace_back(id, client->name());
  }
  return result;
}
asio::awaitable<void> Server::clients_list_notify_all() {
  auto l = Log.guard();
  auto list = get_clients_descs();
  for (const auto &[id, client] : m_clients) {
    DBG(Log.trace("sending to {}", id);)
    co_await client->send_clients_list(list);
  }
}
ClientCtrl::ClientCtrl(ClientId id, std::shared_ptr<Server> server, tcp::socket&& socket)
    : m_id(id)
    , m_server(server)
    , m_socket(std::move(socket))
    , m_send_requests(m_socket.get_executor(), 20) {
  auto endpoint = m_socket.remote_endpoint();
  m_endpoint_str = std::format("{}:{}", endpoint.address().to_string(), endpoint.port());
  m_name = m_endpoint_str;
}

ClientCtrl::~ClientCtrl() { Log.trace("Destroying client {} ({})", m_id, m_name); }

asio::awaitable<void> ClientCtrl::incoming_messages() {
  auto l = Log.guard();
  while (m_socket.is_open()) {
    ClientControlMsgType msg_type;
    auto [err, _] = co_await asio::async_read(m_socket, asio::buffer(&msg_type, sizeof(msg_type)),
                                              asio::as_tuple(asio::use_awaitable));
    if (err) {
      break;
    }

    DBG(Log.info("Conn {}: msg {}", m_id, static_cast<int>(msg_type));)
    switch (msg_type) {
      using enum ClientControlMsgType;
      case SetName: {
        auto new_name = co_await serde::recv<ClientName>(m_socket);
        utils::trim_end(new_name);

        new_name = std::format("{} ({})", new_name, m_endpoint_str);

        Log.info("Setting client name ({}) '{}' -> '{}'", m_id, m_name, new_name);
        m_name = new_name;
        co_await m_send_requests.async_send(
            {},
            [this, name = std::move(new_name)]() -> asio::awaitable<void> {
              co_await serde::send(m_socket, ServerControlMsgType::ClientName);
              co_await serde::send(m_socket, name);
              co_await m_server->clients_list_notify_all();
            },
            asio::use_awaitable);
      } break;
      case GetClientsList: {
        auto clients = m_server->get_clients_descs();
        co_await send_clients_list(clients);
      } break;
      case ConnectBrowser: {
        auto client_token = co_await serde::recv<ClientBrowserToken>(m_socket);
        auto browser_host_id = co_await serde::recv<ClientId>(m_socket);
        asio::co_spawn(m_socket.get_executor(), m_server->process_browser_conn(browser_host_id, m_id, client_token),
                       asio::detached);
      } break;
      case Copy: {
        auto src = co_await serde::recv<RemoteDentries>(m_socket);
        auto dst = co_await serde::recv<RemoteDentry>(m_socket);
        DBG(Log.info("sending from {}:{} to {}:{}", src.id, src.basedir, dst.first, dst.second);)
        asio::co_spawn(m_socket.get_executor(),
                       m_server->process_copy(std::move(src), std::move(dst), weak_from_this()), asio::detached);
      } break;
    }
  }
  m_send_requests.close();
}
asio::awaitable<void> ClientCtrl::outcoming_messages() {
  auto l = Log.guard();
  // Explicitly send id on start
  co_await serde::send(m_socket, ServerControlMsgType::ClientId);
  co_await serde::send(m_socket, m_id);

  // Explicitly send (default) name on start
  co_await serde::send(m_socket, ServerControlMsgType::ClientName);
  co_await serde::send(m_socket, m_name);

  while (m_send_requests.is_open()) {
    auto [err, callback] = co_await m_send_requests.async_receive(asio::as_tuple(asio::use_awaitable));
    if (err) {
      break;
    }
    co_await callback();
  }
}

asio::awaitable<void> ClientCtrl::send_clients_list(const ClientsListPayload& list) {
  co_await m_send_requests.async_send(
      {},
      [this, list]() -> asio::awaitable<void> {
        co_await serde::send(m_socket, ServerControlMsgType::ClientsList);
        co_await serde::send(m_socket, list);
      },
      asio::use_awaitable);
}
asio::awaitable<void> ClientCtrl::request_browser_host(ServerBrowserToken token) {
  co_await m_send_requests.async_send(
      {},
      [this, token]() -> asio::awaitable<void> {
        co_await serde::send(m_socket, ServerControlMsgType::ConnectBrowserRequest);
        co_await serde::send(m_socket, token);
      },
      asio::use_awaitable);
}
asio::awaitable<void> ClientCtrl::request_browser_reader(ClientBrowserToken client_token,
                                                         std::optional<ServerBrowserToken> server_token) {
  co_await m_send_requests.async_send(
      {},
      [this, client_token, server_token]() -> asio::awaitable<void> {
        co_await serde::send(m_socket, ServerControlMsgType::BrowserConnected);
        co_await serde::send(m_socket, client_token);
        co_await serde::send(m_socket, server_token);
      },
      asio::use_awaitable);
}
asio::awaitable<void> ClientCtrl::request_copy_source(CopyToken token, std::string&& path,
                                                      std::vector<std::string>&& dentries) {
  co_await m_send_requests.async_send(
      {},
      [this, token, path = std::move(path), dentries = std::move(dentries)]() -> asio::awaitable<void> {
        co_await serde::send(m_socket, ServerControlMsgType::CopyRequest);
        co_await serde::send(m_socket, token);
        co_await serde::send(m_socket, CopyRole::Source);
        co_await serde::send(m_socket, path);
        co_await serde::send(m_socket, dentries);
      },
      asio::use_awaitable);
}

asio::awaitable<void> ClientCtrl::request_copy_destination(CopyToken token, std::string&& path) {
  co_await m_send_requests.async_send(
      {},
      [this, token, path = std::move(path)]() -> asio::awaitable<void> {
        co_await serde::send(m_socket, ServerControlMsgType::CopyRequest);
        co_await serde::send(m_socket, token);
        co_await serde::send(m_socket, CopyRole::Destination);
        co_await serde::send(m_socket, path);
      },
      asio::use_awaitable);
}

asio::awaitable<void> ClientCtrl::send_watcher_info(std::optional<WatcherInfo> info) {
  try {
    co_await m_send_requests.async_send(
        {},
        [this, info = std::move(info)]() -> asio::awaitable<void> {
          co_await serde::send(m_socket, ServerControlMsgType::WatcherInfo);
          co_await serde::send(m_socket, info);
        },
        asio::use_awaitable);
  } catch (const std::exception& e) {
    Log.err("send_watcher_info: {}", e.what());
  }
}

namespace {
/** Just resends bytes in two directions */
asio::awaitable<void> simple_proxy(tcp::socket& first, tcp::socket& second) {
  auto proxy = [](tcp::socket& src, tcp::socket& dst) -> asio::awaitable<void> {
    std::vector<char> buf(4096);
    while (true) {
      auto size = co_await asio::async_read(src, asio::buffer(buf.data(), buf.size()), asio::transfer_at_least(1),
                                            asio::use_awaitable);
      co_await asio::async_write(dst, asio::buffer(buf.data(), size), asio::use_awaitable);
    }
  };
  try {
    co_await (proxy(first, second) || proxy(second, first));
  } catch (const std::exception &e) {
    Log.trace("simple_proxy: {}", e.what());
  }
}
}  // namespace

/** Performs logic of requesting `Browser` connections, receives new sockets and runs proxy */
asio::awaitable<void> Server::process_browser_conn(ClientId host_id, ClientId reader_id,
                                                   ClientBrowserToken client_token) {
  auto l = Log.guard();
  auto ex = co_await asio::this_coro::executor;
  std::shared_ptr<ClientCtrl> host;
  std::shared_ptr<ClientCtrl> reader;
  bool success = true;

  {
    std::lock_guard l(m_mtx);
    host = m_clients.at(host_id);
    reader = m_clients.at(reader_id);
  }

  try {
    auto server_token = static_cast<ServerBrowserToken>(std::rand());
    DBG(Log.trace("server token={}", server_token);)
    DBG(Log.trace("client token={}", client_token);)

    auto& sockets_chan = new_sockets_chan(m_pending_browsers, server_token);
    DBG(Log.trace("created channel");)

    co_await host->request_browser_host(server_token); // timeout?

    tcp::socket host_socket(ex);
    host_socket.assign(tcp::v4(), co_await sockets_chan.async_receive(asio::use_awaitable));
    DBG(Log.trace("got host sock");)
    co_await reader->request_browser_reader(client_token, server_token);

    tcp::socket reader_socket(ex);
    reader_socket.assign(tcp::v4(), co_await sockets_chan.async_receive(asio::use_awaitable));
    DBG(Log.trace("got reader sock");)

    delete_sockets_chan(m_pending_browsers, server_token);

    co_await simple_proxy(host_socket, reader_socket);

    Log.info("Browser stopped (reader:{}, host:{})", reader_id, host_id);
  } catch (const std::exception& e) {
    Log.err("process_browser_conn: {}", e.what());
    success = false;
  }
  if (!success) {
    // Notify client that establishing connection failed
    co_await reader->request_browser_reader(client_token, std::nullopt);
  }
}

namespace {
/** Resends data of particular type and returns it. See `Server::process_copy` */
template <typename T>
asio::awaitable<T> msg_proxy(tcp::socket& sock_from, tcp::socket& sock_to) {
  auto data = co_await serde::recv<T>(sock_from);
  co_await serde::send(sock_to, data);
  co_return data;
}
}  // namespace

asio::awaitable<std::pair<tcp::socket, tcp::socket>> Server::copy_handshake(RemoteDentries&& src_info,
                                                                            RemoteDentry&& dst_info) {
  auto ex = co_await boost::asio::this_coro::executor;
  auto [dst_id, dst_path] = std::move(dst_info);

  std::shared_ptr<ClientCtrl> src;
  std::shared_ptr<ClientCtrl> dst;
  {
    std::lock_guard l(m_mtx);
    src = m_clients.at(src_info.id);
    dst = m_clients.at(dst_id);
  }

  auto token = static_cast<CopyToken>(std::rand());
  DBG(Log.trace("Generated copy token={}", token);)

  auto& sockets_chan = new_sockets_chan(m_pending_copy_ops, token);
  DBG(Log.trace("created copy chan");)

  co_await src->request_copy_source(token, std::move(src_info.basedir), std::move(src_info.dentries));
  DBG(Log.trace("sent request to src");)

  tcp::socket src_socket(ex);
  src_socket.assign(tcp::v4(), co_await sockets_chan.async_receive(asio::use_awaitable));
  DBG(Log.trace("got src sock");)

  co_await dst->request_copy_destination(token, std::move(dst_path));
  DBG(Log.trace("sent request to dst");)

  tcp::socket dst_socket(ex);
  dst_socket.assign(tcp::v4(), co_await sockets_chan.async_receive(asio::use_awaitable));
  DBG(Log.trace("got dst sock");)
  co_return std::pair{std::move(src_socket), std::move(dst_socket)};
}

/** Connects together `Copy` connections and performs smart proxy, calculating progress and notifying the Watcher */
asio::awaitable<void> Server::process_copy(RemoteDentries&& src_info, RemoteDentry&& dst_info,
                                           std::weak_ptr<ClientCtrl> watcher) {
  auto l = Log.guard();
  auto ex = co_await asio::this_coro::executor;
  bool success = true;
  try {
    auto [src_socket, dst_socket] = co_await copy_handshake(std::move(src_info), std::move(dst_info));

    std::optional<WatcherInfo> info = WatcherInfo{};

    std::chrono::time_point<std::chrono::steady_clock> last_send;
    auto progress_send = [&info, watcher, &last_send](bool force_send = false) -> asio::awaitable<void> {
      // Send progress update, but not too frequently (and if watcher sill exists)
      auto cur_time = std::chrono::steady_clock::now();
      if (force_send || (cur_time - last_send) > 100ms) {
        if (auto watcher_ptr = watcher.lock(); watcher_ptr) {
          co_await watcher_ptr->send_watcher_info(info);
        }
        last_send = cur_time;
      }
    };
    auto files_count = co_await msg_proxy<uint64_t>(src_socket, dst_socket);
    info->files_all = files_count;
    co_await progress_send(true);

    std::vector<char> buf(4096);
    for (uint64_t file_num = 0; file_num < files_count; file_num++) {
      auto meta = co_await msg_proxy<FileMetainfo>(src_socket, dst_socket);
      auto path = co_await msg_proxy<std::string>(src_socket, dst_socket);

      info->cur_file = path;
      info->cur_size = meta.size;
      info->cur_progress = 0;
      co_await progress_send();

      if (S_ISREG(meta.mode)) {
        for (info->cur_progress = 0; info->cur_progress < meta.size;) {
          auto read_size = std::min<uint64_t>(buf.size(), meta.size - info->cur_progress);
          read_size = co_await asio::async_read(src_socket, asio::buffer(buf.data(), read_size),
                                                asio::transfer_at_least(1), asio::use_awaitable);
          co_await asio::async_write(dst_socket, asio::buffer(buf.data(), read_size), asio::use_awaitable);
          info->cur_progress += read_size;
          co_await progress_send();
        }
      } else if (S_ISLNK(meta.mode)) {
        co_await msg_proxy<std::string>(src_socket, dst_socket);
        info->cur_progress = meta.size;
      } else if (S_ISDIR(meta.mode)) {
        info->cur_progress = meta.size;
      }

      info->files_completed++;
      co_await progress_send();
    }
    co_await progress_send(true);
  } catch (const std::exception& e) {
    Log.err("process_copy: {}", e.what());
    success = false;
  }
  if (!success) {
    if (auto wtch = watcher.lock(); wtch) {
      co_await wtch->send_watcher_info(std::nullopt);
    }
  }
}

template <typename Key>
Server::NewSocketsChan& Server::new_sockets_chan(std::unordered_map<Key, Server::NewSocketsChan>& chan_map, Key token) {
  std::lock_guard l(m_mtx);
  auto [it, success] = chan_map.try_emplace(token, m_io_context);
  if (!success) {
    throw std::runtime_error{"Can't create new socket stream"};
  }
  return it->second;
}
template <typename Key>
Server::NewSocketsChan& Server::get_sockets_chan(std::unordered_map<Key, Server::NewSocketsChan>& chan_map, Key token) {
  std::lock_guard l(m_mtx);
  return chan_map.at(token);
}
template <typename Key>
void Server::delete_sockets_chan(std::unordered_map<Key, Server::NewSocketsChan>& chan_map, Key token) {
  std::lock_guard l(m_mtx);
  chan_map.erase(token);
}

}  // namespace remy
