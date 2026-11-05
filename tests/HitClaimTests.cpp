// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.
//
// Exercises the same HitClaimLedger the plugin uses; no engine types involved.

#include "HitClaims.h"

#include <cstdio>

namespace
{
	using namespace std::chrono_literals;
	using FDNG::HitClaim;
	using FDNG::HitClaimLedger;

	constexpr auto kWindow = 500ms;
	constexpr std::uint32_t kVictimA = 0x14;
	constexpr std::uint32_t kVictimB = 0xFF000800;

	int g_failures = 0;

	void Check(bool a_condition, const char* a_what)
	{
		if (!a_condition) {
			std::printf("FAIL: %s\n", a_what);
			++g_failures;
		}
	}

	const HitClaimLedger::Clock::time_point kStart{};
	const HitClaim kPopup{ .suppressPopup = true };
	const HitClaim kLocation{ .suppressLocation = true };

	void TakeWithoutClaimIsEmpty()
	{
		HitClaimLedger ledger{ kWindow };
		const auto claim = ledger.Take(kVictimA, kStart);
		Check(!claim.suppressPopup && !claim.suppressLocation, "no claim yields an empty claim");
	}

	void ClaimIsConsumedByOneHit()
	{
		HitClaimLedger ledger{ kWindow };
		ledger.Add(kVictimA, kPopup, kStart);
		Check(ledger.Take(kVictimA, kStart + 10ms).suppressPopup, "first hit receives the claim");
		Check(!ledger.Take(kVictimA, kStart + 20ms).suppressPopup, "second hit does not");
	}

	void ClaimsAreIsolatedPerVictim()
	{
		HitClaimLedger ledger{ kWindow };
		ledger.Add(kVictimA, kPopup, kStart);
		Check(!ledger.Take(kVictimB, kStart).suppressPopup, "another victim is unaffected");
		Check(ledger.Take(kVictimA, kStart).suppressPopup, "the claimed victim keeps its claim");
	}

	void ClaimExpiresAfterWindow()
	{
		HitClaimLedger ledger{ kWindow };
		ledger.Add(kVictimA, kPopup, kStart);
		Check(!ledger.Take(kVictimA, kStart + kWindow + 1ms).suppressPopup, "claim past the window is ignored");
	}

	void ClaimAtWindowEdgeStillApplies()
	{
		HitClaimLedger ledger{ kWindow };
		ledger.Add(kVictimA, kPopup, kStart);
		Check(ledger.Take(kVictimA, kStart + kWindow).suppressPopup, "claim exactly at the window applies");
	}

	void ExpiredClaimIsClearedNotRevived()
	{
		HitClaimLedger ledger{ kWindow };
		ledger.Add(kVictimA, kPopup, kStart);
		ledger.Take(kVictimA, kStart + kWindow + 1ms);
		Check(!ledger.Take(kVictimA, kStart + 1ms).suppressPopup, "an expired claim stays consumed");
	}

	void ClaimsOnOneHitCombine()
	{
		HitClaimLedger ledger{ kWindow };
		ledger.Add(kVictimA, kLocation, kStart);
		ledger.Add(kVictimA, kPopup, kStart + 5ms);
		const auto claim = ledger.Take(kVictimA, kStart + 10ms);
		Check(claim.suppressPopup && claim.suppressLocation, "both flags survive a second claim");
	}

	void StaleClaimDoesNotCombineWithNewOne()
	{
		HitClaimLedger ledger{ kWindow };
		ledger.Add(kVictimA, kPopup, kStart);
		ledger.Add(kVictimA, kLocation, kStart + kWindow + 100ms);
		const auto claim = ledger.Take(kVictimA, kStart + kWindow + 110ms);
		Check(claim.suppressLocation && !claim.suppressPopup, "an expired claim's flags do not leak into a new one");
	}
}

int main()
{
	TakeWithoutClaimIsEmpty();
	ClaimIsConsumedByOneHit();
	ClaimsAreIsolatedPerVictim();
	ClaimExpiresAfterWindow();
	ClaimAtWindowEdgeStillApplies();
	ExpiredClaimIsClearedNotRevived();
	ClaimsOnOneHitCombine();
	StaleClaimDoesNotCombineWithNewOne();
	return g_failures == 0 ? 0 : 1;
}
