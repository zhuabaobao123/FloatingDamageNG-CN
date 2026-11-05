// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "Capture.h"

namespace FDNG
{
	// Combat analytics. Sessions open on combat (any
	// participant, not just the player) and close once the player is out of
	// combat AND damage has stopped flowing; per-combatant damage, healing,
	// crits, time-to-die, and fled state accumulate in between. Session
	// reports append to <SKSE logs>/FloatingDamageNG-combat.log and feed the
	// live DPS readout.
	//
	// THREADING: the event sinks run on engine threads and only stash POD —
	// they never take _lock, which is what makes _lock safe: with no engine
	// thread ever waiting on it, main-thread code under _lock may freely
	// call engine functions (name lookups etc.) without a lock-order
	// inversion. Everything except the sinks runs on the main thread
	// (RecordDamage/RecordHeal from Capture::ProcessQueued, Tick from the
	// render hook); the only other _lock contenders are read-only snapshot
	// getters from UI/devbench threads.
	class CombatLog :
	    public RE::BSTEventSink<RE::TESCombatEvent>,
	    public RE::BSTEventSink<RE::TESDeathEvent>,
	    public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		struct LiveStats
		{
			bool active{ false };
			float sessionSeconds{ 0.0f };
			float secondsSinceEnd{ 0.0f };  // valid when !active; drives the fade-out
			float playerDamage{ 0.0f };
			float realDPS{ 0.0f };
			float activeDPS{ 0.0f };
			float damageTaken{ 0.0f };
			float biggestHit{ 0.0f };
			int hits{ 0 };
			int crits{ 0 };
			int kills{ 0 };  // killing blows credited to the player
			float healingReceived{ 0.0f };

			enum class DealerRelation
			{
				kPlayer,
				kFollower,
				kHostile,
				kOther
			};
			struct Dealer
			{
				std::string name;
				float damage{ 0.0f };
				DealerRelation relation{ DealerRelation::kOther };
			};
			std::vector<Dealer> topDealers;
		};

		// One row of a drill-down table (per damage source or per target).
		struct BreakdownRow
		{
			std::string name;  // spell/weapon or target name
			DamageKind kind{ DamageKind::kPhysical };
			float total{ 0.0f };
			float mitigated{ 0.0f };  // what the victims shrugged off
			int hits{ 0 };
			int crits{ 0 };
		};

		// Structured per-combatant result — the stats table renders these
		// directly (never pre-format to strings; the UI can't sort strings).
		struct CombatantSummary
		{
			std::string name;
			bool isPlayer{ false };
			bool isFollower{ false };
			bool isHostileToPlayer{ false };
			bool died{ false };
			bool fled{ false };
			float damageDealt{ 0.0f };
			float damageTaken{ 0.0f };
			float healingReceived{ 0.0f };
			int hitsDealt{ 0 };
			int critsDealt{ 0 };
			float timeToDie{ -1.0f };               // first hit taken -> death; -1 when n/a
			std::vector<BreakdownRow> bySource;     // dealt, by weapon/spell, desc
			std::vector<BreakdownRow> byTarget;     // dealt, by victim, desc
			std::vector<BreakdownRow> takenByKind;  // taken + mitigated per damage type (resist profile)
			std::string killedBy;                   // final-blow attacker, when died
			std::vector<std::string> deathRecap;    // last hits leading to death, formatted
		};

		// Finished-session record kept for in-game browsing (SMF stats page).
		struct SessionSummary
		{
			int index{ 0 };
			std::string startedAt;  // wall clock
			std::string location;
			float duration{ 0.0f };
			float playerDamage{ 0.0f };
			float realDPS{ 0.0f };
			float activeDPS{ 0.0f };
			float peakDPS{ 0.0f };                     // max 1 Hz sample
			std::vector<float> dpsSamples;             // player damage per second, 1 Hz
			std::vector<CombatantSummary> combatants;  // sorted by damage dealt, desc
		};

		static CombatLog* GetSingleton();

		// Copy of the recent session history (UI thread safe).
		std::vector<SessionSummary> GetHistory();

		// Register the combat/death sinks (call at kDataLoaded).
		void Register();

		// From Capture (main thread): every classified damage/heal application.
		// a_sourceID identifies the weapon or magic effect (0 = unarmed/other).
		void RecordDamage(RE::Actor* a_attacker, RE::Actor* a_victim, float a_amount, DamageKind a_kind, const HitFlags& a_flags,
			float a_mitigated = 0.0f, RE::FormID a_sourceID = 0);
		void RecordHeal(RE::Actor* a_target, float a_amount);

		// Called once per frame from the renderer: closes the session once the
		// player has left combat, and snapshots the live numbers.
		void Tick();

