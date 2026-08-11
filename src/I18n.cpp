// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#include "I18n.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace FDNG::I18n
{
	namespace
	{
		// Flattened key-value store: "number_prefixes.crit" → "暴击"
		std::unordered_map<std::string, std::string> g_map;

		void Flatten(const nlohmann::json& a_obj, const std::string& a_prefix)
		{
			for (auto it = a_obj.begin(); it != a_obj.end(); ++it) {
				// Skip notes and internal metadata markers
				if (it.key() == "_note") {
					continue;
				}
				const std::string key = a_prefix.empty() ? it.key() : a_prefix + "." + it.key();
				if (it.value().is_object()) {
					Flatten(it.value(), key);
				} else if (it.value().is_string()) {
					g_map[key] = it.value().get<std::string>();
				}
			}
		}
	}

	void Load()
	{
		const std::filesystem::path kJsonPath = L"Data/SKSE/Plugins/FloatingDamageNG.json";

		std::error_code ec;
		if (!std::filesystem::is_regular_file(kJsonPath, ec)) {
			logger::info("FloatingDamageNG.json not found; using English defaults.");
			return;
		}

		try {
			std::ifstream f(kJsonPath);
			nlohmann::json root;
			f >> root;
			Flatten(root, "");
			logger::info("Loaded {} translation(s) from FloatingDamageNG.json.", g_map.size());
		} catch (const std::exception& e) {
			logger::warn("Failed to parse FloatingDamageNG.json: {} — using English defaults.", e.what());
			g_map.clear();
		}
	}

	const char* Get(std::string_view a_key, const char* a_fallback)
	{
		if (auto it = g_map.find(std::string{ a_key }); it != g_map.end() && !it->second.empty()) {
			return it->second.c_str();
		}
		return a_fallback;
	}

	const char* Get(std::string_view a_key)
	{
		// No fallback — return the key itself (identity, for English)
		if (auto it = g_map.find(std::string{ a_key }); it != g_map.end() && !it->second.empty()) {
			return it->second.c_str();
		}
		// This path shouldn't be hit in normal use; return empty string.
		static const std::string empty;
		return empty.c_str();
	}
}
