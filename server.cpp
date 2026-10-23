#include "server.h"

#include "background_worker.h"
#include "client_handler.h"
#include "event_loop.h"
#include "resp.h"
#include "storage.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

enum class ConnectionKind { Client, Replica, Upstream };

struct Connection {
  int fd = -1;
  ConnectionKind kind = ConnectionKind::Client;
  std::string read_buffer;
  std::string write_buffer;
  bool close_after_write = false;
  bool ready = true;
  bool sync_handshake_sent = false;
  bool snapshot_mode = false;
  std::uint64_t snapshot_sequence = 0;
  std::size_t snapshot_entries_remaining = 0;
  std::vector<StorageEntry> snapshot_entries;
};

void ignore_sigpipe(int fd) {
#ifdef __APPLE__
  int value = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#endif
}

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return false;
  }
  return true;
}

void close_fd(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void disconnect_connection(std::unordered_map<int, Connection> &connections, int fd) {
  auto it = connections.find(fd);
  if (it != connections.end()) {
    close_fd(it->second.fd);
    connections.erase(it);
  }
}

std::optional<std::string> take_line(std::string &buffer) {
  const auto newline = buffer.find('\n');
  if (newline == std::string::npos) {
    return std::nullopt;
  }
  std::string line = buffer.substr(0, newline);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  buffer.erase(0, newline + 1);
  return line;
}

int make_listener(const std::string &host, int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
  }

  ignore_sigpipe(fd);

  int yes = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    close_fd(fd);
    throw std::runtime_error(std::string("setsockopt: ") + std::strerror(errno));
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    close_fd(fd);
    throw std::runtime_error("Invalid bind address");
  }

  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    close_fd(fd);
    throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
  }

  if (::listen(fd, 128) < 0) {
    close_fd(fd);
    throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
  }

  if (!set_nonblocking(fd)) {
    close_fd(fd);
    throw std::runtime_error(std::string("fcntl: ") + std::strerror(errno));
  }

  return fd;
}

void queue_output(Connection &connection, const std::string &data) {
  if (!data.empty()) {
    connection.write_buffer.append(data);
  }
}

void flush_connection(EventLoop &loop, Connection &connection) {
  while (!connection.write_buffer.empty()) {
    const auto to_send = std::min<std::size_t>(connection.write_buffer.size(), 65536);
    const ssize_t sent = ::send(connection.fd, connection.write_buffer.data(), to_send, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      connection.close_after_write = true;
      break;
    }
    if (sent == 0) {
      connection.close_after_write = true;
      break;
    }
    connection.write_buffer.erase(0, static_cast<std::size_t>(sent));
  }
  loop.watch(connection.fd, true, !connection.write_buffer.empty());
  if (connection.close_after_write && connection.write_buffer.empty()) {
    connection.write_buffer.clear();
    close_fd(connection.fd);
  }
}

