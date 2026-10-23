void run(const volatile std::sig_atomic_t &stop_requested) {
  while (!stop_requested) {
    try {
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
    } catch (const std::exception &error) {
      std::cerr << "fatal server error: " << error.what() << '\n';
      throw;
    }
  }
}
