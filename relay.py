#!/usr/bin/env python3
"""
claude-telepresence relay v2

MCP-only relay for v2 binary protocol.
- No helper binary, no hooks
- Pure MCP server for tool access
- Binary streaming protocol to client
"""

import argparse
import asyncio
import json
import os
import pty
import signal
import struct
import sys
import fcntl
import termios
import secrets
from typing import Optional, Dict, Any, List, Tuple

# =============================================================================
# Protocol Constants
# =============================================================================

PROTOCOL_VERSION = 2

# Packet types
PKT_HELLO = 0x00
PKT_HELLO_ACK = 0x01
PKT_GOODBYE = 0x0D
PKT_PING = 0x0E
PKT_PONG = 0x0F

PKT_TERM_INPUT = 0x10
PKT_TERM_OUTPUT = 0x11
PKT_TERM_RESIZE = 0x12

PKT_STREAM_OPEN = 0x20
PKT_STREAM_DATA = 0x21
PKT_STREAM_END = 0x22
PKT_STREAM_ERROR = 0x23
PKT_STREAM_CANCEL = 0x24

PKT_WINDOW_UPDATE = 0x28

# Stream types
STREAM_FILE_READ = 0x01
STREAM_FILE_WRITE = 0x02
STREAM_EXEC = 0x03
STREAM_DIR_LIST = 0x04
STREAM_FILE_STAT = 0x05
STREAM_FILE_FIND = 0x06
STREAM_FILE_SEARCH = 0x07
STREAM_MKDIR = 0x08
STREAM_REMOVE = 0x09
STREAM_MOVE = 0x0A
STREAM_FILE_EXISTS = 0x0B
STREAM_REALPATH = 0x0C

# HELLO flags
FLAG_RESUME = 0x01
FLAG_SIMPLE = 0x02

# GOODBYE reasons
GOODBYE_NORMAL = 0x00
GOODBYE_PROTOCOL_ERROR = 0x01
GOODBYE_TIMEOUT = 0x02

# Stream status
STATUS_OK = 0x00
STATUS_ERROR = 0x01
STATUS_CANCELLED = 0x02

# Error codes
ERR_NOT_FOUND = 0x01
ERR_PERMISSION = 0x02
ERR_IO_ERROR = 0x03
ERR_TIMEOUT = 0x04
ERR_CANCELLED = 0x05
ERR_NO_MEMORY = 0x06
ERR_INVALID = 0x07
ERR_EXISTS = 0x08
ERR_NOT_DIR = 0x09
ERR_IS_DIR = 0x0A
ERR_UNKNOWN = 0xFF

# Limits
MAX_PAYLOAD = 16 * 1024 * 1024  # 16 MB
DEFAULT_WINDOW = 256 * 1024     # 256 KB
CHUNK_SIZE = 64 * 1024          # 64 KB
WINDOW_UPDATE_THRESHOLD = 8192  # Send WINDOW_UPDATE every 8KB

# MCP Constants
MCP_PROTOCOL_VERSION = "2024-11-05"
MCP_SERVER_NAME = "telepresence"
MCP_SERVER_VERSION = "2.0.0"

# =============================================================================
# MCP Tool Definitions
# =============================================================================

MCP_TOOLS = [
    {
        "name": "read_file",
        "description": "Read the contents of a file from the remote system. Returns file contents with line numbers. By default reads up to 2000 lines.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Path to the file to read"},
                "offset": {"type": "integer", "description": "Line number to start from (0-based). Default: 0"},
                "limit": {"type": "integer", "description": "Maximum lines to read. Default: 2000"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "write_file",
        "description": "Write content to a file on the remote system. Creates or overwrites.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Path to the file"},
                "content": {"type": "string", "description": "Content to write"}
            },
            "required": ["path", "content"]
        }
    },
    {
        "name": "edit_file",
        "description": "Edit a file by replacing a specific string. The old_string must match exactly.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Path to the file"},
                "old_string": {"type": "string", "description": "Text to find (must be unique)"},
                "new_string": {"type": "string", "description": "Replacement text"},
                "replace_all": {"type": "boolean", "description": "Replace all occurrences. Default: false"}
            },
            "required": ["path", "old_string", "new_string"]
        }
    },
    {
        "name": "get_cwd",
        "description": "Get the current working directory on the remote system.",
        "inputSchema": {"type": "object", "properties": {}, "required": []}
    },
    {
        "name": "list_directory",
        "description": "List directory contents. Defaults to current working directory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Directory path (default: cwd)"}
            },
            "required": []
        }
    },
    {
        "name": "file_info",
        "description": "Get file metadata (size, modification time, type).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Path to file or directory"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "file_exists",
        "description": "Check if a file or directory exists.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Path to check"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "search_files",
        "description": "Search for a pattern in files (like grep). Returns matching lines.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "Search pattern"},
                "path": {"type": "string", "description": "Directory to search"},
                "file_pattern": {"type": "string", "description": "Glob to filter files (e.g., '*.c')"}
            },
            "required": ["pattern", "path"]
        }
    },
    {
        "name": "find_files",
        "description": "Find files matching a name pattern (like glob/find).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "Filename pattern (e.g., '*.c')"},
                "path": {"type": "string", "description": "Directory to search (default: '.')"}
            },
            "required": ["pattern"]
        }
    },
    {
        "name": "execute_command",
        "description": "Execute a shell command. Returns stdout, stderr, and exit status.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to execute"}
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
                "path": {"type": "string", "description": "Directory path to create"}
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
                "path": {"type": "string", "description": "Path to remove"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "move_file",
        "description": "Move or rename a file.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "source": {"type": "string", "description": "Current path"},
                "destination": {"type": "string", "description": "New path"}
            },
            "required": ["source", "destination"]
        }
    },
    {
        "name": "download_url",
        "description": "Download a file from URL using the Linux host's modern TLS.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "url": {"type": "string", "description": "URL to download"},
                "path": {"type": "string", "description": "Remote path to save"}
            },
            "required": ["url", "path"]
        }
    },
    {
        "name": "upload_to_host",
        "description": "Copy a file FROM the remote legacy system TO the Linux host. Use this to back up files or retrieve build artifacts from the remote system.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "remote_path": {"type": "string", "description": "Path on the remote legacy system"},
                "host_path": {"type": "string", "description": "Destination path on Linux host"},
                "overwrite": {"type": "boolean", "description": "Overwrite if file exists. Default: false"}
            },
            "required": ["remote_path", "host_path"]
        }
    },
    {
        "name": "download_from_host",
        "description": "Copy a file FROM the Linux host TO the remote legacy system. Use this to transfer source archives, patches, or binaries to the remote system.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "host_path": {"type": "string", "description": "Path on the Linux host"},
                "remote_path": {"type": "string", "description": "Destination path on remote legacy system"},
                "overwrite": {"type": "boolean", "description": "Overwrite if file exists. Default: false"}
            },
            "required": ["host_path", "remote_path"]
        }
    }
]

