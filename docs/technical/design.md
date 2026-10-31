# Design notes: DOOM baked into the K15 firmware

This is the project's design spec and test record, lightly edited for this repository. It was written while building the thing (2026-10-30/31), so it reads like a lab notebook: addresses, MD5s, decisions and what was tested on real hardware. Everything marked ✅ was verified on a K15. ❓ marks what is still open. "The tester" is the person holding the K15.

Section numbers (§) refer to this page. Friendlier overviews: [the device](device.md) and [development](development.md). User docs start at the [README](../../README.md).

**Status:** the builder ([repository root](../../)) is implemented and flashed. `build.sh <official v159 zip>` produces `out/K15_update_<date>_<time>_v160.zip`, which passes `pkg_verify` (`RESULT: PASS`) and the content diff vs stock. First flash 2026-10-31 (§10). **Stage 2** (same day) added the knob-carousel entry (§1.7), keyboard/gamepad input (§5.1) and dropped the *Settings → Others* row (§11). **Stage 3** reworked the home menu and added options, layouts, rebinding and the cheat menu (§5.2, §12). The boot animations (§6.1) have been flashed and checked on a K15 too.

**How to read this page:** §1–§5 explain how each piece works and why, §6 is the builder, §7–§8 are the safety rules and open items, and §9–§12 are the test and flash records. If you only want the story, read [the device](device.md) and [development](development.md) first.

## 0. Goal and agreed decisions

Build an open-source DOOM firmware builder (GPL-2.0-or-later). It takes a user-supplied **official** `K15_update_*_v159.zip` and outputs a flashable `K15_update_<date>_v160.zip` that adds DOOM to the stock firmware. **No FiiO code or WAD is ever committed or redistributed.** The builder extracts and patches whatever the user provides.

| Topic | Decision |
|---|---|
| Launch | **DOOM "source" tile** in the free 16th cell of the swipe-right source grid ✅ and a 9th **DOOM** entry in the volume-knob input carousel ✅ (§1.7, Stage 2). Both run the same launcher. The 8th row **Settings → Others → "DOOM"** ✅ is opt-in since Stage 2 (tester preference) |
| DOOM "source" UX | Selecting DOOM opens a DOOM home menu: **Start game** (WAD picker: device + USB) / **Options** (WAD importer, settings, …), not the game directly |
| WADs | **None bundled.** Read `*.wad` from the **USB stick root**, plus WADs installed in `/usr/data/doom/wads/` |
| WAD UI | Minimal on-screen picker: list stick and installed WADs, each with **Play** and **Install to device**. Check free space and refuse if there is less than WAD size plus a margin. DOOM II must work (the tester owns it for testing) |
| No WAD | DOOM-styled screen: "Put a .wad on the USB stick root" |
| Config | Persist `default.cfg` and savegames in `/usr/data/doom/`. A `doom.cfg` / `k15.conf` on the stick root overrides them |
| Audio input | Auto-switch to **Local Play** on launch if the current input can't carry SoC audio, and restore the previous input on exit (§3) |
| Exit | DOOM menu Quit, plus **hold the ESC pill for 3 s** |
| Dev override | Launcher prefers `/usr/data/doom/doomk15` if present, logs that, and shows "DEV BUILD" in the picker. adb stays enabled (stock) |
| Identity | `/etc/homebrew_version`: `HOMEBREW=doom`, `BASE=v159`, `BUILT=…`; OTA package labeled v160+ |
| Flashing | Allowed as needed, **but with the tester's sign-off every time** |
| Later | DOOM-themed boot animation (low priority). USB keyboard + gamepad support landed in Stage 2 (§5.1) |

## 1. Launch hook: patched `mq_ui` ✅ (proven on hardware)

### 1.1 How the Others screen works (v159 `mq_ui`, MD5 `6b91aa1d0bdd92b2b167aa8ce8e2a080`)

- All screens are built **once at `mq_ui` start-up** (big init list around `0x43d3e4`). The Others screen init is `0x433aa8`: `lv_obj_create` → generic list builder `0x43c7f4(screen, "/usr/project/config/ui/other_set.json", &state@0x8ab0b4, 0, click_cb=0x433934)`.
- The builder `malloc`s the item array from the JSON `obj_items` count, so **an extra JSON row renders without code changes** ✅. Labels come from `lable.{zh,en,ja}.obj_items[]`, icons from `img[]` (`D:`-prefixed LVGL paths). Stock icons are 47–53 px RGBA PNG (`/usr/project/config/img/menu_setting/other/`).
- Click callback `0x433934`: `if (lv_event_get_code(e) == 7 /*CLICKED*/) { idx = 0x43b540(target); if (idx < 7) jump_table@0x634280[idx](); }`. **Index ≥ 7 is silently ignored**, and the 7-entry jump table is directly followed by a string, so it can't be extended.

### 1.2 The patch ([`tools/patch_mq_ui.py`](../../tools/patch_mq_ui.py))

Two hooks (`--hooks others,sources`, default both). The **cave** is the 208 zero bytes of slack at the end of the R+E LOAD, right after `.plt` (`0x789f30..0x78a000`).

| Where | Stock | Patched |
|---|---|---|
| **others** `0x43397c` | `beqz v1,0x433960` (idx≥7 → return) | `beqz v1,0x433914` |
| `0x433914` (3 zero words, inter-function gap) | `0` | `j 0x789f30; nop` |
| `0x789f30` stub | `0…` | `li v1,7; bne v0,v1,ret; lui a0,hi; jal system@plt(0x789670); addiu a0,lo; ret: j 0x433960; nop` |
| **sources** `0x42a8d8` | `beqz v1,0x42a8ac` (idx≥15 → return) | `beqz v1,0x42e404` |
| `0x42e404` (3 zero words after `jr ra`+delay) | `0` | `j 0x789f4c; nop` |
| `0x789f4c` stub | `0…` | same as above with `li v1,15`, `ret: j 0x42a8ac` |
| **sources** `0x42ada4` (restyler loop 2) | `bnez s8,0x42acec` (gray out) | `j 0x789f68` (delay slot kept) |
| **sources** `0x42a0b4` (tap→index loop 2) | `beqz s8,0x42a238` (enable) | `j 0x789f8c` (delay slot kept) |
| `0x789f68` / `0x789f8c` stubs | `0…` | `li v0,15; beq s0,v0,EN; nop; bnez s8,DIS; nop; EN: j <enable>; nop; DIS: j <disable>; nop` |
| `0x789fb0` | `0…` | shared command string (≤ 80 bytes incl. NUL) |
| PT_LOAD[0] | filesz=memsz=`0x389f30` | `0x38a000` (cave is formally inside the segment) |

- Stubs check `idx == 7` / `idx == 15` **exactly** (the index lookup may return −1).
- The handler's frame is already saved, so `jal system` clobbering `ra` is fine; `0x433960` restores `ra`/`s0`.
- The patcher asserts the stock bytes and an empty cave/gap, so it refuses to run on anything but stock v159 and refuses to run twice.
- Default command: `/usr/bin/k15-doom-launch </dev/null >/dev/null 2>&1 &` (max 79 chars). The `&` matters: `system()` blocks the LVGL thread until the shell returns.
- MD5s (default command): `--hooks others,sources` `24d5926781483fd5fd6aded09ad60053` (the first flashed build); `--hooks others` `92807e913791fdb1c3dbce1c65d0a88b`; Stage 2 default `sources,knob` **`a9bbbc9a15e5e72b37f7e5fef47ee3b3`** (flashed, §11); `others,sources,knob` `b0bfba8c5476c9bbd43c65bd910afc1e`; `knob` only `ed317e1ce2145bd2be6a9cc88c629017`. Since Stage 2 the default is `sources,knob`; `others` is opt-in (with `patch_json.py --others`). Hardware-tested PoC (`--cmd "/usr/data/doom/launch.sh </dev/null >/dev/null 2>&1 &"`, both hooks): `d54a21b5f304c8763f6458b24f89c659`.
- qemu smoke: `qemu-mipsel-static -cpu P5600 -L <rootfs> ./mq_ui.patched` must reach `Error: cannot open framebuffer device` like stock. **Use `./` in the path**: a bare filename makes qemu exit 1 silently, which looks like a broken binary.

### 1.3 Hardware PoC (how it was proven, re-usable for dev without flashing)

```sh
# on device, RAM-only, gone after reboot:
mount -o bind /usr/data/doom/poc/mq_ui.poc /usr/bin/mq_ui
mount -o bind /usr/data/doom/poc/other_set.json /usr/project/config/ui/other_set.json
kill $(pidof mq_ui)        # watchdog: killall mq_player, restarts both within ~5–10 s (no reboot)
```

Result: the 8th row "DOOM" appeared (borrowing `factory.png`), the tap ran the hook (`ppid=1`) and DOOM started. The other 7 rows still work. Killing `mq_ui` only causes an app restart (logged in `/usr/data/fiio/log/process_failed.txt`), **not** a reboot.

A bind-mounted running binary can't be `umount`ed ("busy"): stack another bind mount on top, then `kill` `mq_ui`. The step-by-step version is in [development](development.md#swapping-in-a-patched-mq_ui-no-flash-no-reboot).

### 1.4 Firmware integration ✅ (implemented in [`rootfs-stage.sh`](../../tools/rootfs-stage.sh))

