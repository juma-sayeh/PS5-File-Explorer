# File Explorer Firmware Testing

This protocol checks whether File Explorer runs safely across jailbroken PS5 firmwares and loaders. It does not require exploit changes, kernel patching, package loading, or DRM-related features.

Use `file-explorer-core.elf` first on every firmware. Only test `file-explorer.elf` after the core server is confirmed working.

## Before Testing

Record these details before each run:

- PS5 firmware version.
- Exploit and loader name/version.
- Payload file used: `file-explorer-core.elf` or `file-explorer.elf`.
- PS5 IP address.
- Sender tool and command used.
- Whether `/data/FileExplorer/log.txt` and `/data/FileExplorer/crash.log` exist after the run.

Clear old browser tabs that point to File Explorer before reinjecting. If a stage fails, keep the PS5 powered on long enough to collect `/data/FileExplorer/log.txt`, `/data/FileExplorer/crash.log`, `/api/status`, and `/api/diag` if reachable.

## Stage A: Core Server Only

Purpose: prove the firmware and loader can run File Explorer's standalone HTTP file manager without launcher installation.

1. Send `file-explorer-core.elf` to the loader on port `9021`.
2. Verify the sender console reports that the ELF was delivered without socket/send errors.
3. Watch for File Explorer startup output from the loader console, if available.
4. Confirm `/data/FileExplorer/log.txt` is created or appended.
5. Open `http://PS5_IP:5905/` in a browser on the same network.
6. Open `http://PS5_IP:5905/api/status`.
7. Open `http://PS5_IP:5905/api/diag`.
8. Browse `/data` in the file manager.
9. Browse `/mnt/usb0` with an external drive connected.
10. Create or open `/data/FileExplorer-test`.
11. Upload a small text file to `/data/FileExplorer-test`.
12. Copy that text file to another name in `/data/FileExplorer-test`.
13. Move or rename the copied file.
14. Delete the moved file.
15. Delete the original uploaded test file.
16. Reopen `/api/diag` and save the output.

Pass criteria:

- The server starts on port `5905`.
- `/`, `/api/status`, `/api/diag`, `/fs`, and `/api/fs/*` are reachable.
- Browsing `/data` works.
- Browsing `/mnt/usb0` either works or fails cleanly with a clear permission/path error.
- Upload, copy, move, and delete work inside `/data/FileExplorer-test`.
- No crash log is created during normal file operations.

Fail data to collect:

- Sender console output.
- `/data/FileExplorer/log.txt`.
- `/data/FileExplorer/crash.log`, if present.
- The HTTP status and response body from `/api/status` and `/api/diag`, if reachable.
- Exact browser error if the page times out.

## Stage B: Notification Enabled

Purpose: verify notification support is optional and cannot stop the web server.

1. Start from a PS5 state where Stage A passed.
2. Send `file-explorer-core.elf` again, or start `file-explorer.elf` with launcher disabled using `--no-launcher` or `BFPILOT_NO_LAUNCHER=1` if the loader supports arguments/environment.
3. Trigger the File Explorer notification path by starting the payload and watching for the startup notification attempt.
4. Open `http://PS5_IP:5905/`.
5. Open `http://PS5_IP:5905/api/diag`.
6. Check `/data/FileExplorer/log.txt` for notification return code entries.

Pass criteria:

- If a notification appears, the server still starts.
- If the notification fails, the server still starts.
- `/api/diag` remains reachable after the notification attempt.
- The log records the notification result when available.

Fail data to collect:

- Whether a notification appeared.
- Notification return code from `/data/FileExplorer/log.txt`.
- `/api/diag` output after startup.

## Stage C: Launcher Enabled

Purpose: verify full mode can attempt launcher install/refresh without making launcher failure fatal.

Only run this stage after Stage A passes on the same firmware and loader.

