# File Explorer Build Manual

This project builds a PS5 payload ELF named `file-explorer.elf`.

## Requirements

- `ps5-payload-sdk`
- GNU `make`
- Python 3
- LLVM/Clang tools compatible with the payload SDK
- A shell that can run the SDK wrapper tools

On this Mac, the known working paths are:

```sh
PS5_PAYLOAD_SDK=/Users/jumasayeh/Developer/ps5-payload-sdk
LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config
HOST_LLVM_BINDIR=/opt/homebrew/opt/llvm@18/bin
LLVM_STRIP=/Users/jumasayeh/Developer/ps5-payload-sdk/bin/llvm-strip
```

If `llvm@18` is missing, install it:

```sh
brew install llvm@18
```

## Clean Build

From the repo root:

```sh
cd /path/to/PS5-File-Explorer

make clean PS5_PAYLOAD_SDK=/Users/jumasayeh/Developer/ps5-payload-sdk

make \
  PS5_PAYLOAD_SDK=/Users/jumasayeh/Developer/ps5-payload-sdk \
  LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config \
  HOST_LLVM_BINDIR=/opt/homebrew/opt/llvm@18/bin \
  LLVM_STRIP=/Users/jumasayeh/Developer/ps5-payload-sdk/bin/llvm-strip
```

Output:

```text
file-explorer.elf
```

Generated asset C files are written under:

```text
gen/assets/
```

Both `file-explorer.elf` and `gen/` are ignored by git.

## Verify Build Output

```sh
ls -lh file-explorer.elf
file file-explorer.elf
shasum -a 256 file-explorer.elf
```

Expected file type:

```text
ELF 64-bit LSB pie executable, x86-64, dynamically linked, stripped
```

## Common Build Errors

### `PS5_PAYLOAD_SDK is undefined`

Pass the SDK path explicitly:

```sh
make PS5_PAYLOAD_SDK=/Users/jumasayeh/Developer/ps5-payload-sdk
```

Or export it for the shell session:

```sh
export PS5_PAYLOAD_SDK=/Users/jumasayeh/Developer/ps5-payload-sdk
make
```

### `/llvm-clang: No such file or directory`

The SDK wrapper could not find the real LLVM binaries. Use the explicit LLVM overrides:

```sh
make \
  PS5_PAYLOAD_SDK=/Users/jumasayeh/Developer/ps5-payload-sdk \
  LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config \
  HOST_LLVM_BINDIR=/opt/homebrew/opt/llvm@18/bin \
  LLVM_STRIP=/Users/jumasayeh/Developer/ps5-payload-sdk/bin/llvm-strip
```

Check LLVM exists:

```sh
ls -l /opt/homebrew/opt/llvm@18/bin/llvm-config
ls -l /opt/homebrew/opt/llvm@18/bin/clang
```

## Deploy Through Payload Manager

If the PS5 is running `ftpsrv.elf` on port `2121`, replace the Payload Manager copy:

```sh
curl -T /path/to/PS5-File-Explorer/file-explorer.elf \
  ftp://192.168.1.172:2121/data/pldmgr/payloads/file-explorer/file-explorer.elf
```

Verify the uploaded file:

```sh
curl ftp://192.168.1.172:2121/data/pldmgr/payloads/file-explorer/file-explorer.elf \
  -o /tmp/remote-file-explorer.elf

shasum -a 256 \
  /path/to/PS5-File-Explorer/file-explorer.elf \
  /tmp/remote-file-explorer.elf
```

The hashes should match.

After replacing the file, relaunch File Explorer from Payload Manager. Replacing the ELF on disk does not restart an already-running payload.

## Runtime URL

File Explorer serves the web UI and API on:

```text
http://<PS5_IP>:5905/
```

For the current PS5 used during development:

```text
http://192.168.1.172:5905/
```

## Optional Direct Deploy

The Makefile also has a deploy target:

```sh
make deploy PS5_HOST=<PS5_IP> PS5_PORT=9021
```

This requires a payload loader listening on port `9021`.
