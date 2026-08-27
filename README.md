# NerdQAxe Quad Miner

Open-source firmware for the **NerdQAxe++** (4x BM1370) that mines on **up to four
pools at the same time**, with a configurable hashrate split per pool and live
tuning from the web UI. No paid firmware, no gatekeeping.

> ### ⚠️ Update to v0.1.2 or later if you flashed v0.1.0 or v0.1.1 with the factory image or web flasher
> Those builds had a bug where a **full flash** (which wipes saved settings) left the
> ASICs **unpowered** — the miner booted, joined WiFi, and was reachable in the web UI,
> but never started hashing (`voltage`/`power` read 0). **v0.1.2 fixes it.** Update over
> the air, which keeps your config: on the device firmware page upload `esp-miner-NerdQAxe++.bin`
> then `www.bin` from the
> [latest release](https://github.com/SerpentXSF/NerdQAxe-Quad-Miner/releases/latest).

It is built on top of the excellent
[shufps/ESP-Miner-NerdQAxePlus](https://github.com/shufps/ESP-Miner-NerdQAxePlus)
dual-pool firmware and extends the pool layer from two pools to N.

## What it does

- **Up to 4 pools mined simultaneously.** Configure BTC solo, a BCH pool, a DGB
  pool, and a backup pool, all running together.
- **Per-pool weights.** Give each pool a share of the hashrate (default 25/25/25/25).
  Only the ratios matter, so any split works.
- **Live retuning.** Weight changes apply immediately with no reboot. Only adding
  or removing a pool needs a restart.
- **No hashrate loss.** The board's full hashrate is preserved (~4.7 TH/s stock,
  more if you tune it); the pools are time-sliced at the job level.
- **Web UI.** The settings page has a dynamic pool list: add, remove, and reorder
  pools, set each one's host/port/user/password and weight.

## How it works (job-level split)

All four ASIC chips work one job at a time. Consecutive jobs are handed out across
the configured pools using a smooth weighted round-robin scheduler, so over time
each pool receives its weighted share of the work. Each job carries its pool id;
when a share is found it is submitted back to the pool that job belonged to. The
ASIC difficulty target is set to the minimum required across the active pools so
every pool gets valid shares.

This is the "Path A" approach. A future "Path B" will dedicate a physical BM1370
to a single pool for a true per-chip split.

## Hardware

- NerdQAxe++ (NerdQaxePlus2 board class): 4x BM1370, ESP32-S3, rev7 TPS546 VRM.
- Build target: `BOARD=NERDQAXEPLUS2`.

## Build

Requires ESP-IDF v5.3.x **and Node.js** — the web UI is built from
`firmware/main/http_server/axe-os` as part of the firmware build. The repo ships
a container with both, which is the easiest route:

```
cd firmware/docker && ./build_docker.sh   # ESP-IDF 5.3.3 + Node 20
cd .. && ./docker/idf.sh build            # BOARD defaults to NERDQAXEPLUS2
```

Stock `espressif/idf` images do **not** include Node, so building with one fails
at `npm is not found!`.

Outputs: `firmware/build/esp-miner.bin` (app) and `firmware/build/www.bin` (web UI).

## Update (most people)

If your NerdQAxe++ already runs this firmware (or any NerdQAxe firmware), the easy
path is an **over-the-air update** — no cable, and your pool config is kept.

**Easiest — from the device itself:** open the web UI and go to **Settings →
Release & Update**, then use **Install from GitHub** to pull the latest release
straight onto the miner. Nothing to download by hand.

**Or upload the files yourself:** on the device web UI's firmware update page,
upload from the
[latest release](https://github.com/SerpentXSF/NerdQAxe-Quad-Miner/releases/latest):

- `esp-miner-NerdQAxe++.bin` — the firmware
- `www.bin` — the web UI (upload this too so the site matches the firmware)

The device validates these filenames exactly, so upload them under the names
above — renaming them (or picking the `esp-miner-<version>.bin` asset) makes the
update page reject the file.

## Flash a fresh device

You do not need to build anything.

**Easiest — web flasher (Chrome/Edge):** open the
[**Web Flasher**](https://serpentxsf.github.io/NerdQAxe-Quad-Miner/), plug in the
NerdQAxe++ with a USB-C data cable, and click *Connect & Flash*. It flashes the
factory image straight from your browser.

**Or with esptool:** grab `nerdqaxe-quad-miner-factory-*.bin` from the
[Releases page](https://github.com/SerpentXSF/NerdQAxe-Quad-Miner/releases) and:

```
pip install esptool
python -m esptool --chip esp32s3 --port <PORT> write-flash 0x0 nerdqaxe-quad-miner-factory-<version>.bin
```

Replace `<PORT>` with your serial port (`COMx` on Windows, `/dev/ttyACM0` or
`/dev/tty.usbmodem*` on Linux/macOS). If esptool stalls on the ESP32-S3
USB-Serial/JTAG port, add `--no-stub`.

The factory image is a full flash (bootloader + partitions + app + web UI).

> **A full flash erases saved settings, including WiFi.** After it boots, the miner
> will **not** reappear at its old IP — it starts a temporary setup access point
> (an open network named like `NerdQAxe-xxxx`). This is expected, not a brick. Join
> that AP from a phone or laptop, use the setup page to connect the miner to your
> network, then open its web UI and configure your pools. For a code-only update
> that keeps your config, use the OTA path above instead of a full flash.

## Flash (from a local build)

Full flash of a NerdQAxe++ over its USB port:

```
python -m esptool --chip esp32s3 --port <PORT> \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp-miner.bin \
  0x410000 build/www.bin \
  0xf10000 build/ota_data_initial.bin
```

To build the single-file factory image yourself: `./merge_bin.sh esp-miner-factory.bin`
(run from `firmware/` after a build).

For code-only updates, flashing just `0x10000 esp-miner.bin` is enough and keeps
your saved config. If esptool stalls on the ESP32-S3 USB-Serial/JTAG port, add
`--no-stub`.

## Configure your pools

Open the device web UI and go to **Settings**. Set **Pool mode** to **Dual Pool** to
mine several pools at once, then use **Add Pool** / **Remove** to pick how many you
want: **1, 2, 3, or 4 pools**. Fill in each pool's host, port, and user, and (for
3+ pools) set a **weight** per pool to split the hashrate. Save. Weight changes
apply live; adding or removing a pool takes effect after a restart.

The home dashboard shows each pool's live hashrate share, accepted shares, and
best difficulty.

Pools can also be configured over the REST API:

```
curl -X PATCH http://<device-ip>/api/v2/settings \
  -H "Content-Type: application/json" \
  -d '{"poolMode":1,"pools":[
        {"url":"pool-a","port":3333,"user":"...","weight":40},
        {"url":"pool-b","port":3333,"user":"...","weight":20},
        {"url":"pool-c","port":3333,"user":"...","weight":20},
        {"url":"pool-d","port":3333,"user":"...","weight":20}
      ]}'
```

## Credits

- [shufps/ESP-Miner-NerdQAxePlus](https://github.com/shufps/ESP-Miner-NerdQAxePlus) - the NerdQAxe firmware this is based on.
- The Bitaxe / ESP-Miner project and the NerdAxe community.

## License

GPL-3.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

This is a derivative work of
[shufps/ESP-Miner-NerdQAxePlus](https://github.com/shufps/ESP-Miner-NerdQAxePlus)
(GPL-3.0), itself a fork of
[bitaxeorg/ESP-Miner](https://github.com/bitaxeorg/ESP-Miner) (GPL-3.0). The
upstream license text is preserved at [firmware/LICENSE](firmware/LICENSE), and
bundled third-party components (e.g. `libsecp256k1`, MIT) keep their own
licenses. See [NOTICE](NOTICE) for the full attribution and the statement of
changes.
