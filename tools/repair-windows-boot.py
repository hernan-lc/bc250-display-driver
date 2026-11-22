#!/usr/bin/env python3
"""
repair-windows-boot.py -- offline recovery for the BC-250 WDDM boot/black-screen loop.

Run from Linux with the Windows partition mounted read-write. It patches the
SYSTEM hive (and, if present, the BCD store) so that the experimental
BC250Kmd driver cannot be loaded at the next boot and the built-in Microsoft
Basic Display Adapter can take over.

What it changes (all idempotent):
  * Services\\BC250Kmd\\Start           -> 4 (SERVICE_DISABLED)
  * Services\\BC250Display\\Start       -> 3 (SERVICE_DEMAND_START)
  * Control\\Class\\{4d36...}\\*\\UserModeDriverName(Wow)
                                        -> emptied (kills the non-existent
                                           bc250umd.dll registration)
  * Control\\SystemStartOptions         -> removes the NOVGA token
  * BCD store                           -> removes NOVGA/novga if present
  * Services\\BasicDisplay/BasicRender  -> Start 1 (SYSTEM_START)

Safety: makes a timestamped backup of every hive it modifies and recomputes
the hive checksum. Use --dry-run first, --apply to write.

Requires: python-registry   (pip install python-registry)
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import shutil
import struct
import sys

try:
    from Registry import Registry
except ImportError:
    sys.exit("python-registry is required: pip install python-registry")

CLASS_GUID = "{4d36e968-e325-11ce-bfc1-08002be10318}"
DISPLAY_CLASS = ["Control", "Class", CLASS_GUID]


def log(msg: str) -> None:
    print(msg, flush=True)


def backup(path: str, tag: str) -> str:
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    dst = f"{path}.bc250-backup-{stamp}"
    shutil.copy2(path, dst)
    log(f"  backup -> {dst}")
    return dst


def value_abs_offset(v) -> int:
    """Absolute file offset of a value's data bytes (python-registry internals)."""
    vk = v._vkrecord
    raw_len = vk.raw_data_length()
    if raw_len < 5 or raw_len >= 0x80000000:
        return vk.absolute_offset(0x8)  # inline data (DWORD, small values)
    # Data lives in an HBIN cell: +4 skips the cell size header.
    return vk.abs_offset_from_hbin_offset(vk.unpack_dword(0x8)) + 4


def fix_checksum(buf: bytearray) -> None:
    xsum = 0
    for off in range(0, 0x1FC, 4):
        xsum ^= struct.unpack_from("<I", buf, off)[0]
    if xsum == 0:
        xsum = 1
    if xsum == 0xFFFFFFFF:
        xsum = 0xFFFFFFFE
    struct.pack_into("<I", buf, 0x1FC, xsum)


def set_dword(buf: bytearray, v, value: int, what: str, apply: bool) -> bool:
    if v.value() == value:
        log(f"  [ok] {what} = {value} (already)")
        return False
    off = value_abs_offset(v)
    log(f"  [fix] {what}: {v.value()} -> {value} @0x{off:x}")
    if apply:
        struct.pack_into("<I", buf, off, value)
    return True


def clear_value(buf: bytearray, v, what: str, apply: bool) -> bool:
    off = value_abs_offset(v)
    length = v._vkrecord.data_length()
    if not any(v.raw_data()):
        log(f"  [ok] {what} already empty")
        return False
    log(f"  [fix] {what}: cleared {length} bytes @0x{off:x} (was {v.value()!r})")
    if apply:
        buf[off : off + length] = b"\x00" * length
    return True


def strip_token(buf: bytearray, v, token: str, what: str, apply: bool) -> bool:
    raw = v.raw_data()
    s = raw.decode("utf-16-le", "ignore") if v.value_type() == 1 else raw.decode("ascii", "ignore")
    if token.lower() not in s.lower():
        log(f"  [ok] {what}: token {token} not present")
        return False
    log(f"  [fix] {what}: removing {token}")
    if apply:
        # Same byte length: overwrite the token with spaces (UTF-16 or ASCII).
        pat = token.encode("utf-16-le") if v.value_type() == 1 else token.encode("ascii")
        repl = b" \x00" * len(token) if v.value_type() == 1 else b" " * len(token)
        off = value_abs_offset(v)
        idx = bytes(buf[off : off + len(raw)]).find(pat)
        if idx >= 0:
            buf[off + idx : off + idx + len(pat)] = repl
        else:
            log("  [warn] token bytes not found at value offset")
            return False
    return True


def open_key(reg, base, parts):
    k = base
    for p in parts:
        k = k.subkey(p)
    return k


def try_key(reg, base, parts):
    try:
        return open_key(reg, base, parts)
    except Exception:
        return None


