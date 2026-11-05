// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#include "DevBench.h"

#include "Capture.h"
#include "Export.h"

#include "Settings.h"

#include <DevBenchAPI.h>
#include <nlohmann/json.hpp>

namespace FDNG::DevBench
{
	namespace
	{
		DevBenchAPI::IDevBenchInterface001* g_devbench = nullptr;

		// Runs on devbench's listener thread — reads only FloatingDamageNG's
		// own mutex-guarded copies, never game state, so no main-thread
		// marshaling is needed.
		void StatsHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			std::string action = "summary";
			if (a_argsJson) {
				const auto args = nlohmann::json::parse(a_argsJson, nullptr, false);
				if (args.is_object() && args.contains("action") && args["action"].is_string()) {
					action = args["action"].get<std::string>();
				}
			}

			nlohmann::json result;
			const auto live = CombatLog::GetSingleton()->GetLiveStats();
			result["live"] = {
				{ "inCombat", live.active },
				{ "sessionSeconds", live.sessionSeconds },
				{ "playerDamage", live.playerDamage },
				{ "realDPS", live.realDPS },
				{ "activeDPS", live.activeDPS },
			};
			if (action == "sessions" || action == "summary") {
				auto sessions = nlohmann::json::array();
				for (const auto& s : CombatLog::GetSingleton()->GetHistory()) {
					auto j = Export::SessionToJson(s);
					if (action == "summary") {
						j.erase("dpsSamples");  // keep the summary compact
						j.erase("combatants");
					}
					sessions.push_back(std::move(j));
				}
				result["sessions"] = std::move(sessions);
			}
			if (action == "trace") {
				auto trace = nlohmann::json::array();
				for (const auto& e : Capture::GetSingleton()->GetTrace()) {
					nlohmann::json j = {
						{ "t", e.seconds },
						{ "type", e.type == Capture::TraceEntry::Type::kHit ? "hit" : "raw" },
						{ "victim", e.victimID },
						{ "attacker", e.attackerID },
					};
					if (e.type == Capture::TraceEntry::Type::kHit) {
						j["totalDamage"] = e.totalDamage;
						j["physicalDamage"] = e.physicalDamage;
						j["knockback"] = e.knockback;
						j["percentBlocked"] = e.percentBlocked;
						j["resistedPhysical"] = e.resistedPhysical;
						j["resistedTyped"] = e.resistedTyped;
						j["stagger"] = e.stagger;
						j["weapon"] = e.weaponID;
						j["hitFlags"] = e.hitFlags;
						j["reflected"] = e.reflected;
						j["sneakMult"] = e.sneakMult;
					} else {
						j["source"] = e.source;
						j["av"] = e.av;
						j["mgef"] = e.mgefID;
						j["amount"] = e.amount;
						j["deadAtQueue"] = e.deadAtQueue;
						j["deadAtProcess"] = e.deadAtProcess;
					}
					trace.push_back(std::move(j));
				}
				result["trace"] = std::move(trace);
			}
			a_write(a_sink, result.dump().c_str());
		}
	}

	void Connect()
	{
		// Opt-in: the devbench host serves a local port; exposing combat
		// stats on it requires the user's explicit say-so.
		if (!Settings::GetSingleton()->enableDevBench) {
			logger::info("devbench integration disabled (bEnableDevBench=false).");
			return;
		}
		if (g_devbench) {
			return;  // already registered (UI toggle can call this again)
		}
		g_devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!g_devbench) {
			logger::info("devbench not installed; stats tool not registered.");
			return;
		}

		constexpr auto descriptor = R"({
			"description": "FloatingDamageNG combat analytics: live DPS and finished combat sessions (per-combatant damage, healing, crits, time-to-die). action=summary omits per-session detail; action=sessions includes DPS timelines and combatant breakdowns; action=trace returns the recent raw capture stream (hook payloads with dying/dead state, and HitData fields).",
			"inputSchema": {
				"type": "object",
				"properties": {
					"action": { "type": "string", "enum": ["summary", "sessions", "live", "trace"] }
				}
			},
			"readOnly": true
		})";
		if (g_devbench->RegisterTool("floatingdamage.stats", descriptor, &StatsHandler, nullptr)) {
			logger::info("Registered devbench tool floatingdamage.stats (host build {}).", g_devbench->GetBuildNumber());
		}
	}

	bool IsConnected()
	{
		return g_devbench != nullptr;
	}

	unsigned int HostBuild()
	{
		return g_devbench ? g_devbench->GetBuildNumber() : 0;
	}

	void NotifySessionEnded(const CombatLog::SessionSummary& a_summary)
	{
		if (!g_devbench) {
			return;
		}
		auto payload = Export::SessionToJson(a_summary);
		payload.erase("dpsSamples");  // event stream stays light; fetch detail via the tool
		g_devbench->EmitEvent("floatingdamage.sessionEnded", payload.dump().c_str());
	}
}
