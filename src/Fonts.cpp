// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#include "Fonts.h"

#include "Settings.h"

#include <cctype>
#include <unordered_map>
#include <utility>

namespace FDNG::Fonts
{
	namespace
	{
		// Candidate TTFs, first hit wins: user-configured, a mod-shipped
		// override, then bold Windows system fonts (always present).
		std::vector<std::string> CandidatePaths()
		{
			std::vector<std::string> paths;
			if (const auto& user = Settings::GetSingleton()->fontPath; !user.empty()) {
				paths.push_back(user);
			}
			paths.emplace_back("Data/Interface/FloatingDamageNG/FloatingDamageNG.ttf");

			char windir[MAX_PATH]{};
			if (GetEnvironmentVariableA("WINDIR", windir, MAX_PATH) > 0) {
				paths.push_back(std::format("{}\\Fonts\\arialbd.ttf", windir));
				paths.push_back(std::format("{}\\Fonts\\segoeuib.ttf", windir));
			}
			return paths;
		}
	}

	namespace
	{
		// Per-atlas (per-context) kind fonts, since flat and VR keep separate
		// atlases. nullptr entries fall back to the default font.
		std::unordered_map<ImFontAtlas*, std::array<ImFont*, 9>> g_kindFonts;

		ImFont* LoadInto(ImFontAtlas* a_atlas, const std::string& a_path, std::unordered_map<std::string, ImFont*>& a_seen)
		{
			if (a_path.empty() || GetFileAttributesA(a_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
				return nullptr;
			}
			if (const auto it = a_seen.find(a_path); it != a_seen.end()) {
				return it->second;  // one ImFont per distinct path per atlas
			}
			ImFontConfig cfg;
			cfg.GlyphRanges = a_atlas->GetGlyphRangesChineseSimplifiedCommon();
			ImFont* font = a_atlas->AddFontFromFileTTF(a_path.c_str(), 48.0f, &cfg);
			a_seen[a_path] = font;
			return font;
		}
	}

	void Load()
	{
		auto& io = ImGui::GetIO();
		std::unordered_map<std::string, ImFont*> seen;

		// Default font: first available candidate.
		for (const auto& path : CandidatePaths()) {
			if (ImFont* font = LoadInto(io.Fonts, path, seen)) {
				io.FontDefault = font;
				logger::info("Loaded damage font '{}'.", path);
				break;
			}
		}
		if (!io.FontDefault) {
			logger::warn("No TTF font found; falling back to the embedded bitmap font (numbers will look pixelated).");
		}

		// Per-kind overrides into the same atlas (deduped against the default).
		std::array<ImFont*, 9> kinds{};
		const auto& byKind = Settings::GetSingleton()->fontByKind;
		for (std::size_t i = 0; i < byKind.size(); ++i) {
			kinds[i] = LoadInto(io.Fonts, byKind[i], seen);
		}
		g_kindFonts[io.Fonts] = kinds;
	}

	ImFont* ForKind(DamageKind a_kind)
	{
		auto& io = ImGui::GetIO();
		if (const auto it = g_kindFonts.find(io.Fonts); it != g_kindFonts.end()) {
			if (ImFont* f = it->second[static_cast<std::size_t>(std::to_underlying(a_kind))]) {
				return f;
			}
		}
		return io.FontDefault;
	}

	namespace
	{
		std::vector<std::pair<std::string, std::string>> g_available;
		bool g_availableLoaded = false;

		void ScanAvailable()
		{
			g_available.clear();
			const auto scan = [](const std::filesystem::path& a_dir) {
				std::error_code ec;
				if (!std::filesystem::is_directory(a_dir, ec)) {
					return;
				}
				// increment(ec), not a ranged-for: the range loop's operator++
				// throws on a mid-scan filesystem error, crashing the game.
				for (auto it = std::filesystem::directory_iterator(a_dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
					if (!it->is_regular_file()) {
						continue;
					}
					auto ext = it->path().extension().string();
					std::ranges::transform(ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
					if (ext == ".ttf" || ext == ".otf") {
						g_available.emplace_back(it->path().stem().string(), it->path().string());
					}
				}
			};
			// User drop-in folder first (alongside Presets), then the
			// mod-shipped font, then the system fonts.
			scan("Data/SKSE/Plugins/FloatingDamageNG/Fonts");
			scan("Data/Interface/FloatingDamageNG");
			char windir[MAX_PATH]{};
			if (GetEnvironmentVariableA("WINDIR", windir, MAX_PATH) > 0) {
				scan(std::filesystem::path(windir) / "Fonts");
			}
			std::ranges::sort(g_available, [](const auto& a, const auto& b) { return a.first < b.first; });
		}
	}

	const std::vector<std::pair<std::string, std::string>>& Available()
	{
		if (!g_availableLoaded) {
			ScanAvailable();
			g_availableLoaded = true;
		}
		return g_available;
	}

	void RefreshAvailable()
	{
		ScanAvailable();
		g_availableLoaded = true;
	}
}
