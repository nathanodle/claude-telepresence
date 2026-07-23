# Claude Telepresence - Windows Client (v2 protocol)

This is a Windows port of the claude-telepresence client, speaking the
binary v2 protocol described in `PROTOCOL.md`. It lets you use Claude
Code from a Windows machine while the actual `claude` process and file
operations run on your Linux host via `relay.py`.

It targets Windows 98/98SE all the way through XP/2000 — the source
sticks to Win32 APIs that exist on Windows 95 OSR2/98 (ANSI `A`-suffixed
calls only, no NTFS-only or services APIs, no overlapped I/O/IOCP, no
wide-char/Unicode functions).

## Requirements

### Windows System
- Windows 98/98SE, ME, 2000, XP, or later
- One of the following compilers:
  - MinGW (recommended, works on everything from 98 up)
  - Visual C++ 6.0
  - Visual Studio 2003/2005/2008 or later

### Linux Host
- Python 3.7+
- Claude Code installed and authenticated
- Network access to the Windows machine
- `client.c` built as the relay-side test client if you want to sanity
  check the relay independent of Windows (see main `README.md`)

## Building

### Option 1: Cross-compile from Linux (recommended)

This is the command used to verify `client_winxp.c` builds clean:

```bash
i686-w64-mingw32-gcc -o claude-telepresence.exe client_winxp.c -lws2_32
```

**For real Windows 98 hardware**, add the subsystem-version linker flags
below (see "Windows 98 Compatibility" section) so the Win9x PE loader
will accept the resulting `.exe`:

```bash
i686-w64-mingw32-gcc -o claude-telepresence.exe client_winxp.c -lws2_32 \
  -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
  -Wl,--major-os-version,4 -Wl,--minor-os-version,0
```

Copy the resulting `claude-telepresence.exe` to the Windows machine
(floppy, network share, USB, whatever the vintage box has available).

### Option 2: Native MinGW on Windows

Install MinGW (`mingw32-gcc-g++` package or similar), add `C:\MinGW\bin`
to `PATH`, then from a `cmd.exe`/`COMMAND.COM` prompt:

```cmd
gcc -o claude-telepresence.exe client_winxp.c -lws2_32
```

### Option 3: Visual C++ 6.0 (classic Win98/2000-era toolchain)

```cmd
cl /nologo client_winxp.c ws2_32.lib
```

### Option 4: Visual Studio 2003/2005/2008+

From a "Visual Studio Command Prompt" / "Developer Command Prompt":

```cmd
cl /nologo client_winxp.c ws2_32.lib
```

Note: modern Visual Studio (2015+) builds will not run on Windows 98/2000
unless you specifically target an old toolset — MinGW is the simplest
route for genuinely old hardware.

## Windows 98 Compatibility

The Dell OptiPlex GX1-class Windows 98 target has a few sharp edges this
client works around; know them before you file a bug:

- **PE subsystem version.** Windows 9x's loader rejects executables built
  for a newer subsystem/OS version than it understands. Link with:
  ```
  -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0
  -Wl,--major-os-version,4 -Wl,--minor-os-version,0
  ```
  (subsystem 4.0 = Windows NT 4.0-era baseline, which Win9x's loader
  accepts). Without these flags, a MinGW build defaults to a newer
  subsystem version and the `.exe` will refuse to launch on real Win98.
- **Winsock2.** Windows 98 **Second Edition** ships with `ws2_32.dll`
  (Winsock 2) out of the box. Plain, non-SE Windows 98 does **not** —
  you'll need to install Microsoft's separate "Windows Socket 2 Update"
  (`w95ws2setup.exe`, widely archived) before `claude-telepresence.exe`
  will run. `WSAStartup(MAKEWORD(2,2), ...)` will simply fail with a
  clear error on a machine missing this update.
- **ANSI-only console/file APIs.** All Win32 calls in this file use the
  explicit `A`-suffixed forms (`FindFirstFileA`, `GetFileAttributesA`,
  `ReadConsoleInputA`, `WriteConsoleA`, ...) rather than the `W` (wide
  string) variants, since Win98 has no real UTF-16 Unicode subsystem.
- **No symlinks.** FAT16/FAT32 (and Win98 in general) has no symlink
  concept, so there's no lstat()-equivalent distinction — stat-like
  behavior always follows/reports as a plain file or directory.
