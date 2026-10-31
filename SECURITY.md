# Security and disclaimer

## No warranty, no support promise

FiiO K15 DOOM Edition is a hobby project, shared as-is under the GPL-2.0-or-later ([COPYING](COPYING)), **without any warranty**. Flashing it is your decision and your responsibility. It is a finished project rather than a maintained product: updates, fixes and answers to issues may never come, so expect to be on your own. Forks are welcome.

Going back is simple: flashing the official FiiO K15 update the same way (USB stick → *Settings → Others → FW update → SOC local upgrade*) restores the stock firmware. See [going back to stock](docs/restore.md).

Not affiliated with, endorsed by or supported by FiiO or id Software. "FiiO" and "K15" are FiiO's trademarks; "DOOM" is a trademark of id Software / ZeniMax. FiiO support cannot help with a unit running this firmware: flash the official update before contacting them.

## Your K15 already has an open root shell

Independent of this project, the **stock** K15 firmware runs `adbd` on TCP port 5555 on your network and gives anyone who connects a root shell, without a password. This project uses that for development; it neither adds nor removes it. Keep the K15 on a network you trust.

## DOOM listens on UDP port 6666 while it runs

While DOOM is running, it accepts key presses on UDP port 6666 from anyone on your network (the developer "UDP keyboard"). Those keys work in the game and in the DOOM menu, so someone on your network could play, change DOOM settings or delete installed WADs. It can't reach anything outside DOOM, and the port closes as soon as DOOM exits.

## Reporting a problem

Open a GitHub issue. For something you would rather not post publicly, use GitHub's *Report a vulnerability* (private advisory) on the repository.