As root in the `doomfw-builder` image:
- Replace `/usr/bin/mq_ui` with the patched one, keeping mode `0755 root:root`.
- Only with `patch_json.py --others` (off since Stage 2): append "DOOM" to all three `obj_items` lists and an icon path to `img[]` in `/usr/project/config/ui/other_set.json` ([`patch_json.py`](../../tools/patch_json.py)). Python's `json.dumps(indent=4, ensure_ascii=False)` + `"\n"` reproduces the stock files byte for byte, so the patcher asserts that round-trip and the stock item counts (7 / 15) before editing.
- Append "DOOM" to all three `obj_items` lists and an icon to `img[]` in `/usr/project/config/ui/right_input_mode.json` (16th item = free grid cell).
- Add icons: `/usr/project/config/img/input_setting/doom.png` (62×33), and up to Stage 1 `/usr/project/config/img/menu_setting/other/doom.png` (54×28), rendered by [`make_icon.py`](../../tools/make_icon.py) from [`assets/doom-logo.svg`](../../assets/doom-logo.svg). That's the DOOM wordmark as one even-odd path; the text logo is in the public domain, and "DOOM" stays an id/ZeniMax trademark. It's filled in the stock icon brown `(79,45,8)`, with 8× supersampling and box downscaling for the anti-aliased alpha. The UI recolors the selected state. At ~60 px the filled wordmark reads clearly while an outline version smears (the D/O counters are only a few pixels wide), so `fill` is the default; `make_icon.py preview` shows both. The first PoC icon was a hand-made 7-row block-glyph "DOOM" (no logo data). The same tool also emits `engine/src/k15_logo.h` (7 polygons, 52 points), which the DOOM home menu rasterizes large with a yellow→red gradient. LVGL `D:` maps to `/`, so a PoC JSON can point at `D:/usr/data/...`.
- Add `/usr/bin/k15-doom-launch` (§4), `/usr/bin/doomk15`, `/usr/bin/k15-mqsend`, `/usr/share/doc/doom-fw/NOTICE` (GPL notice + source location) and `/etc/homebrew_version` (`HOMEBREW=doom`, `BASE=v159`, `BUILT=…`, `DOOMFW=<git describe>`).

### 1.5 DOOM as a "source" tile ✅ (proven on hardware)

- The swipe-right source screen is an **8×2 grid**: row 1 = the 8 inputs (index 0–7), row 2 = 7 Local Play shortcuts (8–14) and a **free 16th cell**. Appending a 16th JSON item puts DOOM exactly there. Builder `0x42d280` → `0x43c7f4(…, "right_input_mode.json", …, flags=4, cb=0x42a880)`; click cb: `idx = 0x429f14(target); if (idx < 15) jump_table@0x633470[idx]`.
- Row-2 tiles are grayed and **`lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE)`**'d unless Local Play is selected. That logic is loop 2 of the restyler `0x42aba0(sel)` and of the tap→index function `0x429f14`. Both loops run up to the JSON child count, so they caught tile 15 too: the tile went gray and, as gdb confirmed, no events at all reached the callback. LVGL v8 symbols seen: `0x4bd970 = lv_obj_add_flag`, `0x4bda4c = lv_obj_clear_flag`, `0x4b6930 = lv_event_get_code`, `0x4b68d8 = lv_event_get_target`, `0x4ceff0 = lv_obj_get_child`, `0x4cf0cc = lv_obj_get_child_cnt`. The extra patches keep tile 15 always enabled.
- Tested: on Local Play and on USB, tapping DOOM → launcher switches to Local Play (`input now 8` within ~1 s) → DOOM with sound → quit → input restored to USB, back on the source screen. Tapping DOOM momentarily styles it as "selected" (it counts as idx ≥ 7 in the restyle), which is consistent because the input then becomes Local Play anyway.
- A gdb tracer with read-only `dprintf`s on the click callback (detached on SIGINT) is how the "no events" diagnosis was made.

### 1.6 Volume-knob input carousel (Stage 1B RE, 2026-10-31, static + read-only)

Implemented in Stage 2 as option (b), see §1.7.

**The knob is a Linux input device, not MCU-only.** `x2000_key` (`/dev/input/event1`) advertises keys **249–258**. `mq_player`'s key handler `0x4e6c40(code, value)` (v159 `mq_player`) turns them into `aa06` messages on `/ui` when its knob-mode flag (`*(0x7f8490+8)`) is set; otherwise the same keys go to `0x4e51e0`, presumably volume (`VOL_KNOB_MODE` is a system config key):

| evdev key | `mq_player` sends | `mq_ui` carousel action |
|---|---|---|
| 249 press | `aa06` 0 | commit |
| 251 press | `aa06` 2 | next |
| 252 press | `aa06` 3 | previous |
| 250 press | `aa06` 1 | ignored while open |
| 253 press | other message (code 5) + `0x4def70` | — |
| 254 release | `0x4d5194` | — |
| 258 press | toggles `+264`, sends a `0x1017` event | — |

Which physical control is which code was **confirmed live** on 2026-10-31 with `k15probe watch` (no grab). The MCU changes what the knob sends depending on the mode:

| Code | Seen when | Physical action |
|---|---|---|
| 249 | instant press+release (same ms) | **menu button** (opens the carousel), *and* **knob push while the carousel is open** (commit). Push → "switched immediately" |
| 251 / 252 | carousel open | knob **clockwise = next** / **counter-clockwise = previous** (OPTICAL +2 clicks → LINE IN) |
| 254 | normal mode | knob turn: inverted 0→1 activity pulse per detent, **no direction**. The volume itself goes via the MCU, so **volume works while DOOM runs** (confirmed by the tester) |
| 258 | normal mode | knob push (short or long) → screen off/on toggle (`+264`) |
| 250, 253, 255–257 | never seen | — |

So while DOOM grabs `x2000_key`: the menu button can't open the carousel (good), the knob push is USE, and volume still works. A knob-gesture daemon (option d) can't see the turn direction in normal mode.

**`mq_ui` side (v159, `ui_input_mode_switch.c`):**

| Address | Role |
|---|---|
| `0x41c1ec` | `/ui` dispatcher: message `aa06` → `0x41f8b0(code)` |
| `0x41f8b0` | `malloc(4)`, `lv_async_call(0x42e9f4, &code)` |
| `0x42e9f4` | knob handler. Returns early if the current screen is the sound pop-up `0x8ab154` (log line 147), or if guard flags `+0xd4`/`+0xd8` are set (lines 153/159). If the carousel screen (`0x8aae64`) isn't shown: open it at `idx = cur_input`, set the 3 labels, load the screen (`0x431d80`) and start a 3000 ms `lv_timer` (`0x50cc4c`, cb `0x42e7bc`, handle in `0x8aae68`). If it is shown: reset the timer and handle code 2 → next (`0x42eef8`), 3 → previous (`0x42edc8`), 0 → commit (`0x42ed0c`) |
| `0x42e410` | carousel screen gesture/key callback: next/previous as above |
| `0x42e908` | `LV_EVENT_RELEASED` (8) on child index 1 (center label) → commit |
| `0x42e7bc` | 3 s timer → commit (auto-commit) |
| `0x42f048` | builds the carousel screen (stored in `0x8aae64`) |
| `0x8aae6c` | highlighted index (0–7) |
| `0x8aae70/74/78` | prev / center / next **label** objects |
| `0x8acaa0` (`0x8aca90+0x10`) | current input (0-based; 6 = Streaming/`MEDIA`, 7 = Local Play) |
| `0x8acb14` (`+0x84`) | UI language index (2 = English) |
| `0x79a474` | static `char *labels[13][8]` (13 languages × 8 inputs, in `.data`) |

The carousel is **three text labels** (`lv_label_set_text_fmt(obj, "%s", labels[lang*8+i])`, `0x51e214`), not images, so a DOOM entry only needs the string "DOOM". All three commit paths do `if (idx != cur_input) { cur_input = idx; send_cmd("0657", idx+1, 0) }`, then load the playback screen (`cur_input < 6` picks between `0x8ab3bc` and `0x8abc70`) and delete the timer. The `0657` sites are `0x42e87c`, `0x42e9e4` and `0x42ed48`.

**Every hard-wired 8:**
- Wrap sites use the signed `x % 8` idiom (`sra 31; srl 29; addu; andi 7; subu`) at `0x42e62c`, `0x42e73c`, `0x42eb04`, `0x42edd8` and `0x42ef0c`.
- Bounds checks: `slti 7` at `0x42e500`/`0x42ef00` (wrap to 0), and `idx==0 → 7` at `0x42e664`/`0x42edd0`/`0x42f038`.
- The label stride is `sll 3` in every lookup: five 3-label update sequences, each ending in a tail `j 0x51e214`.

**Design options (for Stage 2):**
- **(b) 9th entry "DOOM"** (preferred by the tester).
  - Replace the five 3-label sequences with calls to one cave stub `set_labels(prev, cur, next)`, where `label(i) = i == 8 ? "DOOM" : labels[lang*8+i]`.
  - Patch the 5 wrap sites and 5 bounds checks to mod 9.
  - In each of the 3 commit paths, test `idx == 8` *before* `cur_input = idx`: run `system("/usr/bin/k15-doom-launch … &")`, never store 8 into `cur_input`, and go back to the previous screen.
  - Estimated ~400–600 B of stubs. The existing cave is full, but the binary has a spare **PT_NULL program header (index 9)**: turn it into a new `PT_LOAD R+X` for a page appended at file end, so the header table doesn't move.
