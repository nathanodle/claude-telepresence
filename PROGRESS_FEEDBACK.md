# Progress Feedback Strategy

Research and design notes for providing progress feedback during long-running operations.

## Problem

Long-running operations (find, search, download, exec) show no progress. User sees Claude "thinking" with no indication anything is happening. Could appear hung for minutes.

## Operations and Where They Run

| Operation | Client (Legacy Unix) | Relay (Linux) |
|-----------|---------------------|---------------|
| find_files | Walks directories, sends matches | Waits, returns to MCP |
| search_files | Walks + searches files, sends matches | Waits, returns to MCP |
| download_url | Writes chunks to disk | Fetches URL, streams chunks |
| execute_command | Forks/execs, streams output | Waits, returns to MCP |

## MCP Progress Support

### Protocol Support (2025-03-26 spec)

MCP fully supports progress notifications via Streamable HTTP transport:

```json
// Client requests progress by including token:
{
  "method": "tools/call",
  "params": {
    "name": "search_files",
    "arguments": {...},
    "_meta": { "progressToken": "abc123" }
  }
}

// Server sends progress notifications:
{
  "method": "notifications/progress",
  "params": {
    "progressToken": "abc123",
    "progress": 1542,
    "total": null,
    "message": "Scanned 1,542 files, 23 matches..."
  }
}
```

Progress value MUST increase with each notification. Total is optional.

### Claude Code Support: NOT YET

From https://github.com/anthropics/claude-code/issues/4157:

> "Claude Code doesn't currently have a generic UI for displaying real-time
> progress from custom MCP servers, though the protocol fully supports it."

All output buffers until tool completion, then displays at once.

## Our Advantage: Terminal Control

We control the PTY/terminal. Can inject progress directly into TERM_OUTPUT, bypassing Claude Code's UI:

```
User's Terminal
┌─────────────────────────────────────────────────────────────┐
│ > search for "TODO" in /usr/src                             │
│ [telepresence] Scanning: 1,234 files, 5 matches...          │ ← Injected
│ [telepresence] Scanning: 2,891 files, 12 matches...         │ ← via
│ [telepresence] Done: 47 matches in 8,442 files (3.2s)       │ ← TERM_OUTPUT
│                                                             │
│ Claude: I found 47 matches. Here are the results...         │
└─────────────────────────────────────────────────────────────┘
```

## Recommended Strategy

### Protocol Addition: PKT_PROGRESS (0x29)

```
PKT_PROGRESS payload:
  stream_id:     u32    - which operation
  progress_type: u8     - what kind of progress
  data:          varies - type-specific

Progress types:
  0x01 FILE_SCAN:  files_checked(u32), matches(u32), current_path(string)
  0x02 BYTE_XFER:  bytes_done(u64), bytes_total(u64)
  0x03 STATUS:     message(string)
```

### find_files / search_files

**Client sends:**
- PKT_PROGRESS (FILE_SCAN) every ~2 seconds or ~500 files
- STREAM_DATA for each match (existing)
- STREAM_END when complete (existing)

**Relay does:**
1. Receives PKT_PROGRESS → injects status into TERM_OUTPUT
2. Receives STREAM_DATA → accumulates results
3. Receives STREAM_END → returns complete MCP response
4. Also emits `notifications/progress` per MCP spec (future-proofing)

### download_url

**Relay does:**
1. Fetches URL (knows Content-Length = total size)
2. As chunks sent to client, tracks bytes transferred
3. Injects progress into TERM_OUTPUT: `Downloading: 1.2 MB / 5.0 MB (24%)`
4. Returns MCP response when STREAM_END received

### execute_command

Options (TBD):
- Show output in real-time via TERM_OUTPUT while buffering for MCP
- Or just show periodic "still running..." status
- Most useful for builds, tests with lots of output

## Terminal Output Format

Use carriage return for in-place updates (portable):

```
\r[telepresence] Scanning: 1,234 files, 5 matches...
\r[telepresence] Scanning: 2,567 files, 8 matches...
\r[telepresence] Done: 47 matches in 8,442 files     \n
```

Simple mode: Same approach works (no ANSI cursor control needed).

## Implementation Status

- [ ] PKT_PROGRESS packet type in protocol
- [ ] Client: send progress during find_files
- [ ] Client: send progress during search_files
- [ ] Relay: handle PKT_PROGRESS, inject into TERM_OUTPUT
- [ ] Relay: download_url progress display
- [ ] Relay: emit MCP notifications/progress (future-proof)

## References

- MCP Transports: https://modelcontextprotocol.io/specification/2025-03-26/basic/transports
- MCP Progress: https://modelcontextprotocol.io/specification/2025-03-26/basic/utilities/progress
- Claude Code Issue: https://github.com/anthropics/claude-code/issues/4157
