#!/usr/bin/env python3
"""
claude-telepresence relay

Bridges Claude Code on Linux to a remote legacy Unix client.
- Spawns Claude Code with PreToolUse hook to proxy Bash commands
- Proxies terminal I/O over TCP
- Proxies file operations and commands to remote client for execution
- Provides MCP server for transparent tool access
"""

import argparse
import asyncio
import json
import os
import pty
import signal
import socket
import struct
import sys
import fcntl
import termios
import re
from typing import Optional, Dict, Any, List

# Paths for telepresence components
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
HOOK_SCRIPT = os.path.join(SCRIPT_DIR, 'plugin', 'hooks', 'telepresence_proxy.py')
HELPER_BIN = '/tmp/telepresence-helper'
SOCKET_PATH = '/tmp/telepresence.sock'

# MCP Protocol Constants
MCP_PROTOCOL_VERSION = "2024-11-05"
MCP_SERVER_NAME = "telepresence"
MCP_SERVER_VERSION = "1.0.0"

# MCP Tool Definitions
MCP_TOOLS = [
    {
        "name": "read_file",
        "description": "Read the contents of a file from the remote system. Returns file contents with line numbers. By default reads up to 2000 lines.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to the file to read (absolute or relative to cwd)"
                },
                "offset": {
                    "type": "integer",
                    "description": "Line number to start reading from (0-based). Default: 0"
                },
                "limit": {
                    "type": "integer",
                    "description": "Maximum number of lines to read. Default: 2000"
                }
            },
            "required": ["path"]
        }
    },
    {
        "name": "write_file",
        "description": "Write content to a file on the remote system. Creates the file if it doesn't exist, overwrites entire file if it does. For partial edits, use edit_file instead.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to the file to write"
                },
                "content": {
                    "type": "string",
                    "description": "Content to write to the file"
                }
            },
            "required": ["path", "content"]
        }
    },
    {
        "name": "edit_file",
        "description": "Edit a file by replacing a specific string with new content. Use this for surgical edits instead of rewriting entire files. The old_string must match exactly (including whitespace/indentation).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to the file to edit"
                },
                "old_string": {
                    "type": "string",
                    "description": "The exact string to find and replace (must be unique in the file)"
                },
                "new_string": {
                    "type": "string",
                    "description": "The string to replace it with"
                },
                "replace_all": {
                    "type": "boolean",
                    "description": "If true, replace all occurrences. Default: false (fails if not unique)"
                }
            },
            "required": ["path", "old_string", "new_string"]
        }
    },
    {
        "name": "get_cwd",
        "description": "Get the current working directory on the remote system. Call this first to know where you are.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "required": []
        }
    },
    {
        "name": "list_directory",
        "description": "List the contents of a directory on the remote system. Returns file and directory names. Defaults to current working directory if no path given.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to the directory to list (defaults to current working directory)"
                }
            },
            "required": []
        }
    },
    {
        "name": "file_info",
        "description": "Get metadata about a file or directory (size, modification time, type).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Absolute path to the file or directory"
                }
            },
            "required": ["path"]
        }
    },
    {
        "name": "file_exists",
        "description": "Check if a file or directory exists on the remote system.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Absolute path to check"
                }
            },
            "required": ["path"]
        }
    },
    {
        "name": "search_files",
        "description": "Search for a pattern in files (like grep). Returns matching lines with file paths and line numbers.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "description": "Search pattern (regular expression)"
                },
                "path": {
                    "type": "string",
                    "description": "Directory to search in (searches recursively)"
                },
                "file_pattern": {
                    "type": "string",
                    "description": "Optional glob pattern to filter files (e.g., '*.c')"
                }
            },
            "required": ["pattern", "path"]
        }
    },
    {
        "name": "find_files",
        "description": "Find files matching a name pattern (like glob/find). Returns list of matching file paths.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "description": "Filename pattern with wildcards (e.g., '*.c', 'Makefile')"
                },
                "path": {
                    "type": "string",
                    "description": "Directory to search in (searches recursively)",
                    "default": "."
                }
            },
            "required": ["pattern"]
        }
    },
    {
        "name": "execute_command",
        "description": "Execute a shell command on the remote system. Returns stdout, stderr, and exit status.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": "Shell command to execute"
                }
            },
            "required": ["command"]
        }
    },
    {
        "name": "make_directory",
        "description": "Create a directory on the remote system.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Absolute path of the directory to create"
                }
            },
            "required": ["path"]
        }
    },
    {
        "name": "remove_file",
        "description": "Remove a file from the remote system.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Absolute path of the file to remove"
                }
            },
            "required": ["path"]
        }
    },
    {
        "name": "move_file",
        "description": "Move or rename a file on the remote system.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "source": {
                    "type": "string",
                    "description": "Current path of the file"
                },
                "destination": {
                    "type": "string",
                    "description": "New path for the file"
                }
            },
            "required": ["source", "destination"]
        }
    },
    {
        "name": "download_url",
        "description": "Download a file from a URL and save it to the remote system. Uses the Linux host's modern SSL/TLS to fetch from HTTPS URLs that the legacy system cannot access directly.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "url": {
                    "type": "string",
                    "description": "URL to download from (http or https)"
                },
                "path": {
                    "type": "string",
                    "description": "Path on remote system to save the file"
                }
            },
            "required": ["url", "path"]
        }
    }
]