- **(a) Intercept only.** E.g. treat a commit while `idx == cur_input` (push without turning) or `aa06` 1 as "launch DOOM". Tiny patch, but not discoverable.
- **(d) No `mq_ui` patch:** a small boot-started daemon watches `x2000_key` without grabbing and launches DOOM on a knob gesture (e.g. a long push). It needs an init hook in the rootfs and must not fight `mq_player`'s own handling of the press.
- **(c)** Replace a stock input slot. That changes stock behavior, so it needs the tester's OK.

Still to verify live (needs the tester): the key-code ↔ control mapping, what sets the knob-mode flag, how `aa06` 1 arises, and whether the volume knob works while DOOM is running. Use the `dd if=/proc/<pid>/mem` peek (`skip=` the address, no stop) to read `0x8aae6c`/`0x8acaa0` live. **Don't attach gdb with `set charset`**: the device's gdb aborts and leaves `mq_ui` SIGSTOPped. Fix that with `kill -CONT`.

### 1.7 Knob carousel hook ✅ (Stage 2, `patch_mq_ui.py --hooks …,knob`)

Design (b) from §1.6, but instead of patching the five wrap sites, five bounds checks and five label sequences one by one, the paths that touch the highlighted index are **redirected wholesale** into new code. The index `0x8aae6c` is read and written only inside `0x42e410…0x42f048` (checked across the whole binary), so nothing else can see the value 8.

**New segment.** The 208-byte cave is full, so the spare **PT_NULL program header (index 9)** becomes `PT_LOAD R+X`, offset `0x3c0000` (the file is zero-padded from 3,905,756 B), vaddr `0x8c0000`, 64 KiB aligned like the other segments, filesz = memsz = 0x20e. It sits above the RW segment's bss end `0x8acdf8`, so the kernel maps the bss first and the heap starts after our page. Branches reach it with plain `j` (same 256 MiB region), so no trampolines. The patcher refuses a non-stock file size or a non-empty PT_NULL.

**Code** (hand-assembled by a tiny assembler in the patcher, ~120 instructions):

| Symbol | Does |
|---|---|
| `step(a0=±1)` `0x8c0000` | `idx = (idx + a0) mod 9`, then falls into `set_labels` |
| `set_labels()` `0x8c002c` | for k = 0..2: `i = (idx + 8 + k) mod 9`; `lv_label_set_text_fmt(obj[k] @0x8aae70+4k, "%s" @0x633ad4, i == 8 ? "DOOM" : labels[lang*8+i] @0x79a474)`; lang `0x8acb14` |
| `g_next` / `g_prev` | `step(±1)`, then the gesture callback's epilogue `0x42e4d4` |
| `k_next` / `k_prev` | `step(±1)`, then the knob handler's epilogue `0x42ea74` |
| `k_open` | `idx = cur_input` (`0x8acaa0`), `set_labels()`, continue at `0x42ebec` (load screen, start the 3 s timer; the stock code stores the same idx again) |
| `c_timer` / `c_release` / `c_knob` | entered with `v0 = idx`, `v1 = cur_input`: equal → stock "unchanged" path; idx == 8 → `system(CMD)`, `v0 = cur_input`, stock "unchanged" path (so no `0657`, `cur_input` never becomes 8, the playback screen loads as usual); otherwise → stock send path |

| Site | Stock | Patched |
|---|---|---|
| `0x42e4f8` gesture "next" | `lui s1,0x8b; lw a0,idx` | `j g_next; nop` |
| `0x42e660` gesture "prev" | `lw s4,idx; bnez s4,…` | `j g_prev; nop` |
| `0x42eaf4` knob opens carousel | `lw v0,16(s0); lui v1,0x7c` | `j k_open; nop` |
| `0x42eef8` knob next | `lui s2,0x8b; lw a0,idx` | `j k_next; nop` |
| `0x42edc8` knob prev | `lui s2,0x8b; lw s4,idx` | `j k_prev; nop` |
| `0x42e810` 3 s timer commit | `bne v1,v0,send` | `j c_timer` (delay slot `addiu a1,v0,1` kept) |
| `0x42e984` center-label release | `bne v1,v0,send` | `j c_release` (delay slot kept) |
| `0x42ed34` knob push commit | `beq v1,v0,eq` | `j c_knob` (delay slot `lui a0,0x63` kept) |

The patched-over delay slots are not branch targets anywhere in the binary. The host functions' frames are live at every site, so `jal` clobbering `ra` is fine (each epilogue reloads it). The stock log lines for next/prev/open are skipped. The 3 s auto-commit launches DOOM too, like it switches inputs (tester: keep it).

Proven with a RAM bind mount on the DOOM firmware before any build (§11): the DOOM entry appears between LOCAL PLAY and the first input and wraps both ways; knob push, center tap and the 3 s timeout launch DOOM; ordinary input switching unchanged; no `0657` with argument 9 was ever sent (`[UI SEND]` in `fiio_ui.log`); no `mq_ui` restarts.

## 2. UI ↔ player IPC (POSIX message queues) ✅

- Two queues, both opened by `mq_ui` and `mq_player`: **`/player`** (UI → player commands) and **`/ui`** (player → UI reports and echoes). `msgsize_max 8192`, `msg_max 10`. Inspect with `mkdir /tmp/mqfs; mount -t mqueue none /tmp/mqfs; cat /tmp/mqfs/player`.
- **Messages are ASCII.** `mq_ui`'s sender `0x43ad54(cmd, ext, payload)` does `snprintf("%s%04X%04X%s", cmd, total_len, ext, payload)`, where `total_len = 12 + strlen(payload)`. A variant at `0x43ab44` does `"%s%04X%s"` (no ext). `mq_ui` logs every send as `[UI SEND]: …` in `/usr/data/fiio/log/fiio_ui.log`, which is a free protocol sniffer.
- Sending is safe for third parties. **Receiving is not**: `mq_receive` would steal messages from the real consumer. Never read `/player` or `/ui`.
- To capture traffic: `strace -f -tt -p $(pidof mq_ui) -e trace=mq_timedsend,mq_timedreceive -s 200` (`strace`/`gdb` exist on the device; busybox has **no `timeout`**, so background and `kill`).
- Known codes seen: `0657` set input (below), `0105` (sent after an input change, ext=8), `0627` (EQ-name query, payload `0000A0A9`). There are 109 `send_cmd` call sites in `mq_ui` (calls to `0x43ad54`); their arguments can be read from an objdump.

## 3. Input switching ✅ (proven, via the stock player path)

