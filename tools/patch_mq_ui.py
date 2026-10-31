#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Patch FiiO K15 v159 mq_ui so extra JSON list items run a shell command.

Two hooks, both on lists built by the generic builder 0x43c7f4, whose click
callbacks map the tapped object to an index and ignore indices past a hard-coded
jump table:

  others   Settings -> Others (other_set.json). cb 0x433934:
           `sltiu v1,v0,7; beqz v1,0x433960` -> extra row index 7.
  sources  Swipe-right source grid (right_input_mode.json, 8x2 grid, 15 stock
           items, 16th cell free). cb 0x42a880:
           `sltiu v1,v0,15; beqz v1,0x42a8ac` -> extra tile index 15.

Each hook's "index too big" branch is redirected to a small inter-function zero
gap holding `j <stub>`. The stubs live in the 208 zero bytes of slack at the end
of the R+E PT_LOAD (0x789f30..0x78a000, right after .plt). PT_LOAD[0] is
extended to cover them. Stub: `if (idx == N) system(CMD);` then the callback's
normal epilogue. Both stubs share one command string.

The source grid greys out and un-clicks row-2 tiles (index >= 8) unless Local
Play is selected, in two loops: the restyler 0x42aba0 (`bnez s8,disable` at
0x42ada4) and the tap->index function 0x429f14 (`beqz s8,enable` at 0x42a0b4).
The sources hook redirects both branches (keeping their delay slots) to stubs
that always take the "enabled" path for tile 15, so DOOM stays tappable on any
input. v0 is dead at all those points.

  knob     Volume-knob input carousel (ui_input_mode_switch.c): a 9th entry
           "DOOM" after the 8 inputs. The highlighted index 0x8aae6c is only
           touched by the carousel code, so the next/prev/open paths are
           redirected wholesale to cave functions step(delta) / set_labels()
           working mod 9 (label 8 = "DOOM", others from labels[lang*8+i]).
           The three commit paths (3 s timer, centre-label release, knob push)
           compare idx with cur_input; that compare becomes `j stub`, which
           for idx == 8 runs CMD and continues as "unchanged input" instead
           of storing 8 in cur_input and sending 0657.
           The 208-byte cave is full, so the knob code lives in a new R+X
           PT_LOAD (the spare PT_NULL program header, index 9) for a page
           appended at file offset 0x3c0000, mapped at 0x8c0000.

