# Claude Telepresence - Windows XP/2000 Client

This is a Windows port of the claude-telepresence client, allowing you to use Claude Code from Windows XP or Windows 2000 machines.

## Requirements

### Windows System
- Windows 2000, XP, Vista, 7, 8, 10, or 11
- One of the following compilers:
  - MinGW (recommended for XP)
  - Visual C++ 6.0
  - Visual Studio 2003/2005/2008 or later

### Linux Host
- Python 3.7+
- Claude Code installed and authenticated
- Network access to Windows machine

## Building

### Option 1: MinGW (Recommended for Windows XP)

MinGW provides a GCC compiler for Windows that works great on XP.

1. Download MinGW from https://osdn.net/projects/mingw/ or use the archived version
2. Install with at least `mingw32-gcc-g++` package
3. Add `C:\MinGW\bin` to your PATH

```cmd
gcc -o claude-telepresence.exe client_winxp.c -lws2_32
```

### Option 2: Visual C++ 6.0 (Classic XP Development)

```cmd
cl /nologo client_winxp.c ws2_32.lib
```

### Option 3: Visual Studio 2003/2005/2008

Open a "Visual Studio Command Prompt" and run:

```cmd
cl /nologo client_winxp.c ws2_32.lib
```

### Option 4: Modern Visual Studio (2015+)

From a "Developer Command Prompt":

```cmd
cl /nologo client_winxp.c ws2_32.lib
```

Note: Modern VS builds may not run on XP unless you configure the XP toolset.

## Usage

### 1. Start the Relay on Linux

On your Linux machine with Claude Code:

```bash
cd claude-telepresence
make telepresence-helper
python3 relay.py --port 5000
```

### 2. Run the Client on Windows

```cmd
claude-telepresence.exe 192.168.1.100 5000
```

Replace `192.168.1.100` with your Linux machine's IP address.

### Command Line Options

```
claude-telepresence.exe [options] <host> <port>

Options:
  -s, --simple   Simple mode: Convert Unicode to ASCII
                 (Recommended for Windows 2000/XP console)
  -r, --resume   Resume previous conversation
  -l, --log      Enable debug logging to C:\telepresence.log
```

### Recommended for Windows XP

The Windows XP console doesn't support Unicode or modern terminal features well. Use simple mode:

```cmd
claude-telepresence.exe -s 192.168.1.100 5000
```

## Differences from Unix Client

1. **Paths**: Uses Windows paths (C:\, backslashes) instead of Unix paths
2. **No symlinks**: Windows XP doesn't have real symlinks, so lstat() behaves like stat()
3. **Log location**: Debug log goes to `C:\telepresence.log` instead of `/tmp/`
4. **Shell**: Commands execute via `cmd.exe` instead of `/bin/sh`

## Troubleshooting

### "WSAStartup failed"
- Winsock isn't initialized properly. This shouldn't happen on XP+.
- Make sure Windows Sockets is installed (it always is on XP).

### Can't connect
- Check firewall settings on both Windows and Linux
- Verify the relay is running: `python3 relay.py --port 5000`
- Test with: `telnet 192.168.1.100 5000`

### Garbled output
- Use `-s` (simple mode) for ASCII-only output
- The Windows console has limited Unicode support

### Commands not working
- Windows uses `cmd.exe` - Linux commands won't work
- Use Windows commands: `dir` instead of `ls`, `type` instead of `cat`

## Building on Linux for Windows (Cross-compile)

If you have MinGW cross-compiler on Linux:

```bash
i686-w64-mingw32-gcc -o claude-telepresence.exe client_winxp.c -lws2_32
```

This creates a Windows executable from Linux.

## Security Warning

This tool uses **unencrypted TCP**. All data including terminal I/O and file contents is sent in plain text. Only use on trusted private networks.
