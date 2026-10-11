// gamebanfix — Plugify C++ port of the Metamod:Source plugin "GameBanFix"
// (two builds were analysed; see BEHAVIOR.md).
//
// What the original does, and what is kept here:
//   * hooks GameSystem_Think_CheckSteamBan (signature: the function the engine runs every frame
//     for the GC-ban / competitive-cooldown kick pass),
//   * pre hook: while sv_kick_players_with_cooldown <= 1, drop every entry of
//     CCSGameRules::sm_mapGcBanInformation whose CGcBanInformation_t::m_uiReason is 20, 22 or 23
//     (the competitive-cooldown reasons), so the pass cannot act on them,
//   * post hook: clear the map (the engine's RemoveAll) so nothing the pass left behind can
//     reach the next player to join (this is the fix for "a banned player joins and then other
//     players cannot join anymore").
//
// Porting notes / deliberate differences:
//   * Metamod's KHook becomes the official polyhook plugin (polyhook::HookDetour + Pre/Post
//     callbacks); addresses come from this plugin's own assets/gamedata.jsonc through s2sdk
//     (same signature bytes as the original's gamebanfix.games.txt).
//   * The original returns "Failed" when a signature is missing and then dereferences a null map
//     pointer when the container is not initialised. Plugify forbids a Failed start (it stops
//     every plugin) and a crash in a hook callback kills the server: this port logs, skips the
//     hook and null-checks the map instead — fail soft.
//   * The convar is not read from inside the hook callback (polyhook callbacks run inside the
//     hooking subsystem): OnPluginUpdate caches sv_kick_players_with_cooldown once per second and
//     the callback reads that cached value.
//   * No CVars, no commands, no config files — the original has none.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plugin_export.h>

#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include "gcban_map.hpp"