bool read_into_buffer(Connection &connection) {
  char chunk[65536];
  while (true) {
    const ssize_t received = ::recv(connection.fd, chunk, sizeof(chunk), 0);
    if (received > 0) {
      connection.read_buffer.append(chunk, static_cast<std::size_t>(received));
      continue;
    }
    if (received == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    return false;
  }
}

void queue_replica_updates(std::unordered_map<int, Connection> &connections,
                           const StorageMutation &mutation) {
  const std::string payload = encode_mutation(mutation);
  for (auto &entry : connections) {
    auto &connection = entry.second;
    if (connection.kind == ConnectionKind::Replica && connection.sync_handshake_sent) {
      queue_output(connection, payload);
    }
  }
}

void send_snapshot_to_replica(Connection &connection, StorageEngine &storage) {
  auto snapshot = storage.snapshot();
  std::string payload;
  payload += "SNAP_BEGIN " + std::to_string(snapshot.sequence) + " " +
              std::to_string(snapshot.entries.size()) + "\n";
  for (const auto &entry : snapshot.entries) {
    payload += encode_mutation(StorageMutation{snapshot.sequence, entry});
  }
  payload += "SNAP_END " + std::to_string(snapshot.sequence) + "\n";
  queue_output(connection, payload);
}

void process_replica_upstream(Connection &connection, StorageEngine &storage,
                              bool &replica_ready) {
  while (true) {
    const std::optional<std::string> line = take_line(connection.read_buffer);
    if (!line) {
      break;
    }

    if (line->rfind("SNAP_BEGIN ", 0) == 0) {
      std::istringstream parser(*line);
      std::string marker;
      std::uint64_t sequence = 0;
      std::size_t count = 0;
      parser >> marker >> sequence >> count;
      connection.snapshot_mode = true;
      connection.snapshot_sequence = sequence;
      connection.snapshot_entries_remaining = count;
      connection.snapshot_entries.clear();
      continue;
    }

    if (line->rfind("SNAP_END ", 0) == 0) {
      std::istringstream parser(*line);
      std::string marker;
      std::uint64_t sequence = 0;
      parser >> marker >> sequence;
      if (connection.snapshot_mode && connection.snapshot_entries_remaining == 0 &&
          sequence == connection.snapshot_sequence) {
        StorageSnapshot snapshot{connection.snapshot_sequence, connection.snapshot_entries};
        storage.replace_snapshot(snapshot);
        connection.snapshot_mode = false;
        connection.snapshot_entries.clear();
        connection.ready = true;
        replica_ready = true;
      }
      continue;
    }

    if (line->rfind("UPDATE ", 0) == 0) {
      try {
        const StorageMutation mutation = decode_mutation(*line + "\n");
        if (connection.snapshot_mode) {
          if (connection.snapshot_entries_remaining > 0) {
            connection.snapshot_entries.push_back(mutation.entry);
            --connection.snapshot_entries_remaining;
          } else {
            storage.apply_replication(mutation);
          }
        } else {
          storage.apply_replication(mutation);
        }
        continue;
      } catch (const std::exception &) {
        throw;
      }
    }

    if (line->empty()) {
      continue;
    }
  }
}

void process_client_text(Connection &connection, StorageEngine &storage,
                          bool read_only, bool ready,
                          std::unordered_map<int, Connection> &connections,
                          bool &replica_ready) {
  while (true) {
    const std::optional<std::string> line = take_line(connection.read_buffer);
    if (!line) {
      break;
    }

    if (*line == "SYNC") {
      connection.kind = ConnectionKind::Replica;
      connection.sync_handshake_sent = true;
      connection.ready = true;
      send_snapshot_to_replica(connection, storage);
      continue;
    }

    const CommandResult result = handle_command(*line, storage, read_only, ready);
    queue_output(connection, result.response);
    if (result.mutation) {
      queue_replica_updates(connections, *result.mutation);
    }
    connection.close_after_write = true;
    break;
  }
  (void)replica_ready;
}

void process_client_resp(Connection &connection, StorageEngine &storage,
                         bool read_only, bool ready,
                         std::unordered_map<int, Connection> &connections) {
  while (!connection.read_buffer.empty()) {
    const auto parsed = parse_resp_request(connection.read_buffer);
    if (parsed.status == RespParseStatus::Incomplete) {
      break;
    }
    if (parsed.status == RespParseStatus::Error) {
      queue_output(connection, resp_error("ERR Protocol error"));
      connection.close_after_write = true;
      break;
    }

    const auto command = handle_resp_command(parsed.arguments, storage, read_only, ready);
    queue_output(connection, command.response);
    if (command.mutation) {
      queue_replica_updates(connections, *command.mutation);
    }
    connection.read_buffer.erase(0, parsed.consumed);
  }
}

void handle_accepted_connection(int fd, std::unordered_map<int, Connection> &connections,
                               EventLoop &loop) {
  if (!set_nonblocking(fd)) {
    close_fd(fd);
    return;
  }
  Connection connection;
  connection.fd = fd;
  connection.kind = ConnectionKind::Client;
  connection.ready = true;
  connections.emplace(fd, connection);
  loop.watch(fd, true, false);
}

void connect_upstream(const std::string &host, int port,
                      std::unordered_map<int, Connection> &connections,
                      EventLoop &loop) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
  }

  ignore_sigpipe(fd);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    close_fd(fd);
    throw std::runtime_error("Invalid replica master address");
  }

  if (!set_nonblocking(fd)) {
    close_fd(fd);
    throw std::runtime_error(std::string("fcntl: ") + std::strerror(errno));
  }

  int result = ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  if (result < 0 && errno != EINPROGRESS) {
    close_fd(fd);
    throw std::runtime_error(std::string("connect: ") + std::strerror(errno));
  }

  Connection connection;
  connection.fd = fd;
  connection.kind = ConnectionKind::Upstream;
  connection.ready = false;
  connection.write_buffer = "SYNC\n";
  connections.emplace(fd, connection);
  loop.watch(fd, true, !connection.write_buffer.empty());
}

}  // namespace