Usage: patch_mq_ui.py <in mq_ui> <out mq_ui> [--hooks sources,knob] [--cmd CMD]
(default sources,knob; add "others" for the Settings -> Others row, which also needs
patch_json.py --others)
"""
import argparse
import hashlib
import struct
import sys

VA_BASE = 0x400000
SYSTEM_PLT = 0x789670
CAVE = 0x789F30
SEG_END = 0x78A000
STR_VA = 0x789FB0
DEFAULT_CMD = "/usr/bin/k15-doom-launch </dev/null >/dev/null 2>&1 &"

# Keep source tile 15 enabled: (site, stock word, stub va, "enabled" target, "disabled"
# target). At both sites s8 != 0 means "grey out" (stock: `bnez s8,dis` / `beqz s8,en`).
# Stub: li v0,15; beq s0,v0,EN; nop; bnez s8,DIS; nop; EN: j en; nop; DIS: j dis; nop.
ENABLE_PATCHES = (
    (0x42ADA4, 0x17C0FFD1, 0x789F68, 0x42ADAC, 0x42ACEC),
    (0x42A0B4, 0x13C00060, 0x789F8C, 0x42A238, 0x42A0BC),
)

# name: (branch site, stock branch word, gap for trampoline, epilogue, index, stub va, stock checks)
HOOKS = {
    "others": dict(branch=0x43397C, stock=0x1060FFF8, gap=0x433914, ret=0x433960, idx=7,
                   stub=0x789F30, checks=((0x433970, 0x0C10ED50), (0x433978, 0x2C430007))),
    "sources": dict(branch=0x42A8D8, stock=0x1060FFF4, gap=0x42E404, ret=0x42A8AC, idx=15,
                    stub=0x789F4C, checks=((0x42A8CC, 0x0C10A7C5), (0x42A8D4, 0x2C43000F),
                                           (0x42E3FC, 0x03E00008))),
}


def off(va):
    return va - VA_BASE


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def branch(op_rs_rt, pc, target):
    return op_rs_rt | (((target - (pc + 4)) >> 2) & 0xFFFF)


# ---------------------------------------------------------------- knob hook
STOCK_SIZE = 3905756
KNOB_OFF = 0x3C0000      # file offset of the new segment (64 KiB aligned, like the others)
KNOB_VA = 0x8C0000       # above the RW segment's bss end 0x8acdf8
PHDR_NULL = 9            # spare PT_NULL program header
LABEL_FMT = 0x51E214     # lv_label_set_text_fmt(obj, fmt, ...)
FMT_S = 0x633AD4         # "%s"
LABELS = 0x79A474        # char *labels[13][8]
IDX = 0x8AAE6C           # highlighted carousel index
OBJS = 0x8AAE70          # prev / centre / next label objects
CUR_INPUT = 0x8ACAA0     # current input, 0-based
LANG = 0x8ACB14          # UI language index

# (site, stock word, stock word of the following delay slot or None to keep it, stub, ...)
# Redirected paths: gesture cb 0x42e410 next/prev (epilogue 0x42e4d4), knob handler
# 0x42e9f4 open/next/prev (epilogue 0x42ea74), and the three commit compares.
KNOB_SITES = {
    "g_next": (0x42E4F8, 0x3C11008B, 0x8E24AE6C),
    "g_prev": (0x42E660, 0x8E34AE6C, 0x16800035),
    "k_open": (0x42EAF4, 0x8E020010, 0x3C03007C),
    "k_next": (0x42EEF8, 0x3C12008B, 0x8E44AE6C),
    "k_prev": (0x42EDC8, 0x3C12008B, 0x8E54AE6C),
    "c_timer": (0x42E810, 0x14620017, None),    # bne v1,v0,send; addiu a1,v0,1
    "c_release": (0x42E984, 0x14620014, None),  # bne v1,v0,send; addiu a1,v0,1
    "c_knob": (0x42ED34, 0x10620007, None),     # beq v1,v0,eq;  lui a0,0x63
}
KNOB_CHECKS = (
    (0x42E4D4, 0x8FBF0054), (0x42EA74, 0x8FBF0054),  # epilogues start with lw ra
    (0x42EBEC, 0x8E84AE64), (0x42E818, 0x2C420006), (0x42E870, 0x3C040063),
    (0x42E98C, 0x2C420006), (0x42E9D8, 0x3C040063), (0x42ED54, 0x2C420006),
    (0x42ED3C, 0x24450001),
)
# commit stub: (equal-path target, send-path target)
COMMITS = {"c_timer": (0x42E818, 0x42E870), "c_release": (0x42E98C, 0x42E9D8),
           "c_knob": (0x42ED54, 0x42ED3C)}

R = {n: i for i, n in enumerate(
    "zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra"
    .split())}


def hi(va):
    return ((va + 0x8000) >> 16) & 0xFFFF


def lo(va):
    return va & 0xFFFF


class Asm:
    """Just enough MIPS32 to write the knob stubs, with forward labels."""

    def __init__(self, base):
        self.base, self.words, self.labels, self.fix = base, [], {}, []

    @property
    def pc(self):
        return self.base + 4 * len(self.words)

    def label(self, name):
        self.labels[name] = self.pc

    def w(self, word):
        self.words.append(word & 0xFFFFFFFF)

    def i(self, op, rs, rt, imm):
        self.w(op << 26 | R[rs] << 21 | R[rt] << 16 | (imm & 0xFFFF))

    def r(self, rs, rt, rd, sa, fn):
        self.w(R[rs] << 21 | R[rt] << 16 | R[rd] << 11 | sa << 6 | fn)

    def addiu(self, rt, rs, imm): self.i(0x09, rs, rt, imm)
    def slti(self, rt, rs, imm): self.i(0x0A, rs, rt, imm)
    def lui(self, rt, imm): self.i(0x0F, "zero", rt, imm)
    def lw(self, rt, off, rs): self.i(0x23, rs, rt, off)
    def sw(self, rt, off, rs): self.i(0x2B, rs, rt, off)
    def li(self, rt, imm): self.addiu(rt, "zero", imm)
    def addu(self, rd, rs, rt): self.r(rs, rt, rd, 0, 0x21)
    def move(self, rd, rs): self.r(rs, "zero", rd, 0, 0x25)
    def sll(self, rd, rt, sa): self.r("zero", rt, rd, sa, 0x00)
    def jr(self, rs): self.r(rs, "zero", "zero", 0, 0x08)
    def nop(self): self.w(0)
    def j(self, target): self.w(j(target))
    def jal(self, target): self.w(jal(target))

    def _br(self, op, rs, rt, target):
        self.fix.append((len(self.words), target))
        self.i(op, rs, rt, 0)

    def beq(self, rs, rt, t): self._br(0x04, rs, rt, t)
    def bne(self, rs, rt, t): self._br(0x05, rs, rt, t)
    def bgez(self, rs, t): self._br(0x01, rs, "at", t)  # REGIMM rt=1
    def b(self, t): self.beq("zero", "zero", t)

    def jl(self, name):  # j to a local label
        self.fix.append((len(self.words), ("j", name)))
        self.w(0x08000000)

    def jall(self, name):
        self.fix.append((len(self.words), ("jal", name)))
        self.w(0x0C000000)

    def finish(self):
        for idx, t in self.fix:
            pc = self.base + 4 * idx
            if isinstance(t, tuple):
                self.words[idx] |= (self.labels[t[1]] >> 2) & 0x03FFFFFF
            else:
                off = (self.labels[t] - (pc + 4)) >> 2
                assert -0x8000 <= off < 0x8000
                self.words[idx] |= off & 0xFFFF
        return b"".join(struct.pack("<I", w) for w in self.words)


def build_knob(cmd):
    """Return (code bytes at KNOB_VA, {stub name: va})."""
    a = Asm(KNOB_VA)
    ih = hi(IDX)  # every global is 0x8a_xxxx: one base register reaches all of them

    # step(a0 = +1 / -1): idx = (idx + delta) mod 9, then fall into set_labels().
    a.label("step")
    a.lui("t0", ih)
    a.lw("t1", lo(IDX), "t0")
    a.addu("t1", "t1", "a0")
    a.bgez("t1", "s_hi")
    a.nop()
    a.li("t1", 8)
    a.label("s_hi")
    a.slti("t2", "t1", 9)
    a.bne("t2", "zero", "s_st")
    a.nop()
    a.move("t1", "zero")
    a.label("s_st")
    a.sw("t1", lo(IDX), "t0")

    # set_labels(): obj[k] <- label((idx + 8 + k) mod 9), k = 0..2 (prev, centre, next).
    a.label("set_labels")
    a.addiu("sp", "sp", -40)
    a.sw("ra", 36, "sp")
    a.sw("s0", 32, "sp")
    a.sw("s1", 28, "sp")
    a.move("s0", "zero")
    a.lui("s1", ih)
    a.label("l_loop")
    a.lw("t1", lo(IDX), "s1")
    a.addiu("t1", "t1", 8)
    a.addu("t1", "t1", "s0")
    for n in ("l_m1", "l_m2"):
        a.slti("t2", "t1", 9)
        a.bne("t2", "zero", n)
        a.nop()
        a.addiu("t1", "t1", -9)
        a.label(n)
    a.slti("t2", "t1", 8)  # anything >= 8 (only 8 after the wrap) is DOOM
    a.beq("t2", "zero", "l_doom")
    a.lui("a2", 0)  # patched below: hi(DOOM string)
    lui_doom = len(a.words) - 1
    a.lw("t3", lo(LANG), "s1")
    a.sll("t3", "t3", 3)
    a.addu("t3", "t3", "t1")
    a.sll("t3", "t3", 2)
    a.lui("t4", hi(LABELS))
    a.addu("t3", "t3", "t4")
    a.b("l_set")
    a.lw("a2", lo(LABELS), "t3")
    a.label("l_doom")
    a.addiu("a2", "a2", 0)  # patched below: lo(DOOM string)
    addiu_doom = len(a.words) - 1
    a.label("l_set")
    a.sll("t5", "s0", 2)
    a.addu("t5", "t5", "s1")
    a.lw("a0", lo(OBJS), "t5")
    a.lui("a1", hi(FMT_S))
    a.jal(LABEL_FMT)
    a.addiu("a1", "a1", lo(FMT_S))
    a.addiu("s0", "s0", 1)
    a.slti("t2", "s0", 3)
    a.bne("t2", "zero", "l_loop")
    a.nop()
    a.lw("ra", 36, "sp")
    a.lw("s0", 32, "sp")
    a.lw("s1", 28, "sp")
    a.jr("ra")
    a.addiu("sp", "sp", 40)

    # Path redirects. The host function's frame is live, so jal clobbering ra is fine:
    # its epilogue reloads ra from the stack.
    for name, delta, ret in (("g_next", 1, 0x42E4D4), ("g_prev", -1, 0x42E4D4),
                             ("k_next", 1, 0x42EA74), ("k_prev", -1, 0x42EA74)):
        a.label(name)
        a.jall("step")
        a.li("a0", delta)
        a.j(ret)
        a.nop()
    # Knob opens the carousel: start at the current input (the stock code stores the
    # same value again after loading the screen) and set the labels, then continue at
    # "load screen + start 3 s timer".
    a.label("k_open")
    a.lui("t0", ih)
    a.lw("t1", lo(CUR_INPUT), "t0")
    a.jall("set_labels")
    a.sw("t1", lo(IDX), "t0")
    a.j(0x42EBEC)
    a.nop()

    # Commit: entered with v0 = idx, v1 = cur_input, the stock delay slot already run.
    for name, (eq, send) in COMMITS.items():
        a.label(name)
        a.beq("v0", "v1", name + "_eq")
        a.li("t0", 8)
        a.bne("v0", "t0", name + "_send")
        a.nop()
        a.lui("a0", 0)  # patched below: hi(cmd)
        a.fix.append((len(a.words) - 1, "CMD_HI"))
        a.jal(SYSTEM_PLT)
        a.addiu("a0", "a0", 0)
        a.fix.append((len(a.words) - 1, "CMD_LO"))
        a.lui("t0", ih)
        a.lw("v0", lo(CUR_INPUT), "t0")  # continue as "input unchanged"
        a.label(name + "_eq")
        a.j(eq)
        a.nop()
        a.label(name + "_send")
        a.j(send)
        a.nop()

    code_len = 4 * len(a.words)
    doom_va = KNOB_VA + code_len
    cmd_va = doom_va + 8
    a.words[lui_doom] |= hi(doom_va)
    a.words[addiu_doom] |= lo(doom_va)
    for idx, t in list(a.fix):
        if t in ("CMD_HI", "CMD_LO"):
            a.words[idx] |= hi(cmd_va) if t == "CMD_HI" else lo(cmd_va)
    a.fix = [f for f in a.fix if f[1] not in ("CMD_HI", "CMD_LO")]
    code = a.finish()
    blob = code + b"DOOM\0\0\0\0" + cmd
    return blob, {n: a.labels[n] for n in KNOB_SITES}


def apply_knob(d, cmd):
    """Patch bytearray d (stock size) in place and return it extended."""
    if len(d) != STOCK_SIZE:
        sys.exit(f"knob: expected stock size {STOCK_SIZE}, got {len(d)} (already patched?)")
    e_phoff, = struct.unpack_from("<I", d, 0x1C)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 0x2A)
    p = e_phoff + PHDR_NULL * e_phentsize
    if e_phnum <= PHDR_NULL or d[p:p + 28] != bytes(28) or \
            struct.unpack_from("<I", d, p + 28)[0] != 4:
        sys.exit("knob: program header 9 is not the expected empty PT_NULL")
    for va, word in KNOB_CHECKS:
        if struct.unpack_from("<I", d, off(va))[0] != word:
            sys.exit(f"knob: stock check failed at {va:#x}")
    blob, stubs = build_knob(cmd)
    for name, (site, word, slot) in KNOB_SITES.items():
        if struct.unpack_from("<I", d, off(site))[0] != word or (
                slot is not None and struct.unpack_from("<I", d, off(site) + 4)[0] != slot):
            sys.exit(f"knob: stock check failed at {site:#x} ({name})")
        struct.pack_into("<I", d, off(site), j(stubs[name]))
        if slot is not None:
            struct.pack_into("<I", d, off(site) + 4, 0)  # nop the old delay slot
    d.extend(bytes(KNOB_OFF - len(d)))
    d.extend(blob)
    # PT_LOAD R+X: type, offset, vaddr, paddr, filesz, memsz, flags, align
    struct.pack_into("<8I", d, p, 1, KNOB_OFF, KNOB_VA, KNOB_VA, len(blob), len(blob), 5,
                     0x10000)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--hooks", default="sources,knob")
    ap.add_argument("--cmd", default=DEFAULT_CMD)
    a = ap.parse_args()
    hooks = [h for h in a.hooks.split(",") if h]
    if not hooks or any(h not in HOOKS and h != "knob" for h in hooks):
        sys.exit(f"--hooks must be a subset of {','.join(HOOKS)},knob")
    knob = "knob" in hooks
    hooks = [h for h in hooks if h != "knob"]
    cmd = a.cmd.encode() + b"\0"
    if hooks and STR_VA + len(cmd) > SEG_END:
        sys.exit("command too long")

    d = bytearray(open(a.src, "rb").read())
    if knob and len(d) != STOCK_SIZE:
        sys.exit(f"knob: expected stock size {STOCK_SIZE}, got {len(d)} (already patched?)")

    def r32(va):
        return struct.unpack_from("<I", d, off(va))[0]

    def w32(va, val):
        struct.pack_into("<I", d, off(va), val)

    if any(d[off(CAVE):off(SEG_END)]):
        sys.exit("cave not empty (already patched?)")
    for h in hooks:
        k = HOOKS[h]
        for va, word in k["checks"] + ((k["branch"], k["stock"]),):
            if r32(va) != word:
                sys.exit(f"{h}: stock check failed at {va:#x} - not stock v159 mq_ui?")
        if any(d[off(k["gap"]):off(k["gap"]) + 8]):
            sys.exit(f"{h}: gap at {k['gap']:#x} not empty")

    e_phoff, = struct.unpack_from("<I", d, 0x1C)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 0x2A)
    for i in range(e_phnum):
        if not hooks:
            break
        p = e_phoff + i * e_phentsize
        p_type, _, p_vaddr, _, p_filesz, p_memsz, p_flags = struct.unpack_from("<7I", d, p)
        if p_type == 1 and p_vaddr == VA_BASE:
            if p_filesz != 0x389F30 or p_memsz != 0x389F30 or p_flags != 5:
                sys.exit("unexpected LOAD[0] header")
            struct.pack_into("<II", d, p + 16, SEG_END - VA_BASE, SEG_END - VA_BASE)
            break
    else:
        if hooks:
            sys.exit("LOAD[0] not found")

    if "sources" in hooks:
        for site, word, _, _, _ in ENABLE_PATCHES:
            if r32(site) != word:
                sys.exit(f"sources: stock check failed at {site:#x}")
        for site, _, st, en, dis in ENABLE_PATCHES:
            w32(site, j(st))  # delay slot (move a0,s1) kept
            b_s8 = branch(0x17C00000, st + 12, st + 28)  # bnez s8,DIS
            code = [
                0x2402000F,                                 # li   v0,15
                branch(0x12020000, st + 4, st + 20),        # beq  s0,v0,EN
                0,
                b_s8,
                0,
                j(en), 0,                                   # EN
                j(dis), 0,                                  # DIS
            ]
            assert st + 4 * len(code) <= STR_VA
            for i, w in enumerate(code):
                w32(st + 4 * i, w)

    for h in hooks:
        k = HOOKS[h]
        s = k["stub"]
        w32(k["branch"], branch(0x10600000, k["branch"], k["gap"]))  # beqz v1,gap
        w32(k["gap"], j(s))
        w32(k["gap"] + 4, 0)
        stub = [
            0x24030000 | k["idx"],                 # li    v1,idx
            branch(0x14430000, s + 4, s + 0x14),   # bne   v0,v1,ret
            0x3C040000 | hi(STR_VA),                     # lui   a0,hi(cmd)   (delay slot)
            jal(SYSTEM_PLT),                       # jal   system
            0x24840000 | lo(STR_VA),                     # addiu a0,a0,lo(cmd)
            j(k["ret"]),                           # ret: j epilogue
            0,                                     # nop
        ]
        assert s + 4 * len(stub) <= STR_VA
        for i, word in enumerate(stub):
            w32(s + 4 * i, word)
    if hooks:
        d[off(STR_VA):off(STR_VA) + len(cmd)] = cmd
    if knob:
        d = apply_knob(d, cmd)
        hooks.append("knob")

    open(a.dst, "wb").write(d)
    print("patched", a.dst, hashlib.md5(d).hexdigest(), "hooks:", ",".join(hooks), "cmd:", a.cmd)


if __name__ == "__main__":
    main()
