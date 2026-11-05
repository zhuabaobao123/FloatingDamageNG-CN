// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "Capture.h"

namespace FDNG
{
	inline constexpr std::size_t kExtraCount = 3;

	// A data-driven motion path for a floating number. The built-in effects
	// are just preset instances of this, so a custom path and a built-in run
	// through the exact same integrator, travelling along the per-number
	// launch direction the spread pattern chose:
	//   travel(t)   = damping > 0 ? speed*(1-e^-damping*t)/damping : speed*t
	//   vertical(t) = riseSpeed*t + 0.5*riseAccel*t^2
	struct MotionProfile
	{
		float riseSpeed{ 45.0f };      // constant upward velocity, game units/s
		float riseAccel{ 0.0f };       // vertical acceleration; negative = arc gravity
		float lateralSpeed{ 0.0f };    // velocity along the launch direction
		float lateralDamping{ 0.0f };  // 0 = linear travel; >0 = exponential ease-out (burst)
	};

	// How each successive number on one target picks its launch direction.
	enum class SpreadPattern : std::int32_t
	{
		kAlternate = 0,          // left/right along the screen-horizontal (bias-able)
		kRotate = 1,             // rotate the launch by spawnAngle each hit (fireworks / clockwise)
		kDiagonalAlternate = 2,  // alternate up-left / up-right diagonals
	};

	// A full spawn "effect": the motion path plus how launch directions are
	// distributed. Built-ins and user customs share this shape.
	struct EffectPreset
	{
		const char* name;
		MotionProfile motion;
		SpreadPattern spread;
		float spawnAngleDeg;  // per-hit rotation (kRotate) or diagonal tilt (kDiagonalAlternate)
	};
	// The one compiled-in effect: the guaranteed zero-file default and
	// fallback, so the mod always has a working motion even if the Presets
	// folder is missing. A plain rise-and-fade matches the genre default
	// (Floating Damage's mode 0). Everything fancier — Arc, Radial, Fireworks
	// — ships as a loose JSON preset the user can see, edit, and share, so
	// each effect has exactly one source of truth (no duplicate picker entry).
	inline constexpr std::array<EffectPreset, 1> kEffectPresets{ {
		{ "Float", { 45.0f, 0.0f, 0.0f, 0.0f }, SpreadPattern::kAlternate, 0.0f },
	} };

	// How a number marks its origin (whose fight it is).
	enum class OriginStyle : std::int32_t
	{
		kOutline = 0,    // text outline in the origin color
		kUnderline = 1,  // thin black outline + origin-colored underline bar
		kBox = 2,        // thin black outline + origin-colored border box
		kNone = 3        // plain black outline, no origin color (don't show the source)
	};

	enum class DotDisplay : std::uint8_t
	{
		kPerTick = 0,
		kSummed,
	};

	// Per-damage-kind metadata in DamageKind order — the single source of truth
	// for the [PerType] INI key suffix and the menu label, so a kind's ordering
	// and spelling live in one place instead of parallel arrays.
	struct PerKindMeta
	{
		const char* iniSuffix;  // sMotion<suffix> / sFont<suffix>
		const char* label;      // menu display name
	};
	inline constexpr std::array<PerKindMeta, 9> kPerKindMeta{ {
		{ "Physical", "Physical" },
		{ "Fire", "Fire" },
		{ "Frost", "Frost" },
		{ "Shock", "Shock" },
		{ "Poison", "Poison" },
		{ "Magic", "Magic" },
		{ "Healing", "Healing" },
		{ "MagickaDrain", "Magicka drain" },
		{ "StaminaDrain", "Stamina drain" },
	} };
	inline constexpr std::size_t kDamageKindCount = kPerKindMeta.size();

	struct Settings
	{
		static Settings* GetSingleton();

		void Load();
		void Save() const;       // write current values back to the INI (used by the SMF config UI)
		void ResetToDefaults();  // in-memory only; Save() to persist