class TelepresenceRelay:
    def __init__(self, host: str, port: int, claude_cmd: str, mcp_port: int = 5001):
        self.host = host
        self.port = port
        self.mcp_port = mcp_port
        self.claude_cmd = claude_cmd
        self.client_reader = None
        self.client_writer = None
        self.master_fd = None
        self.claude_pid = None
        self.helper_server = None
        self.helper_clients = []
        self.pending_requests = {}
        self.mcp_request_id = 0
        self.mcp_session_id = None
        self.remote_cwd = None  # Remote client's working directory
        self.resume_session = False  # Whether to resume previous session

    async def start(self):
        """Start the relay server."""
        # Clean up old socket
        try:
            os.unlink(SOCKET_PATH)
        except OSError:
            pass

        # Copy helper binary to expected location
        helper_src = os.path.join(SCRIPT_DIR, 'telepresence-helper')
        if os.path.exists(helper_src):
            import shutil
            shutil.copy2(helper_src, HELPER_BIN)
            os.chmod(HELPER_BIN, 0o755)

        # Start Unix socket server for helper
        self.helper_server = await asyncio.start_unix_server(
            self.handle_helper_client,
            path=SOCKET_PATH
        )
        os.chmod(SOCKET_PATH, 0o777)
        print(f"[relay] Inject socket ready at {SOCKET_PATH}")

        # Start MCP HTTP server
        mcp_server = await asyncio.start_server(
            self.handle_mcp_connection,
            '127.0.0.1', self.mcp_port
        )
        print(f"[relay] MCP server ready at http://127.0.0.1:{self.mcp_port}/mcp")
        print(f"[relay] Add to Claude: claude mcp add --transport http telepresence http://127.0.0.1:{self.mcp_port}/mcp")

        # Start TCP server for remote client
        server = await asyncio.start_server(
            self.handle_client,
            self.host, self.port
        )
        print(f"[relay] Listening on {self.host}:{self.port}")
        print("[relay] Waiting for telepresence client...")

        # Run both servers
        async with server, mcp_server:
            await asyncio.gather(
                server.serve_forever(),
                mcp_server.serve_forever()
            )

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        """Handle connection from remote telepresence client."""
        addr = writer.get_extra_info('peername')
        print(f"[relay] Client connected from {addr}")

        self.client_reader = reader
        self.client_writer = writer

        try:
            # Wait for hello message from client to get remote cwd and options
            hello_msg = await self.recv_from_client()
            if hello_msg and hello_msg.get('type') == 'hello':
                self.remote_cwd = hello_msg.get('cwd', '/')
                self.resume_session = hello_msg.get('resume', False)
                print(f"[relay] Remote cwd: {self.remote_cwd}")
                if self.resume_session:
                    print(f"[relay] Resume requested: will continue previous session")
            else:
                print(f"[relay] Warning: Expected hello message, got: {hello_msg}")
                self.remote_cwd = '/tmp'
                self.resume_session = False

            # Spawn Claude Code with PTY (using remote cwd)
            self.spawn_claude()

            # Start tasks for bidirectional I/O
            pty_task = asyncio.create_task(self.pty_to_client())
            client_task = asyncio.create_task(self.client_to_pty())

            # Wait for either task to complete (PTY closed or client disconnected)
            done, pending = await asyncio.wait(
                [pty_task, client_task],
                return_when=asyncio.FIRST_COMPLETED
            )

            # Cancel the other task
            for task in pending:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass

        except Exception as e:
            print(f"[relay] Error: {e}")
        finally:
            print("[relay] Session ended")
            self.cleanup()
            writer.close()
            await writer.wait_closed()

    def spawn_claude(self):
        """Spawn Claude Code with PTY and PreToolUse hook."""
        # Create PTY
        master_fd, slave_fd = pty.openpty()

        # Set up environment
        env = os.environ.copy()
        env['TELEPRESENCE_SOCKET'] = SOCKET_PATH
        env['TELEPRESENCE_HELPER'] = HELPER_BIN
        env['TELEPRESENCE_REMOTE_CWD'] = self.remote_cwd or ''
        env['TERM'] = 'xterm-256color'
        print(f"[relay] TELEPRESENCE_SOCKET={env['TELEPRESENCE_SOCKET']}")
        print(f"[relay] TELEPRESENCE_HELPER={env['TELEPRESENCE_HELPER']}")
        print(f"[relay] TELEPRESENCE_REMOTE_CWD={env['TELEPRESENCE_REMOTE_CWD']}")

        # Build settings JSON with PreToolUse hooks
        # IMPORTANT: We must add "ask" permissions for Read/Write/Glob/Grep
        # because read-only tools don't trigger permission checks by default,
        # and hooks only run when permission checks occur.
        hook_config = {
            "type": "command",
            "command": HOOK_SCRIPT
        }
        settings = {
            "permissions": {
                "ask": [
                    "Read",
                    "Write",
                    "Glob",
                    "Grep"
                ]
            },
            "hooks": {
                "PreToolUse": [
                    {"matcher": "Bash", "hooks": [hook_config]},
                    {"matcher": "Read", "hooks": [hook_config]},
                    {"matcher": "Write", "hooks": [hook_config]},
                    {"matcher": "Glob", "hooks": [hook_config]},
                    {"matcher": "Grep", "hooks": [hook_config]},
                ]
            }
        }
        # Write settings to temp file (avoids CLI parsing issues)
        settings_json_path = '/tmp/telepresence-settings.json'
        with open(settings_json_path, 'w') as f:
            json.dump(settings, f, indent=2)
        print(f"[relay] Settings written to: {settings_json_path}")

        # Create temporary .mcp.json file for this session
        # This is the only way to configure HTTP MCP servers without permanent changes
        mcp_json_path = '/tmp/telepresence-mcp.json'
        mcp_config = {
            "mcpServers": {
                "telepresence": {
                    "type": "http",
                    "url": f"http://127.0.0.1:{self.mcp_port}/mcp"
                }
            }
        }
        with open(mcp_json_path, 'w') as f:
            json.dump(mcp_config, f)

        # Build telepresence system prompt and write to file
        # (avoids argument parsing issues with multi-line strings)
        remote_cwd = self.remote_cwd or '(unknown)'
        system_prompt = f"""TELEPRESENCE MODE - You are connected to a REMOTE legacy Unix system via telepresence.

CRITICAL: Your displayed working directory is WRONG. Ignore it.
ACTUAL remote working directory: {remote_cwd}

FILE OPERATIONS:
- DO NOT use Read, Write, Glob, or Grep tools - they are disabled
- USE these MCP tools instead:
  * mcp__telepresence__read_file(path) - read files
  * mcp__telepresence__edit_file(path, old_string, new_string) - edit files
  * mcp__telepresence__write_file(path, content) - write entire files
  * mcp__telepresence__list_directory(path) - list directory (default: cwd)
  * mcp__telepresence__search_files(pattern, path) - grep-like search
  * mcp__telepresence__find_files(pattern, path) - find files by name
  * mcp__telepresence__get_cwd() - confirm current directory

PATHS: Relative paths resolve against {remote_cwd}
SHELL: Bash commands execute on the remote system automatically.

## Legacy Unix Hints

### Compilers
- Don't assume GCC. Use `cc` first - it's the native compiler (Sun Studio on Solaris, MIPSpro on IRIX, xlc on AIX, HP's cc on HP-UX)
- Native compilers often produce better-optimized code for their platform
- Check for gcc with `which gcc` or `gcc --version` before using it
- C89/C90 is safest. Avoid C99 features (// comments, mixed declarations, variable-length arrays)

### Shell & Commands
- Scripts should use #!/bin/sh, not #!/bin/bash - bash may not exist
- Avoid bashisms: use `[ ]` not `[[ ]]`, no `$()` (use backticks), no arrays
- Command flags differ: `find` has no `-maxdepth`, `grep` has no `-r`, `ls` has no `--color`
- Use `man <command>` - it shows the LOCAL system's flags, not Linux's
- `which` may not exist - use `type` or check $PATH manually

### Make
- Might be ancient make, not GNU make. Avoid:
  - Pattern rules (%.o: %.c) - use suffix rules (.c.o:)
  - $(shell ...), ifdef, ifndef, $@ in prerequisites
  - .PHONY targets (just let them be regular targets)
- GNU make might be `gmake` if installed

### File System
- /usr/local may not exist or be read-only
- Home dirs might be /home/<user>, /users/<user>, /u/<user>, or /Users/<user>
- tmp might be /tmp, /var/tmp, or /usr/tmp
- Case-sensitive everywhere (unlike macOS)
- Path limits vary - keep paths under 256 chars to be safe

### Editors & Tools
- vi is universal. vim might not exist
- emacs is often missing
- ed exists everywhere (line editor, for scripts)
- awk is available but might be old awk, not gawk
- sed exists but extended regex (-E) may not

### Archives & Compression
- tar is universal but flags vary (-cvf works everywhere)
- compress (.Z) is universal, gzip may not be installed
- To extract: `uncompress < foo.tar.Z | tar xvf -`
- bzip2, xz, zip may not exist

### Networking
- Use ifconfig, not ip
- netstat exists, ss doesn't
- ftp/telnet are common, ssh might not be (or might be SSH1 only)
- curl/wget may not exist - use ftp or write a simple socket program

### Libraries
- Shared library extensions vary: .so (most), .sl (HP-UX), .a (static)
- Library paths: /usr/lib, /lib, maybe /usr/local/lib
- Use `ldd` (or `chatr` on HP-UX) to check library deps
- Socket libraries: Solaris/HP-UX need -lsocket -lnsl

### Determining System Type
- `uname -a` - shows OS name, hostname, version, architecture
- `uname -s` - just OS name (SunOS, HP-UX, IRIX, AIX, NeXT, Linux)
- `uname -r` - OS version/release
- `uname -m` - machine architecture (sun4u, PA-RISC, mips, powerpc, m68k, i386)
- `/etc/release` (Solaris), `/etc/hp-release` (HP-UX) - detailed version info
- `oslevel` (AIX) - OS level
- `hinv` (IRIX) - hardware inventory
- `hostinfo` (NeXTSTEP) - system info including CPU type

### System Resources
- Before operations that might hit limits, check available resources:
  - Memory: `free` (Linux), `vmstat` (most), `swap -s` (Solaris), `swapinfo` (HP-UX)
  - Disk: `df -k` (universal)
  - Process limits: `ulimit -a`
  - Running processes: `ps -ef`
- Memory may be 64MB-256MB. Don't load huge files into RAM
- Disk might be small. Clean up temp files when done
- If a command hangs or fails mysteriously, resource exhaustion is a likely cause

### Web Access
- The remote system likely lacks modern SSL/TLS for HTTPS
- Don't use curl/wget via Bash - they run on the remote and will fail on modern HTTPS
- Use mcp__telepresence__download_url(url, path) to download files - it fetches via the Linux host then transfers to remote
- WebFetch and WebSearch tools also run on the Linux host and work normally"""

        # Build Claude command with all flags
        # --strict-mcp-config ensures ONLY our MCP config is used (ignores ~/.claude.json)
        claude_cmd = self.claude_cmd
        base_args = ['--settings', settings_json_path,
                     '--mcp-config', mcp_json_path, '--strict-mcp-config',
                     '--append-system-prompt', system_prompt]

        # Add --resume if client requested it
        if self.resume_session:
            base_args.insert(0, '--resume')

        if claude_cmd == 'claude':
            cmd_parts = ['claude'] + base_args
        elif claude_cmd.startswith('claude '):
            rest = claude_cmd[7:]  # everything after 'claude '
            cmd_parts = ['claude'] + base_args + rest.split()
        else:
            cmd_parts = [claude_cmd] + base_args

        print(f"[relay] PreToolUse hooks: Bash, Read, Write, Glob, Grep")
        print(f"[relay] MCP config: {mcp_json_path}")
        print(f"[relay] MCP server: http://127.0.0.1:{self.mcp_port}/mcp")
        print(f"[relay] System prompt: TELEPRESENCE MODE (remote_cwd={remote_cwd})")

        # Fork
        pid = os.fork()
        if pid == 0:
            # Child process
            os.close(master_fd)
            os.setsid()

            # Set up slave as controlling terminal
            fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)

            # Redirect stdio to PTY
            os.dup2(slave_fd, 0)
            os.dup2(slave_fd, 1)
            os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)

            # Execute Claude Code with settings
            os.execvpe(cmd_parts[0], cmd_parts, env)
            sys.exit(1)

        # Parent process
        os.close(slave_fd)
        self.master_fd = master_fd
        self.claude_pid = pid

        # Make master non-blocking
        flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
        fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

        print(f"[relay] Claude Code spawned (PID {pid})")

    async def pty_to_client(self):
        """Forward PTY output to remote client."""
        loop = asyncio.get_event_loop()

        while True:
            try:
                # Read from PTY
                data = await loop.run_in_executor(None, self.read_pty)
                if data is None:
                    # Error or closed
                    break
                if not data:
                    # Timeout, no data yet - keep waiting
                    continue

                # Send to client as terminal output
                await self.send_to_client({
                    'type': 'terminal_output',
                    'data': data.decode('utf-8', errors='replace')
                })

            except OSError:
                break

    def read_pty(self):
        """Blocking read from PTY (run in executor)."""
        import select
        try:
            r, _, _ = select.select([self.master_fd], [], [], 0.1)
            if r:
                return os.read(self.master_fd, 65536)
            return b''
        except OSError:
            return None

    async def client_to_pty(self):
        """Handle messages from remote client."""
        while True:
            try:
                msg = await self.recv_from_client()
                if msg is None:
                    break

                msg_type = msg.get('type')

                if msg_type == 'terminal_input':
                    # Forward to PTY
                    data = msg.get('data', '')
                    os.write(self.master_fd, data.encode('utf-8'))

                elif msg_type == 'resize':
                    # Handle terminal resize
                    rows = msg.get('rows', 24)
                    cols = msg.get('cols', 80)
                    self.resize_pty(rows, cols)

                elif msg_type == 'response':
                    # Response to a proxied request (from helper)
                    req_id = msg.get('id')
                    print(f"[relay] Got response from remote: id={req_id}")
                    if req_id in self.pending_requests:
                        future = self.pending_requests.pop(req_id)
                        future.set_result(msg)
                    else:
                        print(f"[relay] Warning: no pending request for id={req_id}")

            except Exception as e:
                print(f"[relay] Client message error: {e}")
                break

    def resize_pty(self, rows: int, cols: int):
        """Resize the PTY."""
        if self.master_fd:
            import struct
            winsize = struct.pack('HHHH', rows, cols, 0, 0)
            fcntl.ioctl(self.master_fd, termios.TIOCSWINSZ, winsize)

    async def handle_helper_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        """Handle connection from helper (via helper or async)."""
        print(f"[relay] Inject client connected")
        self.helper_clients.append((reader, writer))

        try:
            while True:
                line = await reader.readline()
                if not line:
                    break

                try:
                    request = json.loads(line.decode('utf-8'))
                    print(f"[relay] Inject request: {request.get('type')} id={request.get('id')}")
                    response = await self.handle_helper_request(request)
                    print(f"[relay] Inject response: {str(response)[:100]}...")
                    writer.write((json.dumps(response) + '\n').encode('utf-8'))
                    await writer.drain()
                except json.JSONDecodeError as e:
                    error_response = {'error': f'JSON decode error: {e}'}
                    writer.write((json.dumps(error_response) + '\n').encode('utf-8'))
                    await writer.drain()

        except Exception as e:
            print(f"[relay] Inject client error: {e}")
        finally:
            print(f"[relay] Inject client disconnected")
            self.helper_clients.remove((reader, writer))
            writer.close()

    async def handle_helper_request(self, request: dict) -> dict:
        """Handle a request from helper - forward to remote client."""
        req_id = request.get('id', 0)
        req_type = request.get('type', '')

        print(f"[relay] Forwarding to remote: {req_type} id={req_id}")

        # Forward to remote client for execution
        await self.send_to_client({
            'type': 'request',
            'id': req_id,
            'op': req_type,
            'params': request.get('params', {})
        })

        # Wait for response from client
        future = asyncio.get_event_loop().create_future()
        self.pending_requests[req_id] = future

        try:
            print(f"[relay] Waiting for response id={req_id}...")
            response = await asyncio.wait_for(future, timeout=300.0)  # 5 min timeout
            print(f"[relay] Got response id={req_id}")
            return response
        except asyncio.TimeoutError:
            print(f"[relay] Timeout waiting for id={req_id}")
            return {'id': req_id, 'error': 'Request timeout'}

    async def send_to_client(self, msg: dict):
        """Send a message to the remote client."""
        if not self.client_writer:
            return

        data = json.dumps(msg).encode('utf-8')
        # Length-prefixed framing
        header = struct.pack('>I', len(data))
        self.client_writer.write(header + data)
        await self.client_writer.drain()

    # =========================================================================
    # MCP HTTP Server
    # =========================================================================

    async def handle_mcp_connection(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        """Handle an incoming MCP HTTP connection."""
        try:
            # Read HTTP request line
            request_line = await reader.readline()
            if not request_line:
                return

            request_line = request_line.decode('utf-8', errors='replace').strip()
            parts = request_line.split(' ')
            if len(parts) < 2:
                await self._send_http_error(writer, 400, "Bad Request")
                return

            method, path = parts[0], parts[1]

            # Read headers
            headers = {}
            while True:
                line = await reader.readline()
                if line == b'\r\n' or line == b'\n' or not line:
                    break
                line = line.decode('utf-8', errors='replace').strip()
                if ':' in line:
                    key, value = line.split(':', 1)
                    headers[key.lower().strip()] = value.strip()

            # Only handle POST /mcp
            if path != '/mcp':
                await self._send_http_error(writer, 404, "Not Found")
                return

            if method == 'POST':
                # Read body
                content_length = int(headers.get('content-length', 0))
                body = b''
                if content_length > 0:
                    body = await reader.readexactly(content_length)

                # Parse JSON-RPC request
                try:
                    request = json.loads(body.decode('utf-8'))
                except json.JSONDecodeError as e:
                    await self._send_jsonrpc_error(writer, None, -32700, f"Parse error: {e}")
                    return

                # Handle the request
                response = await self.handle_mcp_jsonrpc(request)
                await self._send_http_json(writer, response, headers.get('mcp-session-id'))

            elif method == 'GET':
                # SSE stream - not implemented yet, return error
                await self._send_http_error(writer, 405, "Method Not Allowed")

            else:
                await self._send_http_error(writer, 405, "Method Not Allowed")

        except Exception as e:
            print(f"[mcp] Connection error: {e}")
            import traceback
            traceback.print_exc()
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    async def _send_http_error(self, writer: asyncio.StreamWriter, status: int, message: str):
        """Send an HTTP error response."""
        body = json.dumps({"error": message}).encode('utf-8')
        response = (
            f"HTTP/1.1 {status} {message}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode('utf-8') + body
        writer.write(response)
        await writer.drain()

    async def _send_http_json(self, writer: asyncio.StreamWriter, data: dict, session_id: str = None):
        """Send an HTTP JSON response."""
        body = json.dumps(data).encode('utf-8')
        headers = [
            "HTTP/1.1 200 OK",
            "Content-Type: application/json",
            f"Content-Length: {len(body)}",
            "Connection: close",
        ]
        if self.mcp_session_id:
            headers.append(f"Mcp-Session-Id: {self.mcp_session_id}")
        headers.append("")
        headers.append("")
        response = "\r\n".join(headers).encode('utf-8') + body
        writer.write(response)
        await writer.drain()

    async def _send_jsonrpc_error(self, writer: asyncio.StreamWriter, req_id, code: int, message: str):
        """Send a JSON-RPC error response."""
        response = {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {
                "code": code,
                "message": message
            }
        }
        await self._send_http_json(writer, response)

    async def handle_mcp_jsonrpc(self, request: dict) -> dict:
        """Handle a JSON-RPC request for MCP."""
        req_id = request.get('id')
        method = request.get('method', '')
        params = request.get('params', {})

        print(f"[mcp] Request: {method} id={req_id}")

        # Dispatch by method
        if method == 'initialize':
            return await self.mcp_initialize(req_id, params)
        elif method == 'initialized':
            # Client acknowledgment - no response needed
            return {"jsonrpc": "2.0", "id": req_id, "result": {}}
        elif method == 'tools/list':
            return await self.mcp_tools_list(req_id, params)
        elif method == 'tools/call':
            return await self.mcp_tools_call(req_id, params)
        elif method == 'ping':
            return {"jsonrpc": "2.0", "id": req_id, "result": {}}
        else:
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {
                    "code": -32601,
                    "message": f"Method not found: {method}"
                }
            }

    async def mcp_initialize(self, req_id, params: dict) -> dict:
        """Handle MCP initialize request."""
        import secrets
        self.mcp_session_id = secrets.token_hex(16)

        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "capabilities": {
                    "tools": {}
                },
                "serverInfo": {
                    "name": MCP_SERVER_NAME,
                    "version": MCP_SERVER_VERSION
                }
            }
        }

    async def mcp_tools_list(self, req_id, params: dict) -> dict:
        """Handle tools/list request."""
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "tools": MCP_TOOLS
            }
        }

    async def mcp_tools_call(self, req_id, params: dict) -> dict:
        """Handle tools/call request."""
        tool_name = params.get('name', '')
        arguments = params.get('arguments', {})

        print(f"[mcp] Tool call: {tool_name} args={list(arguments.keys())}")

        # Check if remote client is connected
        if not self.client_writer:
            return self._mcp_tool_error(req_id, "Remote client not connected")

        try:
            # Dispatch to tool handler
            if tool_name == 'get_cwd':
                result = self._mcp_tool_get_cwd()
            elif tool_name == 'read_file':
                result = await self._mcp_tool_read_file(arguments)
            elif tool_name == 'write_file':
                result = await self._mcp_tool_write_file(arguments)
            elif tool_name == 'edit_file':
                result = await self._mcp_tool_edit_file(arguments)
            elif tool_name == 'list_directory':
                result = await self._mcp_tool_list_directory(arguments)
            elif tool_name == 'file_info':
                result = await self._mcp_tool_file_info(arguments)
            elif tool_name == 'file_exists':
                result = await self._mcp_tool_file_exists(arguments)
            elif tool_name == 'search_files':
                result = await self._mcp_tool_search_files(arguments)
            elif tool_name == 'find_files':
                result = await self._mcp_tool_find_files(arguments)
            elif tool_name == 'execute_command':
                result = await self._mcp_tool_execute_command(arguments)
            elif tool_name == 'make_directory':
                result = await self._mcp_tool_make_directory(arguments)
            elif tool_name == 'remove_file':
                result = await self._mcp_tool_remove_file(arguments)
            elif tool_name == 'move_file':
                result = await self._mcp_tool_move_file(arguments)
            elif tool_name == 'download_url':
                result = await self._mcp_tool_download_url(arguments)
            else:
                return self._mcp_tool_error(req_id, f"Unknown tool: {tool_name}")

            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": result
            }

        except Exception as e:
            print(f"[mcp] Tool error: {e}")
            import traceback
            traceback.print_exc()
            return self._mcp_tool_error(req_id, str(e))

    def _mcp_tool_error(self, req_id, message: str) -> dict:
        """Return an MCP tool error result."""
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "content": [{"type": "text", "text": f"Error: {message}"}],
                "isError": True
            }
        }

    def _mcp_tool_success(self, text: str) -> dict:
        """Return an MCP tool success result."""
        return {
            "content": [{"type": "text", "text": text}],
            "isError": False
        }

    async def _mcp_send_request(self, op_type: str, params: dict) -> dict:
        """Send a request to the remote client and wait for response."""
        self.mcp_request_id += 1
        req_id = self.mcp_request_id + 100000  # Offset to avoid collision with inject requests

        # Send to remote client
        await self.send_to_client({
            'type': 'request',
            'id': req_id,
            'op': op_type,
            'params': params
        })

        # Wait for response
        future = asyncio.get_event_loop().create_future()
        self.pending_requests[req_id] = future

        try:
            response = await asyncio.wait_for(future, timeout=300.0)
            return response
        except asyncio.TimeoutError:
            raise Exception("Request timeout")

    # =========================================================================
    # MCP Tool Implementations
    # =========================================================================

    def _mcp_tool_get_cwd(self) -> dict:
        """Return the current working directory on the remote system."""
        cwd = self.remote_cwd or "(unknown - client did not send cwd)"
        return self._mcp_tool_success(f"Current working directory: {cwd}")

    def _resolve_remote_path(self, path: str) -> str:
        """Resolve a path relative to the remote cwd if not absolute."""
        if not path:
            return path
        if path.startswith('/'):
            return path  # Already absolute
        # Resolve relative to remote cwd
        if self.remote_cwd:
            import posixpath
            return posixpath.normpath(posixpath.join(self.remote_cwd, path))
        return path  # No remote cwd, return as-is

    async def _mcp_tool_read_file(self, args: dict) -> dict:
        """Read file contents with line limits like native Read tool."""
        import base64

        path = args.get('path', '')
        offset = args.get('offset', 0)  # Line offset (0-based, but we display 1-based)
        limit = args.get('limit', 2000)  # Max lines to read

        if not path:
            raise Exception("path is required")

        # Resolve relative paths against remote cwd
        resolved_path = self._resolve_remote_path(path)
        print(f"[mcp] read_file: {path} -> {resolved_path}")

        response = await self._mcp_send_request('fs.readFile', {'path': resolved_path})

        if 'error' in response:
            raise Exception(response['error'])

        content = response.get('result', '')

        # Client always sends base64-encoded file contents
        if content:
            try:
                content = base64.b64decode(content).decode('utf-8', errors='replace')
            except:
                pass  # Decode failed, use as-is (shouldn't happen)

        # Split into lines and apply limits
        lines = content.split('\n')
        total_lines = len(lines)

        # Apply offset and limit
        start = min(offset, total_lines)
        end = min(start + limit, total_lines)
        selected_lines = lines[start:end]

        # Format with line numbers (like cat -n) and truncate long lines
        MAX_LINE_LENGTH = 2000
        formatted_lines = []
        for i, line in enumerate(selected_lines, start=start + 1):
            if len(line) > MAX_LINE_LENGTH:
                line = line[:MAX_LINE_LENGTH] + '... (truncated)'
            formatted_lines.append(f"{i:6}\t{line}")

        result = '\n'.join(formatted_lines)

        # Add info about truncation if needed
        if end < total_lines:
            result += f"\n\n[Showing lines {start+1}-{end} of {total_lines}. Use offset/limit to see more.]"

        return self._mcp_tool_success(result)

    async def _mcp_tool_write_file(self, args: dict) -> dict:
        """Write file contents."""
        path = args.get('path', '')
        content = args.get('content', '')
        if not path:
            raise Exception("path is required")

        resolved_path = self._resolve_remote_path(path)
        print(f"[mcp] write_file: {path} -> {resolved_path}")

        response = await self._mcp_send_request('fs.writeFile', {
            'path': resolved_path,
            'data': content
        })

        if 'error' in response:
            raise Exception(response['error'])

        return self._mcp_tool_success(f"Successfully wrote {len(content)} bytes to {resolved_path}")

    async def _mcp_tool_edit_file(self, args: dict) -> dict:
        """Edit file by replacing old_string with new_string."""
        import base64

        path = args.get('path', '')
        old_string = args.get('old_string', '')
        new_string = args.get('new_string', '')
        replace_all = args.get('replace_all', False)

        if not path:
            raise Exception("path is required")
        if not old_string:
            raise Exception("old_string is required")

        resolved_path = self._resolve_remote_path(path)
        print(f"[mcp] edit_file: {path} -> {resolved_path}")

        # First, read the file
        response = await self._mcp_send_request('fs.readFile', {'path': resolved_path})
        if 'error' in response:
            raise Exception(response['error'])

        content = response.get('result', '')

        # Client always sends base64-encoded file contents
        if content:
            try:
                content = base64.b64decode(content).decode('utf-8', errors='replace')
            except:
                pass  # Decode failed, use as-is

        # Count occurrences
        count = content.count(old_string)

        if count == 0:
            raise Exception(f"old_string not found in file. Make sure whitespace and indentation match exactly.")

        if count > 1 and not replace_all:
            raise Exception(f"old_string found {count} times in file. Use replace_all=true to replace all, or provide more context to make it unique.")

        # Perform replacement
        if replace_all:
            new_content = content.replace(old_string, new_string)
        else:
            new_content = content.replace(old_string, new_string, 1)

        # Write back
        response = await self._mcp_send_request('fs.writeFile', {
            'path': resolved_path,
            'data': new_content
        })

        if 'error' in response:
            raise Exception(response['error'])

        if replace_all:
            return self._mcp_tool_success(f"Replaced {count} occurrence(s) in {resolved_path}")
        else:
            return self._mcp_tool_success(f"Successfully edited {resolved_path}")

    async def _mcp_tool_list_directory(self, args: dict) -> dict:
        """List directory contents."""
        path = args.get('path', '.')
        resolved_path = self._resolve_remote_path(path)
        print(f"[mcp] list_directory: {path} -> {resolved_path}")

        response = await self._mcp_send_request('fs.readdir', {'path': resolved_path})

        if 'error' in response:
            raise Exception(response['error'])

        entries = response.get('result', [])
        if isinstance(entries, list):
            # Handle both string entries and dict entries (with 'name' field)
            names = []
            for entry in entries:
                if isinstance(entry, str):
                    names.append(entry)
                elif isinstance(entry, dict):
                    names.append(entry.get('name', str(entry)))
                else:
                    names.append(str(entry))
            text = '\n'.join(names)
        else:
            text = str(entries)

        return self._mcp_tool_success(text)

    async def _mcp_tool_file_info(self, args: dict) -> dict:
        """Get file metadata."""
        path = args.get('path', '')
        if not path:
            raise Exception("path is required")

        resolved_path = self._resolve_remote_path(path)
        response = await self._mcp_send_request('fs.stat', {'path': resolved_path})

        if 'error' in response:
            raise Exception(response['error'])

        result = response.get('result', {})
        info_lines = []
        if 'size' in result:
            info_lines.append(f"Size: {result['size']} bytes")
        if 'mtime' in result:
            info_lines.append(f"Modified: {result['mtime']}")
        if result.get('isDirectory'):
            info_lines.append("Type: directory")
        elif result.get('isFile'):
            info_lines.append("Type: file")
        elif result.get('isSymbolicLink'):
            info_lines.append("Type: symlink")

        return self._mcp_tool_success('\n'.join(info_lines) or "File exists")

    async def _mcp_tool_file_exists(self, args: dict) -> dict:
        """Check if file exists."""
        path = args.get('path', '')
        if not path:
            raise Exception("path is required")

        resolved_path = self._resolve_remote_path(path)
        response = await self._mcp_send_request('fs.exists', {'path': resolved_path})

        if 'error' in response:
            # Error checking existence - treat as not exists
            return self._mcp_tool_success("false")

        exists = response.get('result', False)
        return self._mcp_tool_success("true" if exists else "false")

    async def _mcp_tool_search_files(self, args: dict) -> dict:
        """Search for pattern in files (native implementation)."""
        pattern = args.get('pattern', '')
        path = args.get('path', '.')
        file_pattern = args.get('file_pattern', '')

        if not pattern:
            raise Exception("pattern is required")

        resolved_path = self._resolve_remote_path(path)
        print(f"[mcp] search_files: pattern={pattern} path={resolved_path} file_pattern={file_pattern}")

        # Use native fs.search instead of shell grep
        response = await self._mcp_send_request('fs.search', {
            'path': resolved_path,
            'pattern': pattern,
            'filePattern': file_pattern or ''
        })

        if 'error' in response:
            raise Exception(response['error'])

        result = response.get('result', [])

        if isinstance(result, list):
            if result:
                return self._mcp_tool_success('\n'.join(result))
            else:
                return self._mcp_tool_success("No matches found")
        else:
            return self._mcp_tool_success(str(result) if result else "No matches found")

    async def _mcp_tool_find_files(self, args: dict) -> dict:
        """Find files matching pattern (native implementation)."""
        pattern = args.get('pattern', '')
        path = args.get('path', '.')

        if not pattern:
            raise Exception("pattern is required")

        resolved_path = self._resolve_remote_path(path)
        print(f"[mcp] find_files: pattern={pattern} path={resolved_path}")

        # Use native fs.find instead of shell find
        response = await self._mcp_send_request('fs.find', {
            'path': resolved_path,
            'pattern': pattern
        })

        if 'error' in response:
            raise Exception(response['error'])

        result = response.get('result', [])

        if isinstance(result, list):
            if result:
                return self._mcp_tool_success('\n'.join(result))
            else:
                return self._mcp_tool_success("No files found")
        else:
            return self._mcp_tool_success(str(result) if result else "No files found")

    async def _mcp_tool_execute_command(self, args: dict) -> dict:
        """Execute shell command."""
        command = args.get('command', '')
        if not command:
            raise Exception("command is required")

        response = await self._mcp_send_request('cp.exec', {'command': command})

        if 'error' in response:
            raise Exception(response['error'])

        result = response.get('result', {})
        stdout = result.get('stdout', '')
        stderr = result.get('stderr', '')
        status = result.get('status', 0)

        output_parts = []
        if stdout:
            output_parts.append(stdout)
        if stderr:
            output_parts.append(f"[stderr]\n{stderr}")
        if status != 0:
            output_parts.append(f"[exit status: {status}]")

        return self._mcp_tool_success('\n'.join(output_parts) or "(no output)")

    async def _mcp_tool_make_directory(self, args: dict) -> dict:
        """Create directory."""
        path = args.get('path', '')
        if not path:
            raise Exception("path is required")

        resolved_path = self._resolve_remote_path(path)
        response = await self._mcp_send_request('fs.mkdir', {'path': resolved_path})

        if 'error' in response:
            raise Exception(response['error'])

        return self._mcp_tool_success(f"Created directory: {resolved_path}")

    async def _mcp_tool_remove_file(self, args: dict) -> dict:
        """Remove file."""
        path = args.get('path', '')
        if not path:
            raise Exception("path is required")

        resolved_path = self._resolve_remote_path(path)
        response = await self._mcp_send_request('fs.unlink', {'path': resolved_path})

        if 'error' in response:
            raise Exception(response['error'])

        return self._mcp_tool_success(f"Removed: {resolved_path}")

    async def _mcp_tool_move_file(self, args: dict) -> dict:
        """Move/rename file."""
        source = args.get('source', '')
        destination = args.get('destination', '')
        if not source or not destination:
            raise Exception("source and destination are required")

        resolved_source = self._resolve_remote_path(source)
        resolved_dest = self._resolve_remote_path(destination)

        response = await self._mcp_send_request('fs.rename', {
            'oldPath': resolved_source,
            'newPath': resolved_dest
        })

        if 'error' in response:
            raise Exception(response['error'])

        return self._mcp_tool_success(f"Moved {resolved_source} to {resolved_dest}")

    async def _mcp_tool_download_url(self, args: dict) -> dict:
        """Download file from URL and save to remote system."""
        import urllib.request
        import urllib.error
        import base64

        url = args.get('url', '')
        path = args.get('path', '')
        if not url:
            raise Exception("url is required")
        if not path:
            raise Exception("path is required")

        resolved_path = self._resolve_remote_path(path)
        print(f"[mcp] download_url: {url} -> {resolved_path}")

        # Fetch URL on Linux host (has modern SSL/TLS)
        try:
            req = urllib.request.Request(url, headers={
                'User-Agent': 'claude-telepresence/1.0'
            })
            with urllib.request.urlopen(req, timeout=60) as response:
                content = response.read()
                content_type = response.headers.get('Content-Type', 'unknown')
        except urllib.error.HTTPError as e:
            raise Exception(f"HTTP error {e.code}: {e.reason}")
        except urllib.error.URLError as e:
            raise Exception(f"URL error: {e.reason}")
        except Exception as e:
            raise Exception(f"Download failed: {e}")

        # Send to client as base64
        encoded = base64.b64encode(content).decode('ascii')

        response = await self._mcp_send_request('fs.writeFile', {
            'path': resolved_path,
            'data': encoded,
            'isBuffer': True
        })

        if 'error' in response:
            raise Exception(response['error'])

        return self._mcp_tool_success(
            f"Downloaded {len(content)} bytes from {url}\n"
            f"Saved to: {resolved_path}\n"
            f"Content-Type: {content_type}"
        )

    # =========================================================================
    # End MCP Server
    # =========================================================================

    async def recv_from_client(self) -> dict:
        """Receive a message from the remote client."""
        if not self.client_reader:
            return None

        try:
            # Read length header
            header = await self.client_reader.readexactly(4)
            length = struct.unpack('>I', header)[0]

            # Read message
            data = await self.client_reader.readexactly(length)
            return json.loads(data.decode('utf-8'))

        except (asyncio.IncompleteReadError, ConnectionResetError):
            return None

    def cleanup(self):
        """Clean up resources."""
        if self.claude_pid:
            try:
                os.kill(self.claude_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            self.claude_pid = None

        if self.master_fd:
            try:
                os.close(self.master_fd)
            except OSError:
                pass
            self.master_fd = None


def main():
    parser = argparse.ArgumentParser(description='Claude Telepresence Relay')
    parser.add_argument('--host', default='0.0.0.0', help='Host to bind to')
    parser.add_argument('--port', '-p', type=int, default=5000, help='Port to listen on')
    parser.add_argument('--mcp-port', type=int, default=5001,
                        help='MCP server port (default: 5001)')
    parser.add_argument('--claude', '-c', default='claude',
                        help='Claude Code command to run (default: claude)')
    args = parser.parse_args()

    relay = TelepresenceRelay(args.host, args.port, args.claude, args.mcp_port)

    try:
        asyncio.run(relay.start())
    except KeyboardInterrupt:
        print("\n[relay] Shutting down...")


if __name__ == '__main__':
    main()
