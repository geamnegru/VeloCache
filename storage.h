#ifndef STORAGE_H
#define STORAGE_H

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct StorageEntry {
  std::string key;
  std::string value;
  std::int64_t expires_at_ms = 0;
};

struct StorageMutation {
  std::uint64_t sequence = 0;
  StorageEntry entry;
};

struct StorageSnapshot {
  std::uint64_t sequence = 0;
  std::vector<StorageEntry> entries;
};

std::string encode_mutation(const StorageMutation &mutation);
StorageMutation decode_mutation(const std::string &line);

class StorageEngine {
public:
  explicit StorageEngine(const std::string &aof_path);
  ~StorageEngine();

  StorageEngine(const StorageEngine &) = delete;
  StorageEngine &operator=(const StorageEngine &) = delete;

  StorageMutation set(
      const std::string &key, const std::string &value,
      std::optional<std::int64_t> ttl_seconds = std::nullopt);
  std::optional<std::string> get(const std::string &key);
  StorageSnapshot snapshot();
  void apply_replication(const StorageMutation &mutation);
  void replace_snapshot(const StorageSnapshot &snapshot);
  void clean_expired();

private:
  using ExpiryIndex = std::multimap<std::int64_t, std::string>;

  struct StoredValue {
    std::string value;
    std::int64_t expires_at_ms = 0;
    std::optional<ExpiryIndex::iterator> expiry;
  };

  void load();
  void require_writable() const;
  void commit_mutation(const StorageMutation &mutation);
  void purge_expired();

  std::string aof_path_;
  int aof_fd_ = -1;
  int lock_fd_ = -1;
  std::uint64_t sequence_ = 0;
  bool writable_ = true;
  ExpiryIndex expirations_;
  std::unordered_map<std::string, StoredValue> db_;
  std::mutex db_mutex_;
};

#endif