1. Send `file-explorer.elf` to the loader on port `9021`.
2. Watch for launcher install or refresh messages.
3. Open `http://PS5_IP:5905/`.
4. Open `http://PS5_IP:5905/api/status`.
5. Open `http://PS5_IP:5905/api/diag`.
6. Record the launcher fields from `/api/diag`.
7. Check whether the File Explorer launcher tile appears or refreshes on the PS5 home screen.
8. If the launcher tile does not appear, keep testing the web server anyway.
9. Browse `/data`.
10. Upload a small text file to `/data/FileExplorer-test`.
11. Delete the uploaded test file.
12. Save `/data/FileExplorer/log.txt`.

Pass criteria:

- Launcher install may succeed or fail.
- Launcher failure must not stop the HTTP server.
- `/api/diag` records AppInstUtil resolution and return codes.
- File browsing and basic file operations still work after any launcher failure.

Launcher fields to record:

- `launcher_enabled`.
- `launcher_attempted`.
- `appinst_init_rc`.
- `title_dir_resolved`.
- `install_title_dir_resolved`.
- `install_title_rc`.
- `uninstall_resolved`.
- `uninstall_rc`.
- `install_all_resolved`.
- `install_all_rc`.
- `user_app_writable`.
- `launcher_install_rc`.
- `launcher_final_state`.

Fail data to collect:

- The last launcher-related lines from `/data/FileExplorer/log.txt`.
- Full `/api/diag` output.
- Whether the tile appeared, refreshed, disappeared, or did not change.
- Whether the web server continued running after launcher failure.

## Stage D: Reinjection

Purpose: verify reinjecting File Explorer does not kill unrelated payloads or interrupt active work in an unsafe way.

1. Start `file-explorer-core.elf`.
2. Open `http://PS5_IP:5905/` and confirm the server is idle.
3. Inject `file-explorer-core.elf` again while idle.
4. Verify the web server still responds.
5. Open `/api/diag` and record uptime, PID, and any old-instance message.
6. Start a copy operation using a test file inside `/data/FileExplorer-test`.
7. While the copy is active, inject `file-explorer-core.elf` again.
8. Verify the old copy job is not killed by the new injection.
9. Verify the web server remains reachable after the copy finishes.
10. Repeat the idle reinjection test once with `file-explorer.elf` only after full mode passed Stage C.

Pass criteria:

- Reinjection while idle leaves one reachable File Explorer server.
- Reinjection during active copy does not kill the active job.
- Existing unrelated payloads are not forcibly unloaded.
- Logs clearly state whether the new instance started, reused an existing state, or exited because another File Explorer instance was already active.

Fail data to collect:

- `/data/FileExplorer/log.txt` from before and after reinjection.
- `/api/diag` before reinjection, during copy, and after copy.
- File copy source, destination, size, and whether the destination completed correctly.
- Sender console output for each injection.

## Compatibility Table

| firmware | exploit/loader | payload used | server starts | file browse | upload | copy | move | delete | launcher install | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| 11.60 |  | `file-explorer-core.elf` |  |  |  |  |  |  | N/A |  |
| 11.60 |  | `file-explorer.elf` |  |  |  |  |  |  |  |  |
| 12.70 |  | `file-explorer-core.elf` |  |  |  |  |  |  | N/A |  |
| 12.70 |  | `file-explorer.elf` |  |  |  |  |  |  |  |  |
|  |  | `file-explorer-core.elf` |  |  |  |  |  |  | N/A |  |
|  |  | `file-explorer.elf` |  |  |  |  |  |  |  |  |

Use `yes`, `no`, or `partial` for result columns. Put exact error codes, HTTP status codes, and log checkpoints in `notes`.

## Recommended Report Format

```text
Firmware:
Exploit/loader:
Payload:
Sender command/tool:
PS5 IP:
Server URL:
Stage passed:
First failing step:
/api/status result:
/api/diag result:
Relevant log lines:
Crash log present:
Notes:
```