def patch_system(path: str, apply: bool) -> bool:
    log(f"\n=== SYSTEM hive: {path} ===")
    reg = Registry.Registry(path)
    with open(path, "rb") as fh:
        buf = bytearray(fh.read())

    changed = False
    control_sets = [k for k in reg.root().subkeys() if k.name().startswith("ControlSet")]
    log(f"  control sets: {[k.name() for k in control_sets]}")

    for cs in control_sets:
        log(f"  [{cs.name()}]")
        svc = try_key(reg, cs, ["Services"])
        if svc:
            for name, start in (("BC250Kmd", 4), ("BC250Display", 3),
                                ("BasicDisplay", 1), ("BasicRender", 1)):
                k = try_key(reg, svc, [name])
                if k:
                    try:
                        v = k.value("Start")
                    except Exception:
                        v = None
                    if v is not None:
                        changed |= set_dword(buf, v, start, f"{cs.name()}\\Services\\{name}\\Start", apply)

        # Kill the fake user-mode driver registration in every display-class instance.
        cls = try_key(reg, cs, DISPLAY_CLASS)
        if cls:
            for inst in cls.subkeys():
                for vname in ("UserModeDriverName", "UserModeDriverNameWow"):
                    try:
                        v = inst.value(vname)
                    except Exception:
                        continue
                    val = v.value()
                    text = " ".join(val) if isinstance(val, list) else str(val)
                    if "bc250umd" in text.lower():
                        changed |= clear_value(buf, v, f"{inst.name()}\\{vname}", apply)

        # NOVGA prevents the Basic Display Adapter fallback.
        control = try_key(reg, cs, ["Control"])
        if control:
            try:
                v = control.value("SystemStartOptions")
            except Exception:
                v = None
            if v is not None:
                changed |= strip_token(buf, v, "NOVGA", "SystemStartOptions", apply)

    if changed and apply:
        fix_checksum(buf)
        backup(path, "system")
        with open(path, "wb") as fh:
            fh.write(buf)
        log("  SYSTEM hive written (checksum updated)")
    elif not changed:
        log("  nothing to do")
    return changed


def patch_bcd(path: str, apply: bool) -> bool:
    log(f"\n=== BCD store: {path} ===")
    if not os.path.exists(path):
        log("  not found, skipping")
        return False
    with open(path, "rb") as fh:
        buf = bytearray(fh.read())
    changed = False
    for tok in ("NOVGA", "novga"):
        for enc in ("utf-16-le", "ascii"):
            pat = tok.encode(enc)
            idx = bytes(buf).find(pat)
            while idx >= 0:
                log(f"  [fix] removing {tok} ({enc}) @0x{idx:x}")
                if apply:
                    buf[idx : idx + len(pat)] = (
                        b" \x00" * len(tok) if enc == "utf-16-le" else b" " * len(tok)
                    )
                changed = True
                idx = bytes(buf).find(pat, idx + len(pat))
    if changed and apply:
        fix_checksum(buf)
        backup(path, "bcd")
        with open(path, "wb") as fh:
            fh.write(buf)
        log("  BCD written (checksum updated)")
    elif not changed:
        log("  no NOVGA token in BCD")
    return changed


def verify(path: str) -> None:
    log(f"\n=== verification: {path} ===")
    reg = Registry.Registry(path)
    for cs in [k for k in reg.root().subkeys() if k.name().startswith("ControlSet")]:
        for name in ("BC250Kmd", "BC250Display", "BasicDisplay"):
            k = try_key(reg, cs, ["Services", name])
            if k:
                log(f"  {cs.name()}\\Services\\{name}\\Start = {k.value('Start').value()}")
        control = try_key(reg, cs, ["Control"])
        if control:
            log(f"  {cs.name()}\\Control\\SystemStartOptions = {control.value('SystemStartOptions').value()!r}")
        cls = try_key(reg, cs, DISPLAY_CLASS)
        if cls:
            for inst in cls.subkeys():
                for vname in ("UserModeDriverName", "UserModeDriverNameWow"):
                    try:
                        log(f"  {inst.name()}\\{vname} = {inst.value(vname).value()!r}")
                    except Exception:
                        pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--windows-dir", default="/run/media/meme/WINDOWS",
                    help="mount point of the Windows partition")
    ap.add_argument("--esp-dir", default="/run/media/meme/SYSTEM",
                    help="mount point of the EFI System Partition (optional)")
    ap.add_argument("--apply", action="store_true", help="actually write changes")
    ap.add_argument("--verify-only", action="store_true", help="only print current state")
    args = ap.parse_args()

    system = os.path.join(args.windows_dir, "Windows", "System32", "config", "SYSTEM")
    bcd = os.path.join(args.esp_dir, "EFI", "Microsoft", "Boot", "BCD")

    if args.verify_only:
        verify(system)
        return 0

    if not os.path.exists(system):
        sys.exit(f"SYSTEM hive not found: {system}")
    if not args.apply:
        log("*** DRY RUN (pass --apply to write) ***")

    patch_system(system, args.apply)
    patch_bcd(bcd, args.apply)
    verify(system)
    if args.apply:
        log("\nDone. Reboot Windows; it should come up with the Microsoft Basic Display Adapter.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
