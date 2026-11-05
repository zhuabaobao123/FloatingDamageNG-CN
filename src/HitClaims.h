// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace FDNG
{
	// What another mod asked FloatingDamageNG not to show for one hit.
	struct HitClaim
	{
		bool suppressPopup{ false };     // withhold the number entirely (analytics still records)
		bool suppressLocation{ false };  // keep the number, drop the locational tag + amp subtext
	};

	// Per-victim pending claims. Each claim is consumed by one hit and expires
	// after the window. Not thread-safe; the owner serializes access. Time is
	// passed in so the expiry rule is testable without sleeping.
	class HitClaimLedger
	{
	public:
		using Clock = std::chrono::steady_clock;

		explicit HitClaimLedger(Clock::duration a_window) :
			_window(a_window)
		{}

		// A second claim on the same unexpired hit combines with the first.
		void Add(std::uint32_t a_victimID, HitClaim a_claim, Clock::time_point a_now)
		{
			std::erase_if(_claims, [&](const auto& a_entry) { return Expired(a_entry.second.stamp, a_now); });
			auto& entry = _claims[a_victimID];
			entry.stamp = a_now;
			entry.claim.suppressPopup |= a_claim.suppressPopup;
			entry.claim.suppressLocation |= a_claim.suppressLocation;
		}

		// Returns the victim's claim (empty if none or expired) and clears it.
		HitClaim Take(std::uint32_t a_victimID, Clock::time_point a_now)
		{
			const auto it = _claims.find(a_victimID);
			if (it == _claims.end()) {
				return {};
			}
			const auto claim = Expired(it->second.stamp, a_now) ? HitClaim{} : it->second.claim;
			_claims.erase(it);
			return claim;
		}

	private:
		struct Entry
		{
			Clock::time_point stamp;
			HitClaim claim;
		};

		bool Expired(Clock::time_point a_stamp, Clock::time_point a_now) const { return a_now - a_stamp > _window; }

		Clock::duration _window;
		std::unordered_map<std::uint32_t, Entry> _claims;
	};
}
