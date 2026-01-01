# Telepresence Binary Protocol v2

Draft specification for streaming binary protocol replacing JSON-based v1.

## 1. Overview

Bidirectional binary streaming protocol for claude-telepresence. Designed for:
- Streaming without memory limits
- Real-time command output
- Minimal parsing overhead (K&R C compatible)
- Simple flow control

**Non-goals:** Encryption, authentication, compression. Assumes trusted network.

## 2. Packet Format

All packets use length-prefixed framing:

```
+----------+------------+-----------------+
| Type (1) | Length (4) | Payload (0-N)   |
+----------+------------+-----------------+
```

- **Type**: 1 byte, packet type
- **Length**: 4 bytes, big-endian, payload size (not including header)
- **Payload**: 0 to 16,777,215 bytes (16MB max)

Total packet size: 5 + Length bytes.

## 3. Data Types

All multi-byte integers are **big-endian** (network byte order).

| Type | Size | Description |
|------|------|-------------|
| uint8 | 1 | Unsigned byte |
| uint16 | 2 | Unsigned 16-bit integer |
| uint32 | 4 | Unsigned 32-bit integer |
| int32 | 4 | Signed 32-bit integer (two's complement) |
| uint64 | 8 | Unsigned 64-bit integer |
| string | N+1 | Null-terminated byte sequence |
| bytes | N | Raw bytes, length from packet |

Strings are raw bytes (filesystem encoding), not assumed UTF-8.

## 4. Packet Types

### 4.1 Control (0x00-0x0F)

| Type | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x00 | HELLO | C→R | version(uint8) + flags(uint8) + window(uint32) + cwd(string) |
| 0x01 | HELLO_ACK | R→C | version(uint8) + flags(uint8) + window(uint32) |
| 0x0E | PING | Both | timestamp(uint64) |
| 0x0F | PONG | Both | timestamp(uint64) |
| 0x0D | GOODBYE | Both | reason(uint8) |

**Version**: Current protocol version is `2`.

**HELLO Flags (bitmask):**
| Bit | Meaning |
|-----|---------|
| 0 | Resume previous session |
| 1 | Simple mode (ASCII terminal) |
| 2-7 | Reserved (must be 0) |

**GOODBYE Reasons:**
| Value | Meaning |
|-------|---------|
| 0x00 | Normal shutdown |
| 0x01 | Protocol error |
| 0x02 | Timeout |
| 0x03 | Resource exhaustion |
| 0xFF | Unknown/other |

### 4.2 Terminal (0x10-0x1F)

| Type | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x10 | TERM_INPUT | C→R | raw bytes |
| 0x11 | TERM_OUTPUT | R→C | raw bytes |
| 0x12 | TERM_RESIZE | C→R | rows(uint16) + cols(uint16) |

Terminal is a single implicit stream, no stream ID needed.

### 4.3 Streams (0x20-0x2F)

| Type | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x20 | STREAM_OPEN | Both | stream_id(uint32) + type(uint8) + metadata... |
| 0x21 | STREAM_DATA | Both | stream_id(uint32) + data... |
| 0x22 | STREAM_END | Both | stream_id(uint32) + status(uint8) + extra... |
| 0x23 | STREAM_ERROR | Both | stream_id(uint32) + code(uint8) + message(string) |
| 0x24 | STREAM_CANCEL | Both | stream_id(uint32) |

### 4.4 Flow Control (0x28-0x2F)

| Type | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x28 | WINDOW_UPDATE | Both | increment(uint32) |

## 5. Stream Types

Specified in STREAM_OPEN metadata byte:

| Type | Name | Initiator | Metadata | Response |
|------|------|-----------|----------|----------|
| 0x01 | FILE_READ | R→C | path(string) | Data chunks |
| 0x02 | FILE_WRITE | R→C | path(string) + mode(uint16) | Status |
| 0x03 | EXEC | R→C | command(string) | Output chunks |
| 0x04 | DIR_LIST | R→C | path(string) | Entry chunks |
| 0x05 | FILE_STAT | R→C | path(string) | Stat record |
| 0x06 | FILE_FIND | R→C | path(string) + pattern(string) | Path chunks |
| 0x07 | FILE_SEARCH | R→C | path(string) + pattern(string) | Match chunks |
| 0x08 | MKDIR | R→C | path(string) | Status |
| 0x09 | REMOVE | R→C | path(string) | Status |
| 0x0A | MOVE | R→C | oldpath(string) + newpath(string) | Status |
| 0x0B | FILE_EXISTS | R→C | path(string) | Status |
| 0x0C | REALPATH | R→C | path(string) | Resolved path |

**FILE_WRITE mode:**
- `0x0000` = default (0644 octal)
- Otherwise = Unix permission bits

Files are always truncated on write. For append semantics, read + modify + write.

## 6. Stream Details

### 6.1 Stream IDs

- Relay-initiated streams: **even** numbers (0, 2, 4, ...)
- Client-initiated streams: **odd** numbers (1, 3, 5, ...)
- Increment by 2 for each new stream
- Do not reuse until stream reaches CLOSED state
- Maximum concurrent streams: 256

### 6.2 EXEC Output Format

STREAM_DATA payload for EXEC streams:
```
stream_id(uint32) + channel(uint8) + data...

channel:
  0x01 = stdout
  0x02 = stderr
```

STREAM_END payload for EXEC streams:
```
stream_id(uint32) + status(uint8) + exit_code(int32)

status:
  0x00 = normal exit (exit_code valid)
  0x01 = killed by signal
  0x02 = timeout
  0xFF = unknown error
```

### 6.3 DIR_LIST Entry Format

Each STREAM_DATA contains one or more entries:
```
type(uint8) + size(uint64) + mtime(uint64) + name(string)

type:
  'f' (0x66) = file
  'd' (0x64) = directory
  'l' (0x6C) = symlink
  '?' (0x3F) = other
```

### 6.4 FILE_STAT Response

Single STREAM_DATA with:
```
exists(uint8) + type(uint8) + mode(uint32) + size(uint64) + mtime(uint64)
```

If exists=0, other fields are undefined.

### 6.5 FILE_SEARCH Match Format

Each STREAM_DATA contains:
```
line_num(uint32) + path(string) + line(string)
```

### 6.6 STREAM_END Status

| Value | Meaning |
|-------|---------|
| 0x00 | Success |
| 0x01 | Error (see STREAM_ERROR) |
| 0x02 | Cancelled |

### 6.7 STREAM_ERROR Codes

| Code | Name | Description |
|------|------|-------------|
| 0x01 | NOT_FOUND | File/path not found |
| 0x02 | PERMISSION | Permission denied |
| 0x03 | IO_ERROR | Read/write failed |
| 0x04 | TIMEOUT | Operation timed out |
| 0x05 | CANCELLED | Cancelled by peer |
| 0x06 | NO_MEMORY | Out of memory |
| 0x07 | INVALID | Invalid request |
| 0x08 | EXISTS | File already exists |
| 0x09 | NOT_DIR | Expected directory |
| 0x0A | IS_DIR | Expected file, got directory |
| 0xFF | UNKNOWN | Unknown error |

## 7. Flow Control

Connection-level sliding window:

1. Each side advertises receive window in HELLO/HELLO_ACK
2. Sender tracks bytes in flight (sent but not acknowledged)
3. Receiver sends WINDOW_UPDATE after consuming data
4. Sender blocks if in-flight bytes >= window

**Window constraints:**
- Minimum: 16 KB
- Maximum: 16 MB
- Recommended: 256 KB

WINDOW_UPDATE payload is the **increment** (bytes consumed), not absolute value.

```
Example:
  Client window = 256KB
  Relay sends 64KB of STREAM_DATA
  Relay in-flight = 64KB, can send 192KB more
  Client processes 64KB, sends WINDOW_UPDATE(65536)
  Relay in-flight = 0KB, can send 256KB
```

## 8. Connection Lifecycle

```
Client                              Relay
  |                                   |
  |  -------- TCP connect ------->    |
  |                                   |
  |  -------- HELLO ------------->    |
  |           (within 10s)            |
  |                                   |
  |  <------- HELLO_ACK ----------    |
  |                                   |
  |  <======= established =======>    |
  |                                   |
  |  -------- GOODBYE ----------->    |
  |  or                               |
  |  <------- GOODBYE ------------    |
  |                                   |
  |  -------- TCP close --------->    |
```

If HELLO_ACK not received within 10 seconds, close connection.

## 9. Stream Lifecycle

```
       STREAM_OPEN
           |
           v
        [OPEN]
           |
     +-----+-----+
     |           |
 STREAM_DATA  STREAM_CANCEL
     |           |
     v           v
  [OPEN]    [CANCELLED]
     |           |
     |     STREAM_END
     |     (status=cancelled)
     |           |
     v           v
 STREAM_END   [CLOSED]
     |
     v
  [CLOSED]

STREAM_ERROR can occur from any state, transitions to [CLOSED].
```

## 10. Path Handling

- Paths are raw byte sequences (filesystem native encoding)
- Maximum path length: 4096 bytes including null terminator
- Relative paths resolve against cwd from HELLO
- Paths MUST NOT contain embedded nulls
- Client SHOULD validate paths stay within allowed scope
- Symlinks: client follows by default (use lstat for no-follow)

## 11. Chunking Guidelines

| Context | Chunk Size | Rationale |
|---------|-----------|-----------|
| FILE_READ/WRITE | 64 KB | Efficiency |
| EXEC output (batch) | 4-8 KB | Balance |
| EXEC output (interactive) | Available | Responsiveness |
| TERM_INPUT/OUTPUT | Available | Latency |

For interactive output, use short timeout (1-10ms) on read, send whatever is available. Don't wait to fill buffer.

## 12. Timeouts

| Event | Timeout | Action |
|-------|---------|--------|
| HELLO_ACK | 10s | Close connection |
| PONG response | 10s | Close connection |
| Idle connection | 300s | Send PING |
| Stream idle | 300s | STREAM_ERROR(TIMEOUT) |

## 13. Error Handling

**Malformed packet (bad length, unknown type < 0x80):**
- Send GOODBYE(reason=PROTOCOL_ERROR)
- Close connection

**Unknown packet type >= 0x80:**
- Ignore (reserved for future extensions)

**Unknown stream type:**
- Send STREAM_ERROR(INVALID) for that stream
- Continue connection

**STREAM_OPEN with existing stream_id:**
- Send STREAM_ERROR(INVALID)
- Continue connection

**Data exceeds window:**
- Protocol violation
- Send GOODBYE(reason=PROTOCOL_ERROR)
- Close connection

## 14. Receiver Implementation

Receivers must handle TCP fragmentation:

```c
/* Pseudocode */
char buf[5 + MAX_PAYLOAD];
int buf_len = 0;

while (connected) {
    n = read(sock, buf + buf_len, sizeof(buf) - buf_len);
    if (n <= 0) break;
    buf_len += n;

    while (buf_len >= 5) {
        type = buf[0];
        length = (buf[1]<<24) | (buf[2]<<16) | (buf[3]<<8) | buf[4];

        if (length > MAX_PAYLOAD) {
            protocol_error();
            break;
        }

        if (buf_len < 5 + length)
            break;  /* incomplete packet */

        handle_packet(type, buf+5, length);

        memmove(buf, buf + 5 + length, buf_len - 5 - length);
        buf_len -= (5 + length);
    }
}
```

## 15. Security Considerations

**This protocol provides NO security.**

- No encryption: all data visible on network
- No authentication: any client can connect
- No integrity: data can be modified in transit

**Deployment requirements:**
- Use only on trusted/isolated networks
- Consider SSH tunnel for untrusted networks
- Client executes commands verbatim - relay must sanitize

**Resource limits enforced:**
- Max packet: 16 MB
- Max path: 4096 bytes
- Max concurrent streams: 256

## 16. Backwards Compatibility

v2 is a clean break from v1 JSON protocol.

**Detection:** First byte distinguishes protocols:
- v1 JSON: starts with `{` (0x7B)
- v2 binary: starts with 0x00 (HELLO)

Relay MAY support both by inspecting first byte.

## 17. Test Vectors

### HELLO
```
Version 2, no flags, 256KB window, cwd="/home/user"

00                            type=HELLO
00 00 00 12                   length=18
02                            version=2
00                            flags=0
00 04 00 00                   window=262144
2F 68 6F 6D 65 2F 75 73       "/home/us"
65 72 00                      "er\0"
```

### HELLO_ACK
```
Version 2, no flags, 256KB window

01                            type=HELLO_ACK
00 00 00 06                   length=6
02                            version=2
00                            flags=0
00 04 00 00                   window=262144
```

### TERM_INPUT
```
User types "ls\n"

10                            type=TERM_INPUT
00 00 00 03                   length=3
6C 73 0A                      "ls\n"
```

### STREAM_OPEN (FILE_READ)
```
Read /etc/passwd, stream_id=2

20                            type=STREAM_OPEN
00 00 00 11                   length=17
00 00 00 02                   stream_id=2
01                            type=FILE_READ
2F 65 74 63 2F 70 61 73       "/etc/pas"
73 77 64 00                   "swd\0"
```

### STREAM_DATA (file content)
```
First chunk "root:x:0:0:root..."

21                            type=STREAM_DATA
00 00 00 18                   length=24
00 00 00 02                   stream_id=2
72 6F 6F 74 3A 78 3A 30       "root:x:0"
3A 30 3A 72 6F 6F 74 2E       ":0:root."
2E 2E 0A                      "..\n"
```

### STREAM_END (success)
```
Stream 2 complete

22                            type=STREAM_END
00 00 00 05                   length=5
00 00 00 02                   stream_id=2
00                            status=OK
```

### STREAM_OPEN (EXEC)
```
Run "make -j4", stream_id=4

20                            type=STREAM_OPEN
00 00 00 0E                   length=14
00 00 00 04                   stream_id=4
03                            type=EXEC
6D 61 6B 65 20 2D 6A 34 00    "make -j4\0"
```

### STREAM_DATA (EXEC stdout)
```
stdout: "Compiling...\n"

21                            type=STREAM_DATA
00 00 00 12                   length=18
00 00 00 04                   stream_id=4
01                            channel=stdout
43 6F 6D 70 69 6C 69 6E       "Compilin"
67 2E 2E 2E 0A                "g...\n"
```

### STREAM_END (EXEC exit 0)
```
Stream 4 exited normally with code 0

22                            type=STREAM_END
00 00 00 09                   length=9
00 00 00 04                   stream_id=4
00                            status=normal
00 00 00 00                   exit_code=0
```

### WINDOW_UPDATE
```
Acknowledge 65536 bytes consumed

28                            type=WINDOW_UPDATE
00 00 00 04                   length=4
00 01 00 00                   increment=65536
```

### STREAM_ERROR
```
Stream 6: file not found

23                            type=STREAM_ERROR
00 00 00 14                   length=20
00 00 00 06                   stream_id=6
01                            code=NOT_FOUND
46 69 6C 65 20 6E 6F 74       "File not"
20 66 6F 75 6E 64 00          " found\0"
```

### GOODBYE
```
Normal shutdown

0D                            type=GOODBYE
00 00 00 01                   length=1
00                            reason=normal
```

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 2.0-draft | 2025-01 | Initial draft |
