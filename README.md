# NerdQAxe Quad Miner

Open-source firmware for the **NerdQAxe++** (4x BM1370) that mines on **up to four
pools at the same time**, with a configurable hashrate split per pool and live
tuning from the web UI. No paid firmware, no gatekeeping.

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
- **No hashrate loss.** The full ~6.3 TH/s of a NerdQAxe++ is preserved; the pools
  are time-sliced at the job level.
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

Requires ESP-IDF v5.3.x. A container is the easiest route:

```
docker run --rm -e BOARD=NERDQAXEPLUS2 \
  -v "<path-to>/firmware:/project" \
  espressif/idf:v5.3.3 \
  bash -lc 'cd /project && idf.py build'
```

The web UI is built automatically from `firmware/main/http_server/axe-os` during
the firmware build (it needs Node.js if you build outside the container).

Outputs: `firmware/build/esp-miner.bin` (app) and `firmware/build/www.bin` (web UI).

## Update (most people)

If your NerdQAxe++ already runs this firmware (or any NerdQAxe firmware), the easy
path is an **over-the-air update** — no cable, and your pool config is kept. On the
device web UI's firmware update page, upload from the
[latest release](https://github.com/SerpentXSF/NerdQAxe-Quad-Miner/releases/latest):

- `esp-miner.bin` — the firmware
- `www.bin` — the web UI (upload this too so the site matches the firmware)

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

The factory image is a full flash (bootloader + partitions + app + web UI). After
it boots, connect to the device's WiFi setup portal, join your network, then open
its web UI and configure your pools.

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

Open the device web UI and go to **Settings**. Set **Pool mode** to **Dual** to
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

GPL-3.0, inherited from ESP-Miner. See [firmware/LICENSE](firmware/LICENSE).
