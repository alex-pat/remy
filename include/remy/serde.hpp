#pragma once

#include <boost/asio.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <optional>
#include <type_traits>
#include <utility>

#include "remy/common.hpp"

/**
 * Serialize / Deserialize
 * Sends some type in socket or receives it.
 * Types supported:
 * - plain data (primitive types, simple structs)
 * - std::string
 * - std::pair
 * - std::vector
 * - std::optional
 * - Dentry
 * - WatcherInfo
 * - and any combination of pair/vector/optional
 */

namespace remy::serde {

using namespace boost;
using namespace boost::asio::ip;

template <typename T>
concept pod = std::is_trivial_v<T> && std::is_standard_layout_v<T>;

template <typename Type>
asio::awaitable<Type> recv(tcp::socket& socket);

template <typename Type>
asio::awaitable<void> send(tcp::socket& socket, const Type& value);

// Trivially copyable
template <typename Type>
  requires pod<Type>
asio::awaitable<void> send(tcp::socket& socket, const Type& value) {
  co_await asio::async_write(socket, asio::buffer(&value, sizeof(Type)), asio::use_awaitable);
}

template <typename Type>
  requires pod<Type>
asio::awaitable<Type> recv(tcp::socket& socket) {
  Type value;
  co_await asio::async_read(socket, asio::buffer(&value, sizeof(Type)), asio::use_awaitable);
  co_return value;
}

// Additional declarations to prevent linkage issues
template <typename Tpair>
  requires std::is_same_v<Tpair, std::pair<typename Tpair::first_type, typename Tpair::second_type>>
asio::awaitable<void> send(tcp::socket& socket, const Tpair& pair);
template <typename Tpair>
  requires std::is_same_v<Tpair, std::pair<typename Tpair::first_type, typename Tpair::second_type>>
asio::awaitable<Tpair> recv(tcp::socket& socket);

template <typename Tvec>
  requires std::is_same_v<Tvec, std::vector<typename Tvec::value_type>>
asio::awaitable<void> send(tcp::socket& socket, const Tvec& vec);
template <typename Tvec>
  requires std::is_same_v<Tvec, std::vector<typename Tvec::value_type>>
asio::awaitable<Tvec> recv(tcp::socket& socket);

template <typename Topt>
  requires std::is_same_v<Topt, std::optional<typename Topt::value_type>>
asio::awaitable<void> send(tcp::socket& socket, const Topt& opt);
template <typename Topt>
  requires std::is_same_v<Topt, std::optional<typename Topt::value_type>>
asio::awaitable<Topt> recv(tcp::socket& socket);

// string
template <>
inline asio::awaitable<void> send(tcp::socket& socket, const std::string& value) {
  co_await send<uint64_t>(socket, value.size());
  co_await asio::async_write(socket, asio::buffer(value.c_str(), value.size()), asio::use_awaitable);
}

template <>
inline asio::awaitable<std::string> recv(tcp::socket& socket) {
  std::string result;

  auto size = co_await recv<uint64_t>(socket);
  result.resize(size);

  co_await asio::async_read(socket, asio::buffer(result.data(), size), asio::use_awaitable);
  co_return result;
}

// Pair
template <typename Tpair>
  requires std::is_same_v<Tpair, std::pair<typename Tpair::first_type, typename Tpair::second_type>>
asio::awaitable<void> send(tcp::socket& socket, const Tpair& pair) {
  co_await send(socket, pair.first);
  co_await send(socket, pair.second);
}

template <typename Tpair>
  requires std::is_same_v<Tpair, std::pair<typename Tpair::first_type, typename Tpair::second_type>>
asio::awaitable<Tpair> recv(tcp::socket& socket) {
  auto first = co_await recv<typename Tpair::first_type>(socket);
  auto second = co_await recv<typename Tpair::second_type>(socket);
  co_return std::pair{std::move(first), std::move(second)};
}

// vector
template <typename Tvec>
  requires std::is_same_v<Tvec, std::vector<typename Tvec::value_type>>
asio::awaitable<void> send(tcp::socket& socket, const Tvec& vec) {
  co_await send<uint64_t>(socket, vec.size());
  for (const auto& item : vec) {
    co_await send(socket, item);
  }
}

template <typename Tvec>
  requires std::is_same_v<Tvec, std::vector<typename Tvec::value_type>>
asio::awaitable<Tvec> recv(tcp::socket& socket) {
  Tvec result;

  auto size = co_await recv<uint64_t>(socket);
  result.reserve(size);

  for (size_t i = 0; i < size; i++) {
    result.push_back(co_await recv<typename Tvec::value_type>(socket));
  }
  co_return result;
}

// Optional
template <typename Topt>
  requires std::is_same_v<Topt, std::optional<typename Topt::value_type>>
asio::awaitable<void> send(tcp::socket& socket, const Topt& opt) {
  co_await send<uint8_t>(socket, opt.has_value() ? 1 : 0);
  if (opt.has_value()) {
    co_await send(socket, *opt);
  }
}

template <typename Topt>
  requires std::is_same_v<Topt, std::optional<typename Topt::value_type>>
asio::awaitable<Topt> recv(tcp::socket& socket) {
  auto has_value = co_await recv<uint8_t>(socket);
  if (has_value) {
    co_return co_await recv<typename Topt::value_type>(socket);
  } else {
    co_return std::nullopt;
  }
}

// Dentry
template <>
inline asio::awaitable<void> send(tcp::socket& socket, const Dentry& dentry) {
  co_await send(socket, dentry.basename);
  co_await send(socket, dentry.metainfo);
}

template <>
inline asio::awaitable<Dentry> recv(tcp::socket& socket) {
  auto basename = co_await recv<decltype(Dentry::basename)>(socket);
  auto metainfo = co_await recv<decltype(Dentry::metainfo)>(socket);
  co_return Dentry{basename, metainfo};
}

// WatcherInfo
template <>
inline asio::awaitable<void> send(tcp::socket& socket, const WatcherInfo& dentry) {
  co_await send(socket, dentry.files_all);
  co_await send(socket, dentry.files_completed);
  co_await send(socket, dentry.cur_size);
  co_await send(socket, dentry.cur_progress);
  co_await send(socket, dentry.cur_file);
}

template <>
inline asio::awaitable<WatcherInfo> recv(tcp::socket& socket) {
  auto files_all = co_await recv<decltype(WatcherInfo::files_all)>(socket);
  auto files_completed = co_await recv<decltype(WatcherInfo::files_completed)>(socket);
  auto cur_size = co_await recv<decltype(WatcherInfo::cur_size)>(socket);
  auto cur_progress = co_await recv<decltype(WatcherInfo::cur_progress)>(socket);
  auto cur_file = co_await recv<decltype(WatcherInfo::cur_file)>(socket);
  co_return WatcherInfo{files_all, files_completed, cur_size, cur_progress, cur_file};
}

}  // namespace remy::serde
