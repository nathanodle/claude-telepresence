# claude-telepresence

Run Claude Code on modern Linux while operating on legacy Unix systems (NeXTSTEP, HP-UX, IRIX, Solaris, AIX) via TCP.

> **⚠️ Security Warning:** This tool is designed for vintage systems that cannot support modern security. There is **no encryption** - all traffic (including terminal I/O and file contents) is sent in plain text over TCP. Do not use on untrusted networks. Run on isolated/private networks only. **Use at your own risk.**

> **🤖 AI-Generated Code:** This codebase was written primarily by Claude (Anthropic's AI) and has not been thoroughly reviewed by a human. It may contain bugs, security vulnerabilities, or other issues. Review the code yourself before using in any important context.

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

## Requirements

**Linux Host:**
- Python 3.7+
- [Claude Code](https://claude.ai/code) installed and authenticated (`claude` command available)
- GCC or compatible C compiler
- Network access to legacy system

**Legacy System:**
- C compiler (cc, gcc, or equivalent)
- TCP/IP networking
- Terminal with at least VT100 support

## Quick Start

### 1. Setup Linux Host

```bash
# Clone the repo
git clone https://github.com/nathanodle/claude-telepresence.git
cd claude-telepresence

# Start the relay (listens on port 5000 by default)
python3 relay.py --port 5000
```

The relay will:
- Start an MCP server on port 5001
- Wait for a client connection on port 5000
- Spawn Claude Code when a client connects

### 2. Setup Legacy Client

Transfer `client.c` to your legacy system (via FTP, NFS, or however you move files), then compile:

```bash
# NeXTSTEP
cc -o claude-telepresence client.c

# HP-UX
cc -o claude-telepresence client.c

# Solaris
cc -o claude-telepresence client.c -lsocket -lnsl

# IRIX / AIX / Tru64
cc -o claude-telepresence client.c

# FreeBSD / NetBSD / OpenBSD
cc -o claude-telepresence client.c

# Linux (for testing)
gcc -o claude-telepresence client.c
```

### 3. Connect

From the legacy system:

```bash
./claude-telepresence <linux-host-ip> 5000
```

You'll see the Claude Code TUI. All your commands and file operations happen on the legacy system!

## Client Options

```bash
./claude-telepresence [options] <host> <port>

Options:
  -s    Simple mode: Convert Unicode to ASCII for old terminals
        (recommended for NeXTSTEP, older HP-UX, etc.)
  -r    Resume previous conversation (continues where you left off)
  -l    Enable debug logging to /tmp/telepresence.log
```

Example with all options:
```bash
./claude-telepresence -s -r -l 192.168.1.100 5000
```

## How It Works

1. **Terminal I/O**: The client connects via TCP and forwards your keystrokes to Claude Code running on Linux. Claude's terminal output streams back to your legacy terminal.

2. **File Operations**: When Claude needs to read/write files, it uses MCP tools that send requests to the client. The client performs the actual file operations locally on the legacy system.

3. **Shell Commands**: When Claude runs shell commands, the relay sends them to the client via MCP. The client executes commands locally and streams output back.

4. **Web Access**: Since legacy systems can't do modern HTTPS, the `download_url` tool fetches files on the Linux host and transfers them to the legacy system.

5. **Host ↔ Remote Transfer**: Files can be transferred directly between the Linux host and legacy system using `upload_to_host` and `download_from_host` tools.

## Features

- Full Claude Code TUI over TCP
- Native file operations (read, write, edit, search, find)
- Shell command execution on remote system
- Unicode to ASCII conversion for old terminals (`-s` flag)
- Animated spinner (`-\|/`), arrows, checkmarks in ASCII
- Web downloads via Linux host (bypasses legacy SSL/TLS limitations)
- Direct file transfer between Linux host and remote system

## MCP Tools

The relay provides these tools to Claude (used automatically):

| Tool | Description |
|------|-------------|
| `read_file` | Read file with line numbers |
| `write_file` | Write entire file |
| `edit_file` | Surgical string replacement |
| `list_directory` | List directory contents |
| `find_files` | Find files by name pattern |
| `search_files` | Search file contents |
| `execute_command` | Run shell command |
| `download_url` | Download URL via Linux, save to /tmp on remote |
| `upload_to_host` | Copy file from remote to Linux host |
| `download_from_host` | Copy file from Linux host to remote |
| `list_host_directory` | List files on Linux host |
| `get_cwd` | Get current directory |
| `file_info` | Get file metadata |
| `file_exists` | Check if path exists |
| `make_directory` | Create directory |
| `remove_file` | Delete file |
| `move_file` | Move/rename file |

## Troubleshooting

**Client won't compile:**
- Check you have a C compiler: `which cc` or `which gcc`
- On Solaris, ensure you're linking socket libs: `-lsocket -lnsl`
- On HP-UX, the bundled K&R compiler works fine

**Can't connect:**
- Verify the relay is running on Linux
- Check firewall allows port 5000
- Test with: `telnet <linux-host> 5000`

**Garbled output:**
- Use `-s` flag for simple/ASCII mode
- Ensure your terminal is set to VT100 or compatible

**Programs complain about unknown terminal type:**
- Set TERM before running curses-based programs: `export TERM=vt100`
- On HP-UX you can also try: `export TERM=hp`

## Known Limitations

**Command timeouts:** Long-running commands may timeout before completion. The relay and client have built-in timeouts that aren't currently configurable. For very long operations, consider breaking them into smaller steps or running them in the background with output redirected to a file. *Configurable timeouts coming in a future release.*

**File transfer size limits:** The `download_url` tool and file operations use fixed-size buffers (currently ~7-10MB). Very large file downloads or transfers may fail or be truncated. For large files, consider using traditional transfer methods (FTP, NFS, etc.) instead of the telepresence file operations. *Improved buffer handling coming in a future release.*

**download_url security:** The `download_url` tool fetches URLs from the relay host without URL validation. Do not run the relay on hosts with access to sensitive internal networks or cloud metadata endpoints (169.254.169.254). The tool also has no download size limit and loads responses fully into memory. *URL validation and size limits coming in a future release.*

## Documentation

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed technical documentation.

## License

MIT
