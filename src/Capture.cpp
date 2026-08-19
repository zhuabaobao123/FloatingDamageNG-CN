// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#include "Capture.h"

#include "CombatLog.h"
#include "NumberManager.h"
#include "Settings.h"

namespace FDNG
{
	namespace
	{
		// Windows never maps the first 64 KiB of a process's address space, so
		// any real Actor* is guaranteed to be >= this.
		constexpr std::uintptr_t kMinValidUserAddress = 0x10000;

		RE::NiPoint3 GetAnchorPos(RE::Actor* a_actor)
		{
			if (const auto middle = a_actor->GetMiddleHighProcess(); middle && middle->headNode) {
				return middle->headNode->world.translate;
			}
			auto pos = a_actor->GetPosition();
			pos.z += a_actor->GetHeight();
			return pos;
		}

		// Estimated fraction of incoming magic the victim resists, from their
		// live resist AVs (elemental x magic resist, engine-capped at 85%
		// each; poison ignores magic resist). Robust for both instant hits
		// and concentration ticks, unlike comparing applied vs magnitude.
		float ResistFraction(RE::Actor* a_victim, const RE::EffectSetting* a_mgef)
		{
			if (!a_mgef) {
				return 0.0f;
			}
			const auto avo = a_victim->AsActorValueOwner();
			const auto resistAV = a_mgef->data.resistVariable;
			if (!avo || resistAV == RE::ActorValue::kNone) {
				return 0.0f;
			}
			const auto capped = [&](RE::ActorValue a_av) {
				return std::clamp(avo->GetActorValue(a_av), 0.0f, 85.0f) / 100.0f;
			};
			const float elemental = capped(resistAV);
			const float magic = resistAV == RE::ActorValue::kPoisonResist ? 0.0f : capped(RE::ActorValue::kResistMagic);
			return std::clamp(1.0f - (1.0f - elemental) * (1.0f - magic), 0.0f, 0.95f);
		}

		// Nearest named skeleton node to the engine's hit position (main
		// thread, victim loaded) - how the hit location is identified since
		// HitData carries no projectile reference.
		const RE::NiAVObject* FindClosestNode(RE::NiAVObject* a_root, const RE::NiPoint3& a_pos, float& a_bestDistSq)
		{
			if (!a_root) {
				return nullptr;
			}
			// Bones only (NiNodes): leaf geometry carries mesh names like
			// 'giant_heavy_cloth:0' that would shadow the skeleton names the
			// location patterns match.
			const auto node = a_root->AsNode();
			if (!node) {
				return nullptr;
			}
			// Lodged projectiles and equipment attach their own node subtrees
			// to the skeleton (e.g. "SteelArrowFlight" from a stuck arrow) and
			// can win "nearest", masking the struck body part. Skip the whole
			// subtree: nothing under a foreign attachment is a body part.
			static constexpr std::array kForeignNodes{ "arrow", "bolt", "flight", "weapon", "shield", "quiver" };
			const RE::NiAVObject* best = nullptr;
			if (!a_root->name.empty()) {
				const std::string_view name{ a_root->name.c_str() };
				const bool foreign = std::ranges::any_of(kForeignNodes, [&](const char* w) {
					return std::ranges::search(name, std::string_view{ w }, [](char a, char b) {
						return std::tolower(static_cast<unsigned char>(a)) == b;
					}).begin() != name.end();
				});
				if (foreign) {
					return nullptr;
				}
				const float d = a_root->world.translate.GetSquaredDistance(a_pos);
				if (d < a_bestDistSq) {
					a_bestDistSq = d;
					best = a_root;
				}
			}
			for (const auto& child : node->GetChildren()) {
				if (const auto found = FindClosestNode(child.get(), a_pos, a_bestDistSq)) {
					best = found;
				}
			}
			return best;
		}