namespace {

// sv_kick_players_with_cooldown values above this disable the entry filtering (the original's
// "if (value > 1) return;"): the pass is then left alone, only the post-hook clear runs.
constexpr std::int32_t kCooldownFilterLimit = 1;

// CGcBanInformation_t::m_uiReason values the original removes below the limit: the competitive
// cooldown reasons (the 0xd00000 bitmask over reasons 20/22/23 in the original binary).
constexpr std::uint32_t kCooldownReasons[] = {20, 22, 23};

std::atomic<std::int32_t> g_kickPlayersWithCooldown{0};
std::atomic<std::uint64_t> g_preCalls{0};
std::atomic<std::uint64_t> g_postCalls{0};
std::atomic<std::uint64_t> g_entriesRemoved{0};
std::atomic<std::uint64_t> g_mapClears{0};

gamebanfix::GcBanMap* g_pBanMap = nullptr;
polyhook::HookHandle g_checkSteamBanHook = nullptr;
void* g_hookedFunction = nullptr; // the detour target, needed by UnhookDetour on unload

void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	Log(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	Log(message, plg::Severity::Error);
}

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// --- hooks -------------------------------------------------------------------------------------

// The engine pass is `void GameSystem_Think_CheckSteamBan(CGameSystem* this, void* arg)`: the
// prologue stores rdi and tests *(bool*)rsi (see BEHAVIOR.md §Behaviour). Neither argument is used
// here; they are only declared so polyhook builds the trampoline with the real ABI.
const plg::vector<polyhook::DataType>& CheckSteamBanArgs() {
	static const plg::vector<polyhook::DataType> args = {
		polyhook::DataType::Pointer, // this
		polyhook::DataType::Pointer, // second argument (unused here)
	};
	return args;
}

void RemoveCooldownEntries() {
	if (g_pBanMap == nullptr) {
		return;
	}
	gamebanfix::GcBanTree tree(g_pBanMap);
	if (!tree.IsValid()) {
		return;
	}
	const std::int32_t before = tree.Count();
	tree.RemoveReasons(kCooldownReasons, sizeof(kCooldownReasons) / sizeof(kCooldownReasons[0]));
	const std::int32_t after = tree.Count();
	if (before > after) {
		g_entriesRemoved.fetch_add(static_cast<std::uint64_t>(before - after), std::memory_order_relaxed);
	}
}

void ClearBanMap() {
	if (g_pBanMap == nullptr) {
		return;
	}
	gamebanfix::GcBanTree tree(g_pBanMap);
	if (!tree.IsValid()) {
		return;
	}
	if (tree.Count() > 0) {
		tree.RemoveAll();
		g_mapClears.fetch_add(1, std::memory_order_relaxed);
	}
}

// Mirrors the original's Detour_GameSystem_Think_CheckSteamBan().
polyhook::ResultType OnCheckSteamBanPre(polyhook::HookHandle /*hook*/, polyhook::ParametersHandle /*params*/,
										int32_t /*count*/, polyhook::ReturnHandle /*ret*/,
										polyhook::CallbackType /*type*/) {
	g_preCalls.fetch_add(1, std::memory_order_relaxed);
	if (g_kickPlayersWithCooldown.load(std::memory_order_relaxed) <= kCooldownFilterLimit) {
		RemoveCooldownEntries();
	}
	return polyhook::ResultType::Ignored;
}

// Mirrors the original's Detour_GameSystem_Think_CheckSteamBan_Post().
polyhook::ResultType OnCheckSteamBanPost(polyhook::HookHandle /*hook*/, polyhook::ParametersHandle /*params*/,
										 int32_t /*count*/, polyhook::ReturnHandle /*ret*/,
										 polyhook::CallbackType /*type*/) {
	g_postCalls.fetch_add(1, std::memory_order_relaxed);
	ClearBanMap();
	return polyhook::ResultType::Ignored;
}

// --- gamedata ----------------------------------------------------------------------------------

bool LoadGamedata(const std::string& location, s2sdk::ConfigId& config, std::string& error) {
	if (!__s2sdk_LoadGameConfigFile) {
		error = "s2sdk LoadGameConfigFile binding is not available";
		return false;
	}
	std::vector<std::string> candidates;
	if (!location.empty()) {
		const std::filesystem::path loc(location);
		candidates.push_back((loc / "gamedata.jsonc").string());
		candidates.push_back((loc.parent_path() / "gamedata.jsonc").string());
		candidates.push_back((loc.parent_path().parent_path() / "gamedata.jsonc").string());
	}

	plg::vector<plg::string> paths;
	std::string checked;
	for (const std::string& candidate : candidates) {
		std::error_code existsError;
		if (std::filesystem::exists(candidate, existsError)) {
			paths.push_back(candidate);
		}
		if (!checked.empty()) {
			checked += ", ";
		}
		checked += candidate;
	}
	if (paths.empty()) {
		error = "gamedata.jsonc not found (looked for: " + checked + ")";
		return false;
	}

	config = s2sdk::LoadGameConfigFile(paths);
	if (config == static_cast<s2sdk::ConfigId>(-1) || config == 0) {
		error = "s2sdk could not load the gamebanfix gamedata";
		return false;
	}
	return true;
}

bool InstallHook(s2sdk::ConfigId config, std::string& error) {
	if (!__polyhook_HookDetour) {
		error = "polyhook HookDetour binding is not available (is the polyhook plugin installed?)";
		return false;
	}

	void* pFunction = s2sdk::GetGameConfigSignature(config, "GameSystem_Think_CheckSteamBan");
	if (pFunction == nullptr) {
		error = "signature GameSystem_Think_CheckSteamBan did not resolve";
		return false;
	}

	polyhook::HookHandle hook = nullptr;
	try {
		hook = polyhook::HookDetour(pFunction, polyhook::DataType::Void, CheckSteamBanArgs(), -1,
									"gamebanfix_GameSystem_Think_CheckSteamBan");
	} catch (const std::exception& ex) {
		error = std::string("polyhook::HookDetour threw: ") + ex.what();
		return false;
	} catch (...) {
		error = "polyhook::HookDetour threw";
		return false;
	}
	if (hook == nullptr) {
		std::string polyhookError;
		try {
			if (__polyhook_GetError) {
				polyhookError = std::string(std::string_view(polyhook::GetError()));
			}
		} catch (...) {
		}
		error = "polyhook::HookDetour failed";
		if (!polyhookError.empty()) {
			error += ": " + polyhookError;
		}
		return false;
	}

	bool preAdded = false;
	bool postAdded = false;
	try {
		preAdded = polyhook::AddCallback(hook, polyhook::CallbackType::Pre, &OnCheckSteamBanPre);
		postAdded = polyhook::AddCallback(hook, polyhook::CallbackType::Post, &OnCheckSteamBanPost);
	} catch (...) {
		preAdded = false;
		postAdded = false;
	}
	if (!preAdded || !postAdded) {
		try {
			if (preAdded) {
				polyhook::RemoveCallback(hook, polyhook::CallbackType::Pre, &OnCheckSteamBanPre);
			}
		} catch (...) {
		}
		try {
			if (postAdded) {
				polyhook::RemoveCallback(hook, polyhook::CallbackType::Post, &OnCheckSteamBanPost);
			}
		} catch (...) {
		}
		// UnhookDetour() removes the detour for every hooker of this address (game_fixes hooks the
		// same function and may have created it): only drop it when nothing is attached anymore.
		if (__polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour) {
			try {
				if (!polyhook::AreCallbacksRegistered(hook)) {
					polyhook::UnhookDetour(pFunction);
				}
			} catch (...) {
			}
		}
		error = "polyhook::AddCallback failed";
		return false;
	}

	g_checkSteamBanHook = hook;
	g_hookedFunction = pFunction;
	LogInfo("gamebanfix: hooked GameSystem_Think_CheckSteamBan at 0x" +
			std::to_string(reinterpret_cast<std::uintptr_t>(pFunction)));
	return true;
}

// sv_kick_players_with_cooldown is read through the cached convar handle; a missing convar keeps
// the last value (0 by default, which means "filter", the original's behaviour for value <= 1).
void RefreshCooldownConvar() {
	if (!__s2sdk_FindConVar || !__s2sdk_GetConVarInt32) {
		return;
	}
	static std::uint64_t convar = 0;
	if (convar == 0) {
		convar = s2sdk::FindConVar("sv_kick_players_with_cooldown");
		if (convar == 0) {
			return;
		}
	}
	g_kickPlayersWithCooldown.store(s2sdk::GetConVarInt32(convar), std::memory_order_relaxed);
}

class GameBanFixPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			std::string location;
			try {
				location = ToString(GetLocation());
			} catch (...) {
			}

