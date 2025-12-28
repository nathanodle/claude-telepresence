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

## Components

### 1. Relay (`relay.py`)

The relay is the orchestrator running on Linux. It:

- **Listens on TCP port 5000** for legacy client connections
- **Spawns Claude Code** in a PTY with:
  - PreToolUse hooks to intercept Bash commands
  - MCP server configuration for file operations
  - System prompt explaining telepresence mode
- **Bridges terminal I/O** between Claude's PTY and the TCP client
- **Runs MCP HTTP server** on port 5001 for file operations
- **Routes file operation requests** to the remote client

**Key functions:**
- `spawn_claude()` - Forks Claude Code with hooks and environment
- `handle_mcp_connection()` - HTTP server for MCP JSON-RPC
- `_mcp_tool_*()` - Individual MCP tool implementations
- `pty_to_client()` / `client_to_pty()` - Bidirectional I/O

### 2. Client (`client.c`)

Portable C89 client for legacy Unix systems. It:

- **Connects to relay** via TCP
- **Sends hello message** with current working directory
- **Runs terminal in raw mode** for full-screen TUI support
- **Forwards keyboard input** to relay as JSON messages
- **Displays terminal output** from Claude
- **Executes local operations** (file read/write, commands) on request

**Platform support:**
- NeXTSTEP (uses sgtty instead of termios, sys/dir.h instead of dirent.h)
- HP-UX, IRIX, Solaris, AIX, Linux

**Key features:**
- Simple mode (`-s`): Converts Unicode to ASCII for old terminals
- Debug logging (`-l`): Logs to `/tmp/telepresence.log`
- Native file operations: read, write, stat, readdir, mkdir, etc.
- Native find/search: Recursive directory walk without shell commands

### 3. MCP Server (embedded in relay.py)

Model Context Protocol server providing file operation tools to Claude:

| Tool | Description |
|------|-------------|
| `get_cwd` | Get remote working directory |
| `read_file` | Read file with line numbers (like native Read) |
| `write_file` | Write entire file |
| `edit_file` | Surgical string replacement (like native Edit) |
| `list_directory` | List directory contents |
| `file_info` | Get file metadata (size, type, mtime) |
| `file_exists` | Check if path exists |
| `find_files` | Find files by name pattern (native, no shell) |
| `search_files` | Search file contents (native, no shell) |
| `execute_command` | Run shell command |
| `make_directory` | Create directory |
| `remove_file` | Delete file |
| `move_file` | Move/rename file |

### 4. PreToolUse Hook (`plugin/hooks/telepresence_proxy.py`)

Intercepts Claude's tool calls:

- **Bash**: Rewrites command to execute via helper on remote system
- **Read/Write/Glob/Grep**: Denies with hint to use MCP tools instead

Note: The hook denial hints currently don't display due to a Claude Code issue with read-only tool hooks. The system prompt guides Claude to use MCP tools.

## Message Protocol

All messages are length-prefixed JSON over TCP:

```
[4 bytes: big-endian length][JSON payload]
```

### Message Types

**Client → Relay:**
```json
{"type": "hello", "cwd": "/Users/njodle"}
{"type": "terminal_input", "data": "ls -la\n"}
{"type": "resize", "rows": 24, "cols": 80}
{"type": "response", "id": 123, "result": {...}}
```

**Relay → Client:**
```json
{"type": "terminal_output", "data": "...\r\n"}
{"type": "request", "id": 123, "op": "fs.readFile", "params": {"path": "/etc/hosts"}}
```

### File Operation Types

| Operation | Params | Response |
|-----------|--------|----------|
| `fs.readFile` | `{path}` | `{result: "base64..."}` |
| `fs.writeFile` | `{path, data}` | `{result: true}` |
| `fs.stat` | `{path}` | `{result: {size, mode, mtime, isFile, isDirectory}}` |
| `fs.readdir` | `{path}` | `{result: [{name, isFile, isDirectory}, ...]}` |
| `fs.exists` | `{path}` | `{result: true/false}` |
| `fs.mkdir` | `{path}` | `{result: true}` |
| `fs.unlink` | `{path}` | `{result: true}` |
| `fs.rename` | `{oldPath, newPath}` | `{result: true}` |
| `fs.find` | `{path, pattern}` | `{result: ["path1", "path2", ...]}` |
| `fs.search` | `{path, pattern, filePattern}` | `{result: ["file:line:content", ...]}` |
| `cp.exec` | `{command}` | `{result: {status, stdout, stderr}}` |

## Data Flow Examples

### User Types a Command

```
1. User types "ls" on NeXT terminal
2. Client reads keystrokes, sends: {"type":"terminal_input","data":"ls\n"}
3. Relay writes "ls\n" to Claude's PTY
4. Claude processes, uses Bash tool
5. Hook intercepts, rewrites to use helper (currently broken for non-Bash)
6. Claude sees output, sends response to PTY
7. Relay reads PTY, sends: {"type":"terminal_output","data":"file1\nfile2\n"}
8. Client displays output on terminal
```

### Claude Reads a File (via MCP)

```
1. Claude calls mcp__telepresence__read_file(path="foo.c")
2. MCP server receives JSON-RPC request
3. Relay sends to client: {"type":"request","id":1,"op":"fs.readFile","params":{"path":"/Users/njodle/foo.c"}}
4. Client opens file, base64 encodes, responds: {"type":"response","id":1,"result":"...base64..."}
5. Relay decodes, formats with line numbers
6. MCP returns result to Claude
```

## Building

### Client (on legacy system)

```bash
# NeXTSTEP
cc -o claude-telepresence client.c

# HP-UX / Solaris
cc -o claude-telepresence client.c -lsocket -lnsl

# Linux (for testing)
gcc -o claude-telepresence client.c
```

### Relay (on Linux)

```bash
# Just needs Python 3.7+
python3 relay.py --port 5000
```

## Running

### Start Relay (Linux)
```bash
cd /path/to/claude-telepresence
python3 relay.py --port 5000
```

### Connect Client (Legacy Unix)
```bash
./claude-telepresence <linux-host> 5000

# With options:
./claude-telepresence -s -l <linux-host> 5000  # simple mode + logging
```

## Current Limitations

1. **Hook hints not showing**: PreToolUse hooks for Read/Write/Glob/Grep don't trigger properly in Claude Code. Claude is guided by system prompt instead.

2. **Search performance**: Native search reads every file line-by-line; can be slow on large directory trees.

3. **No regex in search**: Uses simple substring matching (strstr), not regex.

4. **Symlinks skipped**: Find/search skip symlinks entirely to prevent infinite loops.

5. **Depth limit**: Recursion limited to 64 levels deep.

## Files

```
claude-telepresence/
├── relay.py                 # Main relay + MCP server
├── client.c                 # Portable C client
├── ARCHITECTURE.md          # This file
├── plugin/
│   └── hooks/
│       └── telepresence_proxy.py  # PreToolUse hook
└── .claude/
    └── settings.json.bak    # Backed up hook settings
```

## Environment Variables

Set by relay for Claude:
- `TELEPRESENCE_SOCKET` - Unix socket path for helper
- `TELEPRESENCE_HELPER` - Path to helper binary
- `TELEPRESENCE_REMOTE_CWD` - Remote client's working directory