# =============================================================================
# Packet Encoding/Decoding
# =============================================================================

def encode_packet(pkt_type: int, payload: bytes) -> bytes:
    """Encode a v2 packet: type(1) + length(4) + payload."""
    return struct.pack('>BI', pkt_type, len(payload)) + payload


def encode_string(s: str) -> bytes:
    """Encode null-terminated string."""
    return s.encode('utf-8', errors='replace') + b'\0'


def decode_string(data: bytes, offset: int = 0) -> Tuple[str, int]:
    """Decode null-terminated string, return (string, new_offset)."""
    try:
        end = data.index(b'\0', offset)
    except ValueError:
        # No null terminator found - treat rest of data as string
        return data[offset:].decode('utf-8', errors='replace'), len(data)
    return data[offset:end].decode('utf-8', errors='replace'), end + 1


# =============================================================================
# Relay Implementation
# =============================================================================

class RelayV2:
    def __init__(self, host: str, port: int, mcp_port: int, claude_cmd: str):
        self.host = host
        self.port = port
        self.mcp_port = mcp_port
        self.claude_cmd = claude_cmd

        # Host file transfer base directory (restrict to cwd for security)
        self.host_base_dir = os.path.abspath(os.getcwd())

        # Client connection
        self.client_reader: Optional[asyncio.StreamReader] = None
        self.client_writer: Optional[asyncio.StreamWriter] = None
        self.client_connected = asyncio.Event()

        # Connection state
        self.remote_cwd: str = '/'
        self.remote_window: int = DEFAULT_WINDOW
        self.bytes_in_flight: int = 0           # Bytes we've sent, awaiting client ack
        self.bytes_received_unacked: int = 0    # Bytes received from client, not yet acked
        self.resume_session: bool = False
        self.simple_mode: bool = False

        # Stream management (relay uses even IDs)
        self.next_stream_id: int = 0
        self.pending_streams: Dict[int, asyncio.Future] = {}
        self.stream_data: Dict[int, List[bytes]] = {}

        # PTY
        self.master_fd: Optional[int] = None
        self.claude_pid: Optional[int] = None

        # MCP
        self.mcp_session_id: Optional[str] = None

        # Synchronization
        self.send_lock = asyncio.Lock()
        self.window_available = asyncio.Event()
        self.window_available.set()

        # Packet dispatcher
        self.packet_queue: asyncio.Queue = None
        self.dispatcher_task: asyncio.Task = None

    # =========================================================================
    # Server Startup
    # =========================================================================

    async def start(self):
        """Start TCP and MCP servers."""
        mcp_server = await asyncio.start_server(
            self.handle_mcp_connection,
            '127.0.0.1', self.mcp_port
        )

        tcp_server = await asyncio.start_server(
            self.handle_client,
            self.host, self.port
        )

        print(f"Telepresence Relay v2")
        print(f"  TCP:  {self.host}:{self.port}")
        print(f"  MCP:  http://127.0.0.1:{self.mcp_port}/mcp")
        print(f"Waiting for client...")

        async with tcp_server, mcp_server:
            await asyncio.gather(
                tcp_server.serve_forever(),
                mcp_server.serve_forever()
            )

    # =========================================================================
    # Client Connection
    # =========================================================================

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        """Handle client connection."""
        addr = writer.get_extra_info('peername')
        print(f"\nClient connected: {addr[0]}")

        self.client_reader = reader
        self.client_writer = writer

        # Disable Nagle's algorithm for low-latency
        import socket
        sock = writer.get_extra_info('socket')
        if sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        try:
            # Wait for HELLO
            pkt_type, payload = await self.recv_packet()
            if pkt_type != PKT_HELLO:
                print(f"Protocol error: expected HELLO, got {pkt_type:#x}")
                await self.send_goodbye(GOODBYE_PROTOCOL_ERROR)
                return

            if len(payload) < 6:
                print("Protocol error: HELLO too short")
                await self.send_goodbye(GOODBYE_PROTOCOL_ERROR)
                return

            version = payload[0]
            flags = payload[1]
            window = struct.unpack('>I', payload[2:6])[0]
            cwd, _ = decode_string(payload, 6)

            if version != PROTOCOL_VERSION:
                print(f"Protocol error: version {version} != {PROTOCOL_VERSION}")
                await self.send_goodbye(GOODBYE_PROTOCOL_ERROR)
                return

            self.remote_cwd = cwd
            self.remote_window = window
            self.resume_session = bool(flags & FLAG_RESUME)
            self.simple_mode = bool(flags & FLAG_SIMPLE)

            mode_str = []
            if self.resume_session:
                mode_str.append("resume")
            if self.simple_mode:
                mode_str.append("simple")
            mode_info = f" ({', '.join(mode_str)})" if mode_str else ""
            print(f"  Remote cwd: {cwd}{mode_info}")

            # Send HELLO_ACK
            ack_payload = struct.pack('>BBII', PROTOCOL_VERSION, 0, DEFAULT_WINDOW, 0)
            await self.send_packet(PKT_HELLO_ACK, ack_payload[:6])

            self.client_connected.set()
            self.packet_queue = asyncio.Queue()

            # Spawn Claude Code
            self.spawn_claude()

            # Start I/O tasks
            pty_task = asyncio.create_task(self.pty_to_client())
            dispatcher_task = asyncio.create_task(self.packet_dispatcher())
            terminal_task = asyncio.create_task(self.terminal_handler())

            self.dispatcher_task = dispatcher_task

            done, pending = await asyncio.wait(
                [pty_task, dispatcher_task, terminal_task],
                return_when=asyncio.FIRST_COMPLETED
            )

            for task in pending:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass

        except Exception as e:
            print(f"Error: {e}")
        finally:
            print("Session ended")
            self.cleanup()
            self.client_connected.clear()
            writer.close()
            await writer.wait_closed()

    # =========================================================================
    # Packet I/O
    # =========================================================================

    async def recv_packet(self) -> Tuple[int, bytes]:
        """Receive a v2 packet."""
        header = await self.client_reader.readexactly(5)
        pkt_type = header[0]
        length = struct.unpack('>I', header[1:5])[0]

        if length > MAX_PAYLOAD:
            raise ValueError(f"Payload too large: {length}")

        payload = await self.client_reader.readexactly(length) if length > 0 else b''
        return pkt_type, payload

    async def send_packet(self, pkt_type: int, payload: bytes):
        """Send a v2 packet with flow control."""
        async with self.send_lock:
            if pkt_type in (PKT_STREAM_DATA, PKT_TERM_OUTPUT):
                while self.bytes_in_flight + len(payload) > self.remote_window:
                    self.window_available.clear()
                    await self.window_available.wait()
                self.bytes_in_flight += len(payload)

            packet = encode_packet(pkt_type, payload)
            self.client_writer.write(packet)
            await self.client_writer.drain()

    async def send_goodbye(self, reason: int):
        """Send GOODBYE packet."""
        await self.send_packet(PKT_GOODBYE, bytes([reason]))

    async def ack_received_bytes(self, count: int):
        """Track received bytes and send WINDOW_UPDATE when threshold reached."""
        self.bytes_received_unacked += count
        if self.bytes_received_unacked >= WINDOW_UPDATE_THRESHOLD:
            increment = self.bytes_received_unacked
            self.bytes_received_unacked = 0
            await self.send_packet(PKT_WINDOW_UPDATE, struct.pack('>I', increment))

    # =========================================================================
    # PTY Management
    # =========================================================================

    def spawn_claude(self):
        """Spawn Claude Code with PTY."""
        master_fd, slave_fd = pty.openpty()

        env = os.environ.copy()
        env['TERM'] = 'xterm-256color'

        # Write MCP config
        mcp_config = {
            "mcpServers": {
                "telepresence": {
                    "type": "http",
                    "url": f"http://127.0.0.1:{self.mcp_port}/mcp"
                }
            }
        }
        mcp_config_path = '/tmp/telepresence-mcp-v2.json'
        with open(mcp_config_path, 'w') as f:
            json.dump(mcp_config, f)

        system_prompt = self.build_system_prompt()

        cmd = ['claude', '--mcp-config', mcp_config_path, '--strict-mcp-config',
               '--append-system-prompt', system_prompt]
        if self.resume_session:
            cmd.insert(1, '--resume')

        pid = os.fork()
        if pid == 0:
            # Child
            os.close(master_fd)
            os.setsid()
            fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
            os.dup2(slave_fd, 0)
            os.dup2(slave_fd, 1)
            os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)
            # Close inherited fds
            import resource
            maxfd = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
            for fd in range(3, min(maxfd, 256)):
                try:
                    os.close(fd)
                except OSError:
                    pass
            os.execvpe(cmd[0], cmd, env)
            sys.exit(1)

        # Parent
        os.close(slave_fd)
        self.master_fd = master_fd
        self.claude_pid = pid

        flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
        fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

        print(f"  Claude PID: {pid}")

    def build_system_prompt(self) -> str:
        """Build telepresence system prompt from template file."""
        script_dir = os.path.dirname(os.path.abspath(__file__))
        prompt_file = os.path.join(script_dir, 'telepresence_prompt.txt')

        if not os.path.exists(prompt_file):
            prompt_file = '/etc/telepresence_prompt.txt'
        if not os.path.exists(prompt_file):
            prompt_file = os.path.expanduser('~/.telepresence_prompt.txt')

        if os.path.exists(prompt_file):
            try:
                with open(prompt_file, 'r') as f:
                    template = f.read()
                return template.format(remote_cwd=self.remote_cwd)
            except Exception as e:
                print(f"Warning: Could not load prompt file: {e}")

        return f"""TELEPRESENCE MODE - Connected to REMOTE legacy Unix system.
Remote working directory: {self.remote_cwd}

Use mcp__telepresence__* tools for all remote operations.
Standard Bash/Read/Write tools operate on LOCAL host only."""

    async def pty_to_client(self):
        """Forward PTY output to client."""
        loop = asyncio.get_running_loop()

        while True:
            try:
                data = await loop.run_in_executor(None, self.read_pty)
                if data is None:
                    break
                if data:
                    await self.send_packet(PKT_TERM_OUTPUT, data)
            except Exception:
                break

    def read_pty(self) -> Optional[bytes]:
        """Read from PTY (blocking, run in executor)."""
        import select
        try:
            r, _, _ = select.select([self.master_fd], [], [], 0.01)
            if r:
                return os.read(self.master_fd, 65536)
            return b''
        except OSError:
            return None

    async def packet_dispatcher(self):
        """Read packets from client and dispatch to handlers."""
        while True:
            try:
                pkt_type, payload = await self.recv_packet()

                if pkt_type in (PKT_STREAM_DATA, PKT_STREAM_END, PKT_STREAM_ERROR):
                    if pkt_type == PKT_STREAM_DATA:
                        await self.handle_stream_data(payload)
                        await self.ack_received_bytes(len(payload))
                    elif pkt_type == PKT_STREAM_END:
                        await self.handle_stream_end(payload)
                    elif pkt_type == PKT_STREAM_ERROR:
                        await self.handle_stream_error(payload)

                elif pkt_type in (PKT_TERM_INPUT, PKT_TERM_RESIZE):
                    await self.packet_queue.put((pkt_type, payload))
                    if pkt_type == PKT_TERM_INPUT:
                        await self.ack_received_bytes(len(payload))

                elif pkt_type == PKT_WINDOW_UPDATE:
                    if len(payload) >= 4:
                        increment = struct.unpack('>I', payload[:4])[0]
                        self.bytes_in_flight = max(0, self.bytes_in_flight - increment)
                        self.window_available.set()

                elif pkt_type == PKT_PING:
                    await self.send_packet(PKT_PONG, payload)

                elif pkt_type == PKT_GOODBYE:
                    break

            except asyncio.IncompleteReadError:
                break
            except Exception:
                break

    async def terminal_handler(self):
        """Process terminal packets from queue."""
        while True:
            try:
                pkt_type, payload = await self.packet_queue.get()

                if pkt_type == PKT_TERM_INPUT:
                    if self.master_fd:
                        os.write(self.master_fd, payload)

                elif pkt_type == PKT_TERM_RESIZE:
                    if len(payload) >= 4:
                        rows, cols = struct.unpack('>HH', payload[:4])
                        self.resize_pty(rows, cols)

            except asyncio.CancelledError:
                break
            except Exception:
                break

    def resize_pty(self, rows: int, cols: int):
        """Resize PTY."""
        if self.master_fd:
            winsize = struct.pack('HHHH', rows, cols, 0, 0)
            fcntl.ioctl(self.master_fd, termios.TIOCSWINSZ, winsize)

    # =========================================================================
    # Stream Management
    # =========================================================================

    def alloc_stream_id(self) -> int:
        """Allocate even stream ID."""
        sid = self.next_stream_id
        self.next_stream_id += 2
        return sid

    async def open_stream(self, stream_type: int, metadata: bytes) -> int:
        """Open a stream to client."""
        stream_id = self.alloc_stream_id()

        self.pending_streams[stream_id] = asyncio.get_event_loop().create_future()
        self.stream_data[stream_id] = []

        payload = struct.pack('>IB', stream_id, stream_type) + metadata
        await self.send_packet(PKT_STREAM_OPEN, payload)

        return stream_id

    async def wait_stream(self, stream_id: int, timeout: float = 300.0) -> Tuple[int, bytes]:
        """Wait for stream completion. Returns (status, extra_data)."""
        try:
            result = await asyncio.wait_for(
                self.pending_streams[stream_id],
                timeout=timeout
            )
            return result
        except asyncio.TimeoutError:
            raise Exception("Stream timeout")
        finally:
            self.pending_streams.pop(stream_id, None)

    def get_stream_data(self, stream_id: int) -> bytes:
        """Get accumulated stream data as single bytes."""
        chunks = self.stream_data.pop(stream_id, [])
        return b''.join(chunks)

    def get_stream_chunks(self, stream_id: int) -> List[bytes]:
        """Get stream data as separate chunks (preserves boundaries)."""
        return self.stream_data.pop(stream_id, [])

    async def handle_stream_data(self, payload: bytes):
        """Handle STREAM_DATA from client."""
        if len(payload) < 4:
            return
        stream_id = struct.unpack('>I', payload[:4])[0]
        data = payload[4:]

        if stream_id in self.stream_data:
            self.stream_data[stream_id].append(data)

    async def handle_stream_end(self, payload: bytes):
        """Handle STREAM_END from client."""
        if len(payload) < 5:
            return
        stream_id = struct.unpack('>I', payload[:4])[0]
        status = payload[4]
        extra = payload[5:]

        if stream_id in self.pending_streams:
            self.pending_streams[stream_id].set_result((status, extra))

    async def handle_stream_error(self, payload: bytes):
        """Handle STREAM_ERROR from client."""
        if len(payload) < 5:
            return
        stream_id = struct.unpack('>I', payload[:4])[0]
        error_code = payload[4]
        message, _ = decode_string(payload, 5) if len(payload) > 5 else ("Unknown error", 0)

        if stream_id in self.pending_streams:
            self.pending_streams[stream_id].set_exception(
                Exception(f"Stream error {error_code}: {message}")
            )

    # =========================================================================
    # MCP HTTP Server
    # =========================================================================

    async def handle_mcp_connection(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        """Handle MCP HTTP connection."""
        try:
            request_line = await reader.readline()
            if not request_line:
                return

            parts = request_line.decode('utf-8', errors='replace').strip().split(' ')
            if len(parts) < 2:
                await self.send_http_error(writer, 400, "Bad Request")
                return

            method, path = parts[0], parts[1]

            headers = {}
            while True:
                line = await reader.readline()
                if line in (b'\r\n', b'\n', b''):
                    break
                line = line.decode('utf-8', errors='replace').strip()
                if ':' in line:
                    k, v = line.split(':', 1)
                    headers[k.lower().strip()] = v.strip()

            if path != '/mcp':
                await self.send_http_error(writer, 404, "Not Found")
                return

            if method == 'POST':
                content_length = int(headers.get('content-length', 0))
                body = await reader.readexactly(content_length) if content_length > 0 else b''

                try:
                    request = json.loads(body.decode('utf-8'))
                except json.JSONDecodeError as e:
                    await self.send_jsonrpc_error(writer, None, -32700, str(e))
                    return

                response = await self.handle_mcp_request(request)
                await self.send_http_json(writer, response)
            else:
                await self.send_http_error(writer, 405, "Method Not Allowed")

        except Exception:
            pass
        finally:
            writer.close()
            await writer.wait_closed()

    async def send_http_error(self, writer, status: int, message: str):
        """Send HTTP error."""
        body = json.dumps({"error": message}).encode('utf-8')
        response = f"HTTP/1.1 {status} {message}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body
        writer.write(response)
        await writer.drain()

    async def send_http_json(self, writer, data: dict):
        """Send HTTP JSON response."""
        body = json.dumps(data).encode('utf-8')
        headers = f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n"
        if self.mcp_session_id:
            headers += f"Mcp-Session-Id: {self.mcp_session_id}\r\n"
        headers += "\r\n"
        writer.write(headers.encode() + body)
        await writer.drain()

    async def send_jsonrpc_error(self, writer, req_id, code: int, message: str):
        """Send JSON-RPC error."""
        response = {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}
        await self.send_http_json(writer, response)

    async def handle_mcp_request(self, request: dict) -> dict:
        """Handle MCP JSON-RPC request."""
        req_id = request.get('id')
        method = request.get('method', '')
        params = request.get('params', {})

        if method == 'initialize':
            self.mcp_session_id = secrets.token_hex(16)
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "protocolVersion": MCP_PROTOCOL_VERSION,
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": MCP_SERVER_NAME, "version": MCP_SERVER_VERSION}
                }
            }

        elif method == 'initialized':
            return {"jsonrpc": "2.0", "id": req_id, "result": {}}

        elif method == 'tools/list':
            return {"jsonrpc": "2.0", "id": req_id, "result": {"tools": MCP_TOOLS}}

        elif method == 'tools/call':
            return await self.handle_tool_call(req_id, params)

        elif method == 'ping':
            return {"jsonrpc": "2.0", "id": req_id, "result": {}}

        else:
            return {"jsonrpc": "2.0", "id": req_id, "error": {"code": -32601, "message": f"Unknown method: {method}"}}

    # =========================================================================
    # MCP Tool Handlers
    # =========================================================================

    async def handle_tool_call(self, req_id, params: dict) -> dict:
        """Handle MCP tool call."""
        tool_name = params.get('name', '')
        args = params.get('arguments', {})

        if not self.client_connected.is_set():
            return self.tool_error(req_id, "Client not connected")

        try:
            handlers = {
                'get_cwd': self.tool_get_cwd,
                'read_file': self.tool_read_file,
                'write_file': self.tool_write_file,
                'edit_file': self.tool_edit_file,
                'list_directory': self.tool_list_directory,
                'file_info': self.tool_file_info,
                'file_exists': self.tool_file_exists,
                'search_files': self.tool_search_files,
                'find_files': self.tool_find_files,
                'execute_command': self.tool_execute_command,
                'make_directory': self.tool_make_directory,
                'remove_file': self.tool_remove_file,
                'move_file': self.tool_move_file,
                'download_url': self.tool_download_url,
                'upload_to_host': self.tool_upload_to_host,
                'download_from_host': self.tool_download_from_host,
            }

            handler = handlers.get(tool_name)
            if not handler:
                return self.tool_error(req_id, f"Unknown tool: {tool_name}")

            result = await handler(args)
            return {"jsonrpc": "2.0", "id": req_id, "result": result}

        except Exception as e:
            return self.tool_error(req_id, str(e))

    def tool_error(self, req_id, message: str) -> dict:
        """Return tool error response."""
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {"content": [{"type": "text", "text": f"Error: {message}"}], "isError": True}
        }

    def tool_success(self, text: str) -> dict:
        """Return tool success response."""
        return {"content": [{"type": "text", "text": text}], "isError": False}

    def resolve_path(self, path: str) -> str:
        """Resolve path against remote cwd."""
        if not path or path.startswith('/'):
            return path or self.remote_cwd
        import posixpath
        return posixpath.normpath(posixpath.join(self.remote_cwd, path))

    def resolve_host_path(self, path: str) -> str:
        """Resolve and validate host path, restricting to base directory.

        Raises Exception if path escapes base directory.
        """
        # Expand ~ and make absolute
        expanded = os.path.expanduser(path)
        if not os.path.isabs(expanded):
            expanded = os.path.join(self.host_base_dir, expanded)
        resolved = os.path.abspath(expanded)

        # Security check: must be under base directory
        if not resolved.startswith(self.host_base_dir + os.sep) and resolved != self.host_base_dir:
            raise Exception(f"Host path must be under {self.host_base_dir}")

        return resolved

    # -------------------------------------------------------------------------
    # Tool: get_cwd
    # -------------------------------------------------------------------------

    async def tool_get_cwd(self, args: dict) -> dict:
        """Return remote working directory."""
        return self.tool_success(f"Current working directory: {self.remote_cwd}")

    # -------------------------------------------------------------------------
    # Tool: read_file
    # -------------------------------------------------------------------------

    async def tool_read_file(self, args: dict) -> dict:
        """Read file via FILE_READ stream."""
        path = self.resolve_path(args.get('path', ''))
        offset = args.get('offset', 0)
        limit = args.get('limit', 2000)

        if not path:
            raise Exception("path is required")

        stream_id = await self.open_stream(STREAM_FILE_READ, encode_string(path))
        status, extra = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"Read failed: status={status}")

        content = self.get_stream_data(stream_id)

        try:
            text = content.decode('utf-8', errors='replace')
        except:
            text = content.decode('latin-1')

        lines = text.split('\n')
        total = len(lines)
        selected = lines[offset:offset + limit]

        formatted = '\n'.join(f"{i+offset+1:6}\t{line[:2000]}" for i, line in enumerate(selected))

        if offset + limit < total:
            formatted += f"\n\n[Lines {offset+1}-{offset+len(selected)} of {total}]"

        return self.tool_success(formatted)

    # -------------------------------------------------------------------------
    # Tool: write_file
    # -------------------------------------------------------------------------

    async def tool_write_file(self, args: dict) -> dict:
        """Write file via FILE_WRITE stream."""
        path = self.resolve_path(args.get('path', ''))
        content = args.get('content', '')

        if not path:
            raise Exception("path is required")

        metadata = encode_string(path) + struct.pack('>H', 0o644)
        stream_id = await self.open_stream(STREAM_FILE_WRITE, metadata)

        data = content.encode('utf-8')
        for i in range(0, len(data), CHUNK_SIZE):
            chunk = data[i:i + CHUNK_SIZE]
            payload = struct.pack('>I', stream_id) + chunk
            await self.send_packet(PKT_STREAM_DATA, payload)

        payload = struct.pack('>IB', stream_id, STATUS_OK)
        await self.send_packet(PKT_STREAM_END, payload)

        status, extra = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"Write failed: status={status}")

        return self.tool_success(f"Wrote {len(data)} bytes to {path}")

    # -------------------------------------------------------------------------
    # Tool: edit_file
    # -------------------------------------------------------------------------

    async def tool_edit_file(self, args: dict) -> dict:
        """Edit file via read-modify-write."""
        path = self.resolve_path(args.get('path', ''))
        old_string = args.get('old_string', '')
        new_string = args.get('new_string', '')
        replace_all = args.get('replace_all', False)

        if not path:
            raise Exception("path is required")
        if not old_string:
            raise Exception("old_string is required")

        stream_id = await self.open_stream(STREAM_FILE_READ, encode_string(path))
        status, _ = await self.wait_stream(stream_id)
        if status != STATUS_OK:
            raise Exception("Failed to read file")

        content = self.get_stream_data(stream_id).decode('utf-8', errors='replace')

        count = content.count(old_string)
        if count == 0:
            raise Exception("old_string not found in file")
        if count > 1 and not replace_all:
            raise Exception(f"old_string found {count} times. Use replace_all=true or provide more context.")

        if replace_all:
            new_content = content.replace(old_string, new_string)
        else:
            new_content = content.replace(old_string, new_string, 1)

        metadata = encode_string(path) + struct.pack('>H', 0o644)
        stream_id = await self.open_stream(STREAM_FILE_WRITE, metadata)

        data = new_content.encode('utf-8')
        for i in range(0, len(data), CHUNK_SIZE):
            chunk = data[i:i + CHUNK_SIZE]
            payload = struct.pack('>I', stream_id) + chunk
            await self.send_packet(PKT_STREAM_DATA, payload)

        payload = struct.pack('>IB', stream_id, STATUS_OK)
        await self.send_packet(PKT_STREAM_END, payload)

        status, _ = await self.wait_stream(stream_id)
        if status != STATUS_OK:
            raise Exception("Failed to write file")

        msg = f"Replaced {count} occurrence(s)" if replace_all else "Edit successful"
        return self.tool_success(f"{msg} in {path}")

    # -------------------------------------------------------------------------
    # Tool: list_directory
    # -------------------------------------------------------------------------

    async def tool_list_directory(self, args: dict) -> dict:
        """List directory via DIR_LIST stream."""
        path = self.resolve_path(args.get('path', '.'))

        stream_id = await self.open_stream(STREAM_DIR_LIST, encode_string(path))
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"List failed: status={status}")

        data = self.get_stream_data(stream_id)

        entries = []
        offset = 0
        while offset < len(data):
            if offset + 17 > len(data):
                break
            entry_type = chr(data[offset])
            size = struct.unpack('>Q', data[offset+1:offset+9])[0]
            mtime = struct.unpack('>Q', data[offset+9:offset+17])[0]
            name, new_offset = decode_string(data, offset + 17)
            offset = new_offset

            type_char = '/' if entry_type == 'd' else '@' if entry_type == 'l' else ''
            entries.append(f"{name}{type_char}")

        return self.tool_success('\n'.join(entries) if entries else "(empty directory)")

    # -------------------------------------------------------------------------
    # Tool: file_info
    # -------------------------------------------------------------------------

    async def tool_file_info(self, args: dict) -> dict:
        """Get file info via FILE_STAT stream."""
        path = self.resolve_path(args.get('path', ''))

        if not path:
            raise Exception("path is required")

        stream_id = await self.open_stream(STREAM_FILE_STAT, encode_string(path))
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"Stat failed: status={status}")

        data = self.get_stream_data(stream_id)

        if len(data) < 22:
            raise Exception("Invalid stat response")

        exists = data[0]
        if not exists:
            return self.tool_success("File does not exist")

        ftype = chr(data[1])
        mode = struct.unpack('>I', data[2:6])[0]
        size = struct.unpack('>Q', data[6:14])[0]
        mtime = struct.unpack('>Q', data[14:22])[0]

        type_str = {'f': 'file', 'd': 'directory', 'l': 'symlink'}.get(ftype, 'other')
        import datetime
        mtime_str = datetime.datetime.fromtimestamp(mtime).isoformat()

        return self.tool_success(f"Type: {type_str}\nSize: {size} bytes\nModified: {mtime_str}\nMode: {oct(mode)}")

    # -------------------------------------------------------------------------
    # Tool: file_exists
    # -------------------------------------------------------------------------

    async def tool_file_exists(self, args: dict) -> dict:
        """Check file existence via FILE_EXISTS stream."""
        path = self.resolve_path(args.get('path', ''))

        if not path:
            raise Exception("path is required")

        stream_id = await self.open_stream(STREAM_FILE_EXISTS, encode_string(path))
        status, _ = await self.wait_stream(stream_id)

        data = self.get_stream_data(stream_id)
        exists = data[0] if data else 0

        return self.tool_success("true" if exists else "false")

    # -------------------------------------------------------------------------
    # Tool: search_files
    # -------------------------------------------------------------------------

    async def tool_search_files(self, args: dict) -> dict:
        """Search files via FILE_SEARCH stream."""
        pattern = args.get('pattern', '')
        path = self.resolve_path(args.get('path', '.'))
        file_pattern = args.get('file_pattern', '')

        if not pattern:
            raise Exception("pattern is required")

        metadata = encode_string(path) + encode_string(pattern)
        if file_pattern:
            metadata += encode_string(file_pattern)

        stream_id = await self.open_stream(STREAM_FILE_SEARCH, metadata)
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"Search failed: status={status}")

        data = self.get_stream_data(stream_id)

        matches = []
        offset = 0
        while offset < len(data):
            if offset + 4 > len(data):
                break
            line_num = struct.unpack('>I', data[offset:offset+4])[0]
            fpath, offset = decode_string(data, offset + 4)
            line, offset = decode_string(data, offset)
            matches.append(f"{fpath}:{line_num}: {line}")

        return self.tool_success('\n'.join(matches) if matches else "No matches found")

    # -------------------------------------------------------------------------
    # Tool: find_files
    # -------------------------------------------------------------------------

    async def tool_find_files(self, args: dict) -> dict:
        """Find files via FILE_FIND stream."""
        pattern = args.get('pattern', '')
        path = self.resolve_path(args.get('path', '.'))

        if not pattern:
            raise Exception("pattern is required")

        metadata = encode_string(path) + encode_string(pattern)
        stream_id = await self.open_stream(STREAM_FILE_FIND, metadata)
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"Find failed: status={status}")

        data = self.get_stream_data(stream_id)

        paths = []
        offset = 0
        while offset < len(data):
            fpath, offset = decode_string(data, offset)
            if fpath:
                paths.append(fpath)

        return self.tool_success('\n'.join(paths) if paths else "No files found")

    # -------------------------------------------------------------------------
    # Tool: execute_command
    # -------------------------------------------------------------------------

    async def tool_execute_command(self, args: dict) -> dict:
        """Execute command via EXEC stream."""
        command = args.get('command', '')

        if not command:
            raise Exception("command is required")

        stream_id = await self.open_stream(STREAM_EXEC, encode_string(command))
        status, extra = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"Command failed: status={status}")

        exit_code = 0
        if len(extra) >= 4:
            exit_code = struct.unpack('>i', extra[:4])[0]

        chunks = self.get_stream_chunks(stream_id)

        stdout_parts = []
        stderr_parts = []

        for chunk in chunks:
            if len(chunk) < 1:
                continue
            channel = chunk[0]
            data = chunk[1:]
            if channel == 0x01:
                stdout_parts.append(data)
            elif channel == 0x02:
                stderr_parts.append(data)
            else:
                stdout_parts.append(data)

        stdout = b''.join(stdout_parts).decode('utf-8', errors='replace')
        stderr = b''.join(stderr_parts).decode('utf-8', errors='replace')

        parts = []
        if stdout:
            parts.append(stdout)
        if stderr:
            parts.append(f"[stderr]\n{stderr}")
        if exit_code != 0:
            parts.append(f"[exit status: {exit_code}]")

        return self.tool_success('\n'.join(parts) or "(no output)")

    # -------------------------------------------------------------------------
    # Tool: make_directory
    # -------------------------------------------------------------------------

    async def tool_make_directory(self, args: dict) -> dict:
        """Create directory via MKDIR stream."""
        path = self.resolve_path(args.get('path', ''))

        if not path:
            raise Exception("path is required")

        stream_id = await self.open_stream(STREAM_MKDIR, encode_string(path))
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"mkdir failed: status={status}")

        return self.tool_success(f"Created directory: {path}")

    # -------------------------------------------------------------------------
    # Tool: remove_file
    # -------------------------------------------------------------------------

    async def tool_remove_file(self, args: dict) -> dict:
        """Remove file via REMOVE stream."""
        path = self.resolve_path(args.get('path', ''))

        if not path:
            raise Exception("path is required")

        stream_id = await self.open_stream(STREAM_REMOVE, encode_string(path))
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"remove failed: status={status}")

        return self.tool_success(f"Removed: {path}")

    # -------------------------------------------------------------------------
    # Tool: move_file
    # -------------------------------------------------------------------------

    async def tool_move_file(self, args: dict) -> dict:
        """Move file via MOVE stream."""
        source = self.resolve_path(args.get('source', ''))
        dest = self.resolve_path(args.get('destination', ''))

        if not source or not dest:
            raise Exception("source and destination are required")

        metadata = encode_string(source) + encode_string(dest)
        stream_id = await self.open_stream(STREAM_MOVE, metadata)
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"move failed: status={status}")

        return self.tool_success(f"Moved {source} to {dest}")

    # -------------------------------------------------------------------------
    # Tool: download_url
    # -------------------------------------------------------------------------

    async def tool_download_url(self, args: dict) -> dict:
        """Download URL and write to remote.

        Relative paths are saved to /tmp to avoid cluttering working directory.
        Use absolute path to save elsewhere.
        """
        import urllib.request
        import urllib.error

        url = args.get('url', '')
        raw_path = args.get('path', '')

        # Relative paths go to /tmp, absolute paths used as-is
        if raw_path and not raw_path.startswith('/'):
            import posixpath
            path = posixpath.join('/tmp', raw_path)
        else:
            path = raw_path

        if not url:
            raise Exception("url is required")
        if not path:
            raise Exception("path is required")

        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'claude-telepresence/2.0'})
            with urllib.request.urlopen(req, timeout=60) as resp:
                content = resp.read()
                content_type = resp.headers.get('Content-Type', 'unknown')
        except urllib.error.HTTPError as e:
            raise Exception(f"HTTP error {e.code}: {e.reason}")
        except urllib.error.URLError as e:
            raise Exception(f"URL error: {e.reason}")

        metadata = encode_string(path) + struct.pack('>H', 0o644)
        stream_id = await self.open_stream(STREAM_FILE_WRITE, metadata)

        for i in range(0, len(content), CHUNK_SIZE):
            chunk = content[i:i + CHUNK_SIZE]
            payload = struct.pack('>I', stream_id) + chunk
            await self.send_packet(PKT_STREAM_DATA, payload)

        payload = struct.pack('>IB', stream_id, STATUS_OK)
        await self.send_packet(PKT_STREAM_END, payload)

        status, _ = await self.wait_stream(stream_id)
        if status != STATUS_OK:
            raise Exception("Failed to write downloaded file")

        return self.tool_success(f"Downloaded {len(content)} bytes from {url}\nSaved to: {path}\nContent-Type: {content_type}")

    # -------------------------------------------------------------------------
    # Tool: upload_to_host
    # -------------------------------------------------------------------------

    async def tool_upload_to_host(self, args: dict) -> dict:
        """Copy file from remote legacy system to Linux host."""
        remote_path = self.resolve_path(args.get('remote_path', ''))
        host_path_arg = args.get('host_path', '')
        overwrite = args.get('overwrite', False)

        if not remote_path:
            raise Exception("remote_path is required")
        if not host_path_arg:
            raise Exception("host_path is required")

        # Resolve and validate host path (restricted to relay start directory)
        host_path = self.resolve_host_path(host_path_arg)

        # Check if destination exists
        if os.path.exists(host_path) and not overwrite:
            raise Exception(f"Host file already exists: {host_path} (use overwrite=true to replace)")

        # Read from remote
        stream_id = await self.open_stream(STREAM_FILE_READ, encode_string(remote_path))
        status, _ = await self.wait_stream(stream_id)

        if status != STATUS_OK:
            raise Exception(f"Failed to read remote file: {remote_path}")

        content = self.get_stream_data(stream_id)

        # Write to host
        try:
            parent_dir = os.path.dirname(host_path)
            if parent_dir:
                os.makedirs(parent_dir, exist_ok=True)
            with open(host_path, 'wb') as f:
                f.write(content)
        except OSError as e:
            raise Exception(f"Failed to write host file: {e}")

        return self.tool_success(f"Uploaded {len(content)} bytes\nFrom remote: {remote_path}\nTo host: {host_path}")

    # -------------------------------------------------------------------------
    # Tool: download_from_host
    # -------------------------------------------------------------------------

    async def tool_download_from_host(self, args: dict) -> dict:
        """Copy file from Linux host to remote legacy system."""
        host_path_arg = args.get('host_path', '')
        remote_path = self.resolve_path(args.get('remote_path', ''))
        overwrite = args.get('overwrite', False)

        if not host_path_arg:
            raise Exception("host_path is required")
        if not remote_path:
            raise Exception("remote_path is required")

        # Resolve and validate host path (restricted to relay start directory)
        host_path = self.resolve_host_path(host_path_arg)

        # Check if remote destination exists
        if not overwrite:
            stream_id = await self.open_stream(STREAM_FILE_EXISTS, encode_string(remote_path))
            await self.wait_stream(stream_id)
            data = self.get_stream_data(stream_id)
            exists = data[0] if data else 0
            if exists:
                raise Exception(f"Remote file already exists: {remote_path} (use overwrite=true to replace)")

        # Read from host
        try:
            with open(host_path, 'rb') as f:
                content = f.read()
        except FileNotFoundError:
            raise Exception(f"Host file not found: {host_path}")
        except OSError as e:
            raise Exception(f"Failed to read host file: {e}")

        # Write to remote
        metadata = encode_string(remote_path) + struct.pack('>H', 0o644)
        stream_id = await self.open_stream(STREAM_FILE_WRITE, metadata)

        for i in range(0, len(content), CHUNK_SIZE):
            chunk = content[i:i + CHUNK_SIZE]
            payload = struct.pack('>I', stream_id) + chunk
            await self.send_packet(PKT_STREAM_DATA, payload)

        payload = struct.pack('>IB', stream_id, STATUS_OK)
        await self.send_packet(PKT_STREAM_END, payload)

        status, _ = await self.wait_stream(stream_id)
        if status != STATUS_OK:
            raise Exception(f"Failed to write remote file: {remote_path}")

        return self.tool_success(f"Downloaded {len(content)} bytes\nFrom host: {host_path}\nTo remote: {remote_path}")

    # =========================================================================
    # Cleanup
    # =========================================================================

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

        self.pending_streams.clear()
        self.stream_data.clear()
        self.next_stream_id = 0


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description='Claude Telepresence Relay v2')
    parser.add_argument('--host', default='0.0.0.0', help='Host to bind')
    parser.add_argument('--port', '-p', type=int, default=5000, help='TCP port')
    parser.add_argument('--mcp-port', type=int, default=5001, help='MCP port')
    parser.add_argument('--claude', '-c', default='claude', help='Claude command')
    args = parser.parse_args()

    relay = RelayV2(args.host, args.port, args.mcp_port, args.claude)

    try:
        asyncio.run(relay.start())
    except KeyboardInterrupt:
        print("\nShutting down...")


if __name__ == '__main__':
    main()
