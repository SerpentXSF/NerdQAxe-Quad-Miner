# Releasing

Checklist for cutting a NerdQAxe Quad Miner release. Two steps here have each
already shipped broken once, so they are called out explicitly.

## 1. Build

```bash
BOARD=NERDQAXEPLUS2 idf.py fullclean && BOARD=NERDQAXEPLUS2 idf.py build
./merge_bin.sh nerdqaxe-quad-miner-factory-<tag>.bin
```

Use `fullclean` and pass `BOARD` explicitly. `BOARD` is a compile definition, so
an incremental build silently reuses whatever board the CMake cache was last
configured for — you can ship a binary for the wrong hardware without any error.
Confirm `-- Selected board: NERDQAXEPLUS2` appears in the output.

Bump `firmware/version.txt` in the same change as the code. Shipping different
binaries under one version string makes field reports impossible to interpret.

## 2. Verify on hardware before publishing

Flash a real device and check it mines, all pools reconnect, and the log
websocket still streams. A build that compiles is not a build that works.

## 3. Publish the GitHub release — this is NOT optional

Pushing code, binaries and `docs/manifest.json` does **not** update the device's
**Settings → Release & Update** panel. That panel reads the **GitHub Releases
API**, so until a release exists it keeps offering the previous version — and
will happily "update" a user *backwards*.

```bash
gh release create <tag> <assets...> --repo SerpentXSF/NerdQAxe-Quad-Miner \
  --target <full-40-char-sha> --title "..." --notes "..."
```

`--target` rejects a short SHA (`Release.target_commitish is invalid`).

### Required assets — both naming schemes

The firmware validates upload filenames exactly, so a release needs the
tag-suffixed names *and* the plain names the manual-update path expects:

| Asset | Why |
|---|---|
| `nerdqaxe-quad-miner-factory-<tag>.bin` | **Install from GitHub.** Must match `buildFactoryNameFor()` in `settings.component.ts` or the panel will not offer the release at all. |
| `esp-miner-<tag>.bin` | Provenance / manual download |
| `www-<tag>.bin` | Provenance / manual download |
| `esp-miner-NerdQAxe++.bin` | **Manual Update → Firmware.** Must equal `esp-miner-<deviceModel>.bin` or the UI rejects the file by name. |
| `www.bin` | **Manual Update → Website.** Must be exactly `www.bin`. |

Omitting the last two forces every user to rename files by hand before they can
update.

## 4. The OTA allowlist is pinned to this repository

`GITHUB_REPO` in `main/http_server/handler_ota_factory.cpp` allowlists which URLs
**Install from GitHub** will flash. It is a security control — the endpoint takes
a URL from the client and flashes whatever it finds there.

If this repository is ever renamed, moved, or forked again, **update that define
in the same change**. It shipped pointing at upstream while the frontend pointed
here, and every Install-from-GitHub failed with `400 Invalid or unsafe URL`
until v0.1.5. Keep it pinned to the full repo path, not just the owner, so
another repository under the same owner cannot be used as a firmware source.