		// [CoreFilters]
		bool showEvade{ false };
		bool enableFloatingDamage{ true };  // master switch; the show* flags below only matter while this is on
		bool showPlayerDamageDealt{ true };
		bool showFollowerDamageDealt{ true };
		bool showNPCOnNPCDamage{ true };     // heavily attenuated + distance-culled anyway
		bool showPlayerDamageTaken{ true };  // suppressed while in first person
		bool showHealing{ true };
		bool showMagickaDamage{ false };  // hostile-only (self costs filtered)
		bool showStaminaDamage{ false };  // hostile-only (self costs filtered)
		float maxVisibilityRadiusMeters{ 20.0f };
		int maxConcurrentQuads{ 40 };

		// [DynamicSizing]
		float baseFontPixels{ 48.0f };  // atlas size the numbers draw at before scaling
		float baseFontScale{ 1.0f };
		float logScaleModifier{ 0.25f };
		float maxFontScaleCeiling{ 2.5f };  // headroom so big hits keep growing instead of saturating early
		bool abbreviateNumbers{ false };    // 10000+ only: 12345 -> 12k, 3400000 -> 3.4M

		// [Distance] — how number size responds to how far the target is (the
		// ranged-attack readability knobs). Reference distance is shared: within
		// it, numbers are full size. Flat shrinks past it (floored so distant
		// hits stay legible); VR grows the physical quad past it (capped) to
		// hold angular size. 1.0 flat floor = never shrink.
		float distanceRefMeters{ 3.5f };
		float flatDistanceMinScale{ 0.5f };  // flat: smallest a far number gets
		float vrDistanceMaxBoost{ 8.0f };    // VR: largest a far quad grows
		// Squash-and-stretch: distort the number vertically while it moves
		// fast, easing to normal as it slows (launch "juice"). Opt-in.
		bool squashStretch{ false };
		float stretchIntensity{ 0.5f };

		// [KinematicProfiles] — the active spawn effect (a preset copied here,
		// then freely tuned).
		int motionPreset{ 0 };  // legacy default-seed index (the compiled Float)
		MotionProfile motion{ kEffectPresets[0].motion };
		SpreadPattern spreadPattern{ SpreadPattern::kAlternate };
		float spawnAngleDeg{ 0.0f };  // per-hit rotation / diagonal tilt
		float globalSpeedMultiplier{ 1.1f };
		float quadLifetimeSeconds{ 1.5f };

		// [Origin] — spawn-point offset from the victim's head, view-relative
		// (up, toward the player, and lateral). Previewable against a
		// console-selected target.
		float originOffsetUp{ 0.0f };
		float originOffsetToward{ 0.0f };
		float originOffsetSide{ 0.0f };
		// Rapid hits on one target step apart to de-overlap. Spacing is the
		// per-step gap; bias favors one side instead of alternating evenly
		// (kAlternate only; 0 = symmetric).
		float rapidHitSpread{ 30.0f };
		float rapidHitBias{ 0.0f };  // -1 all left ... 0 alternate ... +1 all right
		bool previewMode{ false };   // spawn sample numbers on the selected ref (or player)

		// [PerType] — optional per-damage-kind overrides, indexed by
		// std::to_underlying(DamageKind); empty = use the global setting.
		// motionByKind names a preset; fontByKind a TTF/OTF path (absolute, or
		// relative to the Skyrim folder).
		std::array<std::string, kDamageKindCount> motionByKind{};
		std::array<std::string, kDamageKindCount> fontByKind{};

		// [Behavior]
		bool showMitigation{ true };
		std::array<ExtraDisplay, kExtraCount> extraDisplay{ ExtraDisplay::kInline, ExtraDisplay::kInline, ExtraDisplay::kInline };
		float minDamageToShow{ 1.0f };
		float minHealToShow{ 5.0f };           // accumulation threshold; filters natural regen trickle
		float dotAccumulationWindow{ 0.35f };  // merge same victim+type events younger than this
		DotDisplay dotDisplay{ DotDisplay::kPerTick };
		float dotTickSeconds{ 1.0f };

