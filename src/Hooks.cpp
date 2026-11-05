// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#include "Hooks.h"

#include "Capture.h"
#include "Settings.h"

namespace FDNG::Hooks
{
	namespace
	{
		// Actor::HandleHealthDamage — fires for every health damage application
		// (melee, ranged, magic, DoT ticks, traps, falls) with the true
		// post-mitigation amount (a_damage is negative for damage). It is
		// SKYRIM_REL_VR_VIRTUAL: slot 0x104 on SE/AE, 0x106 on VR. Hooked on
		// both concrete actor vtables; each needs its own original-func slot.
		// Set while the engine is inside HandleHealthDamage on this thread, so
		// the nested ModActorValue write of the same damage isn't double-counted.
		thread_local int t_inHandleHealthDamage = 0;

		// Set while inside ValueModifierEffect::ModifyActorValue — that hook
		// records the delta with full effect attribution, so any nested AV
		// write must not be recorded again by the ModActorValue fallback.
		thread_local int t_inEffectModify = 0;

		// The effect's magnitude is pre-difficulty and pre-clamp; the nested
		// HandleHealthDamage reports the health actually applied.
		thread_local float t_effectAppliedHealth = 0.0f;
		thread_local const RE::Actor* t_effectTarget = nullptr;

		// Set while the engine runs a health-damage ModActorValue that was
		// already queued, so the HandleHealthDamage it dispatches from inside
		// is not queued a second time.
		thread_local int t_inModActorValue = 0;

		template <class TActor>
		struct HandleHealthDamage
		{
			static void thunk(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
			{
				// Effect ticks re-enter here nested inside ModifyActorValue;
				// the effect hook already captured them with better
				// attribution, so only genuine weapon blows queue.
				if (t_inEffectModify == 0 && t_inModActorValue == 0) {
					Capture::GetSingleton()->OnHealthDamage(a_this, a_attacker, a_damage);
				} else if (a_this == t_effectTarget && a_damage < 0.0f) {
					t_effectAppliedHealth += a_damage;
				}
				++t_inHandleHealthDamage;
				func(a_this, a_attacker, a_damage);
				--t_inHandleHealthDamage;
			}
			static inline REL::Relocation<decltype(thunk)> func;

			static void Install()
			{
				const std::size_t idx = REL::Module::IsVR() ? 0x106 : 0x104;
				REL::Relocation<std::uintptr_t> vtbl{ TActor::VTABLE[0] };
				func = vtbl.write_vfunc(idx, thunk);
			}
		};

		// ActorValueOwner::ModActorValue (vfunc 0x06) — RestoreActorValue routes
		// through it, so a positive kDamage delta on Health is a heal (per tick;
		// Capture accumulates and thresholds out regen trickle). ActorValueOwner
		// is VTABLE[5]: TESObjectREFR contributes secondary vtables first
		// (BSHandleRefObject, BSTEventSink<BSAnimationGraphEvent>,
		// IAnimationGraphManagerHolder), then MagicTarget, then ActorValueOwner
		// at offset 0xB0 (SE/VR) / 0xB8 (AE).
		template <class TActor>
		struct ModActorValue
		{
			static void thunk(RE::ActorValueOwner* a_this, RE::ACTOR_VALUE_MODIFIER a_modifier, RE::ActorValue a_value, float a_amount)
			{
				bool queuedHealthDamage = false;
				if (a_modifier == RE::ACTOR_VALUE_MODIFIER::kDamage && t_inEffectModify == 0) {
					// Ghidra-verified (disassembly of the engine's own ModActorValue body):
					// SE and VR share this this-adjustment; AE alone diverges - not the
					// usual "VR is the odd one out" pattern.
					const auto actor = stl::adjust_pointer<RE::Actor>(a_this, -static_cast<std::ptrdiff_t>(REL::Module::IsAE() ? 0xB8 : 0xB0));
					const auto settings = Settings::GetSingleton();
					if (a_value == RE::ActorValue::kHealth) {
						if (a_amount > 0.0f) {
							Capture::GetSingleton()->OnHealthRestore(actor, a_amount);
						} else if (a_amount < 0.0f && t_inHandleHealthDamage == 0) {
							// Health damage that skipped HandleHealthDamage —
							// the magic / DoT / script path.
							Capture::GetSingleton()->OnMagicDamage(actor, -a_amount);
							queuedHealthDamage = true;
						}
					} else if (a_amount < 0.0f &&
							   ((a_value == RE::ActorValue::kMagicka && settings->showMagickaDamage) ||
								   (a_value == RE::ActorValue::kStamina && settings->showStaminaDamage))) {
						Capture::GetSingleton()->OnResourceDamage(actor, a_value, -a_amount);
					}
				}
				t_inModActorValue += queuedHealthDamage;
				func(a_this, a_modifier, a_value, a_amount);
				t_inModActorValue -= queuedHealthDamage;
			}
			static inline REL::Relocation<decltype(thunk)> func;

