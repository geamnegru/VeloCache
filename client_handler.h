#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include "storage.h"

#include <optional>
#include <string>
#include <vector>

struct CommandResult {
  std::string response;
  std::optional<StorageMutation> mutation;
};

CommandResult handle_command(const std::string &command, StorageEngine &storage,
                             bool read_only, bool ready = true);
CommandResult handle_resp_command(const std::vector<std::string> &arguments,
                                 StorageEngine &storage, bool read_only,
                                 bool ready = true);

#endif
