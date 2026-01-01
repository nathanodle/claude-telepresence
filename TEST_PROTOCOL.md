# Telepresence v2 Test Protocol

Run these tests after connecting to verify functionality.
Record PASS/FAIL for each. Stop on critical failures.

## 1. Basic Connectivity

### 1.1 Confirm environment
- [ ] get_cwd() returns expected path
- [ ] execute_command("uname -a") shows remote system info
- [ ] execute_command("echo $SHELL") works

## 2. File Operations

### 2.1 Write and read
- [ ] write_file("test_tp.txt", "hello world\nline 2\n")
- [ ] read_file("test_tp.txt") returns exact content
- [ ] file_exists("test_tp.txt") returns true
- [ ] file_info("test_tp.txt") shows size=19, type=file

### 2.2 Edit
- [ ] edit_file("test_tp.txt", "hello", "goodbye")
- [ ] read_file confirms "goodbye world\nline 2\n"

### 2.3 Move and remove
- [ ] move_file("test_tp.txt", "test_tp_moved.txt")
- [ ] file_exists("test_tp.txt") returns false
- [ ] file_exists("test_tp_moved.txt") returns true
- [ ] remove_file("test_tp_moved.txt")
- [ ] file_exists("test_tp_moved.txt") returns false

## 3. Directory Operations

### 3.1 Create and list
- [ ] make_directory("test_tp_dir")
- [ ] list_directory("test_tp_dir") returns empty
- [ ] write_file("test_tp_dir/a.txt", "aaa")
- [ ] write_file("test_tp_dir/b.txt", "bbb")
- [ ] list_directory("test_tp_dir") shows a.txt, b.txt

### 3.2 Find files
- [ ] find_files("*.txt", "test_tp_dir") returns a.txt, b.txt

### 3.3 Cleanup
- [ ] remove_file("test_tp_dir/a.txt")
- [ ] remove_file("test_tp_dir/b.txt")
- [ ] execute_command("rmdir test_tp_dir") succeeds

## 4. Search

### 4.1 Content search
- [ ] write_file("test_search.txt", "line one\nfind THIS here\nline three\n")
- [ ] search_files("THIS", "test_search.txt") finds line 2
- [ ] search_files("NOTHERE", "test_search.txt") returns no matches
- [ ] remove_file("test_search.txt")

## 5. Command Execution

### 5.1 Simple commands
- [ ] execute_command("pwd") returns cwd
- [ ] execute_command("ls -la") shows directory listing
- [ ] execute_command("echo test") returns "test"

### 5.2 Exit codes
- [ ] execute_command("true") exit_code=0
- [ ] execute_command("false") exit_code=1 (or non-zero)

### 5.3 Stderr capture
- [ ] execute_command("ls /nonexistent_path_12345") captures error message

### 5.4 Long output (flow control test)
- [ ] execute_command("cat /etc/passwd") returns full file
- [ ] If available: execute_command("ls -laR /usr") handles large output

## 6. Large File Transfer (Flow Control)

### 6.1 Upload test
Create and transfer a 500KB file to test flow control:
```
content = "x" * 500000
write_file("test_large.txt", content)
```
- [ ] write_file completes without timeout
- [ ] file_info shows size ~500000
- [ ] remove_file("test_large.txt")

### 6.2 Download test
- [ ] read_file on a large system file (e.g., /etc/termcap, /usr/dict/words)
- [ ] Verify content received completely (check line count or size)

## 7. Download URL (if network available)

### 7.1 HTTP fetch
- [ ] download_url("http://example.com/", "test_download.html")
- [ ] file_exists("test_download.html") returns true
- [ ] read_file shows HTML content
- [ ] remove_file("test_download.html")

## 8. Host ↔ Remote Transfer

### 8.1 Upload to host
- [ ] write_file("test_upload.txt", "content from remote\n")
- [ ] upload_to_host("test_upload.txt", "/tmp/test_from_remote.txt")
- [ ] Verify /tmp/test_from_remote.txt exists on Linux host (outside telepresence)
- [ ] remove_file("test_upload.txt")

### 8.2 Download from host
- [ ] Create /tmp/test_to_remote.txt on Linux host with "content from host\n"
- [ ] download_from_host("/tmp/test_to_remote.txt", "test_download.txt")
- [ ] read_file("test_download.txt") returns "content from host\n"
- [ ] remove_file("test_download.txt")

## 9. Error Handling

### 9.1 File not found
- [ ] read_file("/nonexistent/path/file.txt") returns error
- [ ] file_exists("/nonexistent/path") returns false

### 9.2 Permission denied (if testable)
- [ ] write_file("/etc/test_no_permission.txt", "x") returns permission error

### 9.3 Invalid operations
- [ ] remove_file("/") fails appropriately
- [ ] list_directory("/nonexistent") returns error

## 10. Simple Mode (if -s flag used)

### 10.1 Display test
- [ ] Output displays without corruption
- [ ] Box-drawing characters show as +, -, |
- [ ] Colors stripped (no garbled escape sequences)
- [ ] Spinner animation works (if applicable)

---

## Results Summary

| Category | Pass | Fail | Skip |
|----------|------|------|------|
| Connectivity | | | |
| File Ops | | | |
| Directory Ops | | | |
| Search | | | |
| Commands | | | |
| Large Files | | | |
| Download URL | | | |
| Host Transfer | | | |
| Error Handling | | | |
| Simple Mode | | | |

## Notes
(Record any issues, warnings, or unexpected behavior)