- **`SET_INPUT_MODE` = `0657000C%04X`** on `/player`, the exact message the UI sends. `mq_player` then does everything itself: `set mcu input mode: N` → `input_mode_switch` → MCU SPI `SET_INPUT_MODE` frame (`FC FE 09 10 04 0N …`). It also echoes to `/ui`, so `mq_ui` refreshes its state (`refresh_input_mode_ui`, `current_main_ui`). Sent by our [`k15-mqsend`](../../launcher/k15-mqsend.c) with tester sign-off: Streaming → Local Play → Streaming. Player and UI logs matched a manual tap exactly. The visible screen doesn't necessarily change if the user is in Settings.
- Input IDs: **USB=1, Streaming (MEDIA)=7, Local Play=8** ✅. Probably 2=Optical, 3=Coax, 4=Line, 5=Balanced, 6=BT (follows `right_input_mode.json` order and `INPUT_MODE_*` names) ❓.
- **Read the current input** lock-free (the player keeps the DB open): `sqlite3 'file:/usr/data/fiio/db/sysconfig.db?immutable=1' 'select INPUT_MODE from SYSCONFIG;'` → `7` ✅. It updates within ~1 s of a switch ✅ (USB `1` → `8`, seen by the PoC launcher). Fallback: last `input_mode_switch:` line in `/usr/data/fiio/log/fiio_player.log`.
- DOOM audio needs input 7 or 8 (see [device: audio](device.md#audio)), plus PC28 high, which `doomk15` already handles. Recommended logic: `prev=$(read)`; if `prev` ∉ {7,8} → send `…0008`, wait until the DB reads 8 (≤3 s); on exit, if we switched → send `…%04X prev`.
- ✅ *Paused stock song still owns `hw:0,4`*: the UI's play/pause message is `0201000C0000`, a **toggle** (the player logs `[START]` and `[PAUSE]` for the same message), so it can't be used blind. Instead the launcher reuses the approved input switch: changing input makes `mq_player` close the PCM. If `/proc/asound/card0/pcm4p/sub0/status` isn't `closed` on input 7/8, the launcher switches to the *other* SoC input (8↔7) and restores the original one on exit. A paused PCM reports `state: RUNNING`. Hardware log: `hw:0,4 busy (state: RUNNING) on input 8` → `switched input 8 -> 7 (now 7, pcm closed)` → DOOM with sound → `restored input 8`. The paused song is stopped (like any input change).

## 4. Launcher `/usr/bin/k15-doom-launch` ✅ ([source](../../launcher/k15-doom-launch))

A busybox `sh` script, started by `mq_ui` via `system(… &)` as root with `ppid=1`. `mq_ui`'s only non-CLOEXEC fds are `fb0` (fd 3) and `event2` (fd 4); the mqueues are CLOEXEC. Busybox ash keeps its script fd at ≥10.
1. `exec 3>&- … 9>&-` drops the inherited fds. Single instance: `pidof doomk15` plus a lock dir `/tmp/k15-doom.lock` with the owner's PID (a dead owner = stale lock, removed). Then it re-runs itself under `setsid … --detached`.
2. Logs to `/usr/data/doom/launch.log`, capped at 64 KB with one `.1` kept. The engine log `doom.log` holds the current run, the previous one goes to `doom.log.1`.
3. Binary: `/usr/data/doom/doomk15` if executable (logs `DEV BUILD`, exports `K15_DEV_BUILD=1` so the menu shows a tag), else `/usr/bin/doomk15`.
4. Input (§3): not 7/8 → Local Play. 7/8 with `hw:0,4` busy → the other SoC input. It waits ≤3 s for the DB to read the target and the PCM to be `closed`. `trap cleanup EXIT` (+ INT/TERM/HUP, which also kill the child) runs `doomk15 --restore`, restores the input and removes the lock, so crashes and `kill -9` of the engine are covered ✅ (rc=137 → restored).
5. Runs `doomk15` without arguments, i.e. the home menu. No `HOME` needed: the config dir is compiled in (§5).

Dev overrides via env: `K15_HOME`, `K15_MQSEND`, `K15_DOOM_SYSBIN`.

## 5. Engine ✅ ([`engine/`](../../engine/))

Imported from the proof-of-concept port with a reproducible build. [`prepare.sh`](../../engine/prepare.sh) (in `doomfw-builder`, like every step since the Docker-only rework) clones doomgeneric `e257a0952706…` and Chocolate Doom `939cbfeb1153…` (tag 2.3.0) once, `git archive`s the pinned commits, applies `patches/doomgeneric/0001-widescreen-hor-plus.patch` (the PoC patch, verbatim) + `0002-k15-integration.patch` and `patches/chocolate-doom/0001-k15-no-sdl.patch`, and generates `opl_k15.c` with `port_opl.py`. [`build.sh`](../../engine/build.sh) compiles with the same image's `mipsel-linux-musl-gcc` (no make: parallel batches); the Docker-only rework reproduced `doomk15` and `k15-mqsend` byte-for-byte. Upstream code builds with `-w`, our sources with `-Wall` (clean). The baseline import reproduced the PoC binary size exactly (628,596 B); the finished engine is ~669 KB.

Source split (`engine/src/`): `k15_plat.c` (fb page flip + evdev grab/hotplug + usbfs XInput + UDP, shared), `doomgeneric_k15.c` (game backend, key/pad mapping, cheat menu, exit paths), `k15_menu.c` (home menu), `k15_cfg.c` (`k15.conf` settings, Stage 3), `k15_touch.c` (touch layout presets, shared by the game and the menu's preview), `k15_gfx.c` (canvas, 8×8 font, icons, AA logo fill), `k15_wad.c` (discovery), `k15_hud.c`, `k15_sound.c`, `font8x8_basic.h` (dhepper/font8x8, public domain), `k15_logo.h` (generated), `k15_icons.h` (16×16 line art).

- **Process flow:** `doomk15` → home menu; choosing a WAD calls `doomgeneric_Create()` in-process with `-iwad …` (fb and grabs stay open). Quitting the game (`I_Quit` → `exit(0)` → atexit) **`execv`s `/proc/self/exe` back into the menu**, on the WAD list. The grabbed evdev fds are handed over (CLOEXEC cleared, numbers in `K15_EVFDS`) and the new process keeps showing our fb page, so there is no frame and no instant where a tap could reach `mq_ui` ✅. It waits for the finger to lift first. `I_Error` → the message is shown in a menu modal after the same re-exec. `doomk15 -iwad X` (adb/dev) skips the menu.
- **Config persistence (root cause found):** doomgeneric compiles `SaveDefaultCollection` **and** `LoadDefaultCollection` out (`#if ORIGCODE`). `configdir` was `"."`, so the name became `.default.cfg`, and `I_Quit` never `exit()`s. Patch 0002 fixes all of it under `-DK15`. Config dir `/usr/data/doom/` (`K15_HOME` overrides). Save/load enabled. Key values 128–255 are kept untranslated on load: doomgeneric's `KEY_FIRE` 0xa3 / `KEY_USE` 0xa2 have no scancode, and the stock loader would have zeroed them, unbinding FIRE after one save/load. `show_endoom=0`; `I_Quit` exits. Saves go to `savegame/<iwad>/`, so DOOM and DOOM II can't collide. ✅ `default.cfg` written on quit and on the ESC-hold exit; `key_fire 163`; FIRE works after reload.
- **WAD discovery:** `/proc/mounts` entries under `/tmp/udisk*` ✅ (a FAT stick mounted as `/dev/sda1 → /tmp/udisk1`), plus `/usr/data/doom/wads/`. IWAD/PWAD and family come from the lump directory (`MAPxx` / `E4M1` / `E2M1` / `FREEDOOM`); the label comes from the file name when known. Hidden/AppleDouble `._*` files and non-WADs are skipped. A PWAD plays with `-iwad <best base> -file <pwad>`: for MAPxx, doom2 → freedoom2 → plutonia → tnt; never shareware. With no base, PLAY is disabled and the row says "NEEDS AN IWAD". The list rescans every 2 s.
- **Menu** (reworked in Stage 3, §5.2): Home (logo, PLAY / WADS / OPTIONS / EXIT, DEV BUILD or version tag), WADS (☆ favorite, PLAY, INSTALL / DELETE with a YES/NO modal; keyboard default NO), OPTIONS (layout, controls, features), "NO WAD FOUND" (shows the USB status, jumps to the list when a stick appears), install progress, LOADING. Touch activates on release (slide off = cancel). Keys, pads, IR and the UDP keyboard move a focus frame.
- **Install:** `statvfs` free ≥ size + 5 MB, else a modal with both numbers. Copies in 256 KB chunks with a progress bar to `wads/.<name>.part`, then `fsync`, `rename` and a directory `fsync` ✅ (DOOM2.WAD, 14.6 MB).
- **Overrides:** stick-root `doom.cfg` → `-k15overlay <file>`, loaded on top of `default.cfg` for that run (saving still goes to `default.cfg`) ✅. `k15.conf` (`/usr/data/doom/`, then the stick root) only accepts the typed keys of `k15_cfg.c` (§5.2). Anything else (e.g. `LD_PRELOAD`, which would reach the `popen`ed `aplay` as root) is ignored ✅. Since Stage 3 the values are no longer exported to the environment at all.
- **ESC-hold exit:** the pill fills red and reads EXIT after 0.3 s; at 3 s it does `I_Quit()` with exit-to-UI set (config saved) ✅ (`ESC held 3024 ms`).
- **DOOM II soak** ✅: `idclev` to MAP15/20/24/27/28/29/30/32 with 2 full held turns each: no `I_Error`, no crash. The raised `MAXVISPLANES` 1024 / `MAXDRAWSEGS` 2048 limits hold.
- **Full view only** (Stage 2, patch `0004-k15-full-view-only.patch`): the widescreen build never draws the vanilla status bar (the HUD replaces it), so `screenblocks` < 11 left the strip under the view unpainted. `R_SetViewSize` now clamps to 11 when `WIDE_DELTA` (menu slider, `-` key, config, stick overlay), the slider stops at full, and `M_Init` fixes an old config value ✅.

### 5.1 Keyboard and gamepad input ✅ (Stage 2)

Findings it builds on: [device: USB input](device.md#usb-input).

- **Classification by capabilities** (`EVIOCGBIT`, `EVIOCGPROP`): *touch* = `INPUT_PROP_DIRECT` or `BTN_TOUCH`, plus MT/ABS position axes (the wired Keychron's `input2` collection exposes every ABS axis including the MT ones but neither of those, and was misread as a touchscreen before); *pad* = `BTN_GAMEPAD` / `BTN_JOYSTICK`; everything else delivers keys.
- **Hotplug:** `K15_InputPump` rescans `/dev/input` every second (dedup by `st_rdev`, so the fds inherited over the menu↔game `execv` aren't opened twice), grabs new nodes and drops a device on `POLLHUP`/`POLLERR`/`ENODEV`. A vanished pad releases every key it held.
- **XInput pads** (interface `ff/5d/01`, no kernel driver): found via sysfs (`bInterfaceClass/SubClass/Protocol`, then the parent's `busnum`/`devnum`), opened as `/dev/bus/usb/BBB/DDD`, interrupt endpoints parsed from the raw descriptors, `USBDEVFS_DISCONNECT` + `CLAIMINTERFACE`, LED packet `01 03 02` (the pad leaves "waiting for host"), then an **async interrupt URB** (`SUBMITURB`, `poll` for `POLLOUT`, `REAPURBNDELAY`, resubmit), so it lives in the same `poll()` as evdev. The fd is `O_CLOEXEC`: the claim is released on exit or exec and re-made by the next process. 20-byte reports → one pad state.
- **One pad abstraction** for XInput and evdev pads: buttons go out through the normal key callback as evdev codes with Xbox labels (`BTN_A/B/X/Y`, `BTN_TL/TR`, `BTN_START/SELECT`, `BTN_THUMBL/R`, `BTN_DPAD_*`, HAT → D-pad), triggers as `BTN_TL2/TR2` (threshold 100/40), the left stick also as `K15_KEY_LS_*` (20000/12000 hysteresis); analog values via `K15_Pad()`. evdev pads map `ABS_X/Y` left, `ABS_RX/RY` right, `ABS_Z/RZ` triggers (xpad-style; untested, no D-input pad was tried).
- **Mapping in the game** (decided at press time; the DOOM keys of each press are remembered per code, so a release always matches even if the menu opened in between):
  - keyboard: vanilla keys; in a level with the menu closed, W/S/A/D become `key_up/down/strafeleft/straferight` while the letter is passed as the typed character (patch `0003-k15-typed-char.patch`: `I_GetEvent` takes `data2` from `K15_TypedChar()`), so `iddqd` and save names still work; `[`/`]` = prev/next weapon; volume keys unmapped (they used to resize the view);
  - pad in game: RT/A `key_fire`, X/B `key_use`, LB/RB `key_prevweapon/nextweapon` (bound to free codes 0xa4/0xa5 if the config leaves them 0), LT/L3 `key_speed`, Y/Select Tab, D-pad arrows, LS left/right strafe, Start Esc;
  - pad analog: once per tic (when `DG_GetKey` runs dry) an `ev_mouse` with `data2` = right-stick turn (deadzone 8000, squared, max 200; keyboard fast turn = 160) and `data3` = left-stick forward (linear, max 50 = `MAXPLMOVE`). Only in `GS_LEVEL` with the menu closed, because `M_Responder` turns mouse motion into menu navigation;
  - pad in DOOM's menu: D-pad/LS arrows, A = `y` tap + Enter, B = `n` on prompts else Backspace, Start = Esc;
  - holding keyboard Esc or pad Start for 3 s exits to the K15 UI (like the ESC pill).
- **Home menu:** D-pad/LS/WASD navigate, A/Start/Space select, B/Select/Backspace back.
- **Touch controls auto-hide** as soon as a key or pad button is pressed (front-panel codes 249–258 excepted) and come back on the next touch, which is swallowed until all fingers are up. The HUD stays. Optional since Stage 3 (`K15_AUTOHIDE`).
- Stage 3 made the WASD / `[` `]` remap and the pad buttons rebindable (§5.2).

### 5.2 Home menu, options and settings ✅ (Stage 3)

**Screens** (`k15_menu.c`; each screen builds `button_t`s, `activate()` dispatches):

- **HOME**: 2×2 grid with icons: **PLAY**, **WADS**, **OPTIONS**, **EXIT**. PLAY's sub-label and action (`play_state()`, rechecked on the 2 s rescan), first match wins:

  | State | Sub-label | PLAY |
  |---|---|---|
  | favorite set (installed) | 16 px yellow ★ + title | starts it (LOADING) |
  | no WADs | `NO WAD FOUND` | NO WAD FOUND screen |
  | one WAD, on the K15 | title | starts it |
  | one WAD, stick only | title | modal: INSTALL / PLAY FROM USB; a tap outside the box or Esc cancels. INSTALL copies it, makes it the favorite and starts the installed copy |
  | more than one WAD | `SELECT WADS` | opens WADS |

  A single WAD that can't be played (a PWAD without a base IWAD) opens WADS, so the user sees "NEEDS AN IWAD". PWADs count as WADs. The title is the game name for a known IWAD file name (`doom2.wad` → DOOM II, `freedoom1.wad` → FREEDOOM PHASE 1, …), else the upper-case file name without `.wad` (a WAD has no title field).
- **WADS**: one row per WAD: ☆ (installed WADs only: set/clear the favorite; yellow = favorite), PLAY, INSTALL (stick) or DELETE (installed). A stick WAD with the same file name (case-insensitive) as an installed one is left out of the scan (`K15_WadScan`), so it is neither listed nor counted. Deleting the installed copy brings it back. The old Options screen is merged into it. Header: free space on the K15.
- **OPTIONS** → **LAYOUT** (preset + PREVIEW, transparency), **CONTROLS** (→ TOUCH, KEYBOARD, GAMEPAD, each tile saying CONNECTED / NOT CONNECTED), **FEATURES**. Every list ends with RESET TO DEFAULTS (YES/NO).
- **PREVIEW**: the real touch overlay (`k15_touch.c`) over a dummy view. Touch lights and flashes the controls; a tap that starts on the view returns; the ESC hold works too.
- **Key capture** (KEYBOARD → tap a key name): the next keyboard key (`< BTN_MISC`, not Esc, not the front panel 249–258) is bound; any other action with that key is cleared.

**Quitting a game** returns to where it was started: HOME for PLAY on the home screen, else the WAD list (`K15_MENU_RETURN` → `K15_MENU_SCREEN` over the `execv`).

**Navigation**: touch fires on release; keys move a focus frame to the nearest button in the pressed direction (score = distance along + 3 × distance across; left/right stay on the row). At a list's top/bottom row the list scrolls and the focus keeps its column. Left/right change the focused value (`< >` rows) or flip a toggle; Enter/A cycles a value. The focus is kept as (action, arg), so it survives redraws. TAB / prev-next-song step linearly. A keyboard or pad hotplugged while the menu shows updates the CONTROLS tiles.

**Settings** (`k15_cfg.c`; `k15.conf` = `KEY=VALUE`): loaded from `/usr/data/doom/k15.conf` (the saved values), then from a stick root (overrides for that run, never saved; the menu shows "SET BY THE USB STICK K15.CONF THIS RUN"). The menu saves on leaving a settings screen, on ☆, and on PLAY/EXIT, only if something changed: temp file, `fsync`, `rename`, directory `fsync`. Unknown keys are logged and dropped on the next save. `K15_DEFAULT_WAD` must be a bare file name (no `/`). It's matched case-insensitively against **installed** WADs only. One that isn't installed (deleted, or an old stick-only favorite) is cleared on the next scan.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `K15_DEFAULT_WAD` | (empty) | installed file name | the favorite, what PLAY starts |
| `K15_LAYOUT` | `CLASSIC` | `CLASSIC` `LEFTY` `MINIMAL` `BIG` | touch layout preset |
| `K15_TRANSPARENCY` | 50 | 0–90 step 10 | 0 = most solid, 50 = the original look; alpha × (100 − T) / 50 |
| `K15_JOY_DEAD` | 22 | 10–50 px | stick deadzone |
| `K15_TAP_USE` | 1 | 0/1 | tap the view = USE |
| `K15_ESC_HOLD` | 3000 | 1000–6000 ms | ESC pill / keyboard Esc / pad Start hold-to-exit |
| `K15_FLASH` | 1 | 0/1 | white flash on a touch control when pressed |
| `K15_KEYBOARD` | 1 | 0/1 | keyboards act in the game (they stay grabbed either way) |
| `K15_KB_<ACTION>` | W S A D [ ] | evdev code, 0 = none | extra key for FORWARD, BACK, TURNLEFT, TURNRIGHT, STRAFELEFT, STRAFERIGHT, FIRE, USE, RUN, STRAFE, PREVWPN, NEXTWPN, MAP |
| `K15_PAD_<BUTTON>` | A FIRE, B USE, X USE, Y MAP, LB PREVWPN, RB NEXTWPN, LT RUN, RT FIRE, L3 RUN, R3 NONE, BACK MAP | `NONE FIRE USE RUN STRAFE PREVWPN NEXTWPN MAP MENU` | pad button action in a level |
| `K15_AUTOHIDE` | 1 | 0/1 | hide the touch controls on key/pad use |
| `K15_CHEATS` | 1 | 0/1 | cheat menu (long-press MAP / hold pad BACK) |
| `K15_MUSIC_GAIN` | 1000 | 0–4000 step 250 | OPL music level (menu shows %) |
| `K15_SHOW_FPS` | 0 | 0/1 | FPS plate bottom-right |
| `K15_DEBUG` | 0 | 0/1 | verbose `doom.log` |
| `K15_TOUCH`, `K15_SINGLEBUF` | 0 | | dev only, not in the menu |

`K15_TOUCH/MUSIC_GAIN/DEBUG/SINGLEBUF` may also come from the environment (adb use).

**Keyboard bindings** are *extra* keys: arrows, Ctrl, Space, Shift, Alt, 1–7, Tab and Esc keep their fixed meaning (`evdev_to_doom`). A bound key acts only in a level with the menu closed, and it sends the action's current DOOM key (`key_up`, `key_fire`, …) with the key's own character as the typed char (patch 0003). So `iddqd` and save names still work, and WASD is just the default binding instead of a special case. In menus the key types its character. Keys aren't stored in `default.cfg`: DOOM has one key per action there, so binding W to `key_up` would have cost the arrows. **Pad bindings** map each button except D-pad and Start (fixed arrows / Esc) to an action. Inside DOOM's menu the pad keeps the fixed navigation (A = Enter + `y`, B = Backspace or `n`).

**Keyboard detection** (`K15_HasKeyboard`): an evdev node with KEY_A, KEY_Z and KEY_SPACE that doesn't share its USB VID:PID (`EVIOCGID`) with a connected pad. The 8BitDo receiver exposes such a keyboard node next to its XInput interface, and before this rule it showed KEYBOARD as connected with only the pad plugged in.

**Touch layouts** (`k15_touch.c`, `K15_TouchLayout/Hit/Draw`, with put-pixel and label callbacks, so the game draws with DOOM's HU font and the preview with the 8×8 font):

| Preset | Stick | FIRE / USE | Pills | Tap = USE zone |
|---|---|---|---|---|
| CLASSIC | (165,148) r80, x < 340 | (918,150) r72 / (768,188) r44 | ESC ENTER MAP WPN, top center | x 340–690 |
| LEFTY | (855,148), x ≥ 680 | (102,150) / (252,188) | same | x 330–680 |
| MINIMAL | as CLASSIC | FIRE only | ESC only | x 340–830 |
| BIG | (185,140) r95, x < 380 | (915,148) r88 / (742,184) r54 | 44 px tall | x 380–690 |

The stick is digital (direction from deadzone + 2:1 slope), so the plan's "joystick size" option was dropped: a radius would only change the drawing. While DOOM's menu is open FIRE sends Enter + `y`, so MINIMAL (no ENTER pill) can still pick menu items. Each touch button remembers the key it sent, so the release always matches.

**Cheat menu** (`doomgeneric_k15.c`): a long press (600 ms) on MAP, or holding pad BACK, in a level opens a panel. MAP and BACK act on release when the feature is on, so a short tap still toggles the automap or does the bound action. Opening releases every held key and taps `key_pause` if the game isn't paused. Buttons: IDDQD, IDKFA, IDFA, IDCLIP (typed as `idspispopd` for DOOM 1 and `idclip` for DOOM 2), IDDT (opens the automap first), CLOSE, and WARP `< MAPxx / ExMy >` limited to levels idclev accepts for this game mode *and* whose lump exists. The cheat is typed through the key queue as typed chars. Closing unpauses, except WARP: `G_InitNew` unpauses by itself, and a pause tap would re-pause the new level. Touch works after the opening finger lifts; a tap outside the panel closes it. D-pad/arrows move on a fixed grid, A/Enter select, B/Esc/BACK close. Logged as `k15: cheat …`.

**FPS**: frames presented per second, as a plate with DOOM's tall digits at the bottom right. It overlaps the CLASSIC FIRE disc slightly; the tester found that fine.

**Host harness** ([`engine/tools/harness/`](../../engine/tools/harness/)): `run.sh <outdir> "<script>" [conf lines]` builds `k15_menu/gfx/wad/cfg/touch.c` with the host gcc against fake platform functions and sparse fake WADs (`fakewads.py`). It replays a script (`u d l r e b n`, `t=X:Y` tap, `p=X:Y`/`R` hold/release, `k=CODE`) and writes every presented frame as PNG (PPM without PIL). `HARNESS_KBD=1` / `HARNESS_PAD=1` fake connected devices.

## 6. Builder ✅ ([`build.sh`](../../build.sh))

```text
fiio-k15-doom/
  README.md SECURITY.md CONTRIBUTING.md COPYING(GPL-2.0) .gitignore(build/ out/ *.wad)
  docs/                        user guide, FAQ and these technical pages
  build.sh, build.ps1          ./build.sh <official K15_update_*_v159.zip> [--date] [--time] [--version]
                               [--animation melt|hellfire|demons|terminal|stock] [--engine-only]
                               (build.ps1: -Date -Time -Version -Animation -EngineOnly). Host
                               needs only Docker; both run tools/build-all.sh in doomfw-builder
  docker/Dockerfile            doomfw-builder (label doomfw.builder.rev=1): stage 1 = the pinned
                               k15 musl-cross-make toolchain, stage 2 =
                               ubuntu 24.04 + squashfs-tools 4.6.1 + python3-pil/numpy +
                               fonts-ubuntu + git + patch, /opt/k15-musl copied in
  engine/                      prepare.sh, build.sh, patches/, src/, tools/
  launcher/                    k15-doom-launch, k15-mqsend.c (launch-poc.sh removed for release)
  dev/k15dev.py                adb shell/push/pull, fb0 screenshot (sysfs pan; dd of fb0, or /dev/mem for DOOM's pages), dev-engine deploy
  VERSION                      1.0.666 (fixed release version; DOOMFW_VERSION env overrides)
  rootfs-overlay/NOTICE        -> /usr/share/doc/doom-fw/NOTICE
  assets/doom-logo.svg
  tools/                       build-all.sh (container pipeline), patch_mq_ui.py, patch_json.py,
                               make_icon.py, bootanim.py, treediff.py, rootfs-stage.sh,
                               pkg_build.py, pkg_verify.py
```

Pipeline:
1. Docker images.
2. `pkg_verify.py` on the input zip must PASS (`--extract` writes the reassembled `rootfs.squashfs` + `xImage`). Requires `current_version=159`, `recovery_version=4`.
3. `prepare.sh`.
4. Compile.
5. `rootfs-stage.sh` as root:
   - stock `mq_ui` MD5 check; refuses an already-modified rootfs;
   - patch → `a9bbbc9a15e5e72b37f7e5fef47ee3b3` (`sources,knob`, §1.2/§1.7; the first flashed build was `24d59267…` with `others,sources`);
   - JSON patch (source grid), icon, boot animation (§6.1), binaries, NOTICE, `homebrew_version` (now with `BOOTANIM=<name>`);
   - `treediff.py` vs stock (type/mode/owner/size/MD5/link target of all 4,911 entries) must show exactly the 10 allowed paths: 8 added, `mq_ui` + `right_input_mode.json` modified (12 with the Others row), plus the replaced `start_up/<n>.jpg` frames (61 for `melt`, 97 for the other animations, none for `stock`);
   - `mksquashfs -comp lzo -b 131072 -noappend -no-xattrs`.
6. `pkg_build.py --version 160` (the doom-fw copy refuses `recovery_version` > 4).
7. `pkg_verify.py` must print `RESULT: PASS`.

The output zip (`K15_update_<YYYYMMDD>_<HHMM>_v160.zip`, UTC build time) and a `.txt` with MD5s and the content diff go to `out/`. The time field is safe: `mq_player` takes the version from the first `v` in the name (`strchr(name,'v')` + `"v%d.zip"`, `0x423668`). Only its multi-zip comparison (`0x4c30f0`) reads `"%d.%d.%d.zip"` at the fixed offset `strlen("K15") + 18`, which assumes `K15_update_YYYYMMDD_v`; with one zip on the stick, as always required, it isn't used.

### 6.1 Boot animation ✅ flashed (`--animation`, [`bootanim.py`](../../tools/bootanim.py))

**How v159 `mq_ui` plays it** (static RE, 2026-10-31; summary in [device](device.md#firmware-updates-the-ota-package)):
- Setup at `0x438c40`: `lv_anim_init`, `var` = the image, `exec_cb` = `0x437cb0`, `ready_cb` = `0x4389bc` (`anim_end_cb`), start 0, end **96**, time **3000 ms**, repeat count 1, default linear path.
- `0x437cb0`: `if (v < 96) snprintf(buf, 128, "%s%d.jpg", "D:/usr/project/config/img/start_up/", v); lv_img_set_src(img, buf)`. So frames **0–95** are shown, ~32 fps. `96.jpg` is never shown. The animation is time-based: a slow decode drops frames, it never stretches the 3 s.
- `anim_end_cb` deletes the animation and the image at once and moves on to the main UI. There's no hold after frame 95, so the last frames are the "hold" of the design.
- Decoder: no libjpeg in `mq_ui`'s `NEEDED`; the strings `_SJPG__`, `.sjpg` and the Huffman errors are LVGL 8's **sjpg** (ChaN's TJpgDec R0.03).

**Decode benchmark on the K15** (a small test program: LVGL 8.3 `tjpgd.c` with its own `tjpgdcnf.h`, static musl, decoding into an RGB888 buffer, best of 5, run from `/usr/data/tmp` and removed afterwards): about **10 ms + 0.27 ms per KB**.

| Frame | Size | Decode |
|---|---|---|
| stock 0 / 20 / 60 | 4.5 / 18.4 / 18.8 KB | 10.6 / 17.6 / 17.7 ms |
| an earlier custom animation, frame 20 (flashed, played fine) | 20.9 KB | 15.6 ms |
| ours, capped (melt/demons/hellfire/terminal) | 16.7–33.6 KB | 15.8–20.2 ms |
| terminal uncapped q75 / q85 / q95 | 48 / 61 / 97 KB | 24.3 / 27.6 / 35.8 ms |

The frame slot is 31.25 ms, and LVGL still has to blit after decoding. So `bootanim.py` writes baseline 4:2:0 JPEG at q85 and steps the quality down by 5 (floor 50) until a frame is ≤ 32 KB, which is within ~2.5 ms of stock.

**Renderer:** `bootanim.py render <name> <outdir> [--stock <dir>]` (numpy + PIL), seeded and deterministic. It runs in `doomfw-builder` from `rootfs-stage.sh` (`PYTHONDONTWRITEBYTECODE=1`, so no root-owned `__pycache__` ends up in the tree). It writes only the frames that differ from stock, and 96 as a copy of 95, so the file set stays the stock one (97 files, `0755 0:0`). The only artwork it uses is the public-domain wordmark (via `make_icon.load_subpaths`) and the public-domain 8×8 font; `demons` sets its caption in Ubuntu Sans (from `fonts-ubuntu` in the image).

| Name | Timeline (frame @ 32 fps) |
|---|---|
| `melt` (default) | 0–35 stock unchanged (FiiO logo forms by ~12). From 36: DOOM's `wipe_doMelt` on frozen stock frame 35: 160 columns, `y[0] = -(rand%16)`, neighbors ±1 clamped to [-15, 0], `dy = y<16 ? y+1 : 8` per tic in 200-line units, one tic per frame. Done by ~77; title card (logo with breathing glow, caption, rising sparks) holds to 95 |
| `hellfire` | PSX fire (37-color palette, 4-px cells, one row per step, 3 steps per frame while igniting, 0–11); logo rises 12–36 (ease-out, fade 12–26, glow flickers with fire heat); caption plate 34–40; caption types on 39–60 with a block cursor |
| `demons` | Red smoke (2 parallax noise layers) and 3,200 drifting embers fade in 0–14; 4,200 embers (70 % letter outline, 30 % fill) fly to the wordmark 6–46 and outline it; the solid logo fades in 30–44; the caption fades in 44–60 while its tracking closes from 16 to 4 px |
| `terminal` | CRT power-on 0–5; header types 6 chars/frame; DOOM's `*_Init` lines one per 3 frames, `R_Init` dots, `K15: … OK` (console done by 49); divider grows 45–51; logo slams in 53–57 (1.9×→1.0×), impact at 57 with a red flash and screen shake; "K15 EDITION" types from 59; `> RIP AND TEAR` types 65–79, then the cursor blinks |

Verified: all four render identically in shape on the host (numpy 2.x, Pillow 13) and in `doomfw-rootfs` (numpy 1.26, Pillow 10.2), and `hellfire` is byte-identical. Full builds: `--animation melt` → `K15_update_20261031_1538_v160.zip` (rootfs MD5 `2a077c4cc3b69352e314286a9d4322a1`, 102,793,216 B, content diff 71 changed, PASS); `--animation stock` → 10 changed, PASS. An unknown name is refused before anything runs. **Flashed and checked on a K15** (after the host renders above): the on-device look and the frame pacing were confirmed by the tester for all four animations.

## 7. Safety rules carried over

- Every flash needs explicit tester sign-off; never ship `recovery_version` > 4.
- Never receive on `/player`/`/ui`. Only send messages the stock UI itself sends.
- The input switch indirectly sends an MCU SET via `mq_player` (same as a user tap), already approved for this feature. Don't send other new `/player` commands without sign-off.
- For dev iterations prefer the bind-mount trick (§1.3) over reflashing; reboot = 100% stock.
- Rollback package: `tools/pkg_build.py --stock <extracted official zip> --rootfs <its rootfs.squashfs>` (v160 "restore").

## 8. Open items

The stages after the first flash (flash test checklist, knob carousel, USB keyboard/gamepad, menu rework) are recorded in §11 and §12.

- ✅ **Flashed** 2026-10-31, see §10.
- ✅ *Fixed in Stage 2 (§5, full view only).* ~~**`screenblocks` ≤ 10 is broken on the widescreen build.**~~ The widescreen patch makes `ST_Drawer` return early (we draw our own HUD), so with any `screenblocks` < 11 the vanilla status-bar area under the shrunk 3D view is never drawn. Old framebuffer content shows through (seen: a strip of the shareware credit screen). It can be reached in game via *Options → Screen Size* or the `-` key (a USB keyboard, later). Fix: clamp `screenblocks` to 11 when `WIDE_DELTA` (in `R_SetViewSize`/`M_SizeDisplay` and on config load), or draw the status bar centered.
- 💡 **Save names must be typed** (the tester used "Y"). Without a keyboard that's awkward: pre-fill the name (e.g. `MAP07 00:43`) when entering save-string edit.
- ✅ *Done in Stage 2:* the Others row is opt-in. **Tester preference:** drop the *Settings → Others → DOOM* row. DOOM belongs under sources (source tile and, later, the knob carousel). That makes the `others` hook optional in `patch_mq_ui.py`/`patch_json.py`.
- Front-panel keys / IR remote: see §10 test 16. The IR remote gives no key events at all, only scancodes.
- ✅ Volume-knob carousel (§1.7) and USB keyboard/gamepad (§5.1) done in Stage 2.
- ✅ Home menu rework, settings, layouts, rebinding, cheat menu, FPS done in Stage 3 (§5.2, §12). Still open: IR remote `MSC_SCAN` mapping; pre-filled save names; a USB hub (stick + keyboard + pad at once) is untested; D-input pads untested.
- ✅ *Flashed and checked on a K15:* DOOM boot animations (`--animation`, §6.1).

## 9. Test record (2026-10-30, all from `/usr/data` + PoC bind mounts, no flash)

| Test | Result |
|---|---|
| Builder on the official `K15_update_20261012_v159.zip` (md5 `a28bf3bc…`) | input PASS, mq_ui stock, patched `24d59267…`, CONTENT-DIFF OK (12), package PASS |
| Launch via adb (= hook path), input USB | `switched input 1 -> 8`, home menu on fb page 2 |
| UDP: Start → list → Play doom1 → in game, FIRE (ammo 50 → 48) | ✅ |
| DOOM menu Quit → back to the WAD list | ✅ same PID (`execv`), grabbed fds 4/5/6 inherited |
| `default.cfg` written, reloaded (`key_fire 163`), saves per IWAD | ✅ |
| Menu EXIT | ✅ pan 0,0, `restored input 1` |
| `kill -9 doomk15` | ✅ launcher: rc=137, pan 0,0, input restored, lock removed |
| Tester: DOOM tile with SVG icon, Others → DOOM, touch menu, ESC hold 3 s | ✅ "no issues" (`ESC held 3024 ms`) |
| Tester: stick with DOOM2.WAD → list within 2 s, header `USB: /tmp/udisk1`, INSTALL | ✅ |
| Tester: paused Local Play song → DOOM has sound → input back to Local Play | ✅ 8→7→8 bounce |
| DOOM II soak, 8 big maps + held 360° turns | ✅ no errors |
| `k15.conf` allow-list (`LD_PRELOAD` ignored), `-k15overlay` cfg (screenblocks 8 for one run) | ✅ |
| Host harness (now `engine/tools/harness/`): No-WAD, Options, PWAD without IWAD, delete modal | ✅ rendered |

## 10. Flash record (2026-10-31)

Package `out/K15_update_20261030_v160.zip` (doom-fw `ec26508-dirty`), flashed by the tester from a USB stick via *Settings → Others → FW update*, with explicit sign-off.

| Item | Before | After |
|---|---|---|
| `/etc/homebrew_version` | an earlier homebrew build, BASE v159 | `HOMEBREW=doom`, BASE v159, `DOOMFW=ec26508-dirty` ✅ |
| rootfs MD5 (`head -c 102760448 /dev/mtdblock_bbt_ro2`) | `7d967198…` | `44c1cd5663eac0e2cc2f57de1c8085ef` ✅ |
| `/usr/bin/mq_ui` | `d54a21b5…` (PoC bind mount) | `24d5926781483fd5fd6aded09ad60053`, no bind mounts ✅ |
| `/usr/bin/doomk15` | — | `db6fa03831022cfa31a000fa2b755df4` ✅ |
| adb / `/usr/data/disableadb` | works / absent | works / absent ✅ |
| `/usr/data` free | 40.2 MiB | 47.4 MiB after deleting PoC leftovers (`doom/poc/`, `doomk15.poc`) |

The flash only rewrites the `rootfs` MTD slot (128 MiB; the image is 98 MiB). `/usr/data` (UBIFS, 67.6 MiB) is untouched: WADs, `default.cfg` and saves all survived. `/` always reads 100 % used because it's a read-only squashfs. The dev binary was renamed to `/usr/data/doom/doomk15.dev` before the flash, so the firmware's own binary was tested.

| # | Test | Result | Evidence |
|---|---|---|---|
| 1 | Boot, stock gold animation, UI responsive | ✅ | no new `process_failed.txt` entries |
| 2 | Settings → Others 8th row "DOOM" + logo; other rows OK | ✅ | tester |
| 3 | Source grid: DOOM tile in cell 16 on every input | ✅ | tester |
| 4 | DOOM menu, no "DEV BUILD" | ✅ | `launch: … bin=/usr/bin/doomk15` |
| 5 | USB input → doom1 with sound → Quit → EXIT → back on USB | ✅ | `switched input 1 -> 8` … `restored input 1` |
| 6 | Hold touch ESC 3 s → K15 UI | ✅ | `ESC held 3003 ms, quitting to the stock UI` |
| 7 | Settings → Others → DOOM | ✅ | `launch.log` (tester wants this row removed, §8) |
| 8 | Paused Local Play → DOOM sound → back on Local Play | ✅ | `hw:0,4 busy … switched input 8 -> 7` … `restored input 8` |
| 9 | Setting + save survive a **real reboot** | ✅ | `show_messages 0`, `savegame/doom2.wad/doomsav0.dsg`; slot loads after `reboot` over adb (the power button only puts it in standby, so uptime didn't reset) |
| 10 | Installed DOOM2.WAD survives the flash, plays | ✅ | `ls wads/` |
| 11 | Install `spare.wad` → DELETE → NO → DELETE → YES | ✅ | tester |
| 12 | `wads/` renamed, no stick → "NO WAD FOUND" → stick inserted → list | ✅ | renamed back afterwards |
| 13 | Stick `doom.cfg` + `k15.conf` | ⚠️ | conf + overlay applied (`-k15overlay /tmp/udisk1/doom.cfg`, `K15_MUSIC_GAIN=1500` louder). `screenblocks 10` exposed the status-bar bug (§8); the saved `default.cfg` kept `screenblocks 11` |
| 14 | Stock features (USB DAC, Local Play, BT/streaming, knob, EQ) | ✅ | deferred at first, then checked by the tester after the later flashes (USB DAC, Local Play and knob in §12; BT, streaming and EQ afterwards) |
| 15 | Double-tap DOOM tile | ✅ | never two instances; the launcher's "already running" guard didn't fire (the second tap seems to land in DOOM's own menu, giving short back-to-back runs) |
| 16 | Front keys / IR remote in DOOM | ℹ️ | see below |

**Test 16, input devices seen by `doomk15`** (`K15_DEBUG=1` logs `ev devN type T code C val V`):

- `x2000_key` (front panel): only **two** front-panel buttons reach Linux, probably the *menu* button and the *volume knob push*. Codes **249** (unmapped, logged) and **258** (`BTN_2` → USE). Code **254** was seen earlier with no attribution. Knob rotation and the other keys go via the MCU.
- `gpio_ir_recv` (IR remote): **no `EV_KEY` at all**. Its kernel keymap advertises 16 ordinary keys (103/108/105/106 arrows, 114/115 volume, 116 power, …), but the FiiO remote's scancodes don't match it, so only `EV_MSC/MSC_SCAN` events arrive. NEC address `0xBEBB`, commands `0x00`–`0x0F` (16 buttons). Tentative labels, in the order pressed: vol+ `05`, vol− `03`, EQ◀ `06`, EQ▶ `02`, mute `04`, prev `07`, play/pause `08`, next `09`, mode▲ `0A`, mode▼ `0D`, light `0B`, fn1 `0E`, fn2 `0C`, fn3 `0F`; `01` and `00` were not labeled. To use the remote in DOOM, map `MSC_SCAN` values in `k15_plat.c`.

## 11. Stage 2 record (2026-10-31): knob carousel, keyboard/gamepad, two flashes

Everything was proven first without flashing (bind-mounted `mq_ui`, dev override `/usr/data/doom/doomk15`), then built, flashed with explicit sign-off, and checked again. The tester did the hands-on tests; logs were read over adb.

**PoC, no flash:**

| # | Test | Result | Evidence |
|---|---|---|---|
| K1 | Carousel shows DOOM between LOCAL PLAY and the first input, wraps both ways | ✅ | tester |
| K2 | Normal input switching by knob push | ✅ | `[UI SEND]: 0657000C0005/6/8/1` |
| K3 | Knob push on DOOM launches DOOM | ✅ | `launch: input=1 bin=/usr/bin/doomk15` |
| K4 | Waiting 3 s on DOOM launches DOOM (kept, like inputs) | ✅ | `launch.log` |
| K5 | Tap on the center label launches DOOM | ✅ | tester |
| K6 | `cur_input` never 8 | ✅ | no `0657…0009`; no `mq_ui` restart after the PoC kill |
| U1 | Home menu: arrows/WS, Enter/Space, Esc/Backspace | ✅ | tester |
| U2 | In game: arrows/WASD, Ctrl, Space, Shift, 1–7, `[`/`]`, Tab | ✅ | tester |
| U3 | `iddqd` with the WASD remap active | ✅ | tester |
| U4 | Save name with W/A/S/D | ✅ | tester |
| U5 | Dongle unplugged/replugged mid-game | ✅ | `input device … gone`, re-grab |
| U6 | Hold Esc 3 s → K15 UI | ✅ | tester |
| P1 | Home menu with the pad (D-pad/stick, A, B) | ✅ | tester |
| P2 | Pad plugged in mid-game: LED "connected", works | ✅ | `XInput pad /dev/bus/usb/001/014 intf 0 IN 0x84 OUT 0x5 (32 B)`; after a replug as `001/016` |
| P3 | Pad in game (analog move/turn, fire, use, weapons, run, automap, D-pad); speed "ok" | ✅ | tester |
| P4 | Start → DOOM menu; nav, A, B; hold Start 3 s → K15 UI | ✅ | `Esc/Start held 3010 ms` |
| P5 | Touch controls hide on key/pad, return on touch (that touch swallowed) | ✅ | tester |
| P6 | View stays full size (`-`, Options → Screen Size) | ✅ | `screenblocks 11` in `default.cfg` |

Found and fixed during these tests: the **wired** Keychron's `event4` (generic HID collection, all ABS axes) was classified as a touchscreen, by the old code too, so its keys were dropped and its ABS events could disturb the touch slots (fixed by the `INPUT_PROP_DIRECT`/`BTN_TOUCH` rule, §5.1). The "not entirely fullscreen" view the tester saw was the old `screenblocks` bug, now reachable with a keyboard (fixed, §5). Unplugging the pad logs one harmless `XInput submit failed (No such device)` from a rescan that still saw the old sysfs entry.

**Flash A** `K15_update_20261031_v160.zip` (doom-fw `a1b50e3-dirty`, hooks `others,sources,knob`): rootfs `102768640` B / `67b0e40b21cac3ff0b93e4a3817347c4` read back from `/dev/mtdblock_bbt_ro2` ✅, `mq_ui` `b0bfba8c…` ✅, `doomk15` `00fc8c0f346a695e11bd961f048d1bd4` ✅, no bind mounts, adb fine, no new restarts. Tile, Others row and carousel launch DOOM without "DEV BUILD"; keyboard + pad hotplug work; stock playback and the volume knob fine. The tester pointed out that the Others row should be gone (the Stage 1 preference), so:

**Flash B** `K15_update_20261031_1346_v160.zip` (first build with the time in the name; zip MD5 `850679af328c4b169f8e15d0799da4d1`, hooks `sources,knob`, `BUILT=2026-10-31T13:46:22Z`):

| Item | Value |
|---|---|
| rootfs (`head -c 102768640 /dev/mtdblock_bbt_ro2`) | `575635d8b99c356d0702bf970453bb81` ✅ |
| `/usr/bin/mq_ui` | `a9bbbc9a15e5e72b37f7e5fef47ee3b3` ✅ (bind-mount sanity run before the build) |
| `/usr/bin/doomk15` | `00fc8c0f346a695e11bd961f048d1bd4` ✅ |
| content diff | 10 paths, `CONTENT-DIFF: OK`; `pkg_verify` `RESULT: PASS` on input and output |
| bind mounts / adb / `process_failed.txt` | none / works / no new entries ✅ |
| Settings → Others | 7 stock rows, all working ✅ |
| Carousel + tile launch DOOM; keyboard/pad | ✅ |

The dev override is parked as `/usr/data/doom/doomk15.dev` (same source as the firmware binary). The PoC copies in `/usr/data/doom/poc/` were deleted afterwards (`/usr/data`: 47.0 MiB free).

## 12. Stage 3 record (2026-10-31): home menu, options, one flash

Design in §5.2. Visuals were iterated with the host harness, then on the device with the dev override, then flashed with explicit sign-off. The tester did the hands-on tests; logs were read over adb.

Design decisions taken with the tester before coding: keyboard bindings live in `k15.conf` as extra keys (not DOOM's single `key_*`); pad buttons cycle through actions; the cheat menu is long-press MAP / hold pad BACK with IDDQD, IDKFA, IDFA, IDCLIP, IDDT and a WARP picker; quitting a PLAY-with-default game returns to HOME; the harness moves into the repo; LAYOUT gets a full-screen preview.

**Dev override, no flash:**

| # | Test | Result | Evidence |
|---|---|---|---|
| 1 | Home 2×2 grid with icons | ✅ after tweak | tester: "a few pixels wider, font a bit smaller" → buttons 312 px, text scale 3 |
| 2 | ☆ default on DOOM2 + PLAY sub-label | ✅ | `k15: saved /usr/data/doom/k15.conf` |
| 3 | PLAY starts the default directly; Quit → HOME | ✅ | tester |
| 4 | Layout presets + PREVIEW (touch, flash, tap to return, ESC hold) | ✅ after tweak | tester: merge PRESET and PREVIEW into one row; "opacity" read as transparency → `K15_TRANSPARENCY` |
| 5 | In-game controls follow the preset | ✅ | tester |
| 8 | Cheat menu: pause, IDDQD, WARP (unpaused), short MAP = automap, CLOSE / tap outside | ✅ | `k15: cheat idspispopd`, `iddqd`, `idclev15`, `idclev16` |
| 9 | FPS counter | ✅ after tweak | first tiny and centered → bottom-right plate with tall digits; screenshot checked |
| 10 | Keychron: rebind FIRE to F; arrows/Ctrl/WASD; `iddqd` | ✅ | tester |
| 11 | 8BitDo: Y = FIRE; hold BACK → cheat menu (D-pad, A, B); short BACK = automap | ✅ | tester |
| 12 | Directional focus in grid, WAD rows, settings rows | ✅ | tester |
| 13 | CONTROLS tiles: pad only / Keychron only / nothing | ✅ after fix | the 8BitDo's keyboard node counted as a keyboard → VID:PID rule (§5.2); KEYBOARD tile always enabled |

Lesson: pushing a new dev binary while DOOM runs isn't enough. Quitting a game `execv`s `/proc/self/exe`, which is still the old, replaced inode. Check `pidof doomk15` is empty before pushing.

**Flash** `K15_update_20261031_1500_v160.zip` (doom-fw `e03f298-dirty`, hooks `sources,knob`, `BUILT=2026-10-31T15:00:30Z`), with explicit sign-off:

| Item | Value |
|---|---|
| zip | 113,196,000 B, MD5 `d823b5583f3ae31961f2d1aafa65451a` |
| rootfs (`head -c 102780928 /dev/mtdblock_bbt_ro2`) | `54077350452ee88c82dd7cd745d2445d` ✅ |
| `/usr/bin/mq_ui` | `a9bbbc9a15e5e72b37f7e5fef47ee3b3` ✅ (unchanged since Stage 2) |
| `/usr/bin/doomk15` | `19fb8cd2b42fe910bcf0fc3b9374502e` ✅ |
| content diff / packages | 10 paths, `CONTENT-DIFF: OK`; `RESULT: PASS` on input and output; `recovery_version=4` |
| bind mounts / adb / `process_failed.txt` | none / works / no new entries ✅ |
| `/usr/data` | `k15.conf` (39 keys), WADs, saves survived ✅ |

After the flash (dev override parked as `doomk15.dev`): version tag without DEV BUILD and the default WAD survived ✅; PLAY → saved layout/FPS in game → quit to HOME ✅; knob carousel launches DOOM ✅ (`launch: … bin=/usr/bin/doomk15`); cheat menu ✅; stock USB DAC / Local Play / volume knob ✅.
