# claude-telepresence

Run Claude Code on modern Linux while operating on legacy Unix systems (NeXTSTEP, HP-UX, IRIX, Solaris, AIX) via TCP.

```
+-----------------------+         TCP          +-----------------------+
|   Legacy Unix Box     |<-------------------->|    Linux Host         |
|   (NeXTSTEP, etc)     |      Port 5000       |                       |
|                       |                       |  +---------------+   |
|  +---------------+    |                       |  |  Claude Code  |   |
|  |    Client     |    |  Terminal I/O        |  |   (claude)    |   |
|  |  (C binary)   |<---+----------------------+--|               |   |
|  +---------------+    |                       |  +-------+-------+   |
|         |             |                       |          |           |
|         |             |  File/Exec Requests   |  +-------v-------+   |
|    Local File        <+----------------------+--|    Relay      |   |
|    Operations         |                       |  |  (relay.py)   |   |
|                       |                       |  +---------------+   |
+-----------------------+                       +-----------------------+
```

## Why?

Legacy Unix systems can't run modern software, but they're still useful for development, learning, or maintaining old codebases. This lets you use Claude Code's AI assistance while all file operations and shell commands execute natively on the legacy system.

## Components

- **relay.py** - Runs on Linux. Spawns Claude Code, bridges terminal I/O, provides MCP server for file operations
- **client.c** - Portable C89 client for legacy systems. Handles terminal, executes local operations on request
- **helper.c** - Helper binary for proxying bash commands from Claude's hooks

## Building

### Client (on legacy system)

```bash
# NeXTSTEP
cc -o claude-telepresence client.c

# HP-UX / Solaris
cc -o claude-telepresence client.c -lsocket -lnsl

# IRIX / AIX
cc -o claude-telepresence client.c

# Linux (for testing)
gcc -o claude-telepresence client.c
```

### Helper (on Linux)

```bash
make telepresence-helper
```

## Running

### Start Relay (Linux)

```bash
python3 relay.py --port 5000
```

### Connect Client (Legacy Unix)

```bash
./claude-telepresence <linux-host> 5000

# With simple mode (ASCII-only output for old terminals):
./claude-telepresence -s <linux-host> 5000

# With debug logging:
./claude-telepresence -s -l <linux-host> 5000
```

## Features

- Full Claude Code TUI over TCP
- Native file operations (read, write, edit, search, find)
- Shell command execution on remote system
- Unicode to ASCII conversion for old terminals (`-s` flag)
- Animated spinner, arrows, checkmarks in ASCII
- Web downloads via Linux host (bypasses legacy SSL/TLS limitations)

## MCP Tools

The relay provides these tools to Claude:

| Tool | Description |
|------|-------------|
| `read_file` | Read file with line numbers |
| `write_file` | Write entire file |
| `edit_file` | Surgical string replacement |
| `list_directory` | List directory contents |
| `find_files` | Find files by name pattern |
| `search_files` | Search file contents |
| `execute_command` | Run shell command |
| `download_url` | Download URL via Linux, save to remote |
| `get_cwd` | Get current directory |
| `file_info` | Get file metadata |
| `file_exists` | Check if path exists |
| `make_directory` | Create directory |
| `remove_file` | Delete file |
| `move_file` | Move/rename file |

## Documentation

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed technical documentation.

## License

MIT
