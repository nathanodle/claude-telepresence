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

# Build the helper binary (used for proxying bash commands)
make telepresence-helper

# Start the relay (listens on port 5000 by default)
python3 relay.py --port 5000
```

The relay will:
- Copy the helper binary to `/tmp/telepresence-helper`
- Start an MCP server on port 5001
- Wait for a client connection on port 5000

### 2. Setup Legacy Client

Transfer `client.c` to your legacy system (via FTP, NFS, or however you move files), then compile:

```bash
# NeXTSTEP
cc -o claude-telepresence client.c

# HP-UX / Solaris
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
  -l    Enable debug logging to /tmp/telepresence.log
```

Example with all options:
```bash
./claude-telepresence -s -l 192.168.1.100 5000
```

## How It Works

1. **Terminal I/O**: The client connects via TCP and forwards your keystrokes to Claude Code running on Linux. Claude's terminal output streams back to your legacy terminal.

2. **File Operations**: When Claude needs to read/write files, it uses MCP tools that send requests to the client. The client performs the actual file operations locally on the legacy system.

3. **Shell Commands**: Claude's bash commands are intercepted by a hook, proxied through a helper binary, and executed on the legacy system via the client.

4. **Web Access**: Since legacy systems can't do modern HTTPS, the `download_url` tool fetches files on the Linux host and transfers them to the legacy system.

## Features

- Full Claude Code TUI over TCP
- Native file operations (read, write, edit, search, find)
- Shell command execution on remote system
- Unicode to ASCII conversion for old terminals (`-s` flag)
- Animated spinner (`-\|/`), arrows, checkmarks in ASCII
- Web downloads via Linux host (bypasses legacy SSL/TLS limitations)

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
| `download_url` | Download URL via Linux, save to remote |
| `get_cwd` | Get current directory |
| `file_info` | Get file metadata |
| `file_exists` | Check if path exists |
| `make_directory` | Create directory |
| `remove_file` | Delete file |
| `move_file` | Move/rename file |

## Troubleshooting

**Client won't compile:**
- Check you have a C compiler: `which cc` or `which gcc`
- On Solaris/HP-UX, ensure you're linking socket libs: `-lsocket -lnsl`

**Can't connect:**
- Verify the relay is running on Linux
- Check firewall allows port 5000
- Test with: `telnet <linux-host> 5000`

**Garbled output:**
- Use `-s` flag for simple/ASCII mode
- Ensure your terminal is set to VT100 or compatible

**Commands not working:**
- The relay auto-copies the helper to `/tmp/telepresence-helper`
- Check that `/tmp` is writable on the Linux host

## Documentation

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed technical documentation.

## License

MIT
