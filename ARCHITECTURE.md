# Claude Telepresence - Architecture Documentation

## Overview

Claude Telepresence allows Claude Code (running on modern Linux) to operate on legacy Unix systems (NeXTSTEP, HP-UX, IRIX, Solaris, AIX) via TCP. The user interacts with Claude through a thin terminal client on the legacy machine, while all AI processing happens on the Linux host.

```
┌─────────────────────┐         TCP          ┌─────────────────────┐
│   Legacy Unix Box   │◄────────────────────►│    Linux Host       │
│   (NeXTSTEP, etc)   │      Port 5000       │                     │
│                     │                       │  ┌───────────────┐  │
│  ┌───────────────┐  │                       │  │  Claude Code  │  │
│  │    Client     │  │  Terminal I/O        │  │   (claude)    │  │
│  │  (C binary)   │◄─┼──────────────────────┼──┤               │  │
│  └───────────────┘  │                       │  └───────┬───────┘  │
│         │           │                       │          │          │
│         │           │  File/Exec Requests   │  ┌───────▼───────┐  │
│    Local File      ◄┼──────────────────────┼──┤    Relay      │  │
│    Operations       │                       │  │  (relay.py)   │  │
│                     │                       │  └───────┬───────┘  │
└─────────────────────┘                       │          │          │
                                              │  ┌───────▼───────┐  │
                                              │  │  MCP Server   │  │
                                              │  │  (HTTP:5001)  │  │
                                              │  └───────────────┘  │
                                              └─────────────────────┘
```

## Protocol

The relay and client communicate using a binary streaming protocol. See [PROTOCOL.md](PROTOCOL.md) for full specification.

Key features:
- Length-prefixed binary packets (5-byte header + payload)
- Stream-based operations with flow control
- Window-based backpressure for large transfers
- No encryption (designed for trusted networks)

## Components

### 1. Relay (`relay.py`)

The relay is the orchestrator running on Linux. It:

- **Listens on TCP port 5000** for legacy client connections
- **Spawns Claude Code** in a PTY with MCP configuration and system prompt
- **Bridges terminal I/O** between Claude's PTY and the TCP client
- **Runs MCP HTTP server** on port 5001 for file operations
- **Translates MCP tool calls** to binary protocol requests to the client

**Key classes:**
- `TelepresenceRelay` - Main relay coordinating all components
- Async I/O using Python's asyncio for concurrent handling

### 2. Client (`client.c`)

Portable C89 client for legacy Unix systems. It:

- **Connects to relay** via TCP
- **Sends HELLO packet** with protocol version and working directory
- **Runs terminal in raw mode** for full-screen TUI support
- **Forwards keyboard input** as TERM_INPUT packets
- **Displays terminal output** from TERM_OUTPUT packets
- **Executes stream operations** (file read/write, commands) locally

**Platform support:**
- NeXTSTEP (uses sgtty instead of termios, sys/dir.h instead of dirent.h)
- HP-UX, IRIX, Solaris, AIX, Linux, FreeBSD, NetBSD, OpenBSD

**Key features:**
- Simple mode (`-s`): Converts Unicode to ASCII for old terminals
- Resume mode (`-r`): Continue previous conversation
- Debug logging (`-l`): Logs to `/tmp/telepresence.log`
- Flow control: Window-based backpressure for large file transfers

### 3. MCP Server (embedded in relay.py)

Model Context Protocol server providing tools to Claude:

| Tool | Description |
|------|-------------|
| `get_cwd` | Get remote working directory |
| `read_file` | Read file with line numbers |
| `write_file` | Write entire file |
| `edit_file` | Surgical string replacement |
| `list_directory` | List directory contents |
| `file_info` | Get file metadata (size, type, mtime) |
| `file_exists` | Check if path exists |
| `find_files` | Find files by name pattern |
| `search_files` | Search file contents |
| `execute_command` | Run shell command |
| `make_directory` | Create directory |
| `remove_file` | Delete file |
| `move_file` | Move/rename file |
| `download_url` | Fetch URL via Linux, save to remote /tmp |
| `upload_to_host` | Copy file from remote to Linux host |
| `download_from_host` | Copy file from Linux host to remote |
| `list_host_directory` | List files on Linux host |

## Data Flow

### Terminal I/O

```
User types "ls" on legacy terminal
  → Client sends TERM_INPUT packet with "ls\n"
  → Relay writes to Claude's PTY
  → Claude processes, generates response
  → Relay reads PTY output
  → Relay sends TERM_OUTPUT packet
  → Client displays on terminal
```

### File Read (MCP Tool)

```
Claude calls mcp__telepresence__read_file(path="foo.c")
  → MCP server receives JSON-RPC request
  → Relay sends STREAM_OPEN (STREAM_FILE_READ) to client
  → Client opens file, streams chunks via STREAM_DATA
  → Client sends STREAM_END with status
  → Relay assembles content, formats with line numbers
  → MCP returns result to Claude
```

### Command Execution

```
Claude calls mcp__telepresence__execute_command(command="make")
  → MCP server receives request
  → Relay sends STREAM_OPEN (STREAM_EXEC) to client
  → Client forks shell, executes command
  → Client streams stdout/stderr via STREAM_DATA
  → Client sends STREAM_END with exit code
  → Relay returns output to Claude
```

## Flow Control

Large file transfers use window-based flow control to prevent buffer overflow on legacy systems with limited memory:

1. Both sides advertise initial window size in HELLO/HELLO_ACK
2. Sender tracks bytes_in_flight
3. When window fills, sender waits for WINDOW_UPDATE
4. Receiver sends WINDOW_UPDATE after processing data
5. Default window: 64KB (suitable for 16MB+ RAM systems)

## Building

### Client (on legacy system)

```bash
# NeXTSTEP
cc -o claude-telepresence client.c

# HP-UX with ANSI compiler
cc -Aa -o claude-telepresence client.c

# Solaris
cc -o claude-telepresence client.c -lsocket -lnsl

# IRIX / AIX / FreeBSD / Linux
cc -o claude-telepresence client.c
```

### Relay (on Linux)

```bash
# Just needs Python 3.7+
python3 relay.py --port 5000
```

## Files

```
claude-telepresence/
├── relay.py                 # Main relay + MCP server
├── client.c                 # Portable C89 client
├── telepresence_prompt.txt  # System prompt for Claude
├── ARCHITECTURE.md          # This file
├── PROTOCOL.md              # Binary protocol specification
├── README.md                # Quick start guide
└── TEST_PROTOCOL.md         # Functional test checklist
```

## Current Limitations

1. **Command timeouts**: Long-running commands may timeout. Use background execution (`nohup ... &`) for operations over ~60 seconds.

2. **No encryption**: All traffic is plaintext. Use only on trusted networks.

3. **Single client**: One client connection at a time.

4. **Search performance**: Native search reads files line-by-line; can be slow on large directory trees.

5. **Symlinks skipped**: Find/search skip symlinks to prevent infinite loops.

6. **Recursion depth**: Limited to 64 levels deep.
