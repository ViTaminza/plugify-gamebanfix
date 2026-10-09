#!/usr/bin/env python3
"""Scan a game binary for the raw byte patterns of this plugin set's gamedata.

Reads the binary as bytes only (never executes/interprets it). Run with:
    python3 -I plugins/gamebanfix/tools/scan_signatures.py [binary]

The patterns are the linuxsteamrt64 ones of
plugins/gamebanfix/assets/gamedata.jsonc (verified by BEHAVIOR.md),
plugins/RestrictedWeapons/assets/gamedata.jsonc and
plugins/BlockerPasses/assets/gamedata.jsonc (which ships its own tool).
"?" bytes are wildcards, the spelling s2sdk's gamedata uses.
"""
import re
import sys

DEFAULT_BINARY = "/root/plugify-port/cs2test/upper/game/csgo/bin/linuxsteamrt64/libserver.so"

PATTERNS = {
    "gamebanfix GameSystem_Think_CheckSteamBan":
        "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC ? 48 8B 05 ? ? ? ? 44 8B 68 ? 45 85 ED 0F 8E ? ? ? ? 8B 15",
    "gamebanfix CCSGameRules__sm_mapGcBanInformation":
        "48 8D 35 ? ? ? ? 89 C2 48 63 46",
    "RestrictedWeapons CCSPlayer_ItemServices_CanAcquire":
        "55 48 89 E5 41 57 41 56 49 89 F6 41 55 41 54 49 89 CC 53 48 89 FB 48 83 EC ? 4C 8B 6F 38",
    "BlockerPasses SetCollisionBounds":
        "48 81 C7 ? ? ? ? E9 ? ? ? ? CC CC CC CC 55 48 8D 15",
}


def compile_pattern(spelling: str) -> bytes:
    regex = bytearray()
    for token in spelling.split():
        if token == "?":
            regex += b"."
        else:
            regex += re.escape(bytes.fromhex(token))
    return bytes(regex)


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BINARY
    with open(path, "rb") as handle:
        data = handle.read()
    print(f"binary: {path} ({len(data)} bytes)")
    for name, spelling in PATTERNS.items():
        hits = [m.start() for m in re.finditer(compile_pattern(spelling), data, re.DOTALL)]
        print(f"{name}: {len(hits)} hit(s) {[hex(h) for h in hits[:20]]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