- **No `getaddrinfo`.** That API isn't present in Windows 98's Winsock;
  the client resolves hosts with `gethostbyname()`/`inet_addr()` only,
  matching what `client.c` does on Unix.
- **`COMMAND.COM` built-ins don't reliably honor redirected stdout
  handles.** Confirmed on real Win98 hardware: `STARTUPINFO`-based pipe
  redirection (the normal Win32 way to capture a child process's output)
  came back completely empty for built-in commands like `VER`/`DIR`/`MEM`
  — they're implemented with legacy DOS-era I/O internally and don't
  reliably see an inherited Win32 handle. The shell's own `>` file
  redirection does work reliably, so EXEC output is captured by wrapping
  every command in `> tempfile` and reading the file back after the
  process exits, not by reading a pipe. See "Known Limitations" below
  for the resulting stderr caveat.
- **`_popen()`'s pipe-EOF detection is unreliable on real Win98.** Also
  confirmed on real hardware: a command could finish (the physical
  console back at a normal prompt) while the read pipe never reported
  "broken," leaving the client waiting forever for output that would
  never come. Command execution uses `CreateProcess()` directly and
  polls the real process exit code via `GetExitCodeProcess()` instead of
  trusting pipe-closed detection.

## Usage

### 1. Start the Relay on Linux

```bash
cd claude-telepresence
python3 relay.py --port 5000
```

⚠️ **The relay always runs spawned Claude sessions with
`--dangerously-skip-permissions`** (its one-time safety warning is
auto-accepted, not shown). This is not a convenience default — it's a
requirement for this client to work at all: the interactive "Allow this
tool?" confirmation that would otherwise appear before every remote
`execute_command`/file write is itself rendered through the same
`--ax-screen-reader` redraw path a dumb console (Win98's, in particular)
can't reliably display or answer. A stuck confirmation looks identical
to a hung command, with no way to unstick it — so bypassing permission
checks isn't optional here.

**This removes ALL permission checks, not just for the remote Windows
target** — including the relay's own local Bash/Read/Write tools on
whatever machine `relay.py` runs on. Only run this relay on a fully
trusted, isolated network. Don't expose it to anything you wouldn't
also hand full local shell access to.

### 2. Run the Client on Windows

```cmd
claude-telepresence.exe 192.168.1.100 5000
```

Replace `192.168.1.100` with your Linux machine's IP address.

### Command Line Options

```
claude-telepresence.exe [options] <host> <port>

Options:
  -s, --simple   Simple mode: ASCII-only terminal (strips ANSI color,
                 converts UTF-8 box-drawing/spinner/emoji glyphs to
                 plain ASCII approximations). Strongly recommended on
                 the Windows 98 console, which has essentially no
                 Unicode/ANSI-color support.
  -r, --resume   Resume previous session
  -l, --log      Enable debug logging (tries .\telepresence-v2.log,
                 falls back to C:\telepresence-v2.log)
```

Recommended on Windows 98:

```cmd
claude-telepresence.exe -s 192.168.1.100 5000
```

## Protocol

This client speaks the v2 binary protocol described in `PROTOCOL.md` —
the same wire format used by the reference Unix client (`client.c`), not
the old JSON-based v1 protocol. It implements the full HELLO/HELLO_ACK
handshake, terminal I/O passthrough (TERM_INPUT/TERM_OUTPUT/TERM_RESIZE),
connection-level flow control (WINDOW_UPDATE), and every stream type in
section 5 of `PROTOCOL.md`:

| Stream type   | Implemented | Windows mechanism |
|---------------|-------------|--------------------|
| FILE_READ     | Yes | `fopen`/`fread` (binary mode) |
| FILE_WRITE    | Yes | `fopen`/`fwrite` (binary mode); Windows has no Unix permission bits, so the `mode` field is accepted but ignored |
| EXEC          | Yes (stdout only — see limitations) | `CreateProcess()` (respects `COMSPEC`) + `GetExitCodeProcess()` polling; output captured via `> tempfile` shell redirect, not a pipe |
| DIR_LIST      | Yes | `FindFirstFileA`/`FindNextFileA` |
| FILE_STAT     | Yes | `FindFirstFileA` (falls back to `GetFileAttributesA` for paths like `C:\` that `FindFirstFile` can't enumerate) |
| FILE_FIND     | Yes | Recursive walk via `FindFirstFileA`/`FindNextFileA` + the same glob matcher as `client.c` |
| FILE_SEARCH   | Yes | Recursive walk + Boyer-Moore-Horspool substring search, ported directly from `client.c` |
| MKDIR         | Yes | `_mkdir` |
| REMOVE        | Yes | `remove()` (files only, matching `client.c`'s `unlink()` semantics — it does not remove directories) |
| MOVE          | Yes | `rename()` |
| FILE_EXISTS   | Yes | `GetFileAttributesA` |
| REALPATH      | Yes | `_fullpath()` |

No stream types were stubbed out or skipped — all twelve from
`PROTOCOL.md` section 5 are implemented.

## Known Limitations

1. **EXEC stderr is not captured.** `client.c` merges a spawned child's
   stdout and stderr into one pipe via `dup2()`. This port doesn't merge
   them: output is captured via the shell's own `> tempfile` redirect
   (see "Windows 98 Compatibility" above for why), and `COMMAND.COM`
   doesn't support `2>&1`-style merged redirection the way `cmd.exe`
   does, so only stdout goes into that file. stderr goes directly to the
   local Windows console instead of being relayed to Claude Code.
2. **File sizes/times are 32-bit-safe but not exhaustively tested past
   4 GB.** `FindFirstFileA`'s `nFileSizeHigh`/`nFileSizeLow` are combined
   into a 64-bit value for the wire format, but FAT16/FAT32 volumes on
   real Win98 hardware cap individual files at 4 GB (FAT32) or 2 GB
   (FAT16) anyway.
3. **No Unix permission bits.** `FILE_STAT`'s `mode` field and
   `FILE_WRITE`'s `mode` parameter are synthesized/ignored respectively,
   since Windows/FAT has no equivalent concept.

STREAM_CANCEL on a running EXEC stream is *not* a limitation here: since
EXEC uses `CreateProcess()` (not `_popen()`), the client holds a real
process `HANDLE` and can `TerminateProcess()` it immediately, the same
way `client.c` does `kill(pid, SIGTERM)` on Unix.

## Differences from the Unix Client

1. **Paths**: Windows paths (`C:\`, backslashes) instead of Unix paths.
2. **No symlinks**: stat-like behavior always reports plain
   file/directory (no lstat-vs-stat distinction).
3. **Log location**: debug log defaults to `.\telepresence-v2.log`,
   falling back to `C:\telepresence-v2.log`.
4. **Shell**: commands execute via whatever `COMSPEC` resolves to —
   `COMMAND.COM` on Windows 9x, `cmd.exe` on the NT family — never
   hardcoded.
5. **stderr handling**: see "Known Limitations" above.

## Troubleshooting

### "WSAStartup failed"
- Plain (non-SE) Windows 98 needs Microsoft's Winsock 2 update installed
  separately — see "Windows 98 Compatibility" above.

### The .exe won't launch on Windows 98 ("not a valid Win32 application" or silently fails)
- It was almost certainly linked without the subsystem-version flags.
  Rebuild with:
  ```bash
  i686-w64-mingw32-gcc -o claude-telepresence.exe client_winxp.c -lws2_32 \
    -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
    -Wl,--major-os-version,4 -Wl,--minor-os-version,0
  ```

### Can't connect
- Check firewall settings on both machines
- Verify the relay is running: `python3 relay.py --port 5000`

### Garbled output
- Use `-s` (simple mode) for ASCII-only output — this matters much more
  on Windows 98's console than on XP+.

### Commands not working
- Windows uses `COMMAND.COM`/`cmd.exe` — Linux commands won't work.
  Use Windows commands: `dir` instead of `ls`, `type` instead of `cat`.
- Remember stderr from executed commands is not relayed (see Known
  Limitations) — it prints directly on the Windows console.

## Building on Linux for Windows (cross-compile) — verified command

```bash
i686-w64-mingw32-gcc -o claude-telepresence.exe client_winxp.c -lws2_32
```

This builds cleanly with zero errors or warnings from
`i686-w64-mingw32-gcc` (GCC 13). Add the subsystem-version flags above
before shipping a binary you intend to run on real Windows 98 hardware.

## Security Warning

This tool uses **unencrypted TCP**. All data — terminal I/O, file
contents, command output — is sent in plain text, exactly as specified
in `PROTOCOL.md` section 15. Only use it on trusted private networks.
