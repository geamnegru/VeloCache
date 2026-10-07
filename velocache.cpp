#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

std::unordered_map<std::string, std::string> db;

int main() {
  int svSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (svSocket < 0) {
    std::cout << "Socket Error \n";
    return 1;
  }

  sockaddr_in adress;
  adress.sin_family = AF_INET;
  adress.sin_port = htons(6379);
  adress.sin_addr.s_addr = INADDR_ANY;

  if (bind(svSocket, (struct sockaddr *)&adress, sizeof(adress)) < 0) {
    std::cout << "Port 6379 busy \n";
    return 1;
  }

  listen(svSocket, 3);
  std::cout << "Started at 6379 \n";

  while (true) {
    int size = sizeof(adress);
    int clientSocket =
        accept(svSocket, (struct sockaddr *)&adress, (socklen_t *)&size);

    char buffer[1024] = {0};
    read(clientSocket, buffer, 1024);

    std::string command(buffer);
    std::cout << "Client sent: " << command;

    std::stringstream ss(command);
    std::string directive, key, value;

    ss >> directive;
    std::string res;

    if (directive == "SET") {
      ss >> key;
      std::getline(ss >> std::ws, value);

      if (!value.empty() && value.back() == '\n')
        value.pop_back();
      if (!value.empty() && value.back() == '\r')
        value.pop_back();

      db[key] = value;
      res = "OK\n";
    } else if (directive == "GET") {
      ss >> key;
      if (!key.empty() && key.back() == '\n')
        key.pop_back();
      if (!key.empty() && key.back() == '\r')
        key.pop_back();

      if (db.find(key) != db.end()) {
        res = db[key] + "\n";
      } else {
        res = "(nil)\n";
      }
    } else if (directive == "PING") {
      res = "PONG\n";
    } else {
      res = "ERR Unknown directive. Use SET, GET or PING\n";
    }

    send(clientSocket, res.c_str(), res.length(), 0);
    close(clientSocket);
  }

  close(svSocket);
  return 0;
}
