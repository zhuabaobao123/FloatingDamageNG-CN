// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace FDNG::I18n
{
	// Load Data/SKSE/Plugins/FloatingDamageNG.json (if present).
	// Falls back gracefully: all Get() calls return their English default
	// when the file is missing or a key is absent.
	void Load();

	// Get a translated string by key path (e.g. "number_prefixes.crit").
	// Returns a_fallback when the key is not found or the file wasn't loaded.
	const char* Get(std::string_view a_key, const char* a_fallback);

	// Convenience: key → self (English identity).
	const char* Get(std::string_view a_key);
}
