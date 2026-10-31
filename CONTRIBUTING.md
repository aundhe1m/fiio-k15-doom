# Contributing

Thanks for stopping by! An honest word first: **this project is finished.** DOOM runs, the demons are dead, and the K15 has gone back to playing music most of the time. Updates will be rare, and issues and pull requests may wait a long time for an answer, or never get one. You're on your own, in the nicest possible way.

**Forks are very welcome.** Port it to a newer firmware, a K17, a toaster. If you do something cool, open a PR or an issue to tell us about it, so others can find it.

## The dev loop

You don't need to flash to work on DOOM. The full story is in [development](docs/technical/development.md); the short version:

1. Connect to your K15 over the network (the stock firmware has adb on port 5555): `export K15_HOST=<ip>` and `pip install -r dev/requirements.txt`.
2. Change the engine in `engine/src/`.
3. `./build.sh --engine-only` builds `build/out/doomk15`.
4. Quit DOOM on the K15, then `python dev/k15dev.py deploy`. The launcher now runs your build and the menu shows DEV BUILD.
5. Test (touch, a keyboard, or `engine/tools/udpkeys.py`), take screenshots with `python dev/k15dev.py shot`, read `/usr/data/doom/doom.log`.
6. Menu work? `engine/tools/harness/run.sh` renders the menu on your PC, no K15 needed.
7. When you're happy: `python dev/k15dev.py undeploy`, do a full `./build.sh <zip>` and flash it.

UI patches (`tools/patch_mq_ui.py`) can be tried with a bind mount and a UI restart, no flash and no reboot. See [development](docs/technical/development.md#swapping-in-a-patched-mq_ui-no-flash-no-reboot).

## Two safety rules

These protect your way back if something goes wrong. Don't break them, and don't send PRs that do:

1. **Never ship a package with `recovery_version` > 4.** The recovery partition is the safety net that installs the firmware. With 4, the updater leaves it alone. `tools/pkg_build.py` refuses anything higher; keep it that way.
2. **Never create `/usr/data/disableadb`.** If that file exists, adb doesn't start, and adb is how you get back in to fix things.

Also good manners: never kill `mq_player` (it reboots the K15), don't write to flash by hand, and keep anything experimental in RAM or `/usr/data` so a reboot undoes it. And never commit FiiO files, WADs or built zips.

Security issues: see [SECURITY.md](SECURITY.md).
