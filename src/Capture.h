// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "HitClaims.h"

namespace FDNG
{
	enum class DamageKind : std::uint8_t
	{
		kPhysical = 0,
		kFire,
		kFrost,
		kShock,
		kPoison,
		kMagic,  // untyped/other magic
		kHealing,
		kMagickaDrain,  // hostile magicka damage (shock secondary, absorb/drain)
		kStaminaDrain   // hostile stamina damage (frost secondary, absorb/drain)
	};

	enum class OriginTier : std::uint8_t
	{
		kPlayer = 0,    // player dealt it
		kFollower,      // a teammate dealt it
		kNPC,           // NPC-on-NPC
		kPlayerVictim,  // player received it
	};

	struct HitFlags
	{
		bool critical{ false };
		bool blocked{ false };
		bool sneak{ false };
		bool powerAttack{ false };
		bool bash{ false };
		bool dotTick{ false };
		bool timedBash{ false };     // kTimedBash - a well-timed bash, distinct from a plain kBash
		bool perfectBlock{ false };  // kBlockWithWeapon with ~0 damage through - a full negation
	};

	struct HitExtras
	{
		float critBonus{ 0.0f };
		float sneakBonus{ 0.0f };
		float reflected{ 0.0f };
	};

	enum class ExtraDisplay : std::uint8_t
	{
		kOff = 0,
		kInline,
		kSeparate,
	};

	inline constexpr int kNoExtra = -1;

	// Word used for the mitigation subtext — mitigation has different causes.
	enum class MitigationLabel : std::uint8_t
	{
		kResisted = 0,  // magical resistance (spells, enchant payloads)
		kBlocked,       // an actual shield/weapon block
		kArmor,         // armor rating on a physical blow
	};

	// One classified damage application, ready to display.
	struct DamageEvent
	{
		RE::FormID victimID{ 0 };
		RE::NiPoint3 anchor;  // world-space spawn point (victim head)
		float amount{ 0.0f };
		float mitigated{ 0.0f };  // resisted/blocked/armor portion, for the subtext
		float ampMult{ 0.0f };    // implied external multiplier (0 = none)
		char location[16]{};      // locational tag (e.g. "HEADSHOT"), empty = none
		MitigationLabel mitLabel{ MitigationLabel::kResisted };
		DamageKind kind{ DamageKind::kPhysical };
		OriginTier origin{ OriginTier::kNPC };
		HitFlags flags;
		HitExtras extras;
		int extraIndex{ kNoExtra };  // kExtraTable row of an informational number; analytics never sees these

		// Public-API popups (Api.cpp): text shown verbatim in place of the
		// number built from `amount`, a world-fixed anchor, and optional styling.
		char customText[28]{};  // empty = a normal damage number
		bool pinned{ false };   // anchor stays at `anchor` instead of following the victim's head
		bool useColor{ false };
		std::uint32_t colorRGB{ 0 };
		float scale{ 1.0f };
		float lifetimeSeconds{ 0.0f };  // 0 = the configured lifetime
	};