			static void Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ TActor::VTABLE[5] };
				func = vtbl.write_vfunc(0x06, thunk);
			}
		};

		// ValueModifierEffect::ModifyActorValue (vfunc 0x20) — THE application
		// point for magic/enchant/potion deltas. The engine writes the AV
		// modifiers here without dispatching ActorValueOwner::ModActorValue,
		// so the AV hook alone misses effect damage entirely. Hooked on every
		// damage-carrying subclass vtable (each has its own slot).
		template <class TEffect>
		struct ModifyActorValueEffect
		{
			static void thunk(RE::ValueModifierEffect* a_this, RE::Actor* a_actor, float a_value, RE::ActorValue a_actorValue)
			{
				const auto outerTarget = t_effectTarget;
				const float outerApplied = t_effectAppliedHealth;
				t_effectTarget = a_actor;
				t_effectAppliedHealth = 0.0f;
				++t_inEffectModify;
				func(a_this, a_actor, a_value, a_actorValue);
				--t_inEffectModify;
				const float appliedHealth = t_effectAppliedHealth;
				t_effectTarget = outerTarget;
				t_effectAppliedHealth = outerApplied;
				Capture::GetSingleton()->OnEffectModify(a_this, a_actor, a_value, a_actorValue, appliedHealth);

				// Dual-value effects (vanilla frost/shock) apply their
				// secondary AV through a DIRECT call to the base worker inside
				// DualValueModifierEffect::ModifyActorValue — it bypasses every
				// vtable, so no hook can see it. This entry fires exactly once
				// per application, so synthesizing the secondary event here
				// reproduces the engine's own math (a_value × weight). The
				// secondary AV is never cached on the instance; the vfunc
				// getter reads it from the MGEF form.
				if (const auto dual = skyrim_cast<RE::DualValueModifierEffect*>(a_this)) {
					const auto secondaryAV = dual->GetAdditionalActorValue();
					if (secondaryAV != RE::ActorValue::kNone) {
						Capture::GetSingleton()->OnEffectModify(a_this, a_actor, a_value * dual->GetSecondaryAVWeight(), secondaryAV, 0.0f);
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;

			static void Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ TEffect::VTABLE[0] };
				func = vtbl.write_vfunc(0x20, thunk);
			}
		};

