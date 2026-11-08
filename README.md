# CM8 Action Gateway

ESP-IDF gateway firmware that reads controls from a Funsnap CM8 remote and
controls a DJI Osmo Action camera.

## Third-party SDK dependency

The DJI Osmo GPS Controller demo is a required Git submodule at
`vendor/dji_osmo_sdk`, pinned to commit
`92fe23e5a749f189593f980a26a105c3bb66aa1c`. This project does not vendor or
modify the DJI source files; the `dji_demo` component compiles selected source
files directly from the submodule.

The app uses a linker wrapper to give DJI's notification processing task an
8 KB stack. The wrapper leaves the submodule source unchanged and adjusts only
that named task at runtime.

The upstream project and its protocol documentation have their own license
terms. Review `vendor/dji_osmo_sdk/LICENSE` and the upstream terms before
fetching, using, or distributing that dependency. The PolyForm Noncommercial
license in this repository covers only files authored for CM8 Action Gateway,
not the submodule or ESP-IDF.

## License and warranty

Files authored for CM8 Action Gateway are provided under the PolyForm
Noncommercial License 1.0.0. As stated in that license, the software is
provided **as is**, without warranty or condition, to the extent permitted by
law. Third-party components retain their own license and warranty terms.

## Fetch the dependency

```sh
git clone --recurse-submodules <your-repository-url>
```

If the project was cloned without submodules:

```sh
git submodule update --init --recursive
```

## Build

Use ESP-IDF 5.5.x. Choose the target for the board, then build:

```sh
idf.py set-target esp32
idf.py build
```

For the ESP32-S3 mini board, use `idf.py set-target esp32s3`. The matching
`sdkconfig.defaults.esp32s3` selects its 4 MB flash layout. The board schematic
shows its WS2812 data input on GPIO48.

## Versioning and releases

See [RELEASING.md](RELEASING.md) for the versioning process, firmware release
files, and suggested GitHub repository description.

## Status LED

On the ESP32-S3 board, the built-in WS2812 LED shows gateway state: red means
the CM8 is disconnected, yellow means the CM8 is connected while the Osmo
Action is not, green means both are connected, and blinking blue means the
maintenance access point is ready. The WROOM test board has no supported
addressable LED output, so this status feature is disabled on that target.

## Gateway behavior

- At startup, scan for and connect to the CM8.
- A CM8 short record press controls recording after the camera protocol link is
  ready.
- A CM8 long record press connects to the camera; after connection, it toggles
  between Photo and Video.
- Holding the CM8 zoom-wheel toggle for three seconds enters firmware update
  mode. Bluetooth is shut down and the temporary `CM8-Gateway-Setup` Wi-Fi AP
  serves the OTA page at `http://192.168.4.1/`.

The OTA AP is open and intended for local, physically initiated maintenance.
