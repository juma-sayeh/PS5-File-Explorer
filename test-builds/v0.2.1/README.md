# BFpilot v0.2.1 Test Build

This is a firmware-compatibility test build, not the latest stable release.

Recommended order:

1. Test `bfpilot-core.elf` first on every firmware/loader.
2. Only test `bfpilot-full.elf` after core mode starts and the file manager works.
3. Treat launcher install failure in full mode as non-fatal if the web server still works.

Test URL:

```text
http://<PS5_IP>:5905/
```

Diagnostics:

```text
http://<PS5_IP>:5905/api/status
http://<PS5_IP>:5905/api/diag
/data/BFpilot/log.txt
/data/BFpilot/crash.log
```

See `docs/FIRMWARE_TESTING.md` for the full firmware test protocol.
