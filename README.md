<div markdown="1">

<img src=".github/meshtastic_logo.png" alt="Meshtastic Logo" width="80"/>
<h1>Meshtastic Firmware+ HYDRA optimization esp32+E22P</h1>
[FW+ fork description](https://github.com/musznik/firmware)

## Hardware

Target board: `hydraE22P` (`variants/esp32/diy/hydra_e22p`)

- MCU: ESP32-D0WD-V3 rev 3.1 (ESP32-WROOM-32D DevKit, `esp32doit-devkit-v1`)
- Flash: 4 MB
- SRAM: 320 KB (327 680 B)
- PSRAM: none (`MAX_NUM_NODES=80` keeps RAM headroom)
- LoRa: Ebyte E22P-868M30S (EU_868, 30 dBm PA, `-D USE_EBYTE_E22P`)
- Build usage (current build): RAM 30.4 %, flash 92.5 % of the 2 424 832 B app partition

## Stats

Snapshot from 2026-11-01:

- Commits: 8
- Contributors: 1 (JacekVV)
- Tracked files: 1804 (714 `.h`, 450 `.cpp`, 185 `.ini`, 105 `.py`, 74 `.json`)

## Changelog

- **v1**: first build with a build-number suffix in the version string, no code changes
- **v2**: Wi-Fi fixes
- **v3**: does not exist, a failed build used up the number
- **v4**: node database up to 50 nodes, inactive and most distant nodes removed first
- **v5**: Bluetooth memory released while Wi-Fi is running
- **v6**: favorite nodes survive a restart
- **v7**: exact position in the map report
- **v8**: selected fixes from Meshtastic 2.7.26
- **v9**: same code as v8, named `2.7.23-hydra-v9`, OTA to all nodes
- **v10**: node database raised from 50 to 80 nodes
- **v11**: reset reason and minimum free memory in MyNodeInfo, health log every 10 min
- **v12**: immediate log of memory drops below 2 KB
- **v13**: count of MeshPackets in use, logged
- **v14**: only the last 10 text messages kept for the app when no API client is connected
- **v15**: log of the API client object size on each TCP connection
- **v16**: Wi-Fi retry capped at 30 s, API memory guards, radio noise floor measurement
- **v17**: copy of every LoRa reception sent over UDP port 4404 for the Raspberry Pi
- **v18**: batched log over Wi-Fi (every 500 ms), RTC black box sent after a reset
- **v19**: 6 KB black box, first and last datagram marked
- **v20**: "Hydra stats" log line (reboots, reset reasons, packet counters)
- **v21**: statistics line after each Wi-Fi connection, log sent earlier
- **v22**: phone queue of 25 text messages on client roles, 10 on routers
- **v23**: oldest log lines dropped when full, Wi-Fi retry every 5 s
- **v24**: fix of the "Hydra telemetry" line, OTA to HM1
- **v25**: device telemetry counter reset on every start
- **v26**: no entry in the journal
- **v27**: Wi-Fi "Hydra telemetry", 

## Credits

- Fork description: [musznik](https://github.com/musznik/firmware) (FW+ / Meshtastic+, [loranet.pl](https://loranet.pl)).
- Upstream firmware: the [Meshtastic](https://meshtastic.org) project and its contributors.
- Board designs: [NanoVHF/Meshtastic-DIY](https://github.com/NanoVHF/Meshtastic-DIY/tree/main).
