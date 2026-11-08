# ESP32 Network Hub

A web-based Wi-Fi management and network diagnostics application for the **ESP32**, built with **ESP-IDF**. The device starts a local access point and serves a browser dashboard for checking device status, scanning nearby networks, connecting to a router, and running basic connectivity diagnostics.

> **Status:** Personal project / active development. Hardware validation and release builds have not been documented yet.

## Features

- **Local dashboard** served directly by the ESP32.
- **Access point + station mode (AP+STA):** connect a client to the hub while the ESP32 connects to a Wi-Fi router.
- **Wi-Fi network scan:** view nearby SSIDs, signal strength, channel, and reported security mode.
- **Connection management:** submit router credentials, inspect connection status, disconnect, and restart the device.
- **Network diagnostics:** inspect gateway and DNS information, resolve a DNS name, and run limited ICMP ping checks.
- **Client and device status:** see connected access-point clients, uptime, heap, signal strength, and reconnection information.
- **Password-protected temporary file sharing:** upload, list, download, and delete files stored in RAM.
- **LittleFS partition configuration** is included in the partition table for future or additional persistent storage use.

## Hardware and software

- ESP32 development board with sufficient flash for the configured partition table
- USB data cable and a supported serial port/driver
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **5.5.6** (the version recorded in `dependencies.lock`)
- Python and the toolchain installed by ESP-IDF

The project currently declares the `esp32` target in its dependency lockfile. Other ESP32-family targets have not been confirmed.

## Build and flash

Open an ESP-IDF terminal, or first source the ESP-IDF environment for your platform. From the repository root:

```bash
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p YOUR_SERIAL_PORT flash monitor
```

Replace `YOUR_SERIAL_PORT` with your board's port (for example, `COM3` on Windows or `/dev/ttyUSB0` on Linux). In `menuconfig`, verify the project configuration and partition table are appropriate for your specific board and flash size before flashing. The project includes `partitions.csv`; check the active partition-table setting rather than assuming it is selected automatically.

To leave the serial monitor, press `Ctrl+]`.

## First use

1. Flash the firmware and open the serial monitor.
2. Look for the access-point SSID and address in the startup log. The current source configures the dashboard at `http://192.168.4.1` by default.
3. Connect a phone or computer to the ESP32 access point and open that address in a browser.
4. Use the dashboard to scan for a router and submit its Wi-Fi credentials.
5. Use the dashboard status and diagnostics views to check the connection.

### Configure access-point credentials before use

The current source contains a **hard-coded access-point password**. It is not suitable as a production credential. Change the AP SSID and password in `main/main.c` before building, and do not publish personal credentials in source control. Use a unique strong password for each device. A future improvement would be moving these values to a secure configuration mechanism instead of compiling them into firmware.

## HTTP API

The firmware serves these routes on its local HTTP server:

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Dashboard page |
| `GET` | `/api/status` | Device, AP, router connection, and runtime status |
| `GET` | `/api/clients` | Clients connected to the ESP32 access point |
| `GET` | `/api/scan` | Scan nearby Wi-Fi networks |
| `POST` | `/api/connect` | Configure and connect to a router; accepts JSON with `ssid` and `password` strings |
| `POST` | `/api/disconnect` | Disconnect the router station interface |
| `POST` | `/api/restart` | Restart the ESP32 |
| `GET` | `/api/diagnostics` | Gateway, DNS, and connectivity checks |
| `GET` | `/api/storage` | Temporary RAM storage capacity and usage |
| `GET` | `/api/files` | List temporary stored files |
| `POST` | `/api/files/upload?name=FILENAME` | Upload a file; provide its password in the `X-File-Password` header and send the file bytes as the request body |
| `GET` | `/api/files/download?name=FILENAME` | Download a file; provide its password in the `X-File-Password` header |
| `POST` | `/api/files/delete?name=FILENAME` | Delete a file; provide its password in the `X-File-Password` header |

File-sharing limits in the current implementation: 64 KiB total RAM storage, up to 10 files, and 32 KiB per file. **Uploaded files are held only in RAM and are erased when the ESP32 restarts.**

## Security and privacy

This project is intended for a trusted local development environment. The current firmware uses **HTTP without TLS** and has no authentication layer for the dashboard or most API routes. Router credentials sent through the dashboard are therefore not protected by transport encryption. The file-sharing password protects file operations but does not secure the rest of the web interface.

- Change the hard-coded AP credentials before use.
- Do not expose the device or its HTTP server to the public internet or an untrusted network.
- Use only on networks and devices you own or are authorized to administer.
- Avoid sending sensitive router credentials over networks you do not trust.
- Review and test the firmware's security before deploying it beyond a lab or personal network.

Network scans and diagnostic checks are limited to functionality initiated by the device owner through the local dashboard.

## Repository layout

```text
.
├── main/                  # ESP-IDF application and embedded dashboard
├── managed_components/    # ESP-IDF managed dependency (LittleFS)
├── partitions.csv         # Flash partition layout
├── dependencies.lock      # Locked ESP-IDF/component versions and target
├── CMakeLists.txt         # ESP-IDF project definition
└── *.py                   # Local flash-backup and inspection utilities
```

The Python utilities at the repository root are for local backup/inspection workflows; they are not required to build the firmware.

## Contributing

Bug reports, documentation fixes, and focused improvements are welcome. Please open an issue to describe substantial changes before submitting a pull request. Include the ESP-IDF version, target board, steps to reproduce, and relevant sanitized logs when reporting a problem. Never attach Wi-Fi credentials, private flash contents, or other secrets.

## License

This project is licensed under the [MIT License](LICENSE). Third-party components retain their own licenses and notices; see the relevant component directories for details.
