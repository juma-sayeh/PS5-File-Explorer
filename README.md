# PS5 File Explorer

File Explorer is a small PS5 payload that starts a browser-based file manager on
port `5905`.

It is meant to stay simple: no PKG installer, no FTP daemon, no mounting tools,
no payload bundle, no exploit code, and no DRM features. The payload starts one
standalone web server and lets the PS5 do file operations locally.

Current prepared release: `file-explorer-v0.2.1`

This repository is a fork of ItsBlurf/BFpilot. See [FORK.md](FORK.md).

## Recommended Build

Use `file-explorer-core.elf` first on every new firmware or loader.

`file-explorer-core.elf` is the recommended build for maximum firmware
compatibility:

- Starts the HTTP file manager on `http://<PS5_IP>:5905/`
- Does not install or refresh the PS5 launcher tile
- Does not compile launcher installer code
- Does not link launcher-only SCE libraries
- Keeps notification attempts optional

Only try `file-explorer-full.elf` after `file-explorer-core.elf` works on the
same firmware and loader.

`file-explorer-full.elf` includes optional launcher tile support. Launcher
install may fail on some firmware, exploit, or loader combinations. That should
not stop the file manager. If launcher install fails but port `5905` still
works, use the web file manager directly from a browser.

## What It Does

- Runs a file-manager web UI at `http://<PS5_IP>:5905/`
- Builds `file-explorer-core.elf` for the safest firmware compatibility path
- Builds `file-explorer-full.elf` for optional PS5 home-screen launcher support
- Installs or refreshes a `File Explorer` launcher tile in full mode
- Browses, uploads, downloads, copies, moves, renames, creates folders, and deletes
- Uploads ZIP and RAR archives and extracts them on the PS5
- Extracts local ZIP/RAR files already stored on the PS5
- Queues upload/archive work in the Activity panel for overnight runs
- Keeps persistent Activity logs under `/data/FileExplorer/logs`
- Creates hidden archive overwrite backups and restores them from the UI
- Can recursively make folders executable with the `Make Executable` action
- Shows progress, speed, ETA, item count, current path/file, and operation logs
- Calculates folder size only when you select a folder
- Creates missing copy/move target folders automatically
- Keeps the default target shortcuts at `/data/homebrew` and `/mnt/usb0`

## Runtime Notes

Inject the ELF to your payload loader on port `9021`.

For first tests on a firmware:

```text
send file-explorer-core.elf to <PS5_IP>:9021
open http://<PS5_IP>:5905/
open http://<PS5_IP>:5905/api/status
open http://<PS5_IP>:5905/api/diag
```

If core works and you want the PS5 launcher tile:

```text
send file-explorer-full.elf to <PS5_IP>:9021
open http://<PS5_IP>:5905/
open http://<PS5_IP>:5905/api/diag
```

Full mode can skip launcher work at runtime:

```text
--no-launcher
BFPILOT_NO_LAUNCHER=1
```

The full build stores launcher marker data in:

```text
/data/FileExplorer
```

The PS5 launcher app itself is installed under:

```text
/user/app/P5FE00001
```

If launcher install fails, keep testing the web server. The file manager can
still work without a launcher tile.

## Diagnostics

Use these endpoints while testing firmware compatibility:

```text
http://<PS5_IP>:5905/api/status
http://<PS5_IP>:5905/api/diag
```

Save these files when reporting failures:

```text
/data/FileExplorer/log.txt
/data/FileExplorer/crash.log
```

`/api/diag` reports the build mode, PID, port, basic path permissions,
notification status, launcher status, and route list. Launcher failure is
nonfatal if the web server remains reachable.

## Troubleshooting

### Port `5905` Not Reachable

- Confirm the payload sender reported a successful send to port `9021`.
- Try `file-explorer-core.elf` before `file-explorer-full.elf`.
- Check whether another File Explorer, BFpilot, or BS5FileManager instance
  already owns port `5905`.
- Open `/data/FileExplorer/log.txt` and look for `bind_5905`, `listen_5905`,
  and `web server listening`.
- Recheck the PS5 IP address and make sure the browser device is on the same
  network.

### Payload Exits Immediately

- Use `file-explorer-core.elf` first.
- Check the sender console for loader-side errors.
- Check `/data/FileExplorer/crash.log` for fatal signal details.
- Check `/data/FileExplorer/log.txt` for the last checkpoint.
- If reinjecting during a copy, move, delete, upload, or archive job, the new
  instance may exit intentionally so the old job is not killed.

### Launcher Install Failed

- This is not fatal.
- Confirm `http://<PS5_IP>:5905/` still opens.
- Open `/api/diag` and record `launcher_attempted`, `appinst_init_rc`,
  `title_dir_resolved`, `install_title_rc`, `install_all_rc`, `uninstall_rc`,
  `launcher_final_state`, `launcher.user_app_writable`, and
  `launcher.launcher_install_rc`.
- If the file manager works, keep using the browser URL and report the launcher
  diagnostics separately.

## Firmware Testing

Use [docs/FIRMWARE_TESTING.md](docs/FIRMWARE_TESTING.md) for the full
stage-by-stage protocol:

- Stage A: core server only
- Stage B: notification enabled
- Stage C: launcher enabled
- Stage D: reinjection

## Project Layout

- `src/lite_main.c` starts the payload, handles reinjection handoff, and starts
  the web server.
- `src/app_installer.c` writes launcher files and registers the PS5 home-screen
  tile in full mode.
- `src/websrv_lite.c` serves the UI, downloads under `/fs`, and JSON APIs.
- `src/transfer.c` contains the file-manager API, Activity queue, and jobs.
- `src/archive_common.c`, `src/zip_archive.c`, and `src/rar_transfer.c` contain
  archive extraction, backup, and placement logic.
- `assets/files.html` is the full web UI that gets embedded into the ELF.
- `assets/icon.png` is the smaller web UI icon.
- `assets-app/` contains the launcher tile metadata and icon.

Generated files go under `gen/` during build. Built ELF files are not tracked in
Git; release builds should be attached to GitHub Releases.

## Build

You need:

- `ps5-payload-sdk`
- GNU `make`
- Python 3
- LLVM/Clang tools that work with the PS5 payload SDK
- A shell that can run the SDK build commands

Build both release files:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean all
```

Build one mode:

```sh
make core
make full
```

Outputs:

```text
file-explorer-core.elf
file-explorer-full.elf
```

Deploy helper:

```sh
make deploy-core PS5_HOST=<PS5_IP> PS5_PORT=9021
make deploy-full PS5_HOST=<PS5_IP> PS5_PORT=9021
```

## Extra Notes

File Explorer avoids kernel-level cleanup and does not force-unload unrelated
payloads.

If you reinject while an older File Explorer, BFpilot, or BS5FileManager
instance is already running, the new payload checks whether a file job is
active. If the old instance is idle, it asks it to shut down cleanly and then
takes over port `5905`. If a file operation is running, the new injection exits
instead of interrupting the active job.

Cancel is cooperative. Large operations may take a moment to stop, especially on
external USB drives, but the job code checks for cancellation during scan, copy,
move cleanup, delete, upload, and archive extraction.
