# gamebanfix (Plugify)

Plugify port of the Metamod:Source plugin 'GameBanFix' (the plugin that shipped as addons/gamebanfix). Hooks the engine's per-frame GC-ban kick pass (GameSystem_Think_CheckSteamBan, found by signature) and, while sv_kick_players_with_cooldown <= 1, removes the competitive-cooldown entries (reasons 20/22/23) from CCSGameRules::sm_mapGcBanInformation before the pass runs; a post hook clears the map so a stale entry cannot reach the next player to join. This is the fix for the bug where one game-banned player joining made the whole server unjoinable. No methods, no commands, no config files.

Plugify port (fork) of **GameBanFix** by Cruze (aiolos1045, Vauff) (Metamod (C++)).
Original: https://github.com/Cruze03/GameBanFix
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.0.2
- Plugify dependencies: s2sdk (>=2.18.2), polyhook (>=2.5.6)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `gamebanfix-1.0.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/gamebanfix`.

## License
GPL-3.0, see LICENSE.

