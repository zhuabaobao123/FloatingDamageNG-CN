// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "Capture.h"
#include "Settings.h"

namespace FDNG
{
	// One live floating number. Lives in a fixed pool slot; no heap allocation
	// after startup (zero allocations in the combat path).
	struct Number
	{
		bool active{ false };
		RE::FormID victimID{ 0 };
		RE::NiPoint3 anchor;     // victim head; refreshed each frame while the victim is loaded
		RE::NiPoint3 spread;     // anti-stacking displacement (fixed at spawn)
		RE::NiPoint3 launchDir;  // unit launch direction (may be diagonal for burst patterns)
		MotionProfile motion;    // resolved effect (per-kind override or global), fixed at spawn
		float age{ 0.0f };       // seconds since spawn
		float lifetime{ 1.5f };
		float amount{ 0.0f };
		float mitigated{ 0.0f };
		float ampMult{ 0.0f };
		char location[16]{};
		MitigationLabel mitLabel{ MitigationLabel::kResisted };
		DamageKind kind{ DamageKind::kPhysical };
		OriginTier origin{ OriginTier::kNPC };
		HitFlags flags;
		HitExtras extras;
		int extraIndex{ kNoExtra };
		bool custom{ false };  // API popup: `text` is caller-supplied and never rebuilt or merged
		bool pinned{ false };  // anchor is world-fixed; not re-anchored to the victim's head
		bool useColor{ false };
		std::uint32_t colorRGB{ 0 };
		float scale{ 1.0f };
		char text[28]{};     // "104" / "CRIT 104" / "HEADSHOT 104"
		char subtext[64]{};  // "(-45 resisted) x2.5 +12 crit +30 sneak 8 reflected"
	};

	// Per-frame, per-number draw parameters resolved by Resolve(); the
	// projection fields are filled by the renderer's shared camera pass.
	struct ResolvedNumber
	{
		const Number* number{ nullptr };
		RE::NiPoint3 worldPos;  // anchor + kinematic offset
		float scale{ 1.0f };
		float alpha{ 1.0f };
		float screenX{ 0.0f };  // normalized camera projection (y up)
		float screenY{ 0.0f };
		bool projected{ false };  // in front of the camera with valid coords
		bool inView{ true };      // projected within the margined frustum (NPC numbers only)
	};

	class NumberManager
	{
	public:
		static constexpr std::size_t kPoolCapacity = 128;
		static constexpr std::size_t kQueueCapacity = 64;

		static NumberManager* GetSingleton();

		// Game thread: queue a classified damage event for display.
		void Enqueue(const DamageEvent& a_event);

		// Render thread: drain the queue (merging DoT ticks), advance ages,
		// retire dead numbers, and resolve draw parameters for the live ones.
		// Returns entries in a caller-owned buffer (reused across frames).
		void Update(std::vector<ResolvedNumber>& a_out);

		// Main thread: when preview mode is on, periodically spawn sample
		// numbers on the console-selected reference (or the player) so the
		// user can tune fonts/motion/offset live. `prid <id>` to pick a
		// target from the console.
		void PreviewTick();

	private:
		void Spawn(const DamageEvent& a_event);
		void BuildText(Number& a_number) const;

		RE::NiPoint3 KinematicOffset(const Number& a_number) const;

		std::array<Number, kPoolCapacity> _pool{};

		std::mutex _queueLock;
		std::array<DamageEvent, kQueueCapacity> _queue{};
		std::size_t _queueCount{ 0 };

		std::chrono::steady_clock::time_point _lastUpdate{};
		std::chrono::steady_clock::time_point _lastPreview{};
	};
}
