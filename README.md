# 🚀 VeloCache

A lightweight, low-level in-memory Key-Value store written from scratch in **modern C++** using native Unix POSIX sockets.

---

## 💡 Overview

**VeloCache** is a core systems programming utility designed to handle fast data caching in memory. Unlike standard applications that rely on high-level frameworks, this engine interfaces directly with the **macOS/Linux kernel network stack** to process incoming connections over a custom network transmission pipeline.

The repository features a single-file implementation optimized for memory speed, structured string parsing, and raw TCP/IP communication.

---

## 🛠️ Features Implemented

*   **Native Unix Sockets:** Uses `<sys/socket.h>` directly to bind, listen, and manage network connections on port `6379`.
*   **O(1) Data Retrieval:** Implements a fast static data layout using `std::unordered_map` for key-value pairings.
*   **Custom Tokenizer Engine:** Employs `std::stringstream` alongside manual string trimming logic to extract commands, target keys, and complex string values containing whitespace characters.
*   **Buffer Fault Tolerance:** Allocates a static 1024-byte buffer pool ensuring consistent network payload reading.

---

## ⚡ Supported Commands

VeloCache responds natively to the following custom standard protocol:

| Command | Arguments | Description | Server Response |
| :--- | :--- | :--- | :--- |
| **`SET`** | `[key] [value]` | Saves or updates a value in the memory database. | `OK\n` |
| **`GET`** | `[key]` | Searches for the key and dumps its exact value. | `[value]\n` or `(nil)\n` |
| **`PING`** | — | Standard operational connectivity health check. | `PONG\n` |

---

## 🚀 Compilation & Running

### 🔧 Build

Compile the single translation unit directly via your terminal (e.g., Alacritty):

```bash
g++ -std=c++17 velocache.cpp -o velocache
```

### 🛰️ Fire it Up

Execute the freshly built binary layout:

```bash
./velocache
```
*Console Output: `Started at 6379`*

---

## 🧪 Testing In The Terminal

Leave the server running in one session, open a separate terminal pane, and stream bytes over raw network pipes using `netcat` (`nc`):

```bash
# 1. Allocate a generic value to memory
echo "SET database_url 127.0.0.1" | nc localhost 6379
# Returns: OK

# 2. Extract that same value from memory
echo "GET database_url" | nc localhost 6379
# Returns: 127.0.0.1

# 3. Query a non-existent key layout
echo "GET session_token" | nc localhost 6379
# Returns: (nil)

# 4. Standard connection check
echo "PING" | nc localhost 6379
# Returns: PONG
```

---