		// Close and persist any open session immediately (game save, load,
		// new game) so data isn't lost when the process goes away before the
		// idle close fires. Main thread only.
		void Flush();

		// a_topRows: how many damage-dealt leaders to include (0 = none).
		LiveStats GetLiveStats(int a_topRows = 0);

		RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_event, RE::BSTEventSource<RE::TESCombatEvent>*) override;
		RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override;
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

	private:
		using Clock = std::chrono::steady_clock;

		struct DamageAgg
		{
			float total{ 0.0f };
			float mitigated{ 0.0f };
			int hits{ 0 };
			int crits{ 0 };
		};

		struct Combatant
		{
			std::string name;
			bool isFollower{ false };
			bool isHostileToPlayer{ false };
			float damageDealt{ 0.0f };
			float damageTaken{ 0.0f };
			float healingReceived{ 0.0f };
			int hitsDealt{ 0 };
			int critsDealt{ 0 };
			float firstHitTakenAt{ -1.0f };  // session-relative seconds
			float diedAt{ -1.0f };
			// Drill-down aggregates (keys resolved to names at session close).
			std::unordered_map<std::uint64_t, DamageAgg> bySource;  // dealt; key = sourceID | kind<<32
			std::unordered_map<RE::FormID, DamageAgg> byTarget;     // dealt, per victim
			std::array<DamageAgg, 9> takenByKind{};                 // indexed by DamageKind
			RE::FormID killedByID{ 0 };
			std::vector<std::string> deathRecap;
		};

		// One damage application, kept briefly for death recaps.
		struct RecapEvent
		{
			float sessionTime{ 0.0f };
			RE::FormID victimID{ 0 };
			RE::FormID attackerID{ 0 };
			RE::FormID sourceID{ 0 };
			float amount{ 0.0f };
			DamageKind kind{ DamageKind::kPhysical };
		};

		void EnsureSession(RE::Actor* a_hint);
		void CloseSession();
		void WriteDiskReport(const SessionSummary& a_summary);
		void BuildDeathRecap(RE::FormID a_victimID, Combatant& a_combatant);

		Combatant& GetCombatant(RE::Actor* a_actor);
		float SessionSeconds() const;

		// Close only after the fight actually stops: the player being out of
		// combat is not enough while NPCs are still trading blows.
		static constexpr auto kIdleClose = std::chrono::seconds(4);

		// Active-DPS gap rule (ACT/Details convention): time between player
		// hits counts as "active" only up to this many seconds.
		static constexpr float kActiveGap = 3.0f;

		std::mutex _lock;

		bool _sessionActive{ false };
		int _sessionIndex{ 0 };
		Clock::time_point _sessionStart{};
		Clock::time_point _sessionEnd{};
		std::string _location;
		std::unordered_map<RE::FormID, Combatant> _combatants;

		float _playerDamage{ 0.0f };
		float _playerActiveSeconds{ 0.0f };
		float _lastPlayerHitAt{ -1.0f };
		float _playerTaken{ 0.0f };
		float _playerBiggestHit{ 0.0f };
		int _playerHits{ 0 };
		int _playerCrits{ 0 };

		std::vector<float> _dpsSamples;
		float _lastSampleDamage{ 0.0f };

		static constexpr std::size_t kHistoryCapacity = 20;
		std::deque<SessionSummary> _history;

		Clock::time_point _lastTickCheck{};
		Clock::time_point _lastDamageAt{};

		// Sink→main-thread handoff (POD only; sinks never take _lock). The
		// death timestamp is taken in the sink: the drain runs up to 1 s
		// later, and overkill ticks recorded in between must not steal
		// killing-blow credit.
		struct PendingDeath
		{
			RE::FormID id{ 0 };
			Clock::time_point at{};
		};
		std::atomic<RE::FormID> _combatHint{ 0 };
		// Set when a combat-gated menu (Sleep/Wait, Crafting, Book) opens - the
		// engine only allows that if it already considers the player out of
		// combat, so it force-closes our session instead of waiting on the
		// idle timer. Doesn't touch engine combat state, only our own tracking.
		std::atomic<bool> _forceCloseOnGatedMenu{ false };
		std::mutex _deathLock;
		std::array<PendingDeath, 32> _deaths{};
		std::size_t _deathCount{ 0 };

		// Recent damage ring for death recaps (main thread, under _lock).
		static constexpr std::size_t kRecapCapacity = 128;
		static constexpr float kRecapWindowSeconds = 10.0f;
		static constexpr std::size_t kRecapMaxLines = 8;
		std::deque<RecapEvent> _recentDamage;
	};
}
