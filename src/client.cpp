#include "remy/client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/system/detail/errc.hpp>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>

#include "remy/common.hpp"
#include "remy/logger.hpp"
#include "remy/serde.hpp"
#include "remy/sys.hpp"

namespace remy {

using namespace boost;
using namespace boost::asio::ip;
using namespace asio::experimental::awaitable_operators;

namespace {
void statx_to_metainfo(const std::filesystem::path& path, FileMetainfo& out_info);
void apply_attrs(const std::filesystem::path& path, const FileMetainfo& metainfo);
}  // namespace

ClientNet::ClientNet(const Config& conf)
    : m_conf(conf)
    , m_control_sock(m_io_context)
    , m_send_requests(m_io_context, 100) {
  std::srand(std::time({}));
}

void ClientNet::run(std::weak_ptr<ClientUi> ui) {
  m_ui = ui;

  asio::co_spawn(m_io_context, run_control_coro(), asio::detached);

  m_net_thread = std::jthread([this]() {
    try {
      m_io_context.run();
    } catch (const std::exception& e) {
      auto msg = Log.crit("Executor error: {}", e.what());
      m_ui.lock()->critical_error(std::move(msg));
    }
  });
}
void ClientNet::stop() {
  if (!m_io_context.stopped()) {
    m_io_context.stop();
    m_net_thread.join();
  }
}

void ClientNet::set_default_hostname() {
  auto hostname = remy::get_hostname();
  if (!hostname.empty()) {
    Log.info("Setting hostname: {}", hostname);
    set_new_name(hostname);
  }
}

asio::awaitable<void> ClientNet::run_control_coro() {
  try {
    auto l = Log.guard();
    tcp::resolver resolver(m_io_context);

    auto resolved = co_await resolver.async_resolve(m_conf.addr, "", asio::use_awaitable);

    m_endpoint = resolved.begin()->endpoint();
    m_endpoint.port(m_conf.port);

    co_await m_control_sock.async_connect(m_endpoint, asio::use_awaitable);

    co_await serde::send(m_control_sock, ConnectionType::Control);

    set_default_hostname();

    auto l1 = Log.guard("operator");
    co_await (incoming_control_msg() || outcoming_control_msg());

  } catch (const std::exception& e) {
    auto msg = Log.crit("Client control error: {}", e.what());
    m_ui.lock()->critical_error(std::move(msg));
  }
}

asio::awaitable<void> ClientNet::incoming_control_msg() {
  try {
    while (m_control_sock.is_open()) {
      ServerControlMsgType type;
      auto [err, _] = co_await asio::async_read(m_control_sock, asio::buffer(&type, sizeof(type)),
                                                asio::as_tuple(asio::use_awaitable));
      if (err) {
        auto msg = Log.crit("Can't receive control message: {} ({})", err.value(), err.message());
        m_ui.lock()->critical_error(std::move(msg));
        break;
      }

      switch (type) {
        using enum ServerControlMsgType;
        case ClientId: {
          m_id = co_await serde::recv<remy::ClientId>(m_control_sock);
        } break;
        case ClientName: {
          auto name = co_await serde::recv<remy::ClientName>(m_control_sock);
          m_ui.lock()->update_name(std::move(name));
        } break;
        case ClientsList: {
          auto clients = co_await serde::recv<ClientsListPayload>(m_control_sock);
          m_ui.lock()->update_clients(clients);
        } break;
        case BrowserConnected: {
          auto client_token = co_await serde::recv<ClientBrowserToken>(m_control_sock);
          auto server_token = co_await serde::recv<std::optional<ServerBrowserToken>>(m_control_sock);
          if (server_token.has_value()) {
            co_spawn(m_io_context, run_browser_reader_coro(client_token, *server_token), asio::detached);
          } else {
            std::lock_guard l(m_mtx);
            m_ui.lock()->browser_error(m_browser_ids[client_token]);
          }
        } break;
        case ConnectBrowserRequest: {
          auto server_token = co_await serde::recv<ServerBrowserToken>(m_control_sock);
          co_spawn(m_io_context, run_browser_host_coro(server_token), asio::detached);
        } break;
        case CopyRequest: {
          auto token = co_await serde::recv<CopyToken>(m_control_sock);
          auto role = co_await serde::recv<CopyRole>(m_control_sock);
          auto path = co_await serde::recv<std::string>(m_control_sock);
          if (role == CopyRole::Source) {
            auto dentries = co_await serde::recv<std::vector<std::string>>(m_control_sock);
            co_spawn(m_io_context, run_copy<CopySender>(token, std::move(path), std::move(dentries)), asio::detached);
          } else {
            co_spawn(m_io_context, run_copy<CopyReceiver>(token, std::move(path)), asio::detached);
          }
        } break;
        case WatcherInfo: {
          auto watcher_info = co_await serde::recv<std::optional<remy::WatcherInfo>>(m_control_sock);
          m_ui.lock()->update_watcher_info(std::move(watcher_info));
        } break;
      }
    }
  } catch (const std::exception& e) {
    auto msg = Log.crit("Incoming control err: {}", e.what());
    m_ui.lock()->critical_error(std::move(msg));
  }
  m_send_requests.close();
}
asio::awaitable<void> ClientNet::outcoming_control_msg() {
  auto l = Log.guard();
  co_await serde::send(m_control_sock, ClientControlMsgType::GetClientsList);  // already request a clients list

  while (m_send_requests.is_open()) {
    auto [err, callback] = co_await m_send_requests.async_receive(asio::as_tuple(asio::use_awaitable));
    if (err) {
      break;
    }
    co_await callback();
  }
  m_control_sock.close();
}

void send_request(SendsChannel& channel, SendAction func) {
  while (channel.try_send(boost::system::error_code{}, func) == false) {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(100ms);
  }
}

void ClientNet::request_clients_list() {
  send_request(m_send_requests, [this]() -> asio::awaitable<void> {
    co_await serde::send(m_control_sock, ClientControlMsgType::GetClientsList);
  });
}
void ClientNet::set_new_name(const ClientName& name) {
  send_request(m_send_requests, [this, new_name = name]() -> asio::awaitable<void> {
    co_await serde::send(m_control_sock, ClientControlMsgType::SetName);
    co_await serde::send(m_control_sock, new_name);
  });
}
void ClientNet::request_browser_connection(UiBrowserId browser_id, ClientId host_id) {
  send_request(m_send_requests, [this, browser_id, host_id]() -> asio::awaitable<void> {
    auto client_token = static_cast<ClientBrowserToken>(std::rand());
    // add to map
    {
      std::lock_guard l(m_mtx);
      m_browser_ids[client_token] = browser_id;
    }

    co_await serde::send(m_control_sock, ClientControlMsgType::ConnectBrowser);
    co_await serde::send(m_control_sock, client_token);
    co_await serde::send(m_control_sock, host_id);
  });
}
void ClientNet::request_copy(RemoteDentries from, RemoteDentry to) {
  send_request(m_send_requests, [this, from, to]() -> asio::awaitable<void> {
    co_await serde::send(m_control_sock, ClientControlMsgType::Copy);
    co_await serde::send(m_control_sock, from);
    co_await serde::send(m_control_sock, to);
    Log.trace("Copy request sent");
  });
}

asio::awaitable<void> ClientNet::run_browser_reader_coro(ClientBrowserToken client_token,
                                                         ServerBrowserToken serv_token) {
  BrowserReader* reader = nullptr;
  UiBrowserId browser_id;
  {
    std::lock_guard l(m_mtx);
    browser_id = m_browser_ids[client_token];
    auto [it, success] = m_browser_readers.try_emplace(browser_id, browser_id, m_io_context, m_ui, serv_token);
    if (!success) {
      auto msg = Log.warn("Can't allocate new connection");
      m_ui.lock()->show_warning(std::move(msg));
      co_return;
    }
    reader = &it->second;
  }

  try {
    co_await reader->connect(m_endpoint);
    Log.info("Client connected");
    co_await (reader->incoming_msg() || reader->outcoming_msg());
  } catch (const std::exception& e) {
    auto msg = Log.warn("Client-Client conenction error: {}", e.what());
    m_ui.lock()->show_warning(std::move(msg));
  }
  {
    std::lock_guard l(m_mtx);
    m_browser_readers.erase(browser_id);
  }
}

void ClientNet::disconnect_browser(UiBrowserId id) {
  std::lock_guard l(m_mtx);

  if (m_browser_readers.contains(id)) {
    m_browser_readers.at(id).stop();
  }
}

boost::asio::awaitable<void> ClientNet::BrowserReader::connect(const tcp::endpoint& endpoint) {
  co_await m_socket.async_connect(endpoint, asio::use_awaitable);

  co_await serde::send(m_socket, ConnectionType::Browser);
  co_await serde::send(m_socket, m_token);
}
void ClientNet::BrowserReader::stop() {
  if (m_socket.is_open()) {
    m_socket.close();
  }
}

boost::asio::awaitable<void> ClientNet::BrowserReader::incoming_msg() {
  try {
    while (m_socket.is_open()) {
      BrowserHostMsgType msg_type;
      auto [err, _] = co_await asio::async_read(m_socket, asio::buffer(&msg_type, sizeof(msg_type)),
                                                asio::as_tuple(asio::use_awaitable));
      if (err) {
        if (err != boost::system::errc::operation_canceled) {
          Log.err("{}", err.message());
          m_ui.lock()->browser_error(m_browser_id);
        }
        break;
      }
      switch (msg_type) {
        case BrowserHostMsgType::PathDents: {
          auto payload = co_await serde::recv<PathDentsPayload>(m_socket);
          Log.info("Received PathDentsPayload: {}: success={}", payload.first, payload.second.has_value());
          m_ui.lock()->show_dents(m_browser_id, std::move(payload));
        } break;
        case BrowserHostMsgType::DeleteResponse: {
          auto msg = co_await serde::recv<std::string>(m_socket);
          m_ui.lock()->show_delete_result(std::move(msg));
        } break;
        case BrowserHostMsgType::MkdirResponse: {
          auto msg = co_await serde::recv<std::string>(m_socket);
          m_ui.lock()->show_mkdir_result(std::move(msg));
        } break;
      }
    }
  } catch (const std::exception& e) {
    auto msg = Log.warn("Client-Client incoming msg error: {}", e.what());
    m_ui.lock()->browser_error(m_browser_id);
  }
  m_send_requests.close();
}
boost::asio::awaitable<void> ClientNet::BrowserReader::outcoming_msg() {
  co_await serde::send(m_socket, BrowserClientMsgType::GetDents);
  co_await serde::send(m_socket, std::string());

  while (m_send_requests.is_open()) {
    auto [err, callback] = co_await m_send_requests.async_receive(asio::as_tuple(asio::use_awaitable));
    if (err) {
      break;
    }
    co_await callback();
  }
  stop();
}

void ClientNet::cd(UiBrowserId id, const std::string& dir) {
  std::lock_guard l(m_mtx);
  m_browser_readers.at(id).cd(dir);
}
void ClientNet::request_delete(UiBrowserId id, RemoteDentries entries) {
  std::lock_guard l(m_mtx);
  m_browser_readers.at(id).request_delete(std::move(entries));
}
void ClientNet::request_mkdir(UiBrowserId id, const std::string& basedir, const std::string& name) {
  std::lock_guard l(m_mtx);
  m_browser_readers.at(id).request_mkdir(basedir, name);
}
void ClientNet::BrowserReader::cd(const std::string& new_dir) {
  send_request(m_send_requests, [this, dir = new_dir]() -> asio::awaitable<void> {
    co_await serde::send(m_socket, BrowserClientMsgType::GetDents);
    co_await serde::send(m_socket, dir);
  });
}
void ClientNet::BrowserReader::request_delete(RemoteDentries entries) {
  send_request(m_send_requests, [this, ents = std::move(entries)]() -> asio::awaitable<void> {
    co_await serde::send(m_socket, BrowserClientMsgType::Delete);
    co_await serde::send(m_socket, ents);
  });
}
void ClientNet::BrowserReader::request_mkdir(const std::string& basedir, const std::string& name) {
  send_request(m_send_requests, [this, bdir = basedir, n = name]() -> asio::awaitable<void> {
    co_await serde::send(m_socket, BrowserClientMsgType::Mkdir);
    co_await serde::send(m_socket, std::make_pair(bdir, n));
  });
}

asio::awaitable<void> ClientNet::run_browser_host_coro(ServerBrowserToken serv_token) {
  try {
    m_browser_hosts_count++;
    BrowserHost browser(m_io_context, m_ui, serv_token);
    co_await browser.connect(m_endpoint);

    co_await (browser.incoming_msg() || browser.outcoming_msg());
    Log.trace("BrowserHost: stopped");
  } catch (const std::exception& e) {
    auto msg = Log.warn("BrowserHost: {}", e.what());
    m_ui.lock()->show_warning(std::move(msg));
  }
  m_browser_hosts_count--;
}
boost::asio::awaitable<void> ClientNet::BrowserHost::connect(const tcp::endpoint& endpoint) {
  co_await m_socket.async_connect(endpoint, asio::use_awaitable);

  co_await serde::send(m_socket, ConnectionType::Browser);
  co_await serde::send(m_socket, m_token);
}
boost::asio::awaitable<void> ClientNet::BrowserHost::incoming_msg() {
  while (m_socket.is_open()) {
    BrowserClientMsgType msg_type;
    auto [err, _] = co_await asio::async_read(m_socket, asio::buffer(&msg_type, sizeof(msg_type)),
                                              asio::as_tuple(asio::use_awaitable));
    if (err) {
      break;
    }
    switch (msg_type) {
      case BrowserClientMsgType::GetDents: {
        auto dir = co_await serde::recv<std::string>(m_socket);
        co_await m_send_requests.async_send(
            {},
            [dir = std::move(dir), this]() -> asio::awaitable<void> {
              bool success = true;
              PathDents dents;
              try {
                std::filesystem::path dir_path(dir);
                if (dir_path.empty()) {
                  dir_path = std::filesystem::current_path();
                }
                for (const auto& dentry : std::filesystem::directory_iterator(dir_path)) {
                  FileMetainfo meta = {};
                  statx_to_metainfo(dentry.path(), meta);
                  dents.emplace_back(dentry.path().filename(), meta);
                }
              } catch (const std::exception& e) {
                Log.err("GetDents: {}", e.what());
                success = false;
              }
              co_await serde::send(m_socket, BrowserHostMsgType::PathDents);
              if (success) {
                co_await serde::send<PathDentsPayload>(m_socket, {dir, dents});
              } else {
                co_await serde::send<PathDentsPayload>(m_socket, {dir, std::nullopt});
              }
            },
            asio::use_awaitable);
        } break;
      case BrowserClientMsgType::Delete: {
        auto entries = co_await serde::recv<RemoteDentries>(m_socket);
        co_await m_send_requests.async_send(
            {},
            [ents = std::move(entries), this]() -> asio::awaitable<void> {
              std::uintmax_t removed_count = 0;
              std::string error_msg;
              std::filesystem::path base_path(ents.basedir);
              Log.info("Deleting: dir={}, {} items", ents.basedir, ents.dentries.size());

              for (const auto& name : ents.dentries) {
                std::error_code ec;
                removed_count += std::filesystem::remove_all(base_path / name, ec);
                if (ec) {
                  error_msg += std::format("\n{}: {}", name, ec.message());
                }
              }

              std::string result = std::format("Removed {} items.", removed_count);
              if (!error_msg.empty()) {
                result += " Errors:" + error_msg;
              }
              co_await serde::send(m_socket, BrowserHostMsgType::DeleteResponse);
              co_await serde::send(m_socket, result);
            },
            asio::use_awaitable);
      } break;
      case BrowserClientMsgType::Mkdir: {
        auto [basedir, name] = co_await serde::recv<std::pair<std::string, std::string>>(m_socket);
        co_await m_send_requests.async_send(
            {},
            [bdir = std::move(basedir), n = std::move(name), this]() -> asio::awaitable<void> {
              std::error_code ec;
              std::string result;
              bool created = std::filesystem::create_directory(std::filesystem::path(bdir) / n, ec);
              if (created) {
                result = std::format("Directory '{}' created successfully.", n);
              } else if (!ec) {
                result = std::format("Directory '{}' already exists.", n);
              } else {
                result = std::format("Failed to create directory '{}': {}", n, ec.message());
              }
              co_await serde::send(m_socket, BrowserHostMsgType::MkdirResponse);
              co_await serde::send(m_socket, result);
            },
            asio::use_awaitable);
      } break;
    }
  }
  m_send_requests.close();
}
boost::asio::awaitable<void> ClientNet::BrowserHost::outcoming_msg() {
  while (m_send_requests.is_open()) {
    auto [err, callback] = co_await m_send_requests.async_receive(asio::as_tuple(asio::use_awaitable));
    if (err) {
      break;
    }
    co_await callback();
  }
  m_socket.close();
}

asio::awaitable<void> ClientNet::ClientCopy::connect(const boost::asio::ip::tcp::endpoint& endpoint, CopyToken token) {
  co_await m_socket.async_connect(endpoint, asio::use_awaitable);

  co_await serde::send(m_socket, ConnectionType::Copy);
  co_await serde::send(m_socket, token);
}

/** Main logic of sending files */
asio::awaitable<void> CopySender::process() {
  collect_metadata();
  co_await serde::send<uint64_t>(m_socket, m_dentries.size());

  std::vector<char> buf(4096);
  for (const auto& [path, meta] : m_dentries) {
    Log.trace("File: {}", path.c_str());
    co_await serde::send(m_socket, meta);

    std::string remote_rel_path{path.c_str() + m_base_len};
    co_await serde::send(m_socket, remote_rel_path);
    DBG(Log.trace("sent rel path={}", remote_rel_path);)

    if (S_ISREG(meta.mode)) {
      std::ifstream file;
      file.exceptions(std::ifstream::badbit);
      file.open(path);

      for (size_t progress = 0; progress < meta.size;) {
        file.read(buf.data(), buf.size());
        auto read_size = file.gcount();
        co_await asio::async_write(m_socket, asio::buffer(buf.data(), read_size), asio::use_awaitable);
        progress += read_size;
      }
    } else if (S_ISLNK(meta.mode)) {
      auto target = std::filesystem::read_symlink(path);
      co_await serde::send<std::string>(m_socket, target);
    }  // S_ISDIR is ignored (nothing to do)
  }
  Log.info("Sending {} files completed", m_dentries.size());
}

/** Collects names and metadata of all files to be sent, to `m_dentries` */
void CopySender::collect_metadata() {
  FileMetainfo meta = {};

  while (!m_base_path.empty() && m_base_path.back() == '/') [[unlikely]] {
    m_base_path.resize(m_base_path.size() - 1);
  }

  std::filesystem::path base_path{m_base_path};
  m_base_len = m_base_path.size() > 0 ? m_base_path.size() + 1 : 0;

  DBG(Log.trace("base={}, len={}", m_base_path, m_base_len);)

  for (const auto& name : m_input_names) {
    auto path = base_path / name;
    statx_to_metainfo(path, meta);
    m_dentries.emplace_back(path, meta);
    if (!std::filesystem::is_directory(path)) {
      continue;
    }

    for (const auto& dentry : std::filesystem::recursive_directory_iterator(path)) {
      statx_to_metainfo(dentry.path(), meta);
      if (S_ISREG(meta.mode) || S_ISDIR(meta.mode) || S_ISLNK(meta.mode)) {
        m_dentries.emplace_back(dentry, meta);
      } else {
        Log.warn("File '{}' is ignored (not supported)", dentry.path().c_str());
      }
    }
  }
  Log.trace("[Sender] m_dentries.size()={}", m_dentries.size());
}

/** Main logic of receiving files */
asio::awaitable<void> CopyReceiver::process() {
  auto files_amount = co_await serde::recv<uint64_t>(m_socket);
  Log.trace("Receiver: files_amount {}", files_amount);

  std::vector<char> buf(4096);
  for (uint64_t i = 0; i < files_amount; i++) {
    auto meta = co_await serde::recv<FileMetainfo>(m_socket);
    auto rel_path = co_await serde::recv<std::string>(m_socket);
    DBG(Log.trace("Receiver: {} received path={}", i, rel_path);)

    auto path = std::filesystem::path{m_base_path} / rel_path;

    if (S_ISREG(meta.mode)) {
      std::ofstream file;
      file.exceptions(std::ofstream::badbit);
      file.open(path, std::ios::trunc | std::ios::binary);

      for (size_t progress = 0; progress < meta.size;) {
        // Log.trace("progress: {} / {}", progress, meta.size);
        auto read_size = std::min<uint64_t>(buf.size(), meta.size - progress);
        // Log.trace("trying to read: {} bytes", read_size);
        read_size = co_await asio::async_read(m_socket, asio::buffer(buf.data(), read_size), asio::use_awaitable);
        file.write(buf.data(), read_size);
        progress += read_size;
      }
    } else if (S_ISLNK(meta.mode)) {
      auto target = co_await serde::recv<std::string>(m_socket);
      std::filesystem::create_symlink(target, path);
    } else if (S_ISDIR(meta.mode)) {
      std::filesystem::create_directory(path);
    }
    apply_attrs(path, meta);
  }
  Log.info("Receiving {} files completed", files_amount);
}

namespace {
void statx_to_metainfo(const std::filesystem::path& path, FileMetainfo& out_info) {
#ifndef ANDROID
  struct statx statx_info;
  static constexpr auto STATX_MASK =
      STATX_SIZE | STATX_UID | STATX_GID | STATX_TYPE | STATX_MODE | STATX_ATIME | STATX_MTIME;

  if (statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_MASK, &statx_info) == -1) {
    const char* err_desc = strerror(errno);
    throw std::runtime_error{path.string() + ": statx: " + err_desc};
  }

