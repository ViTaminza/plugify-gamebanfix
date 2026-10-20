# GameBanFix — what the binary does (analysis)

Sources analysed (all binary-only; `gbf.tar.gz` contains **no** sources, only the same `.so`, its
gamedata file and the `.vdf`):

| file | md5 | note |
|---|---|---|
| `gamebanfix.so`, earlier build | `fabaf799…` | the build shipped with the analysis material |
| `gamebanfix.so`, newer build | `df90f7f1…` | the build a server actually ran — analysed as the authority |
| `gamebanfix.games.txt` (identical in both) | — | Metamod gamedata: 2 signatures |

Metamod plugin, log tag `[GameBanFix]`, "Fix Game Bans — Fixes issue where if a player with game
ban joins, other players even without a ban are then unable to join". Built with **KHook**
(`KHook::Function<void>`, `CKHookFunction::Configure`) and statically linked protobuf/tier1.
No CVars, no commands, no config files.

## Behaviour

The engine runs a per-frame pass **`GameSystem_Think_CheckSteamBan`** that walks
`CCSGameRules::sm_mapGcBanInformation` (keyed by Steam account id, value `CGcBanInformation_t`)
and kicks every connected account it finds matching its own rules. The known bug: entries that the
pass acted on (or the competitive-cooldown reasons it can also act on) survive and are then applied
to *whoever joins next*, which is what makes the server unjoinable after one game-banned player
connects.

The plugin hooks that function and:

* **pre hook** (`Detour_GameSystem_Think_CheckSteamBan`) — if the convar
  `sv_kick_players_with_cooldown` is `<= 1` (the check returns early when it is above 1), walk the
  map in order and remove every entry whose `CGcBanInformation_t::m_uiReason` is **20, 22 or 23**
  (the original tests the reason against a bitmask of those three bits and skips everything above
  0x17). Removal = the engine's normal `Unlink(node)` plus a free-list push that points the node's
  left link at its own slot, chains the old free head into the node's right link, makes the node
  the new free head and decrements the element count. The successor is computed *before* the
  unlink. The map pointer and the hooked address are resolved once at plugin load.
* **post hook** (`Detour_GameSystem_Think_CheckSteamBan_Post`) — if the map still has elements,
  clear it. The older build inlines the container's `RemoveAll()` (free-list sweep over all slots,
  stop at `m_nLastAlloc`, then write `m_nRoot = -1` and `m_nNumElements = 0` over the root/count
  pair as one qword); the newer build calls its own compiled `CUtlRBTree<…>::RemoveAll()` — same
  code. This is the part that stops a stale entry from reaching the next player.

The newer build additionally has an (uncalled, dead) `addresses::InitializeBanMap()` and reads the
convar through the newer `ConVarRef`/`ConVarData` API; behaviour is identical. Failure handling:
if a signature does not resolve, the original calls `Error(...)` and returns `Failed` from `Load`
(and would dereference a null map if the container had no storage). The port logs and stays off
instead (see the plugin's header comment).

## Layout the fix depends on (verified against the current `libserver.so`)

`GcBanMap` (the object the gamedata signature points at, `0x28` bytes):

| off | type | meaning (use in the analysed code) |
|---|---|---|
| `+0x08` | int32 | slot capacity (the RemoveAll sweep bound) |
| `+0x0C` | int32 | allocation flags; `m_pMemory` is only used when `& 0x7FFFFFFF` is non-zero |
| `+0x10` | ptr | node array |
| `+0x18` | int32 | `m_nRoot` (`-1` = empty) |
| `+0x1C` | int32 | `m_nNumElements` (element count; `RemoveAll` clears it to `0` in the same qword as the root) |
| `+0x20` | int32 | `m_nFirstFree` |
| `+0x24` | int32 | last slot ever touched — the `RemoveAll` sweep stops there |

`GcBanNode`, `0x38` bytes: `+0x00` left, `+0x04` right (doubles as the free-list link), `+0x08`
parent, `+0x0C` colour byte, `+0x10` key (account id), `+0x18` `m_uiReason`, `+0x20`/`+0x28` doubles,
`+0x30` account id. A slot is free while `m_iLeft == own index` — that is what the plugin's
free-push writes and what the `RemoveAll` sweep tests.

## Verified signatures (current game build)

Both patterns of the original gamedata match exactly **once** in the current `libserver.so`
(`.text` file `0x9fedc0` = VA `0x9ffdc0`; VA = file + `0x1000`):

* `GameSystem_Think_CheckSteamBan` → VA `0x13dad60`
  (`55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC ? 48 8B 05 …`),
* `CCSGameRules__sm_mapGcBanInformation` → VA `0x154c5c0`, the `lea`'s rip-relative target
  (`+7 + *(int32*)(match+3)`) is the map at **VA `0x294c520`** (`lea rsi,[rip+0x13fff59]`).

Both are mirrored into `assets/gamedata.jsonc` (s2sdk format; the address entry uses the
`read_offs32` indirection step, which is exactly `match + 3 + 4 + disp`).

Independent confirmation of the semantics: the Slynx GameFixes plugin (the fork this workspace
ports as `game_fixes`) implements the same fix on the same map — same two signatures, same
"reasons 20/22/23 below `sv_kick_players_with_cooldown 2`" rule, `RemoveAt` in the pre hook and
`RemoveAll` in the post hook.

## Port

`plugins/gamebanfix` (manifest name `gamebanfix`, the original `.vdf` alias):

* `src/gcban_map.{hpp,cpp}` — the container operations (in-order walk, `Unlink` with the CLRS
  red-black delete fixup, `FreeNode`, `RemoveReasons`, the engine's `RemoveAll`) against the exact
  layout above; no engine headers needed.
* `src/plugin.cpp` — s2sdk gamedata load, polyhook `HookDetour` on the signature with Pre/Post
  callbacks, `sv_kick_players_with_cooldown` cached once per second (never read from inside the
  hook callback), fail-soft on any missing piece.
* `tests/gamebanfix_tests.cpp` — randomised insert/remove against an in-test red-black container with
  full invariant checking after every operation (the container code cannot be validated against the
  engine offline, so the tests are the safety net).

Needs a running-server check: that the hooked function still matches on the server's own build
(the signature bytes above were verified against a copy of `libserver.so`, not the running one)
and that `sv_kick_players_with_cooldown` is `1` or lower there (otherwise the fix is inert by
design).
