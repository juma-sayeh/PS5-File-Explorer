# BFpilot

BFpilot is a small PS5 payload that gives you a browser-based file manager on
port `5905`.

It is meant to stay simple: no PKG installer, no FTP daemon, no mounting tools,
and no extra payload bundle. The payload starts one web server, refreshes a PS5
home-screen launcher tile, and lets the PS5 do file operations locally.

Current release: `v0.2.0`

## What It Does

- Runs a file-manager web UI at `http://<PS5_IP>:5905/`
- Installs or refreshes a `BFpilot` launcher tile on the PS5 home screen
- Uses a dark-only UI
- Uses the same folder-and-sword icon for the PS5 launcher and web UI
- Browses, uploads, downloads, copies, moves, renames, creates folders, and deletes
- Shows progress for copy, move, delete, and browser uploads
- Reports size, speed, ETA, item count, and current path/file while working
- Calculates folder size only when you select a folder
- Creates missing copy/move target folders automatically
- Keeps the default target shortcuts at `/data/homebrew` and `/mnt/usb0`

The launcher tile opens:

```text
http://127.0.0.1:5905/
```

Injecting the payload does not auto-open the browser. It only refreshes the app
tile and starts the web server.

## Runtime Notes

Inject the ELF to your payload loader on port `9021`.

After it starts, open this from a PC, phone, tablet, or the PS5 browser:

```text
http://<PS5_IP>:5905/
```

The payload stores launcher marker data in:

```text
/data/BFpilot
```

The PS5 launcher app itself is installed under:

```text
/user/app/BFPILOT01
```

The installer removes older launcher title IDs from previous names before
registering `BFPILOT01`, so stale tiles from earlier builds do not get in the
way.

## Project Layout

- `src/lite_main.c` starts the payload, refreshes the launcher tile, handles
  reinjection handoff, and starts the web server.
- `src/app_installer.c` writes the launcher files and registers the PS5
  home-screen tile.
- `src/websrv_lite.c` serves the UI, downloads under `/fs`, and JSON APIs.
- `src/transfer.c` contains the file-manager API and long-running jobs.
- `assets/files.html` is the full web UI that gets embedded into the ELF.
- `assets/icon.png` is the smaller web UI icon.
- `assets-app/` contains the launcher tile metadata and icon.

Generated files go under `gen/` during build. The built ELF is not tracked in
Git; release builds should be attached to GitHub Releases.

## Build

You need:

- `ps5-payload-sdk`
- GNU `make`
- Python 3
- LLVM/Clang tools that work with the PS5 payload SDK
- A shell that can run the SDK build commands

Build:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean
make
```

Output:

```text
bfpilot.elf
```

Deploy helper:

```sh
make deploy PS5_HOST=<PS5_IP> PS5_PORT=9021
```

## Extra Notes

BFpilot avoids kernel-level cleanup and does not force-unload other payloads.

If you reinject while an older BFpilot or BS5FileManager instance is already
running, the new payload checks whether a file job is active. If the old
instance is idle, it asks it to shut down cleanly and then takes over port
`5905`. If a copy, move, or delete is running, the new injection exits instead
of interrupting the active job.

Cancel is cooperative. Large operations may take a moment to stop, especially on
external USB drives, but the job code checks for cancellation during scan, copy,
move cleanup, and delete.