void run_server(const ServerOptions &options,
                const volatile std::sig_atomic_t &stop_requested) {
  StorageEngine storage(options.aof_path);
  const bool replica_mode = !options.master_host.empty();
  auto loop = make_event_loop();
  BackgroundWorker worker;

  int listener_fd = make_listener(options.bind_host, options.port);
  std::unordered_map<int, Connection> connections;
  connections.emplace(listener_fd, Connection{listener_fd, ConnectionKind::Client, {}, {}, false, true, false, 0, 0, {}});
  loop->watch(listener_fd, true, false);

  if (replica_mode) {
    connect_upstream(options.master_host, options.master_port, connections, *loop);
  }

  while (!stop_requested) {
    const auto events = loop->wait(replica_mode ? 50 : 100);
    for (const auto &event : events) {
      if (event.fd == listener_fd) {
        if (!event.readable) {
          continue;
        }
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);
        const int accepted = ::accept(listener_fd,
                                     reinterpret_cast<sockaddr *>(&client_address),
                                     &client_length);
        if (accepted < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
          }
          throw std::runtime_error(std::string("accept: ") + std::strerror(errno));
        }
        handle_accepted_connection(accepted, connections, *loop);
        continue;
      }

      auto it = connections.find(event.fd);
      if (it == connections.end()) {
        continue;
      }

      Connection &connection = it->second;
      if (event.failed) {
        disconnect_connection(connections, event.fd);
        continue;
      }

      if (event.readable) {
        if (!read_into_buffer(connection)) {
          disconnect_connection(connections, event.fd);
          continue;
        }

        if (connection.kind == ConnectionKind::Upstream) {
          process_replica_upstream(connection, storage, connection.ready);
          if (!connection.write_buffer.empty()) {
            loop->watch(connection.fd, true, true);
          }
          continue;
        }

        if (connection.kind == ConnectionKind::Replica && !connection.sync_handshake_sent) {
          connection.sync_handshake_sent = true;
          send_snapshot_to_replica(connection, storage);
          continue;
        }

        if (!connection.read_buffer.empty()) {
          const bool this_is_resp = connection.read_buffer[0] == '*';
          if (this_is_resp) {
            process_client_resp(connection, storage,
                                replica_mode, connection.ready,
                                connections);
          } else {
            process_client_text(connection, storage,
                                replica_mode, connection.ready,
                                connections, connection.ready);
          }
          if (connection.close_after_write && connection.write_buffer.empty()) {
            disconnect_connection(connections, connection.fd);
          }
        }
      }

      if (event.writable && !connection.write_buffer.empty()) {
        flush_connection(*loop, connection);
        if (connection.close_after_write && connection.write_buffer.empty()) {
          disconnect_connection(connections, connection.fd);
        }
      }
    }
  }

  for (auto &entry : connections) {
    if (entry.second.fd >= 0) {
      ::shutdown(entry.second.fd, SHUT_RDWR);
      close_fd(entry.second.fd);
    }
  }
  connections.clear();
}