		OriginTier ClassifyOrigin(RE::Actor* a_victim, RE::Actor* a_attacker)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (a_victim == player) {
				return OriginTier::kPlayerVictim;
			}
			if (a_attacker == player) {
				return OriginTier::kPlayer;
			}
			if (a_attacker && a_attacker->IsPlayerTeammate()) {
				return OriginTier::kFollower;
			}
			return OriginTier::kNPC;
		}
	}

	Capture* Capture::GetSingleton()
	{
		static Capture singleton;
		return &singleton;
	}

	void Capture::Register()
	{
		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESMagicEffectApplyEvent>(this);
			logger::info("Registered TESMagicEffectApplyEvent sink.");
		}
	}

	DamageKind Capture::ClassifyMagicKind(const RE::EffectSetting* a_mgef)
	{
		if (!a_mgef) {
			return DamageKind::kMagic;
		}
		switch (a_mgef->data.resistVariable) {
		case RE::ActorValue::kResistFire:
			return DamageKind::kFire;
		case RE::ActorValue::kResistFrost:
			return DamageKind::kFrost;
		case RE::ActorValue::kResistShock:
			return DamageKind::kShock;
		case RE::ActorValue::kPoisonResist:
			return DamageKind::kPoison;
		default:
			break;
		}
		if (a_mgef->data.archetype == RE::EffectSetting::Archetype::kValueModifier &&
			a_mgef->data.primaryAV == RE::ActorValue::kHealth &&
			!a_mgef->IsDetrimental()) {
			return DamageKind::kHealing;
		}
		return DamageKind::kMagic;
	}

	RE::BSEventNotifyControl Capture::ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*)
	{
		if (!a_event || !a_event->target) {
			return RE::BSEventNotifyControl::kContinue;
		}
		const auto mgef = RE::TESForm::LookupByID<RE::EffectSetting>(a_event->magicEffect);
		if (!mgef || !(mgef->IsHostile() || mgef->IsDetrimental())) {
			return RE::BSEventNotifyControl::kContinue;
		}
		const auto kind = ClassifyMagicKind(mgef);
		if (kind == DamageKind::kMagic && mgef->data.primaryAV != RE::ActorValue::kHealth) {
			// Untyped and not health-targeting - not useful for attribution.
			return RE::BSEventNotifyControl::kContinue;
		}

		const auto casterRef = a_event->caster.get();
		const auto caster = casterRef ? casterRef->As<RE::Actor>() : nullptr;

		std::scoped_lock lk{ _lock };
		_recentMagic[a_event->target->GetFormID()] =
			RecentMagic{ Clock::now(), kind, caster ? caster->GetFormID() : 0, a_event->magicEffect };
		return RE::BSEventNotifyControl::kContinue;
	}

	void Capture::OnHitData(const RE::HitData& a_hitData)
	{
		const auto target = a_hitData.target.get();
		if (!target) {
			return;
		}

		PendingHit pending;
		pending.stamp = Clock::now();
		pending.physicalDamage = a_hitData.physicalDamage;
		pending.resistedPhysical = std::max(0.0f, a_hitData.resistedPhysicalDamage);
		pending.resistedTyped = std::max(0.0f, a_hitData.resistedTypedDamage);
		pending.weaponID = a_hitData.weapon ? a_hitData.weapon->GetFormID() : 0;
		pending.flags.critical = a_hitData.flags.any(RE::HitData::Flag::kCritical);
		pending.flags.blocked = a_hitData.flags.any(RE::HitData::Flag::kBlocked);
		pending.flags.sneak = a_hitData.flags.any(RE::HitData::Flag::kSneakAttack);
		pending.flags.powerAttack = a_hitData.flags.any(RE::HitData::Flag::kPowerAttack);
		pending.flags.bash = a_hitData.flags.any(RE::HitData::Flag::kBash, RE::HitData::Flag::kTimedBash);

		// totalDamage is post-block; reconstruct the blocked portion so the
		// mitigation subtext and analytics carry the true amount.
		if (pending.flags.blocked) {
			const float pb = std::clamp(a_hitData.percentBlocked, 0.0f, 0.9f);
			pending.blockedDamage = a_hitData.totalDamage * pb / (1.0f - pb);
		}

		// Ranged hits get locational + amplification treatment. The projectile
		// itself is unreachable from HitData (sourceRef is not the projectile
		// on current runtimes), so location is resolved later from hitPosition
		// against the victim's skeleton - ALD's fallback approach.
		const auto settings = Settings::GetSingleton();
		pending.hitPos = a_hitData.hitPosition;
		pending.ranged = a_hitData.weapon &&
		                 (a_hitData.weapon->IsBow() || a_hitData.weapon->IsCrossbow());
		if (settings->debugLog) {
			logger::debug("HitData: target={:08X} ranged={} totalDmg={:.1f}",
				target->GetFormID(), pending.ranged, a_hitData.totalDamage);
		}

		if (pending.ranged && settings->showAmplification) {
			// Locational mods scale totalDamage but not physicalDamage, so a
			// total exceeding the physical+crit baseline implies an external
			// multiplier. Heuristic - display styling only.
			const float critMult = pending.flags.critical ? std::max(a_hitData.criticalDamageMult, 1.0f) : 1.0f;
			const float baseline = a_hitData.physicalDamage * critMult;
			if (baseline > 0.1f) {
				const float amp = a_hitData.totalDamage / baseline;
				if (amp >= settings->amplificationThreshold) {
					pending.ampMult = amp;
				}
			}
		}

		std::scoped_lock lk{ _lock };
		_pendingHits[target->GetFormID()] = pending;
	}

	// ---- Hook entries: queue POD only (any thread) -------------------------

	void Capture::QueueRaw(const RawEvent& a_event)
	{
		std::scoped_lock lk{ _rawLock };
		if (_rawCount < kRawCapacity) {
			_raw[_rawCount++] = a_event;
		}
	}

	bool Capture::FindRecentMagic(RE::FormID a_victimID, DamageKind& a_kindOut, RE::Actor*& a_attackerOut, RE::FormID* a_mgefOut)
	{
		std::scoped_lock lk{ _lock };
		const auto it = _recentMagic.find(a_victimID);
		if (it == _recentMagic.end() || Clock::now() - it->second.stamp >= kMagicWindow) {
			return false;
		}
		a_kindOut = it->second.kind;
		a_attackerOut = it->second.casterID != 0 ? RE::TESForm::LookupByID<RE::Actor>(it->second.casterID) : nullptr;
		if (a_mgefOut) {
			*a_mgefOut = it->second.mgefID;
		}
		return true;
	}

	void Capture::OnHealthDamage(RE::Actor* a_victim, RE::Actor* a_attacker, float a_damage)
	{
		if (!a_victim || a_damage >= 0.0f) {
			return;
		}
		// a_attacker comes from whoever called into this hooked vtable slot,
		// not just the engine - treat an implausible pointer as null instead
		// of dereferencing it.
		if (a_attacker && reinterpret_cast<std::uintptr_t>(a_attacker) < kMinValidUserAddress) {
			static thread_local bool warned = false;
			if (!warned) {
				warned = true;
				logger::warn("OnHealthDamage: implausible attacker pointer {:p} ignored (caller bug?)", static_cast<void*>(a_attacker));
			}
			a_attacker = nullptr;
		}
		RawEvent raw;
		raw.source = RawEvent::Source::kWeaponHit;
		raw.victimID = a_victim->GetFormID();
		raw.attackerID = a_attacker ? a_attacker->GetFormID() : 0;
		raw.amount = a_damage;
		QueueRaw(raw);
	}

	void Capture::OnHealthRestore(RE::Actor* a_target, float a_amount)
	{
		if (!a_target || a_amount <= 0.0f) {
			return;
		}
		RawEvent raw;
		raw.source = RawEvent::Source::kAVDelta;
		raw.victimID = a_target->GetFormID();
		raw.amount = a_amount;
		QueueRaw(raw);
	}

	void Capture::OnMagicDamage(RE::Actor* a_victim, float a_amount)
	{
		if (!a_victim || a_amount <= 0.0f) {
			return;
		}
		RawEvent raw;
		raw.source = RawEvent::Source::kAVDelta;
		raw.victimID = a_victim->GetFormID();
		raw.amount = -a_amount;
		QueueRaw(raw);
	}

	void Capture::OnResourceDamage(RE::Actor* a_victim, RE::ActorValue a_value, float a_amount)
	{
		if (!a_victim || a_amount <= 0.0f) {
			return;
		}
		RawEvent raw;
		raw.source = RawEvent::Source::kResource;
		raw.av = a_value;
		raw.victimID = a_victim->GetFormID();
		raw.amount = -a_amount;
		QueueRaw(raw);
	}

	void Capture::OnEffectModify(RE::ValueModifierEffect* a_effect, RE::Actor* a_target, float a_value, RE::ActorValue a_actorValue)
	{
		if (!a_effect || !a_target) {
			return;
		}
		RawEvent raw;
		raw.source = RawEvent::Source::kEffect;
		// The engine often passes kNone here and applies to the effect's own
		// AV - resolve from the member in that case (plain read, safe here).
		raw.av = a_actorValue != RE::ActorValue::kNone ? a_actorValue : a_effect->actorValue;
		raw.victimID = a_target->GetFormID();
		raw.amount = a_value;
		if (raw.amount == 0.0f) {
			return;  // zero applies carry no display information (see ProcessEffect)
		}
		// Safe on this thread: the effect is alive inside its own vfunc, the
		// handle table resolve is thread-safe, and GetFormID is a plain read.
		if (const auto caster = a_effect->caster.get()) {
			raw.attackerID = caster->GetFormID();
		}
		if (const auto mgef = a_effect->GetBaseObject()) {
			raw.mgefID = mgef->GetFormID();
		}
		QueueRaw(raw);
	}

	// ---- Main-thread processing --------------------------------------------

	void Capture::ProcessQueued()
	{
		std::array<RawEvent, kRawCapacity> batch;
		std::size_t count = 0;
		{
			std::scoped_lock lk{ _rawLock };
			count = _rawCount;
			if (count == 0) {
				return;
			}
			std::copy_n(_raw.begin(), count, batch.begin());
			_rawCount = 0;
		}

		const auto settings = Settings::GetSingleton();
		for (std::size_t i = 0; i < count; ++i) {
			const auto& raw = batch[i];
			const auto victim = RE::TESForm::LookupByID<RE::Actor>(raw.victimID);
			if (!victim) {
				continue;
			}
			if (settings->debugLog) {
				logger::debug("Raw: src={} av={} victim={:08X} attacker={:08X} amount={:+.2f}",
					std::to_underlying(raw.source), std::to_underlying(raw.av),
					raw.victimID, raw.attackerID, raw.amount);
			}
			switch (raw.source) {
			case RawEvent::Source::kWeaponHit:
				ProcessWeaponHit(raw, victim);
				break;
			case RawEvent::Source::kAVDelta:
				ProcessAVDelta(raw, victim);
				break;
			case RawEvent::Source::kEffect:
				ProcessEffect(raw, victim);
				break;
			case RawEvent::Source::kResource:
				ProcessResource(raw, victim);
				break;
			}
		}
	}

	void Capture::ProcessWeaponHit(const RawEvent& a_raw, RE::Actor* a_victim)
	{
		const auto amount = -a_raw.amount;
		if (amount < Settings::GetSingleton()->minDamageToShow) {
			return;
		}

		// The pending HitData contributes crit/block/sneak/bash flags,
		// mitigation, weapon identity, hit position, and the amplification
		// estimate.
		HitFlags flags;
		float mitigated = 0.0f;
		float ampMult = 0.0f;
		char location[16]{};
		auto mitLabel = MitigationLabel::kArmor;
		bool ranged = false;
		RE::FormID weaponID = 0;
		RE::NiPoint3 hitPos;
		{
			std::scoped_lock lk{ _lock };
			if (const auto it = _pendingHits.find(a_raw.victimID);
				it != _pendingHits.end() && Clock::now() - it->second.stamp < kHitWindow) {
				flags = it->second.flags;
				mitigated = it->second.resistedPhysical + it->second.resistedTyped + it->second.blockedDamage;
				// Word by dominant cause: a real block > armor soak > the
				// enchant portion being magically resisted.
				if (flags.blocked) {
					mitLabel = MitigationLabel::kBlocked;
				} else if (it->second.resistedTyped > it->second.resistedPhysical) {
					mitLabel = MitigationLabel::kResisted;
				}
				ampMult = it->second.ampMult;
				ranged = it->second.ranged;
				weaponID = it->second.weaponID;
				hitPos = it->second.hitPos;
				_pendingHits.erase(it);
			}
		}

		// Locational tag for ranged hits: nearest skeleton node to the
		// engine's contact point, matched against the configured patterns.
		const auto settings = Settings::GetSingleton();
		if (ranged && settings->showHitLocation && a_victim->Is3DLoaded()) {
			constexpr float kNodeSearchRadius = 55.0f;  // reject contact points farther off the skeleton, game units
			float bestDistSq = kNodeSearchRadius * kNodeSearchRadius;
			if (const auto node = FindClosestNode(a_victim->Get3D(), hitPos, bestDistSq)) {
				if (settings->debugLog) {
					logger::debug("Hit location: node='{}' dist={:.1f}", node->name.c_str(), std::sqrt(bestDistSq));
				}
				for (const auto& tag : settings->locationTags) {
					if (std::regex_match(node->name.c_str(), tag.pattern)) {
						std::snprintf(location, sizeof(location), "%s", tag.label.c_str());
						break;
					}
				}
			}
		}

		const auto attacker = a_raw.attackerID ? RE::TESForm::LookupByID<RE::Actor>(a_raw.attackerID) : nullptr;
		AuditRecord(a_raw.victimID, a_raw.amount);
		EmitDamage(a_victim, attacker, amount, DamageKind::kPhysical, flags, mitigated, ampMult, location, mitLabel, weaponID);
	}

	void Capture::ProcessAVDelta(const RawEvent& a_raw, RE::Actor* a_victim)
	{
		if (a_raw.amount > 0.0f) {
			ProcessRestore(a_victim, a_raw.amount, nullptr);
			return;
		}

		// Classify by the most recent hostile apply event; anything unmatched
		// is falls/traps/script damage (effect damage arrives as kEffect with
		// exact attribution).
		DamageKind kind = DamageKind::kPhysical;
		RE::Actor* attacker = nullptr;
		RE::FormID mgefID = 0;
		FindRecentMagic(a_raw.victimID, kind, attacker, &mgefID);

		AuditRecord(a_raw.victimID, a_raw.amount);
		EmitPooledDamage(a_victim, attacker, -a_raw.amount, kind, 0.0f, mgefID);
	}

	void Capture::ProcessEffect(const RawEvent& a_raw, RE::Actor* a_victim)
	{
		const auto settings = Settings::GetSingleton();
		const auto mgef = a_raw.mgefID ? RE::TESForm::LookupByID<RE::EffectSetting>(a_raw.mgefID) : nullptr;
		const auto caster = a_raw.attackerID ? RE::TESForm::LookupByID<RE::Actor>(a_raw.attackerID) : nullptr;

		if (a_raw.av == RE::ActorValue::kHealth) {
			if (a_raw.amount > 0.0f) {
				ProcessRestore(a_victim, a_raw.amount, caster);
				return;
			}
			if (a_raw.amount == 0.0f) {
				// Zero-value applies are NOT a reliable full-resist signal -
				// they occur for unrelated reasons and produced spurious
				// RESISTED tags. Immune targets suppress the effect earlier
				// in the pipeline instead; drop these.
				return;
			}
			auto kind = ClassifyMagicKind(mgef);
			if (kind == DamageKind::kHealing) {
				kind = DamageKind::kMagic;
			}
			// Resisted-portion estimate for the subtext, from the victim's
			// live resist values: pre-resist = applied / (1 - f).
			float mitigated = 0.0f;
			if (settings->showMitigation) {
				const float f = ResistFraction(a_victim, mgef);
				if (f > 0.01f) {
					mitigated = -a_raw.amount * f / (1.0f - f);
				}
			}
			AuditRecord(a_raw.victimID, a_raw.amount);
			EmitPooledDamage(a_victim, caster, -a_raw.amount, kind, mitigated, a_raw.mgefID);
			return;
		}

		if (a_raw.amount < 0.0f &&
			((a_raw.av == RE::ActorValue::kMagicka && settings->showMagickaDamage) ||
				(a_raw.av == RE::ActorValue::kStamina && settings->showStaminaDamage))) {
			// Hostile-only: self costs never come through effects, but
			// beneficial effects with resource costs do - require a
			// detrimental effect from someone else.
			if (!mgef || !(mgef->IsHostile() || mgef->IsDetrimental()) || !caster || caster == a_victim) {
				return;
			}
			const auto kind = a_raw.av == RE::ActorValue::kMagicka ? DamageKind::kMagickaDrain : DamageKind::kStaminaDrain;
			EmitPooledDamage(a_victim, caster, -a_raw.amount, kind, 0.0f, a_raw.mgefID);
		}
	}

	void Capture::ProcessResource(const RawEvent& a_raw, RE::Actor* a_victim)
	{
		// Hostile attribution is mandatory: the caster's own spell costs,
		// sprinting, and power attacks arrive as identical deltas.
		DamageKind ignored = DamageKind::kMagic;
		RE::Actor* attacker = nullptr;
		RE::FormID mgefID = 0;
		FindRecentMagic(a_raw.victimID, ignored, attacker, &mgefID);
		if (!attacker || attacker == a_victim) {
			return;
		}

		const auto kind = a_raw.av == RE::ActorValue::kMagicka ? DamageKind::kMagickaDrain : DamageKind::kStaminaDrain;
		EmitPooledDamage(a_victim, attacker, -a_raw.amount, kind, 0.0f, mgefID);
	}

	void Capture::ProcessRestore(RE::Actor* a_target, float a_amount, RE::Actor* a_healer)
	{
		const auto settings = Settings::GetSingleton();
		if (!settings->showHealing) {
			return;
		}

		const auto origin = ClassifyOrigin(a_target, a_healer);
		if (origin == OriginTier::kNPC && !settings->showNPCOnNPCDamage) {
			return;
		}

		// Only heals that repair actual damage are visible in-game; a
		// full-health actor's restore calls are engine bookkeeping.
		if (a_target->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth) >= -0.01f) {
			return;
		}

		AuditRecord(a_target->GetFormID(), a_amount);

		// Pool heal ticks until they clear the regen-noise threshold.
		float emit = 0.0f;
		float ignoredMitigated = 0.0f;
		{
			std::scoped_lock lk{ _lock };
			if (!_tickAccums[PoolKey(a_target->GetFormID(), DamageKind::kHealing)].Accumulate(
					Clock::now(), a_amount, 0.0f, settings->minHealToShow, kHealWindow, emit, ignoredMitigated)) {
				return;
			}
		}

		CombatLog::GetSingleton()->RecordHeal(a_target, emit);

		// Must stay after RecordHeal: analytics records every application
		// regardless of display filters.
		if (a_target->IsDead()) {
			return;
		}

		if (!settings->enableFloatingDamage) {
			return;
		}

		DamageEvent event;
		event.victimID = a_target->GetFormID();
		event.anchor = GetAnchorPos(a_target);
		event.amount = emit;
		event.kind = DamageKind::kHealing;
		event.origin = origin;

		if (settings->debugLog) {
			logger::debug("Heal: target={:08X} amount={:.1f} origin={}",
				event.victimID, event.amount, std::to_underlying(event.origin));
		}

		NumberManager::GetSingleton()->Enqueue(event);
	}

	void Capture::EmitPooledDamage(RE::Actor* a_victim, RE::Actor* a_attacker, float a_amount, DamageKind a_kind, float a_mitigated, RE::FormID a_sourceID)
	{
		const auto settings = Settings::GetSingleton();

		// Concentration spells tick in sub-point deltas; pool them per
		// victim+type until they clear the display threshold.
		float emit = a_amount;
		float emitMitigated = a_mitigated;
		if (a_amount < settings->minDamageToShow) {
			std::scoped_lock lk{ _lock };
			if (!_tickAccums[PoolKey(a_victim->GetFormID(), a_kind)].Accumulate(
					Clock::now(), a_amount, a_mitigated, settings->minDamageToShow, kMagicWindow, emit, emitMitigated)) {
				return;
			}
		}

		EmitDamage(a_victim, a_attacker, emit, a_kind, HitFlags{}, emitMitigated, 0.0f, nullptr, MitigationLabel::kResisted, a_sourceID);
	}

	void Capture::EmitDamage(RE::Actor* a_victim, RE::Actor* a_attacker, float a_amount, DamageKind a_kind, const HitFlags& a_flags, float a_mitigated,
		float a_ampMult, const char* a_location, MitigationLabel a_mitLabel, RE::FormID a_sourceID)
	{
		const auto settings = Settings::GetSingleton();
		const auto origin = ClassifyOrigin(a_victim, a_attacker);

		// Analytics sees every application, independent of the display filters.
		CombatLog::GetSingleton()->RecordDamage(a_attacker, a_victim, a_amount, a_kind, a_flags, a_mitigated, a_sourceID);

		// Must stay after RecordDamage: analytics records every application
		// regardless of display filters.
		if (a_victim->IsDead()) {
			return;
		}

		DamageEvent event;
		event.victimID = a_victim->GetFormID();
		event.anchor = GetAnchorPos(a_victim);
		event.amount = a_amount;
		event.mitigated = a_mitigated;
		event.ampMult = a_ampMult;
		if (a_location && a_location[0]) {
			std::snprintf(event.location, sizeof(event.location), "%s", a_location);
		}
		event.mitLabel = a_mitLabel;
		event.kind = a_kind;
		event.origin = origin;
		event.flags = a_flags;

		if (!settings->enableFloatingDamage) {
			return;
		}
		switch (origin) {
		case OriginTier::kPlayer:
			if (!settings->showPlayerDamageDealt) {
				return;
			}
			break;
		case OriginTier::kFollower:
			if (!settings->showFollowerDamageDealt) {
				return;
			}
			break;
		case OriginTier::kNPC:
			if (!settings->showNPCOnNPCDamage) {
				return;
			}
			break;
		case OriginTier::kPlayerVictim:
			// The renderer pins/suppresses player numbers per camera mode.
			if (!settings->showPlayerDamageTaken) {
				return;
			}
			break;
		}

		if (settings->debugLog) {
			logger::debug("Damage: victim={:08X} amount={:.1f} kind={} origin={} crit={}",
				event.victimID, event.amount, std::to_underlying(event.kind),
				std::to_underlying(event.origin), event.flags.critical);
		}

		NumberManager::GetSingleton()->Enqueue(event);
	}

	void Capture::AuditRecord(RE::FormID a_victimID, float a_delta)
	{
		if (!Settings::GetSingleton()->deltaAudit) {
			return;
		}
		std::scoped_lock lk{ _lock };
		_audit[a_victimID].capturedNet += a_delta;
	}

	void Capture::AuditTick()
	{
		if (!Settings::GetSingleton()->deltaAudit) {
			return;
		}
		const auto now = Clock::now();
		if (now - _lastAudit < std::chrono::seconds(1)) {
			return;
		}
		_lastAudit = now;

		std::scoped_lock lk{ _lock };
		for (auto it = _audit.begin(); it != _audit.end();) {
			const auto actor = RE::TESForm::LookupByID<RE::Actor>(it->first);
			if (!actor || actor->IsDead()) {
				it = _audit.erase(it);
				continue;
			}
			const float health = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			auto& entry = it->second;
			if (entry.lastHealth >= 0.0f) {
				// Observed net = captured net + regen (regen >= 0). If the
				// observed change is meaningfully below the captured floor,
				// damage bypassed both capture hooks.
				const float observedNet = health - entry.lastHealth;
				const float floorNet = entry.capturedNet;
				if (observedNet < floorNet - 5.0f) {
					logger::warn("[audit] '{}' [{:08X}]: observed {:+.1f} hp vs captured {:+.1f} - ~{:.1f} damage NOT captured",
						actor->GetName(), it->first, observedNet, floorNet, floorNet - observedNet);
				}
			}
			entry.lastHealth = health;
			entry.capturedNet = 0.0f;
			++it;
		}
	}
}
