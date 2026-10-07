#include "server.h"

#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
volatile std::sig_atomic_t stop_requested = 0;

void stop_server(int) { stop_requested = 1; }

int parse_port(const std::string &text) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
    throw std::invalid_argument("Invalid port");
  }
  auto port = std::stoll(text);
  if (port < 1 || port > 65535) {
    throw std::invalid_argument("Port must be between 1 and 65535");
  }
  return static_cast<int>(port);
}
}

int main(int argc, char *argv[]) {
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(nullptr);

  try {
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
      std::string argument = argv[i];
      if (argument == "--help") {
        std::cout << "Usage: velocache [--bind HOST] [--port PORT] [--aof PATH] "
                     "[--replicaof HOST PORT]\n";
        return 0;
      }
      if (argument == "--replicaof" && i + 2 < argc) {
        options.master_host = argv[++i];
        options.master_port = parse_port(argv[++i]);
      } else if (argument == "--port" && i + 1 < argc) {
        options.port = parse_port(argv[++i]);
      } else if (argument == "--bind" && i + 1 < argc) {
        options.bind_host = argv[++i];
      } else if (argument == "--aof" && i + 1 < argc) {
        options.aof_path = argv[++i];
      } else {
        throw std::invalid_argument("Unknown or incomplete option: " + argument);
      }
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);
    run_server(options, stop_requested);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "VeloCache: " << error.what() << '\n';
    return 1;
  }
}