		// [Style] — how a number's origin (whose fight it is) is marked. The
		// fill color always encodes the damage kind, so origin uses a
		// secondary channel in the relationship colors below.
		OriginStyle originStyle{ OriginStyle::kOutline };
		float styleThickness{ 4.0f };  // outline / underline / box border, panel px

		// [Colors] 0xRRGGBB (alpha applied at draw time)
		std::uint32_t colorPhysical{ 0xFF3B30 };
		std::uint32_t colorCritical{ 0xFFCC00 };
		std::uint32_t colorBlocked{ 0x4A4A4A };
		std::uint32_t colorEvade{ 0x8FD3FF };
		std::uint32_t colorFire{ 0xFF9500 };
		std::uint32_t colorFrost{ 0x5AC8FA };
		std::uint32_t colorShock{ 0xAF52DE };
		std::uint32_t colorPoison{ 0x4CD964 };
		std::uint32_t colorMagic{ 0xC7B8E8 };  // untyped magic
		std::uint32_t colorHealing{ 0x34C759 };
		std::uint32_t colorCritBonus{ 0xFFE680 };
		std::uint32_t colorSneakBonus{ 0x8FA8FF };
		std::uint32_t colorReflected{ 0xE0E0E0 };
		std::uint32_t colorMagickaDamage{ 0x4169E1 };  // royal blue
		std::uint32_t colorStaminaDamage{ 0x2E8B57 };  // sea green
		// Origin marker colors (relationship palette: incoming red, ally
		// blue, unrelated gray; your own hits stay neutral black).
		std::uint32_t colorOriginPlayer{ 0x000000 };
		std::uint32_t colorOriginTaken{ 0x8C1414 };
		std::uint32_t colorOriginFollower{ 0x19508C };
		std::uint32_t colorOriginNPC{ 0x3C3C3C };

		// [Locational] — engine-derived hit location (projectile impact node),
		// mod-agnostic: works in vanilla and with any locational-damage mod.
		struct LocationTag
		{
			std::regex pattern;  // matched against the struck skeleton node name
			std::string label;   // e.g. "HEADSHOT"
		};
		bool showHitLocation{ true };
		std::vector<LocationTag> locationTags;  // built in Load(); first match wins
		// Show the implied external multiplier (e.g. x2.5) when a projectile
		// hit's totalDamage exceeds its physical+crit baseline — the pattern
		// locational-damage mods use. Heuristic; projectile hits only.
		bool showAmplification{ true };
		float amplificationThreshold{ 1.4f };

		// [FirstPerson] — player-received numbers (damage and heals) in first
		// person: pinned to a screen spot on flat, anchored ~1 m ahead at
		// chest height in VR. Off = suppressed entirely in first person.
		bool showFirstPersonNumbers{ true };
		float firstPersonX{ 0.5f };   // flat pin, fraction of screen width
		float firstPersonY{ 0.72f };  // flat pin, fraction of screen height

		// [Font]
		std::string fontPath;  // empty = auto (mod override, then Windows bold fonts)

		// [Analytics]
		bool enableCombatLog{ true };       // master switch for the analytics module
		bool writeLogToDisk{ true };        // append session reports to FloatingDamageNG-combat.log
		bool exportJsonl{ false };          // append sessions to FloatingDamageNG-sessions.jsonl (one JSON object/line)
		bool exportCsv{ false };            // append per-combatant rows to FloatingDamageNG-combatants.csv
		bool enableLiveDPSWindow{ false };  // opt-in: an uninvited second HUD element surprises users
		bool hudShowTaken{ true };
		bool hudShowHits{ true };  // hit count, crit rate, biggest hit
		int hudTopRows{ 3 };
		int hudCorner{ 0 };          // 0 top-right, 1 top-left, 2 bottom-right, 3 bottom-left
		float hudFontScale{ 1.0f };  // 0.75-2.0, multiplies the panel's text size
		bool hudColorStats{ true };  // tint values by meaning and dealers by relationship
		bool logFollowerPerformance{ true };
		float hudRecapHoldSeconds{ 6.0f };
		float postCombatWindowFadeSeconds{ 5.0f };
		// Opt-in: registering with devbench exposes combat stats on the port
		// the devbench host binds, so it is off unless the user enables it.
		bool enableDevBench{ false };

