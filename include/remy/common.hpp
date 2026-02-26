#pragma once

#include <linux/stat.h>

#include <bit>
#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace remy {

struct Config {
  std::string addr;
  uint16_t port;
  std::string log_path;
};

/** Connection type, sent by client right after connecting */
enum class ConnectionType : uint8_t {
  /**
   * Main client-server connection for working with general info and requesting connections/actions.
   * After creating connection, server sends `ServerControlMsgType::ClientId` with unique id of this client.
   * See also: `ClientControlMsgType`, `ServerControlMsgType`
   */
  Control,
  /**
   * Allows one client ('BrowserClient') to browse directories of other client ('BrowserHost'). Connected only after
   * request from server: it generates `ServerBrowserToken` which should be sent by clients after `ConnectionType`. Then
   * server works as proxy and clients exchange `BrowserClientMsgType` and `BrowserHostMsgType` messages.
   */
  Browser,
  /**
   * Used by both Sender and Receiver of files. Clients should provide server-generated `CopyToken`.
   * Server works as proxy but also notifies a client who requested to copy ('Watcher').
   * Later payload: number of files + [FileMetainfo + string(path) + type-dependent]+
   */
  Copy,
};

/** Client->Server messages/requests */
enum class ClientControlMsgType : uint8_t {
  /** Set new name for this client. Followed by `ClientName`. Answer: `ServerControlMsgType::ClientName` */
  SetName,
  /** Request to receive list of all clients connected.
   *  Answer: `ServerControlMsgType::ClientsList` + `ClientsListPayload` */
  GetClientsList,
  /** Request to create Browser connections for this client as 'BrowserReader'.
   *  Payload is `ClientBrowserToken` (client-generated token) + `ClientId` (BrowserHost id) */
  ConnectBrowser,
  /** Request to copy files from one client to another.
   *  Payload is `RemoteDentries` of source + `RemoteDentry` of destination.
   *  After that `ServerControlMsgType::WatcherInfo` will be sent with copy progress. */
  Copy,
};
/** Server->Client messages/requests */
enum class ServerControlMsgType : uint8_t {
  /** Unique id of this client, sent first after connecting. Payload: `ClientId`(uint64_t) */
  ClientId,
  /** New name of this client. Payload: `ClientName`(string) */
  ClientName,
  /** List of all currently connected clients. See `ClientsListPayload`. Can be sent without request, on updates */
  ClientsList,
  /** Notifies that BrowserHost is connected to server, so now this client (BrowserClient) should create its `Browser`
   * connection. Payload: `ClientBrowserToken` + `optional<ServerBrowserToken>`(nullopt if something failed) */
  BrowserConnected,
  /** Requests a client to create `Browser` connection to be BrowserHost. Payload is `ServerBrowserToken` */
  ConnectBrowserRequest,
  /** Request to create `Copy` connection.
   *  Payload: `CopyToken` + `CopyRole` + string(path - source base or destination directory depending on role)
   *  + (vector<string> (source dentries, if source role) */
  CopyRequest,
  WatcherInfo,
};

/** Browser connection: BrowserClient->BrowserHost message */
enum class BrowserClientMsgType : uint8_t {
  /** Payload is string(path) */
  GetDents,
  /** Payload is `RemoteDentries` */
  Delete,
};
/** Browser connection: BrowserHost->BrowserClient message */
enum class BrowserHostMsgType : uint8_t {
  /** Answer to GetDents. Payload is `PathDentsPayload` */
  PathDents,
  /** Answer to Delete. Payload is string(result message) */
  DeleteResponse,
};

static_assert(std::endian::native == std::endian::little);
struct FileMetainfo {
  uint64_t size;
  uint32_t uid;
  uint32_t gid;
  uint16_t mode;

  statx_timestamp atime;
  statx_timestamp mtime;
};

using ClientId = uint64_t;
using ClientName = std::string;

using ClientsListPayload = std::vector<std::pair<ClientId, ClientName>>;
using ClientBrowserToken = uint64_t;
using ServerBrowserToken = uint64_t;
/** UI pane id. Used in communication between ClientUi and ClientNet to understand which `Browser` connection
 * corresponds to which UI pane. */
using UiBrowserId = size_t;

/**
 * Each Control/Browser connection can asyncronously send and receive messages, so for each connection run two
 * coroutines concurrently: one reads from socket (usually named `incomming_messages()`) and one writes to socket
 * (usually named `outcomming_messages()`) and only reading coroutine should read and only sending coroutine should
 * send. Outcomming messages can come from different sources (e.g. server can send message as response to incoming
 * message, or client can send a message by request from UI). So all logic for outcoming messages is placed in function
 * `SendAction` sent to `SendsChannel` - sending coroutine will read actions from channel and call.
 */
using SendAction = std::function<boost::asio::awaitable<void>()>;
using SendsChannel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, SendAction)>;

struct Dentry {
  std::string basename;
  FileMetainfo metainfo = {};
};
using PathDents = std::vector<Dentry>;
using PathDentsPayload = std::pair<std::string, std::optional<PathDents>>;

/** Several directory items on a remote client. Used in Copy and Delete operations */
struct RemoteDentries {
  ClientId id;
  std::string basedir;
  std::vector<std::string> dentries;
};
/** Path at a remote client */
using RemoteDentry = std::pair<ClientId, std::string>;

/** Server-generated unique id of copy operation */
using CopyToken = uint64_t;
enum class CopyRole : uint8_t {
  Source,
  Destination,
};

/** Progress of copy operation */
struct WatcherInfo {
  uint64_t files_all;
  uint64_t files_completed;

  uint64_t cur_size;
  uint64_t cur_progress;
  std::string cur_file;
};

constexpr uint16_t REMY_DEFAULT_PORT = 7312;

}  // namespace remy