		// The engine's hit-dispatch call site (same one KillFeed hooks): the
		// live HitData carries damage split, mitigation, and crit/block/sneak
		// flags. The callee's signature differs between AE and SE/VR, so two
		// thunks exist and Install() picks by runtime (the NG DLL serves all
		// three from one binary).
		struct SendHitEventSE
		{
			static void thunk(RE::ScriptEventSourceHolder* a_holder,
				RE::NiPointer<RE::TESObjectREFR>& a_target,
				RE::NiPointer<RE::TESObjectREFR>& a_aggressor,
				RE::FormID a_source, RE::FormID a_projectile, RE::HitData& a_hitData)
			{
				Capture::GetSingleton()->OnHitData(a_hitData);
				func(a_holder, a_target, a_aggressor, a_source, a_projectile, a_hitData);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SendHitEventAE
		{
			static void thunk(RE::AIProcess* a_targetProcess, RE::HitData& a_hitData)
			{
				Capture::GetSingleton()->OnHitData(a_hitData);
				func(a_targetProcess, a_hitData);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
		constexpr std::array<const char*, 1> kSiteNames{ "melee/projectile" };

		// Dodge mods wrap this call as `orig(a) || ownCheck(a)`, so we must wrap last
		// (install late) to see their result; the engine bails before any HitData exists.
		template <int Site>
		struct IsGhostSite
		{
			static bool thunk(RE::Actor* a_victim)
			{
				const bool ghost = func(a_victim);
				if (ghost && a_victim == RE::PlayerCharacter::GetSingleton()) {
					Capture::GetSingleton()->OnEvade(kSiteNames[Site]);
				}
				return ghost;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		constexpr int kMeleeSite = 0;
		constexpr std::uintptr_t kVrMeleeSiteRva = 0x631b35;

		struct InstalledSite
		{
			std::uintptr_t site{ 0 };
			std::uintptr_t stub{ 0 };
		};
		std::array<InstalledSite, 1> g_evadeSites;
		bool g_evadeVerified{ false };

		std::uintptr_t CallDestination(std::uintptr_t a_site)
		{
			if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) {
				return 0;
			}
			const auto rel = *reinterpret_cast<const std::int32_t*>(a_site + 1);
			return a_site + 5 + rel;
		}

		template <int Site, class Thunk>
		void InstallSite(std::uintptr_t a_site)
		{
			const auto bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			if (!CallDestination(a_site) || bytes[5] != 0x84 || bytes[6] != 0xC0) {
				logger::warn("Evade hook skipped: unexpected bytes at the {} ghost-check site.", kSiteNames[Site]);
				return;
			}
			stl::write_thunk_call<Thunk>(a_site);
			g_evadeSites[Site] = { a_site, CallDestination(a_site) };
			logger::info("Evade hook installed ({} ghost check).", kSiteNames[Site]);
		}
	}

	void InstallEvade()
	{
		if (!Settings::GetSingleton()->showEvade) {
			return;
		}
		// VR has one build, so it uses fixed RVAs instead of address-library IDs.
		const bool vr = REL::Module::IsVR();
		InstallSite<kMeleeSite, IsGhostSite<kMeleeSite>>(
			vr ? REL::Offset(kVrMeleeSiteRva).address() : REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37673, 38627), OFFSET(0x45, 0x47) }.address());
	}

	void VerifyEvade()
	{
		if (g_evadeVerified) {
			return;
		}
		g_evadeVerified = true;
		for (std::size_t i = 0; i < g_evadeSites.size(); ++i) {
			const auto& s = g_evadeSites[i];
			if (s.site && CallDestination(s.site) != s.stub) {
				logger::warn("Evade hook: another plugin re-patched the {} ghost-check site after us; mod-granted i-frames may not show \"Evade\".", kSiteNames[i]);
			}
		}
	}

	void Install()
	{
		HandleHealthDamage<RE::Character>::Install();
		HandleHealthDamage<RE::PlayerCharacter>::Install();

		ModActorValue<RE::Character>::Install();
		ModActorValue<RE::PlayerCharacter>::Install();

		ModifyActorValueEffect<RE::ValueModifierEffect>::Install();
		ModifyActorValueEffect<RE::DualValueModifierEffect>::Install();
		ModifyActorValueEffect<RE::PeakValueModifierEffect>::Install();
		ModifyActorValueEffect<RE::AbsorbEffect>::Install();
		ModifyActorValueEffect<RE::AccumulatingValueModifierEffect>::Install();
		ModifyActorValueEffect<RE::TargetValueModifierEffect>::Install();
		ModifyActorValueEffect<RE::ValueAndConditionsEffect>::Install();

		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(37633, 38586), OFFSET(0x16A, 0xFA) };
		if (REL::Module::IsAE()) {
			stl::write_thunk_call<SendHitEventAE>(target.address());
		} else {
			stl::write_thunk_call<SendHitEventSE>(target.address());
		}

		logger::info("Damage hooks installed (HandleHealthDamage idx {:#x}, hit thunk {}).",
			REL::Module::IsVR() ? 0x106 : 0x104, REL::Module::IsAE() ? "AE" : "SE/VR");
	}
}
