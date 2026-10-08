#include "server.h"
#include "background_worker.h"
#include "client_handler.h"
#include "event_loop.h"
#include "resp.h"
#include "storage.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_line = 4 * 1024 * 1024 + 256;
constexpr std::size_t max_output = 64 * 1024 * 1024;
constexpr std::size_t io_budget = 256 * 1024;

class SocketHandle {
public:
  explicit SocketHandle(int value = -1) : fd(value) {}
  ~SocketHandle() {
    if (fd >= 0)
      close(fd);
  }
  SocketHandle(const SocketHandle &) = delete;
  SocketHandle &operator=(const SocketHandle &) = delete;
  int get() const { return fd; }
  int release() {
    int result = fd;
    fd = -1;
    return result;
  }

private:
  int fd;
};

[[noreturn]] void socket_error(const std::string &operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

void configure_socket(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    socket_error("configure socket");
  }
  int enabled = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
    socket_error("SO_NOSIGPIPE");
}

sockaddr_in resolve_address(const std::string &host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  int result = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                           &addresses);
  if (result != 0)
    throw std::runtime_error("Cannot resolve " + host + ": " + gai_strerror(result));
  sockaddr_in address = *reinterpret_cast<sockaddr_in *>(addresses->ai_addr);
  freeaddrinfo(addresses);
  return address;
}

