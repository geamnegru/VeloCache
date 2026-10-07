#ifndef SERVER_H
#define SERVER_H

#include <csignal>
#include <string>

struct ServerOptions {
  std::string bind_host = "0.0.0.0";
  int port = 6379;
  std::string aof_path = "velocache.aof";
  std::string master_host;
  int master_port = 6379;
};

void run_server(const ServerOptions &options,
                const volatile std::sig_atomic_t &stop_requested);

#endif