	// Damage-capture front end. THREADING CONTRACT: the engine writes actor
	// values from job threads, script threads, and the main thread, so the
	// hook entry points (OnXxx) only queue POD (form IDs + amounts) — no actor
	// method calls, no map growth beyond a fixed ring. ProcessQueued() drains
	// on the main thread each frame and does everything else there:
	// resolution, classification, filters, analytics, spawning.
	class Capture :
	    public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
	{
	public:
		static Capture* GetSingleton();

		// Register the magic-effect sink (call at kDataLoaded).
		void Register();

		// Hook entries — queue-only, any thread. a_damage/a_value keep the
		// engine's sign (negative = damage).
		void OnHealthDamage(RE::Actor* a_victim, RE::Actor* a_attacker, float a_damage);
		void OnHealthRestore(RE::Actor* a_target, float a_amount);
		void OnMagicDamage(RE::Actor* a_victim, float a_amount);
		void OnResourceDamage(RE::Actor* a_victim, RE::ActorValue a_value, float a_amount);
		// a_appliedHealth: health delta (negative) the engine applied during this effect; used for health damage in place of a_value.
		void OnEffectModify(RE::ValueModifierEffect* a_effect, RE::Actor* a_target, float a_value, RE::ActorValue a_actorValue, float a_appliedHealth);

		// From the hit-dispatch thunk: stash per-victim hit metadata so the
		// matching weapon-hit event can consume it (POD only).
		void OnHitData(const RE::HitData& a_hitData);

		// Public API (FloatingDamageNGAPI.h): another mod claims the display of
		// the next weapon hit on a victim. Any thread; the claim is consumed by
		// that hit's processing and expires on its own.
		void ClaimHit(RE::FormID a_victimID, HitClaim a_claim);

		// A melee or projectile hit on the player negated by i-frames; any thread, coalesced.
		void OnEvade(const char* a_source);

		// Main thread (render tick): drain and process the raw queue.
		void ProcessQueued();

		// Debug audit (bDeltaAudit): 1 Hz comparison of each tracked actor's
		// observed health delta vs the captured event stream; logs suspected
		// missed damage. Main thread.
		void AuditTick();

		// One captured engine callback; recorded only while devbench is enabled.
		struct TraceEntry
		{
			enum class Type : std::uint8_t
			{
				kRaw,
				kHit,
			};
			Type type{ Type::kRaw };
			double seconds{ 0.0 };
			std::uint8_t source{ 0 };
			std::uint8_t av{ 0 };
			RE::FormID victimID{ 0 };
			RE::FormID attackerID{ 0 };
			RE::FormID mgefID{ 0 };
			float amount{ 0.0f };
			bool deadAtQueue{ false };
			bool deadAtProcess{ false };
			float totalDamage{ 0.0f };
			float physicalDamage{ 0.0f };
			float knockback{ 0.0f };
			float percentBlocked{ 0.0f };
			float resistedPhysical{ 0.0f };
			float resistedTyped{ 0.0f };
			float stagger{ 0.0f };
			RE::FormID weaponID{ 0 };
			float reflected{ 0.0f };
			float sneakMult{ 1.0f };
			std::uint32_t hitFlags{ 0 };
		};

		std::vector<TraceEntry> GetTrace();

		RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override;

	private:
		using Clock = std::chrono::steady_clock;

		// Raw hook payload; everything an OnXxx may capture on a foreign thread.
		struct RawEvent
		{
			enum class Source : std::uint8_t
			{
				kWeaponHit,  // HandleHealthDamage (attacker known)
				kAVDelta,    // ActorValueOwner::ModActorValue fallback (scripts, falls, traps)
				kEffect,     // ValueModifierEffect::ModifyActorValue (mgef + caster known)
				kResource,   // magicka/stamina kDamage delta via the AV fallback
			};
			Source source{ Source::kAVDelta };
			RE::ActorValue av{ RE::ActorValue::kHealth };
			RE::FormID victimID{ 0 };
			RE::FormID attackerID{ 0 };
			RE::FormID mgefID{ 0 };
			float amount{ 0.0f };  // signed engine delta (negative = damage)
			bool victimDead{ false };
			bool overTime{ false };
		};

		struct PendingHit
		{
			Clock::time_point stamp;
			RE::NiPoint3 hitPos;  // engine contact point, for locational lookup
			RE::FormID weaponID{ 0 };
			float physicalDamage{ 0.0f };
			float resistedPhysical{ 0.0f };  // armor
			float resistedTyped{ 0.0f };     // enchant payload resisted
			float blockedDamage{ 0.0f };     // reconstructed from percentBlocked
			float totalDamage{ 0.0f };       // passed to DamageHealth, before the difficulty multiplier
			float ampMult{ 0.0f };
			float reflected{ 0.0f };
			float sneakMult{ 1.0f };
			bool ranged{ false };  // bow/crossbow hit — locational + amplification apply
			HitFlags flags;
		};

		struct RecentMagic
		{
			Clock::time_point stamp;
			DamageKind kind{ DamageKind::kMagic };
			RE::FormID casterID{ 0 };
			RE::FormID mgefID{ 0 };
		};

		// Sub-threshold tick pool: concentration effects apply in sub-point
		// per-frame deltas (damage and healing alike); pool them until they
		// are worth a number. Stale pools reset rather than emit, which is
		// also what silences natural regen (it never clears the threshold
		// within a window).
		struct TickAccum
		{
			Clock::time_point windowStart;
			float amount{ 0.0f };
			float mitigated{ 0.0f };

			// Like Accumulate, but only releases once per interval, so a
			// continuous effect reads as steady ticks instead of a running sum.
			bool AccumulateTicked(Clock::time_point a_now, float a_amount, float a_mitigated,
				float a_threshold, std::chrono::milliseconds a_staleWindow, std::chrono::duration<float> a_interval,
				float& a_outAmount, float& a_outMitigated)
			{
				if (amount == 0.0f || a_now - windowStart > a_staleWindow + a_interval) {
					windowStart = a_now;
					amount = 0.0f;
					mitigated = 0.0f;
				}
				amount += a_amount;
				mitigated += a_mitigated;
				if (amount < a_threshold || a_now - windowStart < a_interval) {
					return false;
				}
				a_outAmount = amount;
				a_outMitigated = mitigated;
				amount = 0.0f;
				mitigated = 0.0f;
				windowStart = a_now;
				return true;
			}

			// Returns true once the pooled total clears the threshold, with
			// the totals in the out-params; the pool resets but the window
			// stays open so an ongoing stream keeps merging.
			bool Accumulate(Clock::time_point a_now, float a_amount, float a_mitigated,
				float a_threshold, std::chrono::milliseconds a_window,
				float& a_outAmount, float& a_outMitigated)
			{
				if (amount == 0.0f || a_now - windowStart > a_window) {
					windowStart = a_now;
					amount = 0.0f;
					mitigated = 0.0f;
				}
				amount += a_amount;
				mitigated += a_mitigated;
				if (amount < a_threshold) {
					return false;
				}
				a_outAmount = amount;
				a_outMitigated = mitigated;
				amount = 0.0f;
				mitigated = 0.0f;
				windowStart = a_now;
				return true;
			}
		};

		static DamageKind ClassifyMagicKind(const RE::EffectSetting* a_mgef);

		static float WeaponCritBonus(RE::Actor* a_attacker, RE::Actor* a_victim, RE::FormID a_weaponID);

		void QueueRaw(const RawEvent& a_event);
		void RecordTrace(TraceEntry a_entry);

		// Pool key: one accumulator per victim and damage kind.
		static std::uint64_t PoolKey(RE::FormID a_victimID, DamageKind a_kind)
		{
			return static_cast<std::uint64_t>(a_victimID) | (static_cast<std::uint64_t>(std::to_underlying(a_kind)) << 32);
		}

		// Fresh-entry lookup in the recent hostile-apply map. Returns true
		// when an unexpired entry exists; outputs its kind, effect, and
		// resolved caster (null when the apply had none).
		bool FindRecentMagic(RE::FormID a_victimID, DamageKind& a_kindOut, RE::Actor*& a_attackerOut, RE::FormID* a_mgefOut = nullptr);

		// Main-thread processing per source.
		void ProcessWeaponHit(const RawEvent& a_raw, RE::Actor* a_victim);
		void ProcessAVDelta(const RawEvent& a_raw, RE::Actor* a_victim);
		void ProcessEffect(const RawEvent& a_raw, RE::Actor* a_victim);
		void ProcessResource(const RawEvent& a_raw, RE::Actor* a_victim);
		void ProcessRestore(RE::Actor* a_target, float a_amount, RE::Actor* a_healer);

		// Shared tail: analytics, display filters, spawn. Main thread.
		// a_sourceID identifies the weapon or magic effect for the analytics
		// drill-down (0 = unarmed/untracked).
		void EmitDamage(RE::Actor* a_victim, RE::Actor* a_attacker, float a_amount, DamageKind a_kind, const HitFlags& a_flags, float a_mitigated,
			float a_ampMult = 0.0f, const char* a_location = nullptr,
			MitigationLabel a_mitLabel = MitigationLabel::kResisted, RE::FormID a_sourceID = 0, const HitExtras& a_extras = {},
			bool a_suppressPopup = false);

		// EmitDamage with sub-threshold tick pooling (concentration spells
		// apply in sub-point per-frame deltas).
		void EmitPooledDamage(RE::Actor* a_victim, RE::Actor* a_attacker, float a_amount, DamageKind a_kind, float a_mitigated = 0.0f, RE::FormID a_sourceID = 0, bool a_overTime = false);

		// Correlation windows: a weapon hit's HandleHealthDamage lands the same
		// frame; magic apply can precede its first damage tick by a bit longer.
		static constexpr auto kHitWindow = std::chrono::milliseconds(200);
		static constexpr auto kMagicWindow = std::chrono::milliseconds(1500);
		// Heal ticks that don't reach the display threshold within this window
		// are natural regen trickle — discard them.
		static constexpr auto kHealWindow = std::chrono::milliseconds(1500);

		HitClaim TakeClaim(RE::FormID a_victimID);

		// A claim is made by the hit's own handler, within the same frame or two
		// of the hit; the margin only absorbs a slow frame.
		static constexpr auto kClaimWindow = std::chrono::milliseconds(500);

		void AuditRecord(RE::FormID a_victimID, float a_delta);  // negative = damage

		// Raw ring: written by hooks on any thread, drained on the main thread.
		static constexpr std::size_t kRawCapacity = 256;
		std::mutex _rawLock;
		std::array<RawEvent, kRawCapacity> _raw{};
		std::size_t _rawCount{ 0 };

		std::atomic<bool> _evadePending{ false };
		std::atomic<std::int64_t> _lastEvadeTicks{ 0 };

		std::mutex _lock;  // guards the maps below (event sink writes off-main)
		std::unordered_map<RE::FormID, PendingHit> _pendingHits;
		std::unordered_map<RE::FormID, RecentMagic> _recentMagic;
		HitClaimLedger _claims{ kClaimWindow };
		std::unordered_map<std::uint64_t, TickAccum> _tickAccums;  // keyed by PoolKey (healing included)

		static constexpr std::size_t kTraceCapacity = 4096;
		std::deque<TraceEntry> _trace;  // guarded by _lock
		Clock::time_point _traceStart{};

		// Audit state (bDeltaAudit only; main thread)
		struct AuditEntry
		{
			float lastHealth{ -1.0f };
			float capturedNet{ 0.0f };  // sum of captured deltas since last tick
		};
		std::unordered_map<RE::FormID, AuditEntry> _audit;
		Clock::time_point _lastAudit{};
	};
}
