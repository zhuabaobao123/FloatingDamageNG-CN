// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#include "CombatLog.h"

#include "DevBench.h"
#include "Export.h"
#include "I18n.h"
#include "Settings.h"

#include <fstream>

namespace FDNG
{
	namespace
	{
		std::string CurrentLocationName()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return "<unknown>";
			}
			if (const auto location = player->GetCurrentLocation()) {
				if (const char* name = location->GetName(); name && name[0]) {
					return name;
				}
			}
			if (const auto cell = player->GetParentCell()) {
				if (const char* name = cell->GetName(); name && name[0]) {
					return name;
				}
			}
			return "<wilderness>";
		}

		std::filesystem::path LogPath()
		{
			auto dir = SKSE::log::log_directory();
			return dir ? *dir / "FloatingDamageNG-combat.log" : std::filesystem::path{ "FloatingDamageNG-combat.log" };
		}

		std::string WallClockNow()
		{
			const auto now = std::chrono::system_clock::now();
			return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(now));
		}

		const char* KindName(DamageKind a_kind)
		{
			switch (a_kind) {
			case DamageKind::kFire:
				return I18n::Get("Fire", "fire");
			case DamageKind::kFrost:
				return I18n::Get("Frost", "frost");
			case DamageKind::kShock:
				return I18n::Get("Shock", "shock");
			case DamageKind::kPoison:
				return I18n::Get("Poison", "poison");
			case DamageKind::kMagic:
				return I18n::Get("Magic", "magic");
			case DamageKind::kHealing:
				return I18n::Get("Healing", "healing");
			case DamageKind::kMagickaDrain:
				return I18n::Get("Magicka drain", "magicka drain");
			case DamageKind::kStaminaDrain:
				return I18n::Get("Stamina drain", "stamina drain");
			default:
				return I18n::Get("Physical", "physical");
			}
		}

		// Weapon/spell display name for a source key; falls back to the kind.
		std::string SourceName(RE::FormID a_sourceID, DamageKind a_kind)
		{
			if (a_sourceID != 0) {
				if (const auto form = RE::TESForm::LookupByID(a_sourceID)) {
					if (const char* name = form->GetName(); name && name[0]) {
						return name;
					}
				}
			}
			return a_kind == DamageKind::kPhysical ? I18n::Get("_dll_patches.damage_kind.unarmed", "Unarmed/other") : KindName(a_kind);
		}

		std::string ActorNameByID(RE::FormID a_id)
		{
			if (const auto actor = a_id ? RE::TESForm::LookupByID<RE::Actor>(a_id) : nullptr) {
				if (const char* name = actor->GetName(); name && name[0]) {
					return name;
				}
			}
			return std::format("<{:08X}>", a_id);
		}
	}

	CombatLog* CombatLog::GetSingleton()
	{
		static CombatLog singleton;
		return &singleton;
	}

	void CombatLog::Register()
	{
		bool any = false;
		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESCombatEvent>(this);
			holder->AddEventSink<RE::TESDeathEvent>(this);
			any = true;
		} else {
			logger::warn("ScriptEventSourceHolder unavailable; combat/death sinks not registered.");
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
			any = true;
		} else {
			logger::warn("UI singleton unavailable; menu-open sink not registered.");
		}
		if (any) {
			logger::info("Combat analytics sinks registered.");
		}
	}

	float CombatLog::SessionSeconds() const
	{
		const auto end = _sessionActive ? Clock::now() : _sessionEnd;
		return std::chrono::duration<float>(end - _sessionStart).count();
	}

	CombatLog::Combatant& CombatLog::GetCombatant(RE::Actor* a_actor)
	{
		// Main thread, _lock held; the engine reads here are safe because no
		// other thread contends _lock while also holding engine locks (sinks
		// stash POD only) - but keep them minimal and first-touch only.
		auto& c = _combatants[a_actor->GetFormID()];
		if (c.name.empty()) {
			const char* name = a_actor->GetName();
			c.name = (name && name[0]) ? name : std::format("<{:08X}>", a_actor->GetFormID());
			c.isFollower = a_actor->IsPlayerTeammate();
			c.isHostileToPlayer = a_actor->IsHostileToActor(RE::PlayerCharacter::GetSingleton());
		}
		return c;
	}

	void CombatLog::EnsureSession(RE::Actor* a_hint)
	{
		// Main thread only; _sessionActive is mutated exclusively here and in
		// CloseSession, so the unlocked pre-check is safe.
		if (_sessionActive) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		// A session is any fight worth logging - the player's own, or an
		// NPC-only brawl flagged by the hint combatant.
		if (!player || (!player->IsInCombat() && !(a_hint && a_hint->IsInCombat()))) {
			return;
		}
		const auto location = CurrentLocationName();

		std::scoped_lock lk{ _lock };
		_sessionActive = true;
		++_sessionIndex;
		_sessionStart = Clock::now();
		_lastDamageAt = _sessionStart;
		_location = location;
		_combatants.clear();
		_recentDamage.clear();
		_playerDamage = 0.0f;
		_playerActiveSeconds = 0.0f;
		_lastPlayerHitAt = -1.0f;
		_dpsSamples.clear();
		_lastSampleDamage = 0.0f;
		// Discard any flag set before this session existed - the 1 Hz poll gate
		// can otherwise delay the drain past a new, unrelated session's start.
		_forceCloseOnGatedMenu.store(false, std::memory_order_relaxed);
		logger::info("Combat session #{} started @ {}", _sessionIndex, _location);
	}

	void CombatLog::RecordDamage(RE::Actor* a_attacker, RE::Actor* a_victim, float a_amount, DamageKind a_kind, const HitFlags& a_flags,
		float a_mitigated, RE::FormID a_sourceID)
	{
		if (!Settings::GetSingleton()->enableCombatLog || !a_victim) {
			return;
		}
		EnsureSession(a_victim);
		if (!_sessionActive) {
			return;
		}

		std::scoped_lock lk{ _lock };
		// Damage the player RECEIVES must not refresh the idle-close timer: a
		// lingering DoT or environmental hazard after combat would otherwise
		// keep the session open forever (esp. when the player never fights
		// back). Genuinely ongoing combat holds the session open via
		// IsInCombat() instead; damage the player deals or NPC-vs-NPC damage
		// still counts here. Stat recording below is unaffected.
		if (!a_victim->IsPlayerRef()) {
			_lastDamageAt = Clock::now();
		}

		const auto now = SessionSeconds();
		const auto kindIdx = static_cast<std::size_t>(std::to_underlying(a_kind));
		auto& victim = GetCombatant(a_victim);
		victim.damageTaken += a_amount;
		if (kindIdx < victim.takenByKind.size()) {
			victim.takenByKind[kindIdx].total += a_amount;
			victim.takenByKind[kindIdx].mitigated += a_mitigated;
			++victim.takenByKind[kindIdx].hits;
		}
		if (victim.firstHitTakenAt < 0.0f) {
			victim.firstHitTakenAt = now;
		}

		// Death recap ring: what hit whom, kept just long enough to explain
		// a death when the death event lands.
		_recentDamage.push_back({ now, a_victim->GetFormID(),
			a_attacker ? a_attacker->GetFormID() : 0, a_sourceID, a_amount, a_kind });
		while (_recentDamage.size() > kRecapCapacity) {
			_recentDamage.pop_front();
		}

		if (a_attacker) {
			auto& attacker = GetCombatant(a_attacker);
			attacker.damageDealt += a_amount;
			++attacker.hitsDealt;
			if (a_flags.critical) {
				++attacker.critsDealt;
			}

			auto& source = attacker.bySource[static_cast<std::uint64_t>(a_sourceID) | (static_cast<std::uint64_t>(kindIdx) << 32)];
			source.total += a_amount;
			source.mitigated += a_mitigated;
			++source.hits;
			if (a_flags.critical) {
				++source.crits;
			}
			auto& target = attacker.byTarget[a_victim->GetFormID()];
			target.total += a_amount;
			++target.hits;
			if (a_flags.critical) {
				++target.crits;
			}

			if (a_attacker == RE::PlayerCharacter::GetSingleton()) {
				_playerDamage += a_amount;
				_playerActiveSeconds += _lastPlayerHitAt < 0.0f ? 1.0f : std::min(now - _lastPlayerHitAt, kActiveGap);
				_lastPlayerHitAt = now;
			}
		}
	}

	void CombatLog::RecordHeal(RE::Actor* a_target, float a_amount)
	{
		if (!Settings::GetSingleton()->enableCombatLog || !a_target || !_sessionActive) {
			return;
		}
		std::scoped_lock lk{ _lock };
		// Healing the player (regen/potions) likewise must not hold a session
		// open after combat; NPC healing during an NPC fight still counts.
		if (!a_target->IsPlayerRef()) {
			_lastDamageAt = Clock::now();
		}
		GetCombatant(a_target).healingReceived += a_amount;
	}

	RE::BSEventNotifyControl CombatLog::ProcessEvent(const RE::TESCombatEvent* a_event, RE::BSTEventSource<RE::TESCombatEvent>*)
	{
		// Engine thread - POD handoff only (see class comment).
		if (!a_event || a_event->newState != RE::ACTOR_COMBAT_STATE::kCombat) {
			return RE::BSEventNotifyControl::kContinue;
		}
		if (const auto& actor = a_event->actor) {
			_combatHint.store(actor->GetFormID(), std::memory_order_relaxed);
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl CombatLog::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		// Engine thread - POD handoff only (see class comment). A successful
		// open is the engine's own proof combat already ended - more reliable
		// than our poll. Guarded on enableCombatLog: an unguarded flag would
		// persist across a later re-enable and force-close an unrelated session.
		if (!a_event || !a_event->opening || !Settings::GetSingleton()->enableCombatLog) {
			return RE::BSEventNotifyControl::kContinue;
		}
		if (a_event->menuName == "Sleep/Wait Menu" || a_event->menuName == "Crafting Menu" || a_event->menuName == "Book Menu") {
			_forceCloseOnGatedMenu.store(true, std::memory_order_relaxed);
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl CombatLog::ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*)
	{
		// Engine thread - POD handoff only (see class comment).
		if (!a_event || !a_event->dead || !a_event->actorDying) {
			return RE::BSEventNotifyControl::kContinue;
		}
		const auto id = a_event->actorDying->GetFormID();
		std::scoped_lock lk{ _deathLock };
		if (_deathCount < _deaths.size()) {
			_deaths[_deathCount++] = { id, Clock::now() };
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	void CombatLog::Tick()
	{
		if (!Settings::GetSingleton()->enableCombatLog) {
			return;
		}

		// Combat-start hint from the event sink: open a session for NPC-only
		// fights too (main thread, engine calls safe here).
		if (const auto hintID = _combatHint.exchange(0, std::memory_order_relaxed)) {
			EnsureSession(RE::TESForm::LookupByID<RE::Actor>(hintID));
		}

		// Cheap 1 Hz poll - combat-state exit has no reliable single event.
		const auto now = Clock::now();
		if (now - _lastTickCheck < std::chrono::seconds(1)) {
			return;
		}
		_lastTickCheck = now;

		// Drain deaths recorded by the sink (up to ~1 s late; the sink's
		// timestamp keeps recap/TTD accurate regardless).
		std::array<PendingDeath, 32> deaths{};
		std::size_t deathCount = 0;
		{
			std::scoped_lock lk{ _deathLock };
			deathCount = _deathCount;
			std::copy_n(_deaths.begin(), deathCount, deaths.begin());
			_deathCount = 0;
		}

		// Engine reads happen before taking _lock (see class comment).
		const auto player = RE::PlayerCharacter::GetSingleton();
		const bool playerInCombat = player && player->IsInCombat();
		// Drain every tick regardless of session state so a stale flag can't
		// linger and force-close a later, unrelated session.
		const bool gatedMenuOpened = _forceCloseOnGatedMenu.exchange(false, std::memory_order_relaxed);

		std::scoped_lock lk{ _lock };
		if (!_sessionActive) {
			return;
		}

		for (std::size_t i = 0; i < deathCount; ++i) {
			if (const auto it = _combatants.find(deaths[i].id); it != _combatants.end()) {
				it->second.diedAt = std::chrono::duration<float>(deaths[i].at - _sessionStart).count();
				BuildDeathRecap(deaths[i].id, it->second);
			}
		}

		// 1 Hz DPS timeline sample for the stats page graph (capped: 15 min).
		constexpr std::size_t kMaxDpsSamples = 900;
		if (_dpsSamples.size() < kMaxDpsSamples) {
			_dpsSamples.push_back(_playerDamage - _lastSampleDamage);
			_lastSampleDamage = _playerDamage;
		}

		// Close only once damage has stopped: NPC-only fights keep a session
		// alive even though the player never enters combat. A combat-gated menu
		// opening only proves the PLAYER's own combat state cleared, not that an
		// NPC-only fight elsewhere ended, so it force-closes just the sessions
		// the player is actually part of.
		if (gatedMenuOpened && player && _combatants.contains(player->GetFormID())) {
			logger::info("Combat session force-closed: a combat-gated menu opened.");
			CloseSession();
		} else if (!playerInCombat && now - _lastDamageAt > kIdleClose) {
			CloseSession();
		}
	}

	void CombatLog::BuildDeathRecap(RE::FormID a_victimID, Combatant& a_combatant)
	{
		// _lock held (Tick). Walk the recent-damage ring for the fatal window;
		// the last matching event is the killing blow. Events after the
		// sink-stamped death time are overkill ticks on the corpse — they
		// must not steal killing-blow credit.
		const float diedAt = a_combatant.diedAt;
		for (const auto& ev : _recentDamage) {
			if (ev.victimID != a_victimID || diedAt - ev.sessionTime > kRecapWindowSeconds || ev.sessionTime > diedAt + 0.1f) {
				continue;
			}
			a_combatant.killedByID = ev.attackerID;
			const auto recapAttacker = ev.attackerID ? ActorNameByID(ev.attackerID) : std::string{ I18n::Get("_dll_patches.death_recap.the_world", "the world") };
			char recapBuf[256];
			snprintf(recapBuf, sizeof(recapBuf),
				I18n::Get("_dll_patches.death_recap.entry", "%.1fs before death: %.0f %s from %s (%s)"),
				std::max(diedAt - ev.sessionTime, 0.0f), ev.amount, KindName(ev.kind), recapAttacker.c_str(), SourceName(ev.sourceID, ev.kind).c_str());
			a_combatant.deathRecap.push_back(recapBuf);
			if (a_combatant.deathRecap.size() > kRecapMaxLines) {
				a_combatant.deathRecap.erase(a_combatant.deathRecap.begin());
			}
		}
	}

	void CombatLog::Flush()
	{
		std::scoped_lock lk{ _lock };
		if (_sessionActive) {
			CloseSession();
		}
	}

	void CombatLog::CloseSession()
	{
		_sessionActive = false;
		_sessionEnd = Clock::now();

		const auto settings = Settings::GetSingleton();
		const float duration = std::max(SessionSeconds(), 0.01f);
		const float realDPS = _playerDamage / duration;
		const float activeDPS = _playerActiveSeconds > 0.0f ? _playerDamage / _playerActiveSeconds : 0.0f;

		logger::info("Combat session #{} ended: {:.1f}s, player dmg {:.0f} (DPS {:.1f} real / {:.1f} active)",
			_sessionIndex, duration, _playerDamage, realDPS, activeDPS);

		SessionSummary summary;
		summary.index = _sessionIndex;
		summary.startedAt = WallClockNow();
		summary.location = _location;
		summary.duration = duration;
		summary.playerDamage = _playerDamage;
		summary.realDPS = realDPS;
		summary.activeDPS = activeDPS;
		summary.dpsSamples = _dpsSamples;
		for (const float sample : _dpsSamples) {
			summary.peakDPS = std::max(summary.peakDPS, sample);
		}

		const auto playerID = RE::PlayerCharacter::GetSingleton() ? RE::PlayerCharacter::GetSingleton()->GetFormID() : 0;
		for (const auto& [id, c] : _combatants) {
			if (c.isFollower && !settings->logFollowerPerformance) {
				continue;
			}
			CombatantSummary cs;
			cs.name = c.name;
			cs.isPlayer = id == playerID;
			cs.isFollower = c.isFollower;
			cs.isHostileToPlayer = c.isHostileToPlayer;
			cs.died = c.diedAt >= 0.0f;
			cs.fled = !cs.died && c.isHostileToPlayer && c.damageTaken > 0.0f;  // threat cleared with health still above zero
			cs.damageDealt = c.damageDealt;
			cs.damageTaken = c.damageTaken;
			cs.healingReceived = c.healingReceived;
			cs.hitsDealt = c.hitsDealt;
			cs.critsDealt = c.critsDealt;
			if (cs.died && c.firstHitTakenAt >= 0.0f) {
				cs.timeToDie = c.diedAt - c.firstHitTakenAt;
			}

			// Drill-down rows, names resolved now (main thread, session over).
			for (const auto& [key, agg] : c.bySource) {
				const auto srcID = static_cast<RE::FormID>(key & 0xFFFFFFFF);
				const auto kind = static_cast<DamageKind>(key >> 32);
				cs.bySource.push_back({ SourceName(srcID, kind), kind, agg.total, agg.mitigated, agg.hits, agg.crits });
			}
			for (const auto& [victimID, agg] : c.byTarget) {
				const auto it = _combatants.find(victimID);
				cs.byTarget.push_back({ it != _combatants.end() ? it->second.name : ActorNameByID(victimID),
					DamageKind::kPhysical, agg.total, agg.mitigated, agg.hits, agg.crits });
			}
			for (std::size_t k = 0; k < c.takenByKind.size(); ++k) {
				if (const auto& agg = c.takenByKind[k]; agg.total > 0.0f || agg.mitigated > 0.0f) {
					cs.takenByKind.push_back({ KindName(static_cast<DamageKind>(k)),
						static_cast<DamageKind>(k), agg.total, agg.mitigated, agg.hits, agg.crits });
				}
			}
			const auto byTotal = [](const BreakdownRow& a, const BreakdownRow& b) { return a.total > b.total; };
			std::sort(cs.bySource.begin(), cs.bySource.end(), byTotal);
			std::sort(cs.byTarget.begin(), cs.byTarget.end(), byTotal);
			std::sort(cs.takenByKind.begin(), cs.takenByKind.end(), byTotal);
			constexpr std::size_t kMaxRows = 12;
			if (cs.bySource.size() > kMaxRows) {
				cs.bySource.resize(kMaxRows);
			}
			if (cs.byTarget.size() > kMaxRows) {
				cs.byTarget.resize(kMaxRows);
			}
			if (cs.died) {
				cs.killedBy = c.killedByID ? ActorNameByID(c.killedByID) : "the world";
				cs.deathRecap = c.deathRecap;
			}
			summary.combatants.push_back(std::move(cs));
		}
		std::sort(summary.combatants.begin(), summary.combatants.end(),
			[](const auto& a, const auto& b) { return a.damageDealt > b.damageDealt; });

		if (settings->writeLogToDisk) {
			WriteDiskReport(summary);
		}
		Export::WriteSession(summary);

		DevBench::NotifySessionEnded(summary);

		_history.push_back(std::move(summary));
		while (_history.size() > kHistoryCapacity) {
			_history.pop_front();
		}
	}

	void CombatLog::WriteDiskReport(const SessionSummary& a_summary)
	{
		const auto path = LogPath();
		Export::RotateIfOversized(path);

		std::ofstream out(path, std::ios::app);
		if (!out) {
			logger::warn("Could not open combat log file for writing.");
			return;
		}
		out << std::format("=== Session #{} - {} @ {} - {:.1f}s ===\n", a_summary.index, a_summary.startedAt, a_summary.location, a_summary.duration);
		out << std::format("  Player: {:.0f} dmg | DPS {:.1f} real / {:.1f} active ({:.1f}s active, peak {:.0f})\n",
			a_summary.playerDamage, a_summary.realDPS, a_summary.activeDPS, _playerActiveSeconds, a_summary.peakDPS);
		for (const auto& c : a_summary.combatants) {
			if (c.isPlayer) {
				continue;  // already summarized in the "Player:" line above
			}
			std::string fate;
			if (c.died) {
				fate = c.timeToDie >= 0.0f ? std::format(" - died (TTD {:.1f}s, by {})", c.timeToDie, c.killedBy) :
				                             std::format(" - died (by {})", c.killedBy);
			} else if (c.fled) {
				fate = " - survived/fled";
			}
			out << std::format("  {}{} - dealt {:.0f} ({} hits, {} crit{}), taken {:.0f}{}{}\n",
				c.name,
				c.isFollower ? " (follower)" : (c.isHostileToPlayer ? " (hostile)" : ""),
				c.damageDealt, c.hitsDealt, c.critsDealt, c.critsDealt == 1 ? "" : "s",
				c.damageTaken,
				c.healingReceived > 0.0f ? std::format(", healed +{:.0f}", c.healingReceived) : "",
				fate);
		}
		out << '\n';
	}

	std::vector<CombatLog::SessionSummary> CombatLog::GetHistory()
	{
		std::scoped_lock lk{ _lock };
		return { _history.begin(), _history.end() };
	}

	CombatLog::LiveStats CombatLog::GetLiveStats()
	{
		std::scoped_lock lk{ _lock };
		LiveStats stats;
		stats.active = _sessionActive;
		if (_sessionIndex == 0) {
			stats.secondsSinceEnd = FLT_MAX;
			return stats;
		}
		stats.sessionSeconds = SessionSeconds();
		stats.secondsSinceEnd = _sessionActive ? 0.0f : std::chrono::duration<float>(Clock::now() - _sessionEnd).count();
		stats.playerDamage = _playerDamage;
		const float duration = std::max(stats.sessionSeconds, 0.01f);
		stats.realDPS = _playerDamage / duration;
		stats.activeDPS = _playerActiveSeconds > 0.0f ? _playerDamage / _playerActiveSeconds : 0.0f;
		return stats;
	}
}
