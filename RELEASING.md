# Releases

## Version numbers

Use Semantic Versioning tags in the form `vMAJOR.MINOR.PATCH`:

- Increase `MAJOR` for incompatible changes.
- Increase `MINOR` for compatible features.
- Increase `PATCH` for compatible fixes.

For example, after merging the release changes to `main`, create and push an
annotated tag:

```sh
git tag -a v0.1.0 -m "Release v0.1.0"
git push origin v0.1.0
```

The GitHub Actions workflow builds both ESP32 targets for pull requests and
updates to `main`. Pushing a tag beginning with `v` also builds both targets.
When both builds succeed, the workflow creates a GitHub Release with generated
release notes and attaches two firmware files per target.

## Firmware files

- `*-full.bin` is the merged image for first-time flashing. Write it at flash
  address `0x0`.
- `*-ota.bin` is the application image for upload through the gateway's
  maintenance page.

The assets are named by target, for example
`cm8-action-gateway-esp32s3-full.bin` and
`cm8-action-gateway-esp32s3-ota.bin`.

## GitHub repository description

Suggested short description for the GitHub repository settings:

> ESP-IDF gateway connecting a Funsnap CM8 BLE remote to a DJI Osmo Action 4, with recording, mode switching, and Wi-Fi firmware updates.
