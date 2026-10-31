# Going back to stock

Had your fill of demons? Getting rid of DOOM is the same procedure as installing it, just with FiiO's own zip. No shame in it: "Hey, not too rough" is a valid difficulty setting. Here's how:

1. Download the official `K15_update_20261012_v159.zip` (see [building](building.md#1-download-the-official-firmware)), or any newer official update.
2. Copy it to the root of a FAT32/exFAT USB stick, not unzipped, as the only `K15_update_*.zip` on it.
3. *Settings → Others → FW update → SOC local upgrade*, and wait for the reboot. The [flashing guide](flashing.md) has screenshots.

That's all. We tested it: the K15 comes back with FiiO's boot animation, no DOOM tile and no DOOM in the knob carousel.

The flash doesn't touch `/usr/data`, so DOOM's settings, saves and installed WADs stay there, unused (and they're back if you flash DOOM again). To clean them up too, run `rm -rf /usr/data/doom` over adb (see [development](technical/development.md)).
