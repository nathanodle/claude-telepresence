#!/usr/bin/env python3
"""
PreToolUse hook for telepresence - proxies tools to remote machine.

Uses the native C helper for all operations:
  helper <socket> exec <command>
  helper <socket> read <path>
  helper <socket> write <path>  (stdin)
  helper <socket> stat <path>
  helper <socket> ls <path>
"""

import json
import os
import sys

def get_env():
    socket_path = os.environ.get('TELEPRESENCE_SOCKET')
    helper_path = os.environ.get('TELEPRESENCE_HELPER', '/tmp/telepresence-helper')
    return socket_path, helper_path

def allow():
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "allow"
        }
    }

def make_bash_command(helper_path, socket_path, cmd, *args):
    """Build a bash command that calls the helper."""
    # Escape arguments for shell
    def shell_escape(s):
        return "'" + s.replace("'", "'\\''") + "'"

    parts = [shell_escape(helper_path), shell_escape(socket_path), cmd]
    parts.extend(shell_escape(a) for a in args)
    return ' '.join(parts)

def proxy_bash(tool_input, socket_path, helper_path):
    """Proxy Bash command via helper exec."""
    command = tool_input.get('command', '')
    if not command:
        return allow()

    proxied = make_bash_command(helper_path, socket_path, 'exec', command)

    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "allow",
            "permissionDecisionReason": "Proxying to remote via telepresence",
            "updatedInput": {"command": proxied}
        }
    }

def deny_with_mcp_hint(tool_name, mcp_tool, params_hint):
    """Deny tool and tell Claude to use MCP tool instead."""
    # Get remote cwd from environment if available
    remote_cwd = os.environ.get('TELEPRESENCE_REMOTE_CWD', '')
    cwd_note = f" Remote cwd: {remote_cwd}" if remote_cwd else ""

    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": (
                f"TELEPRESENCE MODE: {tool_name} disabled. "
                f"Use mcp__telepresence__{mcp_tool}({params_hint}) instead.{cwd_note}"
            )
        }
    }

def proxy_read(tool_input, socket_path, helper_path):
    """Deny Read and guide Claude to use MCP read_file."""
    file_path = tool_input.get('file_path', '')
    if not file_path:
        return allow()

    return deny_with_mcp_hint("Read", "read_file", f"path='{file_path}'")

def proxy_write(tool_input, socket_path, helper_path):
    """Deny Write and guide Claude to use MCP write_file."""
    file_path = tool_input.get('file_path', '')
    if not file_path:
        return allow()

    return deny_with_mcp_hint("Write", "write_file", f"path='{file_path}', content=<your content>")

def proxy_glob(tool_input, socket_path, helper_path):
    """Deny Glob and guide Claude to use MCP find_files."""
    pattern = tool_input.get('pattern', '')
    path = tool_input.get('path', '.')
    if not pattern:
        return allow()

    return deny_with_mcp_hint("Glob", "find_files", f"pattern='{pattern}', path='{path}'")

def proxy_grep(tool_input, socket_path, helper_path):
    """Deny Grep and guide Claude to use MCP search_files."""
    pattern = tool_input.get('pattern', '')
    path = tool_input.get('path', '.')
    if not pattern:
        return allow()

    return deny_with_mcp_hint("Grep", "search_files", f"pattern='{pattern}', path='{path}'")

def log_debug(msg):
    """Write debug log to file."""
    try:
        with open('/tmp/telepresence-hook.log', 'a') as f:
            f.write(f"{msg}\n")
    except:
        pass

def main():
    socket_path, helper_path = get_env()
    log_debug(f"Hook called: socket={socket_path}, helper={helper_path}")

    if not socket_path:
        log_debug("No socket path, allowing")
        print(json.dumps(allow()))
        return 0

    try:
        input_data = json.load(sys.stdin)
        log_debug(f"Input: {json.dumps(input_data)}")
    except json.JSONDecodeError as e:
        log_debug(f"JSON decode error: {e}")
        print(json.dumps(allow()))
        return 0

    tool_name = input_data.get('tool_name', '')
    tool_input = input_data.get('tool_input', {})

    handlers = {
        'Bash': proxy_bash,
        'Read': proxy_read,
        'Write': proxy_write,
        'Glob': proxy_glob,
        'Grep': proxy_grep,
    }

    handler = handlers.get(tool_name)
    if handler:
        result = handler(tool_input, socket_path, helper_path)
        log_debug(f"Handler {tool_name} returned: {json.dumps(result)}")
    else:
        result = allow()
        log_debug(f"No handler for {tool_name}, allowing")

    print(json.dumps(result))
    return 0

if __name__ == '__main__':
    sys.exit(main())