			s2sdk::ConfigId config = 0;
			std::string error;
			if (!LoadGamedata(location, config, error)) {
				LogError("gamebanfix: " + error + " - the fix stays off");
				return {}; // never Failed: that would stop every Plugify plugin
			}
			m_configId = config;

			void* pMap = s2sdk::GetGameConfigAddress(config, "CCSGameRules__sm_mapGcBanInformation");
			if (pMap == nullptr) {
				LogError("gamebanfix: address CCSGameRules__sm_mapGcBanInformation did not resolve - fix off");
				return {};
			}
			g_pBanMap = static_cast<gamebanfix::GcBanMap*>(pMap);
			{
				// An empty container (no map loaded yet) is fine - the fields are only read when
				// the pass runs. This rejects a signature hit that is not the container.
				gamebanfix::GcBanTree tree(g_pBanMap);
				if (!tree.IsValid()) {
					LogError("gamebanfix: the resolved ban map does not look like the container - the fix stays off");
					g_pBanMap = nullptr;
					return {};
				}
			}

			RefreshCooldownConvar();

			if (!InstallHook(config, error)) {
				LogError("gamebanfix: " + error + " - the fix stays off");
				g_pBanMap = nullptr;
				return {};
			}

			LogInfo("gamebanfix: ready (ban map at 0x" + std::to_string(reinterpret_cast<std::uintptr_t>(pMap)) +
					", sv_kick_players_with_cooldown=" + std::to_string(g_kickPlayersWithCooldown.load()) + ")");
		} catch (const std::exception& ex) {
			LogError(std::string("gamebanfix: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("gamebanfix: OnPluginStart failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds /*dt*/) override {
		try {
			RefreshCooldownConvar();
		} catch (...) {
			// Reading a convar must never take the server down.
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			g_pBanMap = nullptr;
			// The callbacks of this plugin have to go before the detour is dropped: a registered
			// handler is a function pointer into this library.
			const polyhook::HookHandle hook = g_checkSteamBanHook;
			g_checkSteamBanHook = nullptr;
			if (hook != nullptr) {
				// One try block per callback: a throw on the first must not leave the second
				// registered (it would keep a function pointer into this library alive).
				try {
					polyhook::RemoveCallback(hook, polyhook::CallbackType::Pre, &OnCheckSteamBanPre);
				} catch (...) {
				}
				try {
					polyhook::RemoveCallback(hook, polyhook::CallbackType::Post, &OnCheckSteamBanPost);
				} catch (...) {
				}
			}
			// The detour must go away before the shared object is unloaded: its trampoline holds
			// pointers into this plugin. UnhookDetour() removes it for EVERY hooker of the target
			// address, though, and game_fixes hooks the same GameSystem_Think_CheckSteamBan - so it
			// is only dropped when no callback is attached to the shared hook anymore (checked
			// after our own RemoveCallback above).
			if (g_hookedFunction != nullptr) {
				if (hook != nullptr && __polyhook_AreCallbacksRegistered && __polyhook_UnhookDetour) {
					try {
						if (!polyhook::AreCallbacksRegistered(hook)) {
							polyhook::UnhookDetour(g_hookedFunction);
						}
					} catch (...) {
					}
				}
				g_hookedFunction = nullptr;
			}
			if (m_configId != 0) {
				try {
					s2sdk::CloseGameConfigFile(m_configId);
				} catch (...) {
				}
				m_configId = 0;
			}
			LogInfo("gamebanfix: stopped (pre=" + std::to_string(g_preCalls.load()) +
					", post=" + std::to_string(g_postCalls.load()) +
					", entries removed=" + std::to_string(g_entriesRemoved.load()) +
					", map clears=" + std::to_string(g_mapClears.load()) + ")");
		} catch (...) {
		}
		return {};
	}

private:
	s2sdk::ConfigId m_configId = 0;
};

GameBanFixPlugin g_gameBanFixPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_gameBanFixPlugin)