  out_info.size = statx_info.stx_size;
  out_info.uid = statx_info.stx_uid;
  out_info.gid = statx_info.stx_gid;
  out_info.mode = statx_info.stx_mode;
  out_info.atime = statx_info.stx_atime;
  out_info.mtime = statx_info.stx_mtime;
#else
  struct stat stat_info;
  if (stat(path.c_str(), &stat_info) == -1) {
    const char* err_desc = strerror(errno);
    throw std::runtime_error{path.string() + ": stat: " + err_desc};
  }

  out_info.size = stat_info.st_size;
  out_info.uid = stat_info.st_uid;
  out_info.gid = stat_info.st_gid;
  out_info.mode = stat_info.st_mode;
  out_info.atime = {
      .tv_sec = stat_info.st_atim.tv_sec,
      .tv_nsec = static_cast<__u32>(stat_info.st_atim.tv_nsec),
  };
  out_info.mtime = {
      .tv_sec = stat_info.st_mtim.tv_sec,
      .tv_nsec = static_cast<__u32>(stat_info.st_mtim.tv_nsec),
  };
#endif
}

void apply_attrs(const std::filesystem::path& path, const FileMetainfo& metainfo) {
  if (chmod(path.c_str(), metainfo.mode & ~S_IFMT) == -1) {
    Log.err("'{}' chmod: {}", path.c_str(), strerror(errno));
  }

  const std::array times = {
      timespec{
          .tv_sec = static_cast<time_t>(metainfo.atime.tv_sec),
          .tv_nsec = static_cast<long>(metainfo.atime.tv_nsec),
      },
      timespec{
          .tv_sec = static_cast<time_t>(metainfo.mtime.tv_sec),
          .tv_nsec = static_cast<long>(metainfo.mtime.tv_nsec),
      },
  };
  if (utimensat(AT_FDCWD, path.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) == -1) {
    Log.err("'{}' utimensat: {}", path.c_str(), strerror(errno));
  }

  if (lchown(path.c_str(), metainfo.uid, metainfo.gid) == -1) {
    Log.err("'{}' lchown: {}", path.c_str(), strerror(errno));
  }
}
}  // namespace

}  // namespace remy