int create_listener(const ServerOptions &options) {
  auto address = resolve_address(options.bind_host, options.port);
  SocketHandle fd(socket(AF_INET, SOCK_STREAM, 0));
  if (fd.get() < 0)
    socket_error("socket");
  configure_socket(fd.get());
  int enabled = 1;
  if (setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
    socket_error("SO_REUSEADDR");
  if (bind(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
    socket_error("bind");
  if (listen(fd.get(), 128) < 0)
    socket_error("listen");
  return fd.release();
}

std::uint64_t parse_counter(const std::string &token) {
  if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("Invalid replication counter");
  try {
    return std::stoull(token);
  } catch (const std::exception &) {
    throw std::invalid_argument("Replication counter overflow");
  }
}

std::vector<std::string> fields(const std::string &line) {
  std::stringstream stream(line);
  std::vector<std::string> result;
  std::string field;
  while (stream >> field)
    result.push_back(field);
  return result;
}

enum class ConnectionKind { Client, Replica, Upstream };
enum class ClientProtocol { Unknown, Inline, Resp };

struct Connection {
  ConnectionKind kind = ConnectionKind::Client;
  ClientProtocol protocol = ClientProtocol::Unknown;
  std::string input;
  std::string output;
  std::size_t written = 0;
  bool close_after_write = false;
  bool connecting = false;
  bool peer_eof = false;
  bool waiting = false;
  std::uint64_t identity = 0;
  Clock::time_point last_activity = Clock::now();
};

class Server {
public:
  explicit Server(const ServerOptions &configuration)
      : options(configuration), storage(options.aof_path),
        listener(create_listener(options)) {
    if (is_replica())
      master_address = resolve_address(options.master_host, options.master_port);
    last_sequence = storage.snapshot().sequence;
    loop.watch(listener.get(), true, false);
    loop.watch(worker.notification_fd(), true, false);
    std::cout << "Started at " << options.port << " ("
              << (is_replica() ? "slave" : "master") << ", kqueue, AOF: "
              << options.aof_path << ")\n" << std::flush;
  }

  ~Server() {
    for (const auto &item : connections)
      close(item.first);
  }

  void run(const volatile std::sig_atomic_t &stop_requested) {
    while (!stop_requested) {
      worker.drain();
      tick();
      process_pending_clients();
      auto events = loop.wait(pending_clients.empty() ? 100 : 0);
      for (const auto &event : events) {
        if (event.fd == listener.get() && event.readable)
          accept_clients();
      }
      for (const auto &event : events) {
        if (event.fd == listener.get())
          continue;
        if (event.fd == worker.notification_fd()) {
          worker.drain();
          continue;
        }
        if (connections.find(event.fd) == connections.end())
          continue;
        if (event.failed) {
          disconnect(event.fd);
          continue;
        }
        auto &connection = connections.at(event.fd);
        if (connection.connecting) {
          int error = 0;
          socklen_t size = sizeof(error);
          if (getsockopt(event.fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 ||
              error != 0) {
            disconnect(event.fd);
            continue;
          }
          connection.connecting = false;
          if (!queue(event.fd, "SYNC\n"))
            continue;
        }
        if (event.readable)
          read_connection(event.fd);
        if (event.writable && connections.find(event.fd) != connections.end())
          flush(event.fd);
      }
    }
  }

private:
  ServerOptions options;
  StorageEngine storage;
  EventLoop loop;
  BackgroundWorker worker;
  SocketHandle listener;
  std::unordered_map<int, Connection> connections;
  std::unordered_set<int> pending_clients;
  std::uint64_t next_identity = 0;
  bool cleanup_pending = false;
  sockaddr_in master_address{};
  int upstream = -1;
  bool replica_ready = false;
  bool receiving_snapshot = false;
  std::uint64_t last_sequence = 0;
  StorageSnapshot pending_snapshot;
  std::size_t expected_entries = 0;
  std::size_t snapshot_bytes = 0;
  std::unordered_set<std::string> snapshot_keys;
  Clock::time_point retry_at = Clock::now();
  Clock::time_point maintenance_at = Clock::now();
  bool listener_paused = false;
  Clock::time_point accept_retry_at = Clock::now();

  bool is_replica() const { return !options.master_host.empty(); }

  void disconnect(int fd) {
    auto found = connections.find(fd);
    if (found == connections.end())
      return;
    loop.remove(fd);
    close(fd);
    connections.erase(found);
    pending_clients.erase(fd);
    if (fd == upstream) {
      upstream = -1;
      replica_ready = false;
      receiving_snapshot = false;
      pending_snapshot = {};
      snapshot_keys.clear();
      retry_at = Clock::now() + std::chrono::milliseconds(500);
    }
  }

  void watch(int fd) {
    const auto &connection = connections.at(fd);
    loop.watch(fd, !connection.close_after_write && !connection.connecting &&
                       !connection.peer_eof && !connection.waiting,
               connection.connecting || connection.written < connection.output.size());
  }

  bool queue(int fd, const std::string &data) {
    auto found = connections.find(fd);
    if (found == connections.end())
      return false;
    auto &connection = found->second;
    if (data.size() > max_output ||
        connection.output.size() - connection.written > max_output - data.size()) {
      disconnect(fd);
      return false;
    }
    if (connection.written != 0) {
      connection.output.erase(0, connection.written);
      connection.written = 0;
    }
    connection.output += data;
    watch(fd);
    return true;
  }

  void broadcast(const std::string &data) {
    std::vector<int> replicas;
    for (const auto &item : connections) {
      if (item.second.kind == ConnectionKind::Replica)
        replicas.push_back(item.first);
    }
    for (int fd : replicas)
      queue(fd, data);
  }

  void tick() {
    auto now = Clock::now();
    if (listener_paused && now >= accept_retry_at) {
      loop.watch(listener.get(), true, false);
      listener_paused = false;
    }
    std::vector<int> stale;
    for (const auto &item : connections) {
      auto age = now - item.second.last_activity;
      if (!item.second.waiting &&
          ((item.second.kind == ConnectionKind::Client && age > std::chrono::seconds(30)) ||
           (item.second.kind == ConnectionKind::Upstream && age > std::chrono::seconds(5))))
        stale.push_back(item.first);
    }
    for (int fd : stale)
      disconnect(fd);
    if (is_replica() && upstream < 0 && now >= retry_at)
      connect_master();
    if (now >= maintenance_at) {
      if (!cleanup_pending) {
        cleanup_pending = worker.submit(1, [this] {
          storage.clean_expired();
          return [this] { cleanup_pending = false; };
        });
      }
      if (!is_replica()) {
        broadcast("HEARTBEAT " + std::to_string(last_sequence) + "\n");
      }
      maintenance_at = now + std::chrono::seconds(1);
    }
  }

  void accept_clients() {
    for (int count = 0; count < 128; ++count) {
      SocketHandle fd(accept(listener.get(), nullptr, nullptr));
      if (fd.get() < 0) {
        if (errno == EINTR) {
          --count;
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return;
        if (errno == ECONNABORTED)
          continue;
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
          loop.watch(listener.get(), false, false);
          listener_paused = true;
          accept_retry_at = Clock::now() + std::chrono::milliseconds(100);
          return;
        }
        socket_error("accept");
      }
      if (connections.size() >= 4096)
        continue;
      configure_socket(fd.get());
      loop.watch(fd.get(), true, false);
      Connection connection;
      connection.identity = ++next_identity;
      connections.emplace(fd.get(), std::move(connection));
      fd.release();
    }
  }

  void connect_master() {
    retry_at = Clock::now() + std::chrono::milliseconds(500);
    SocketHandle fd(socket(AF_INET, SOCK_STREAM, 0));
    if (fd.get() < 0)
      socket_error("upstream socket");
    configure_socket(fd.get());
    int result = connect(fd.get(), reinterpret_cast<sockaddr *>(&master_address),
                         sizeof(master_address));
    if (result < 0 && errno != EINPROGRESS)
      return;
    Connection connection;
    connection.kind = ConnectionKind::Upstream;
    connection.identity = ++next_identity;
    connection.connecting = result < 0;
    loop.watch(fd.get(), !connection.connecting, connection.connecting);
    connections.emplace(fd.get(), std::move(connection));
    upstream = fd.release();
    if (result == 0)
      queue(upstream, "SYNC\n");
  }

  bool matches(int fd, std::uint64_t identity) const {
    auto found = connections.find(fd);
    return found != connections.end() && found->second.identity == identity;
  }

  void resume(int fd, std::uint64_t identity) {
    if (!matches(fd, identity))
      return;
    auto &connection = connections.at(fd);
    connection.waiting = false;
    connection.last_activity = Clock::now();
    if (!connection.input.empty() || connection.peer_eof)
      pending_clients.insert(fd);
    watch(fd);
  }

  void complete_command(int fd, std::uint64_t identity, CommandResult result,
                        const std::string &wire, bool resp) {
    if (result.mutation) {
      last_sequence = result.mutation->sequence;
      broadcast(wire);
    }
    if (!matches(fd, identity))
      return;
    auto &connection = connections.at(fd);
    connection.waiting = false;
    connection.close_after_write = !resp;
    connection.last_activity = Clock::now();
    if (queue(fd, result.response) && resp)
      resume(fd, identity);
  }

  void submit_command(int fd, std::string command,
                      std::vector<std::string> arguments, bool resp) {
    auto &connection = connections.at(fd);
    const auto identity = connection.identity;
    const bool read_only = is_replica();
    const bool ready = !read_only || replica_ready;
    std::size_t bytes = resp_max_bulk_size + command.size() + 256;
    for (const auto &argument : arguments)
      bytes += argument.size();
    connection.waiting = true;
    if (!worker.submit(bytes, [this, fd, identity, read_only, ready, resp,
                                command = std::move(command),
                                arguments = std::move(arguments)] {
          auto result = resp ? handle_resp_command(arguments, storage, read_only, ready)
                             : handle_command(command, storage, read_only, ready);
          auto wire = result.mutation ? encode_mutation(*result.mutation) : "";
          return [this, fd, identity, resp, result = std::move(result),
                    wire = std::move(wire)]() mutable {
            complete_command(fd, identity, std::move(result), wire, resp);
          };
        })) {
      complete_command(fd, identity,
                       {resp ? resp_error("ERR background worker queue is full")
                             : "ERR Background worker queue is full\n", std::nullopt},
                       "", resp);
    } else {
      watch(fd);
    }
  }

  void serve_client(int fd, std::string command) {
    if (!command.empty() && command.back() == '\r')
      command.pop_back();
    auto &connection = connections.at(fd);
    const auto identity = connection.identity;
    connection.input.clear();
    if (command == "SYNC" && !is_replica()) {
      connection.waiting = true;
      if (!worker.submit(max_output, [this, fd, identity] {
            auto snapshot = storage.snapshot();
            std::string output = "SNAP_BEGIN " + std::to_string(snapshot.sequence) +
                                 " " + std::to_string(snapshot.entries.size()) + "\n";
            for (const auto &entry : snapshot.entries) {
              auto line = encode_mutation({snapshot.sequence, entry});
              if (line.size() > max_output || output.size() > max_output - line.size())
                return BackgroundWorker::Completion([this, fd, identity] {
                  if (matches(fd, identity))
                    disconnect(fd);
                });
              output += line;
            }
            output += "SNAP_END " + std::to_string(snapshot.sequence) + "\n";
            return BackgroundWorker::Completion(
                [this, fd, identity, output = std::move(output)] {
                  if (!matches(fd, identity))
                    return;
                  auto &peer = connections.at(fd);
                  peer.kind = ConnectionKind::Replica;
                  peer.waiting = false;
                  queue(fd, output);
                });
          })) {
        disconnect(fd);
      } else {
        watch(fd);
      }
      return;
    }
    std::stringstream stream(command);
    std::string directive;
    stream >> directive;
    if (directive == "PING") {
      connection.close_after_write = true;
      queue(fd, "PONG\n");
      return;
    }
    submit_command(fd, std::move(command), {}, false);
  }

  void persist_replication(std::uint64_t sequence, std::size_t bytes,
                           std::function<void()> operation, bool snapshot) {
    const int fd = upstream;
    const auto identity = connections.at(fd).identity;
    connections.at(fd).waiting = true;
    if (!worker.submit(bytes, [this, fd, identity, sequence, snapshot,
                                operation = std::move(operation)] {
          try {
            operation();
          } catch (const std::invalid_argument &) {
            return BackgroundWorker::Completion([this, fd, identity] {
              if (matches(fd, identity))
                disconnect(fd);
            });
          }
          return BackgroundWorker::Completion([this, fd, identity, sequence, snapshot] {
            last_sequence = sequence;
            if (!matches(fd, identity))
              return;
            if (snapshot) {
              replica_ready = true;
              std::cout << "Replica synchronized\n" << std::flush;
            }
            resume(fd, identity);
          });
        })) {
      disconnect(fd);
    } else {
      watch(fd);
    }
  }

  void receive_replication(const std::string &line) {
    if (line.rfind("SNAP_BEGIN ", 0) == 0) {
      auto tokens = fields(line);
      if (tokens.size() != 3 || receiving_snapshot || replica_ready)
        throw std::invalid_argument("Unexpected snapshot start");
      pending_snapshot = {};
      pending_snapshot.sequence = parse_counter(tokens[1]);
      auto count = parse_counter(tokens[2]);
      if (count > 1000000)
        throw std::invalid_argument("Snapshot too large");
      expected_entries = static_cast<std::size_t>(count);
      snapshot_bytes = 0;
      snapshot_keys.clear();
      receiving_snapshot = true;
      return;
    }
    if (line.rfind("SNAP_END ", 0) == 0) {
      auto tokens = fields(line);
      if (tokens.size() != 2 || !receiving_snapshot ||
          parse_counter(tokens[1]) != pending_snapshot.sequence ||
          pending_snapshot.entries.size() != expected_entries)
        throw std::invalid_argument("Incomplete snapshot");
      auto snapshot = std::move(pending_snapshot);
      const auto sequence = snapshot.sequence;
      pending_snapshot = {};
      snapshot_keys.clear();
      receiving_snapshot = false;
      persist_replication(sequence, max_output,
                          [this, snapshot = std::move(snapshot)] {
                            storage.replace_snapshot(snapshot);
                          }, true);
      return;
    }
    if (line.rfind("HEARTBEAT ", 0) == 0) {
      auto tokens = fields(line);
      if (tokens.size() != 2 || !replica_ready ||
          parse_counter(tokens[1]) != last_sequence)
        throw std::invalid_argument("Invalid replication heartbeat");
      return;
    }
    auto mutation = decode_mutation(line);
    if (receiving_snapshot) {
      if (pending_snapshot.entries.size() >= expected_entries ||
          mutation.sequence != pending_snapshot.sequence ||
          line.size() > max_output - snapshot_bytes ||
          !snapshot_keys.insert(mutation.entry.key).second)
        throw std::invalid_argument("Invalid snapshot entry");
      snapshot_bytes += line.size();
      pending_snapshot.entries.push_back(std::move(mutation.entry));
    } else {
      if (!replica_ready)
        throw std::invalid_argument("Update before snapshot");
      const auto sequence = mutation.sequence;
      auto bytes = resp_max_bulk_size + mutation.entry.key.size() +
                   mutation.entry.value.size() + 256;
      persist_replication(sequence, bytes, [this, mutation = std::move(mutation)] {
        storage.apply_replication(mutation);
      }, false);
    }
  }

  void protocol_error(int fd, const std::string &message) {
    auto &connection = connections.at(fd);
    connection.close_after_write = true;
    connection.input.clear();
    pending_clients.erase(fd);
    queue(fd, resp_error("ERR Protocol error: " + message));
  }

  void finish_input(int fd) {
    auto &connection = connections.at(fd);
    if (!connection.peer_eof)
      return;
    connection.close_after_write = true;
    pending_clients.erase(fd);
    if (connection.output.empty())
      disconnect(fd);
    else
      watch(fd);
  }

  void process_client_input(int fd, std::size_t &budget) {
    while (connections.find(fd) != connections.end()) {
      auto &connection = connections.at(fd);
      if (connection.waiting)
        return;
      if (connection.input.empty()) {
        finish_input(fd);
        return;
      }
      if (connection.protocol == ClientProtocol::Unknown) {
        auto first = connection.input.front();
        connection.protocol = first == '*' || first == '$' || first == '+' ||
                                      first == '-' || first == ':'
                                  ? ClientProtocol::Resp
                                  : ClientProtocol::Inline;
      }
      if (connection.protocol == ClientProtocol::Inline) {
        auto end = connection.input.find('\n');
        if (end != std::string::npos) {
          if (end > max_line)
            disconnect(fd);
          else
            serve_client(fd, connection.input.substr(0, end));
        } else if (connection.input.size() > max_line) {
          disconnect(fd);
        } else if (connection.peer_eof) {
          serve_client(fd, connection.input);
        }
        return;
      }
      if (budget == 0) {
        pending_clients.insert(fd);
        return;
      }
      auto parsed = parse_resp_request(connection.input);
      if (parsed.status == RespParseStatus::Incomplete) {
        if (connection.peer_eof)
          protocol_error(fd, "incomplete request");
        return;
      }
      if (parsed.status == RespParseStatus::Error) {
        protocol_error(fd, parsed.error);
        return;
      }
      connection.input.erase(0, parsed.consumed);
      --budget;
      const auto &directive = parsed.arguments.front();
      bool ping = directive.size() == 4;
      const std::string expected = "PING";
      for (std::size_t i = 0; ping && i < directive.size(); ++i) {
        char byte = directive[i];
        if (byte >= 'a' && byte <= 'z')
          byte -= 'a' - 'A';
        ping = byte == expected[i];
      }
      if (ping) {
        auto result = handle_resp_command(parsed.arguments, storage, is_replica(),
                                          !is_replica() || replica_ready);
        if (!queue(fd, result.response))
          return;
      } else {
        submit_command(fd, "", std::move(parsed.arguments), true);
        return;
      }
    }
  }

  void process_pending_clients() {
    std::vector<int> pending(pending_clients.begin(), pending_clients.end());
    for (int fd : pending) {
      pending_clients.erase(fd);
      auto found = connections.find(fd);
      if (found == connections.end() || found->second.close_after_write ||
          found->second.waiting)
        continue;
      if (found->second.kind == ConnectionKind::Upstream) {
        process_replication_input(fd);
      } else {
        std::size_t budget = 64;
        process_client_input(fd, budget);
      }
    }
  }

  void process_replication_input(int fd) {
    std::size_t budget = 64;
    while (connections.find(fd) != connections.end()) {
      auto &connection = connections.at(fd);
      if (connection.waiting)
        return;
      auto position = connection.input.find('\n');
      if (position == std::string::npos) {
        if (connection.input.size() > max_line)
          disconnect(fd);
        return;
      }
      if (position > max_line) {
        disconnect(fd);
        return;
      }
      if (budget-- == 0) {
        pending_clients.insert(fd);
        return;
      }
      std::string line = connection.input.substr(0, position);
      connection.input.erase(0, position + 1);
      try {
        receive_replication(line);
      } catch (const std::invalid_argument &error) {
        std::cerr << "Replication resync: " << error.what() << '\n';
        disconnect(fd);
        return;
      }
    }
  }

  void read_connection(int fd) {
    std::array<char, 8192> buffer{};
    std::size_t consumed = 0;
    std::size_t command_budget = 64;
    while (consumed < io_budget && connections.find(fd) != connections.end()) {
      auto &connection = connections.at(fd);
      if (connection.waiting)
        return;
      auto count = recv(fd, buffer.data(), buffer.size(), 0);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return;
        disconnect(fd);
        return;
      }
      if (count == 0) {
        if (connection.kind == ConnectionKind::Client) {
          connection.peer_eof = true;
          process_client_input(fd, command_budget);
          if (connections.find(fd) != connections.end())
            watch(fd);
        } else {
          disconnect(fd);
        }
        return;
      }
      consumed += static_cast<std::size_t>(count);
      connection.last_activity = Clock::now();
      if (connection.kind == ConnectionKind::Replica) {
        disconnect(fd);
        return;
      }
      connection.input.append(buffer.data(), static_cast<std::size_t>(count));
      if (connection.kind == ConnectionKind::Client) {
        process_client_input(fd, command_budget);
        auto found = connections.find(fd);
        if (found == connections.end() || found->second.close_after_write ||
            found->second.kind != ConnectionKind::Client || found->second.waiting ||
            command_budget == 0)
          return;
        continue;
      }
      process_replication_input(fd);
      if (connections.find(fd) == connections.end() || connections.at(fd).waiting)
        return;
    }
  }

  void flush(int fd) {
    auto &connection = connections.at(fd);
    std::size_t sent = 0;
    while (connection.written < connection.output.size() && sent < io_budget) {
      auto remaining = std::min(connection.output.size() - connection.written,
                                io_budget - sent);
      auto count = send(fd, connection.output.data() + connection.written,
                        remaining, 0);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        disconnect(fd);
        return;
      }
      if (count == 0) {
        disconnect(fd);
        return;
      }
      connection.written += static_cast<std::size_t>(count);
      sent += static_cast<std::size_t>(count);
      connection.last_activity = Clock::now();
    }
    if (connection.written == connection.output.size()) {
      connection.output.clear();
      connection.written = 0;
      if (connection.close_after_write) {
        disconnect(fd);
        return;
      }
    }
    watch(fd);
  }
};
}

void run_server(const ServerOptions &options,
                const volatile std::sig_atomic_t &stop_requested) {
  Server server(options);
  server.run(stop_requested);
}