		// [Debug]
		bool debugLog{ false };
		bool deltaAudit{ false };  // 1 Hz health-delta vs captured-events comparison
	};

	// Single source of truth for the color set: INI load/save and the UI
	// pickers iterate this table, so adding a color touches only Settings.h
	// (plus whatever maps a kind to it in the renderer).
	struct ColorDef
	{
		const char* iniKey;
		const char* uiLabel;
		std::uint32_t Settings::* field;
	};
	// Per-hit extras: INI, UI, the subtext segment and the informational number all read this table.
	struct ExtraDef
	{
		const char* iniKey;
		const char* uiLabel;
		const char* uiTip;
		float HitExtras::* value;
		bool HitFlags::* requiresFlag;  // null = shown whenever the value is present
		std::uint32_t Settings::* color;
		const char* segment;
	};
	inline constexpr std::array<ExtraDef, kExtraCount> kExtraTable{ {
		{ "iCritBonusDisplay", "Crit bonus", "The flat bonus a critical hit added: a \"12 crit\" segment under the number, or its own number.",
			&HitExtras::critBonus, &HitFlags::critical, &Settings::colorCritBonus, "crit" },
		{ "iSneakBonusDisplay", "Sneak attack bonus", "The extra damage the sneak attack multiplier added: a \"30 sneak\" segment under the number, or its own number.",
			&HitExtras::sneakBonus, &HitFlags::sneak, &Settings::colorSneakBonus, "sneak" },
		{ "iReflectedDisplay", "Reflected damage", "Melee damage the target's Reflect Damage returns to the attacker: a \"12 reflected\" segment under the number, or its own number.",
			&HitExtras::reflected, nullptr, &Settings::colorReflected, "reflected" },
	} };

	// Whole-number value of an extra for this hit; 0 when it does not apply.
	inline int ExtraShownValue(const ExtraDef& a_extra, const HitFlags& a_flags, const HitExtras& a_extras)
	{
		if (a_extra.requiresFlag && !(a_flags.*a_extra.requiresFlag)) {
			return 0;
		}
		return static_cast<int>(std::lround(a_extras.*a_extra.value));
	}

	inline constexpr std::array<ColorDef, 19> kColorTable{ {
		{ "sPhysical", "Physical", &Settings::colorPhysical },
		{ "sCritical", "Critical", &Settings::colorCritical },
		{ "sBlocked", "Blocked", &Settings::colorBlocked },
		{ "sEvade", "Evade", &Settings::colorEvade },
		{ "sFire", "Fire", &Settings::colorFire },
		{ "sFrost", "Frost", &Settings::colorFrost },
		{ "sShock", "Shock", &Settings::colorShock },
		{ "sPoison", "Poison", &Settings::colorPoison },
		{ "sMagic", "Magic (untyped)", &Settings::colorMagic },
		{ "sHealing", "Healing", &Settings::colorHealing },
		{ "sCritBonus", "Extra: crit bonus", &Settings::colorCritBonus },
		{ "sSneakBonus", "Extra: sneak bonus", &Settings::colorSneakBonus },
		{ "sReflected", "Extra: reflected", &Settings::colorReflected },
		{ "sMagickaDamage", "Magicka damage", &Settings::colorMagickaDamage },
		{ "sStaminaDamage", "Stamina damage", &Settings::colorStaminaDamage },
		{ "sOriginPlayer", "Marker: your hits", &Settings::colorOriginPlayer },
		{ "sOriginTaken", "Marker: damage you take", &Settings::colorOriginTaken },
		{ "sOriginFollower", "Marker: follower damage", &Settings::colorOriginFollower },
		{ "sOriginNPC", "Marker: NPC fights", &Settings::colorOriginNPC },
	} };
}
