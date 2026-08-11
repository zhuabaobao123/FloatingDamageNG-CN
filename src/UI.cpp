// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.
//
// Optional in-game config + combat-stats UI, hosted by SKSE Menu Framework 3.
// SMF resolves everything through GetProcAddress at call time, so this whole
// module is inert when the framework DLL isn't installed. Widgets draw through
// the ImGuiMCP indirection table (the HOST's ImGui context) - never through
// our own statically-linked ImGui.

// This TU is built WITHOUT the project PCH (see xmake.lua): the SMF header's
// cimgui-style typedefs are incompatible with the real imgui.h the PCH pulls
// in, so the includes below reproduce the non-ImGui part of pch.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <RE/Skyrim.h>
#include <REX/REX.h>
#include <SKSE/SKSE.h>

#include <shellapi.h>  // ShellExecuteA (WIN32_LEAN_AND_MEAN drops it from Windows.h)

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace logger = SKSE::log;

#include "SKSEMenuFramework.h"

#include "CombatLog.h"
#include "DevBench.h"
#include "Fonts.h"
#include "I18n.h"
#include "Presets.h"
#include "Settings.h"
#include "StyleMetrics.h"
#include "UI.h"

namespace FDNG::UI
{
	namespace
	{
		void RgbToFloat3(std::uint32_t a_rgb, float a_out[3])
		{
			a_out[0] = static_cast<float>((a_rgb >> 16) & 0xFF) / 255.0f;
			a_out[1] = static_cast<float>((a_rgb >> 8) & 0xFF) / 255.0f;
			a_out[2] = static_cast<float>(a_rgb & 0xFF) / 255.0f;
		}

		std::uint32_t Float3ToRgb(const float a_in[3])
		{
			const auto ch = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
			return (ch(a_in[0]) << 16) | (ch(a_in[1]) << 8) | ch(a_in[2]);
		}

		// devbench writes the port it bound to runtime.json on startup; we read
		// it for display only (the integration itself is in-process). Cached -
		// file I/O every frame would hitch while the panel is open.
		int ReadDevBenchPort()
		{
			static int cached = -1;
			static std::chrono::steady_clock::time_point lastRead;
			const auto now = std::chrono::steady_clock::now();
			if (cached >= 0 && now - lastRead < std::chrono::seconds(2)) {
				return cached;
			}
			lastRead = now;
			cached = 0;
			std::ifstream in("Data/SKSE/Plugins/devbench/runtime.json");
			std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (const auto pos = text.find("\"port\""); pos != std::string::npos) {
				if (const auto colon = text.find(':', pos); colon != std::string::npos) {
					cached = std::atoi(text.c_str() + colon + 1);
				}
			}
			return cached;
		}

		void ColorRow(const char* a_label, std::uint32_t& a_color)
		{
			float rgb[3];
			RgbToFloat3(a_color, rgb);
			// a_label comes from kColorTable — translate at display time
			if (ImGuiMCP::ColorEdit3(I18n::Get(a_label, a_label), rgb, 0)) {
				a_color = Float3ToRgb(rgb);
			}
		}

		// One-line explanation on hover for controls whose label can't carry it.
		void Tip(const char* a_text)
		{
			if (ImGuiMCP::IsItemHovered(0)) {
				ImGuiMCP::SetTooltip("%s", I18n::Get(a_text, a_text));
			}
		}

		// Open a folder in Explorer, creating it first so the button always
		// lands somewhere (the drop-in dirs may not exist yet on a fresh install).
		void OpenFolder(const char* a_path)
		{
			std::error_code ec;
			std::filesystem::create_directories(a_path, ec);
			ShellExecuteA(nullptr, "open", a_path, nullptr, nullptr, SW_SHOWNORMAL);
		}

		// Only http(s) so a shared preset's url can't launch an arbitrary path
		// or command through ShellExecute.
		bool IsWebUrl(const std::string& a_url)
		{
			return a_url.starts_with("http://") || a_url.starts_with("https://");
		}

		void OpenUrl(const std::string& a_url)
		{
			if (IsWebUrl(a_url)) {
				ShellExecuteA(nullptr, "open", a_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
		}

		// A clickable link: link-blue text that opens the URL in the browser.
		void LinkText(const std::string& a_url)
		{
			ImGuiMCP::TextColored(ImGuiMCP::ImVec4{ 0.36f, 0.66f, 1.0f, 1.0f }, "%s", a_url.c_str());
			if (ImGuiMCP::IsItemClicked(0)) {
				OpenUrl(a_url);
			}
			if (ImGuiMCP::IsItemHovered(0)) {
				ImGuiMCP::SetTooltip("%s", I18n::Get("ui.common.open_in_browser", "Open in browser"));
			}
		}

		// The preset the live motion fields currently equal (nullptr = none).
		// Returns the whole Effect so the panel can show its name plus the
		// description/attribution, not just a label.
		const Presets::Effect* MatchingPreset(const Settings* a_s, const std::vector<Presets::Effect>& a_presets)
		{
			for (const auto& p : a_presets) {
				if (std::fabs(p.motion.riseSpeed - a_s->motion.riseSpeed) < 0.5f &&
					std::fabs(p.motion.riseAccel - a_s->motion.riseAccel) < 0.5f &&
					std::fabs(p.motion.lateralSpeed - a_s->motion.lateralSpeed) < 0.5f &&
					std::fabs(p.motion.lateralDamping - a_s->motion.lateralDamping) < 0.05f &&
					p.spread == a_s->spreadPattern &&
					std::fabs(p.spawnAngleDeg - a_s->spawnAngleDeg) < 0.5f) {
					return &p;
				}
			}
			return nullptr;
		}

		bool IContains(const char* a_hay, const char* a_needle)
		{
			if (!a_needle || !a_needle[0]) {
				return true;
			}
			const std::string_view hay{ a_hay };
			const auto it = std::search(hay.begin(), hay.end(), a_needle, a_needle + std::strlen(a_needle),
				[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
			return it != hay.end();
		}

		// A combo whose popup carries a type-to-filter box — for long lists
		// (fonts, combatants) a plain dropdown can't scan. Returns true and
		// updates a_index when a row is picked.
		bool SearchableCombo(const char* a_label, int* a_index, const std::vector<const char*>& a_options)
		{
			const char* preview = (*a_index >= 0 && *a_index < static_cast<int>(a_options.size())) ? a_options[*a_index] : "";
			if (!ImGuiMCP::BeginCombo(a_label, preview, 0)) {
				return false;
			}
			// One popup is open at a time, so a shared buffer is fine; reset
			// and focus it when the popup first appears.
			static char s_filter[64]{};
			if (ImGuiMCP::IsWindowAppearing()) {
				s_filter[0] = '\0';
				ImGuiMCP::SetKeyboardFocusHere(0);
			}
			ImGuiMCP::InputTextWithHint("##fdng_combofilter", I18n::Get("ui.common.type_to_filter", "Type to filter..."), s_filter, sizeof(s_filter), 0, nullptr, nullptr);
			bool changed = false;
			for (int i = 0; i < static_cast<int>(a_options.size()); ++i) {
				if (!IContains(a_options[static_cast<std::size_t>(i)], s_filter)) {
					continue;
				}
				if (ImGuiMCP::Selectable(a_options[static_cast<std::size_t>(i)], i == *a_index, 0, { 0, 0 })) {
					*a_index = i;
					changed = true;
				}
			}
			ImGuiMCP::EndCombo();
			return changed;
		}

		std::uint32_t ToImCol(std::uint32_t a_rgb, std::uint8_t a_alpha = 0xFF)
		{
			// ImU32 is ABGR-packed.
			return (static_cast<std::uint32_t>(a_alpha) << 24) | ((a_rgb & 0xFF) << 16) | (a_rgb & 0x00FF00) | ((a_rgb >> 16) & 0xFF);
		}

		// Live sample of the current style, drawn with the menu font - same
		// outline/underline/box logic as the in-world renderer.
		void DrawStylePreview(const Settings* s)
		{
			auto* dl = ImGuiMCP::GetWindowDrawList();
			ImGuiMCP::ImVec2 base;
			ImGuiMCP::GetCursorScreenPos(&base);
			const float h = 64.0f;
			const float w = 560.0f;
			ImGuiMCP::ImDrawListManager::AddRectFilled(dl, base, { base.x + w, base.y + h }, 0xE0101010, 4.0f, 0);

			struct Sample
			{
				const char* text;
				std::uint32_t fill;
				std::uint32_t marker;
			};
			const Sample samples[] = {
				{ "36", s->colorFire, s->colorOriginPlayer },
				{ "CRIT 58", s->colorCritical, s->colorOriginPlayer },
				{ "22", s->colorFrost, s->colorOriginTaken },
				{ "17", s->colorShock, s->colorOriginFollower },
				{ "9", s->colorPhysical, s->colorOriginNPC },
				{ "+25", s->colorHealing, s->colorOriginPlayer },
			};

			const float t = s->styleThickness;
			const float fontH = ImGuiMCP::GetFontSize();
			const auto metrics = ComputeStyleMetrics(s->originStyle, t);
			float x = base.x + 16.0f;
			const float y = base.y + (h - fontH) * 0.5f;
			for (const auto& sm : samples) {
				const auto fill = ToImCol(sm.fill);
				const auto marker = ToImCol(sm.marker);
				const bool ringStyle = s->originStyle == OriginStyle::kOutline || s->originStyle == OriginStyle::kNone;
				const auto textOutline = s->originStyle == OriginStyle::kOutline ? marker : ToImCol(0x000000);
				const float tt = ringStyle ? t : 1.0f;
				for (const auto& o : kRingOffsets) {
					ImGuiMCP::ImDrawListManager::AddText(dl, { x + o[0] * tt, y + o[1] * tt }, textOutline, sm.text);
				}
				ImGuiMCP::ImDrawListManager::AddText(dl, { x, y }, fill, sm.text);

				ImGuiMCP::ImVec2 textSz;
				ImGuiMCP::CalcTextSize(&textSz, sm.text, nullptr, false, -1.0f);
				if (s->originStyle == OriginStyle::kUnderline) {
					ImGuiMCP::ImDrawListManager::AddRectFilled(dl,
						{ x, y + fontH + kUnderlineGap }, { x + textSz.x, y + fontH + kUnderlineGap + t }, marker, 0.0f, 0);
				} else if (s->originStyle == OriginStyle::kBox) {
					ImGuiMCP::ImDrawListManager::AddRect(dl,
						{ x - metrics.padX + t * 0.5f, y - metrics.padTop + t * 0.5f },
						{ x + textSz.x + metrics.padX - t * 0.5f, y + fontH + metrics.padBottom - t * 0.5f }, marker, 3.0f, 0, t);
				}
				x += textSz.x + 32.0f;
			}
			ImGuiMCP::Dummy({ w, h + 6.0f });
		}

		// Shared Save / Reload / Reset row shown at the bottom of every
		// settings page (all act on the one Settings singleton).
		void SaveRow(Settings* a_settings)
		{
			ImGuiMCP::Separator();
			if (ImGuiMCP::Button(I18n::Get("ui.common.save_to_ini", "Save to INI"), { 0, 0 })) {
				a_settings->Save();
			}
			ImGuiMCP::SameLine(0.0f, -1.0f);
			if (ImGuiMCP::Button(I18n::Get("ui.common.reload_ini", "Reload INI"), { 0, 0 })) {
				a_settings->Load();
			}
			ImGuiMCP::SameLine(0.0f, -1.0f);
			static bool s_confirmReset = false;
			if (ImGuiMCP::IsWindowAppearing()) {
				s_confirmReset = false;
			}
			if (!s_confirmReset) {
				if (ImGuiMCP::Button(I18n::Get("ui.common.reset_to_defaults", "Reset to defaults"), { 0, 0 })) {
					s_confirmReset = true;
				}
			} else {
				if (ImGuiMCP::Button(I18n::Get("ui.common.confirm_reset", "Confirm reset"), { 0, 0 })) {
					a_settings->ResetToDefaults();
					s_confirmReset = false;
				}
				ImGuiMCP::SameLine(0.0f, -1.0f);
				if (ImGuiMCP::Button(I18n::Get("ui.common.cancel", "Cancel"), { 0, 0 })) {
					s_confirmReset = false;
				}
			}
			ImGuiMCP::TextDisabled("%s", I18n::Get("ui.common.changes_apply_immediately", "Changes apply immediately; Save writes them to the INI. Font changes need a restart."));
		}

		// "Numbers" page: everything about how the numbers look and move.
		void __stdcall RenderNumbers()
		{
			auto* s = Settings::GetSingleton();

			// Live preview up top: it exercises everything below (motion,
			// font, colors, offset), so it isn't specific to any one section.
			ImGuiMCP::Checkbox(I18n::Get("ui.numbers.live_preview", "Live preview"), &s->previewMode);
			ImGuiMCP::SameLine(0.0f, -1.0f);
			if (s->previewMode) {
				const char* target = I18n::Get("ui.numbers.you_no_target", "you (no target selected)");
				if (const auto sel = RE::Console::GetSelectedRef()) {
					if (const char* nm = sel->GetName(); nm && nm[0]) {
						target = nm;
					} else {
						target = I18n::Get("ui.numbers.selected_reference", "selected reference");
					}
				}
				ImGuiMCP::Text(I18n::Get("ui.numbers.spawning_samples_on", "- spawning sample numbers on: %s"), target);
				Tip(I18n::Get("ui.tips.console_to_target", "Open the console and click an NPC (or 'prid <FormID>') to target it; otherwise it falls back to you."));
			} else {
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.numbers.sample_numbers_hint", "- sample numbers to tune motion/font/colors live"));
			}
			ImGuiMCP::Separator();

			ImGuiMCP::Checkbox(I18n::Get("ui.numbers.enable_floating_damage", "Enable floating damage numbers"), &s->enableFloatingDamage);
			Tip(I18n::Get("ui.tips.enable_floating_damage", "Master switch - off hides every number below regardless of the individual settings. Combat log/DPS tracking are unaffected."));
			ImGuiMCP::Separator();

			ImGuiMCP::BeginDisabled(!s->enableFloatingDamage);
			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.numbers.what_to_show", "What to show"), ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.your_damage", "Your damage"), &s->showPlayerDamageDealt);
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.follower_damage", "Follower damage"), &s->showFollowerDamageDealt);
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.npc_vs_npc_damage", "NPC-vs-NPC damage"), &s->showNPCOnNPCDamage);
				Tip(I18n::Get("ui.tips.npc_fights", "Fights you aren't part of. Rendered smaller with the NPC marker color, hidden beyond the visibility radius."));
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.damage_you_take", "Damage you take"), &s->showPlayerDamageTaken);
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.player_numbers_first_person", "Player numbers in first person"), &s->showFirstPersonNumbers);
				Tip(I18n::Get("ui.tips.first_person_pin", "Pinned to a screen spot on flat, anchored ahead of you in VR. Off = hidden while in first person."));
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.healing", "Healing"), &s->showHealing);
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.magicka_damage", "Magicka damage"), &s->showMagickaDamage);
				Tip(I18n::Get("ui.tips.magicka_drain_hostile", "Hostile drains only (shock spells, absorb effects). Your own casting costs never show."));
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.stamina_damage", "Stamina damage"), &s->showStaminaDamage);
				Tip(I18n::Get("ui.tips.stamina_drain_hostile", "Hostile drains only (frost spells, absorb effects). Sprinting and power attacks never show."));
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.mitigation_subtext", "Mitigation subtext"), &s->showMitigation);
				Tip(I18n::Get("ui.tips.mitigation_subtext", "The small \"(-45 armor)\" / \"(-20 resisted)\" line under a number."));
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.hit_location_tags", "Hit location tags"), &s->showHitLocation);
				Tip(I18n::Get("ui.tips.hit_location_tags", "HEADSHOT etc. on bow/crossbow hits, from where the arrow actually struck."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.npc_visibility_radius", "NPC visibility radius (m)"), &s->maxVisibilityRadiusMeters, 5.0f, 100.0f, "%.0f", 0);
				ImGuiMCP::SliderInt(I18n::Get("ui.numbers.max_numbers_on_screen", "Max numbers on screen"), &s->maxConcurrentQuads, 5, 128, "%d", 0);
			}

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.numbers.size", "Size"), ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
				const auto& avail = Fonts::Available();
				std::vector<const char*> fontNames;
				fontNames.reserve(avail.size() + 1);
				fontNames.push_back(I18n::Get("ui.common.auto", "(auto)"));
				int fontSel = 0;
				for (int i = 0; i < static_cast<int>(avail.size()); ++i) {
					fontNames.push_back(avail[static_cast<std::size_t>(i)].first.c_str());
					if (avail[static_cast<std::size_t>(i)].second == s->fontPath) {
						fontSel = i + 1;
					}
				}
				if (SearchableCombo(I18n::Get("ui.numbers.font", "Font"), &fontSel, fontNames)) {
					s->fontPath = fontSel == 0 ? std::string{} : avail[static_cast<std::size_t>(fontSel - 1)].second;
				}
				Tip(I18n::Get("ui.tips.font_filter", "Type to filter your installed fonts. Applies on game restart; (auto) uses the mod font, then a bold Windows system font."));
				ImGuiMCP::SameLine(0.0f, -1.0f);
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.common.restart", "(restart)"));
				if (ImGuiMCP::Button(I18n::Get("ui.numbers.open_fonts_folder", "Open fonts folder"), { 0, 0 })) {
					OpenFolder("Data/SKSE/Plugins/FloatingDamageNG/Fonts");
				}
				ImGuiMCP::SameLine(0.0f, -1.0f);
				if (ImGuiMCP::Button(I18n::Get("ui.numbers.rescan", "Rescan"), { 0, 0 })) {
					Fonts::RefreshAvailable();
				}
				Tip(I18n::Get("ui.tips.drop_fonts", "Drop .ttf/.otf files in the folder, click Rescan to list them here. A picked font takes effect on the next game start."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.font_size_px", "Font size (px)"), &s->baseFontPixels, 16.0f, 128.0f, "%.0f", 0);
				Tip(I18n::Get("ui.tips.atlas_resolution", "The atlas resolution numbers rasterize at. Higher = crisper and larger; applies immediately."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.size_multiplier", "Size multiplier"), &s->baseFontScale, 0.1f, 3.0f, "%.2f", 0);
				Tip(I18n::Get("ui.tips.overall_size", "Overall number size. The final size is capped by 'Max size multiplier' below."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.big_hits_grow_by", "Big hits grow by"), &s->logScaleModifier, 0.0f, 1.0f, "%.2f", 0);
				Tip(I18n::Get("ui.tips.big_hits_grow_log", "How much larger high-damage numbers render (logarithmic in the damage). 0 = all numbers equal size."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.max_size_multiplier", "Max size multiplier"), &s->maxFontScaleCeiling, 1.0f, 4.0f, "%.2f", 0);
				Tip(I18n::Get("ui.tips.ceiling", "Ceiling on the final size. Kept at or above the base multiplier so raising the base always has an effect."));
				s->maxFontScaleCeiling = std::max(s->maxFontScaleCeiling, s->baseFontScale);
				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.abbreviate_big_numbers", "Abbreviate big numbers"), &s->abbreviateNumbers);
				Tip(I18n::Get("ui.tips.abbreviate_numbers", "Show 10000+ as 1.2k / 3.4M to keep late-game numbers compact."));

				ImGuiMCP::SeparatorText(I18n::Get("ui.numbers.distance", "Distance"));
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.tips.distance_readability", "How size responds to how far the target is (ranged readability)."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.reference_distance_m", "Reference distance (m)"), &s->distanceRefMeters, 1.0f, 30.0f, "%.1f", 0);
				Tip(I18n::Get("ui.tips.reference_distance", "Numbers are full size within this range. Raise it to keep numbers big further out (e.g. for bows)."));
				if (REL::Module::IsVR()) {
					ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.max_size_boost_distance", "Max size boost at distance"), &s->vrDistanceMaxBoost, 1.0f, 16.0f, "%.1f", 0);
					Tip(I18n::Get("ui.tips.distance_boost_vr", "How much far numbers grow to stay readable at range. Higher = bigger distant numbers."));
				} else {
					ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.min_size_at_distance", "Min size at distance"), &s->flatDistanceMinScale, 0.1f, 1.5f, "%.2f", 0);
					Tip(I18n::Get("ui.tips.distance_min_scale", "How small a far number may shrink. 1.0 = never shrinks. Raise this if ranged numbers look too small."));
				}
			}

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.numbers.motion_effect", "Motion effect"), ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
				const auto& presets = Presets::All();
				static std::string lastLoaded;
				const Presets::Effect* match = MatchingPreset(s, presets);
				std::string preview;
				if (match) {
					preview = match->name;
				} else if (!lastLoaded.empty()) {
					preview = std::format("{} {}", lastLoaded, I18n::Get("ui.common.modified", "(modified)"));
				} else {
					preview = I18n::Get("ui.common.custom", "Custom");
				}
				if (ImGuiMCP::BeginCombo(I18n::Get("ui.numbers.preset", "Preset"), preview.c_str(), 0)) {
					for (const auto& p : presets) {
						if (ImGuiMCP::Selectable(p.name.c_str(), false, 0, { 0, 0 })) {
							s->motion = p.motion;
							s->spreadPattern = p.spread;
							s->spawnAngleDeg = p.spawnAngleDeg;
							lastLoaded = p.name;
						}
					}
					ImGuiMCP::EndCombo();
				}
				Tip(I18n::Get("ui.tips.load_preset", "Load a starting point (built-ins plus any JSON in Data/SKSE/Plugins/FloatingDamageNG/Presets), then tune the sliders. Built-ins are defined the same way."));
				if (match && !match->description.empty()) {
					ImGuiMCP::TextDisabled("%s", match->description.c_str());
				}
				if (match && !match->source.empty()) {
					ImGuiMCP::TextDisabled(I18n::Get("ui.numbers.source_label", "Source: %s"), match->source.c_str());
				}
				if (match && IsWebUrl(match->url)) {
					LinkText(match->url);
				}
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.rise_speed", "Rise speed (units/s)"), &s->motion.riseSpeed, -50.0f, 200.0f, "%.0f", 0);
				Tip(I18n::Get("ui.tips.rise_speed", "How fast a number rises. Negative makes it sink."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.vertical_accel", "Vertical accel"), &s->motion.riseAccel, -400.0f, 200.0f, "%.0f", 0);
				Tip(I18n::Get("ui.tips.vertical_accel", "Negative = gravity: arcs the number up then pulls it back down (arc/fireworks). Positive = accelerates upward. 0 = steady rise."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.launch_speed", "Launch speed (units/s)"), &s->motion.lateralSpeed, 0.0f, 250.0f, "%.0f", 0);
				Tip(I18n::Get("ui.tips.lateral_speed", "How fast a number shoots sideways / outward."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.slowdown", "Slowdown"), &s->motion.lateralDamping, 0.0f, 20.0f, "%.1f", 0);
				Tip(I18n::Get("ui.tips.slowdown", "0 = keeps its speed; higher = bursts out fast then coasts to a stop (Radial/Fireworks feel)."));

				int pattern = static_cast<int>(s->spreadPattern);
				const char* patterns[] = {
					I18n::Get("ui.numbers.spread_alternate", "Alternate (left/right)"),
					I18n::Get("ui.numbers.spread_rotate", "Rotate (fireworks)"),
					I18n::Get("ui.numbers.spread_diagonal", "Diagonal alternate")
				};
				if (ImGuiMCP::Combo(I18n::Get("ui.numbers.spread_pattern", "Spread pattern"), &pattern, patterns, 3, -1)) {
					s->spreadPattern = static_cast<SpreadPattern>(pattern);
				}
				Tip(I18n::Get("ui.tips.spread_pattern", "How successive hits on one target fan out so they don't overlap."));
				if (s->spreadPattern == SpreadPattern::kAlternate) {
					ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.rapid_hit_spacing", "Rapid-hit spacing"), &s->rapidHitSpread, 0.0f, 120.0f, "%.0f", 0);
					ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.side_bias", "Side bias"), &s->rapidHitBias, -1.0f, 1.0f, "%.2f", 0);
					Tip(I18n::Get("ui.tips.side_bias", "-1 all left, 0 even alternation, +1 all right."));
				} else if (s->spreadPattern == SpreadPattern::kRotate) {
					ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.rotation_per_hit", "Rotation per hit (deg)"), &s->spawnAngleDeg, 0.0f, 360.0f, "%.0f", 0);
					Tip(I18n::Get("ui.tips.rotation_per_hit", "Degrees each successive number turns around the target (144 = a 5-point star spray)."));
				} else {
					ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.diagonal_tilt", "Diagonal tilt (deg)"), &s->spawnAngleDeg, 0.0f, 90.0f, "%.0f", 0);
					Tip(I18n::Get("ui.tips.diagonal_tilt", "Launch tilt above horizontal for the alternating diagonals."));
				}

				ImGuiMCP::Checkbox(I18n::Get("ui.numbers.squash_and_stretch", "Squash and stretch"), &s->squashStretch);
				Tip(I18n::Get("ui.tips.squash_stretch", "Distort numbers vertically while they fly fast, easing to normal as they settle (launch juice)."));
				if (s->squashStretch) {
					ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.stretch_amount", "Stretch amount"), &s->stretchIntensity, 0.0f, 1.0f, "%.2f", 0);
				}
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.speed", "Speed"), &s->globalSpeedMultiplier, 0.25f, 3.0f, "%.2f", 0);
				Tip(I18n::Get("ui.tips.global_speed", "Scales how fast the whole animation plays - including how fast it ages, so a higher speed also fades numbers sooner. Raise Lifetime to keep them on screen as long."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.lifetime", "Lifetime (s)"), &s->quadLifetimeSeconds, 0.5f, 4.0f, "%.2f", 0);

				ImGuiMCP::Separator();
				static char presetName[48]{};
				static char presetDesc[128]{};
				static char presetSrc[128]{};
				static char presetUrl[192]{};
				static std::string saveMsg;
				if (ImGuiMCP::InputTextWithHint("##fdng_preset_name", I18n::Get("ui.numbers.preset_name_to_save", "Preset name to save..."), presetName, sizeof(presetName), 0, nullptr, nullptr)) {
					saveMsg.clear();
				}
				ImGuiMCP::InputTextWithHint("##fdng_preset_desc", I18n::Get("ui.numbers.description_optional", "Description (optional)"), presetDesc, sizeof(presetDesc), 0, nullptr, nullptr);
				ImGuiMCP::InputTextWithHint("##fdng_preset_src", I18n::Get("ui.numbers.source_credit_optional", "Source / credit (optional)"), presetSrc, sizeof(presetSrc), 0, nullptr, nullptr);
				ImGuiMCP::InputTextWithHint("##fdng_preset_url", I18n::Get("ui.numbers.link_optional", "Link (http/https, optional)"), presetUrl, sizeof(presetUrl), 0, nullptr, nullptr);
				if (ImGuiMCP::Button(I18n::Get("ui.numbers.save_preset", "Save preset"), { 0, 0 }) && presetName[0]) {
					if (Presets::Save({ presetName, false, s->motion, s->spreadPattern, s->spawnAngleDeg, presetDesc, presetSrc, presetUrl })) {
						saveMsg = I18n::Get("ui.numbers.saved_msg", "Saved '{}'.");
						size_t pos = saveMsg.find("{}");
						if (pos != std::string::npos) saveMsg.replace(pos, 2, presetName);
						presetName[0] = presetDesc[0] = presetSrc[0] = presetUrl[0] = '\0';
					} else {
						saveMsg = I18n::Get("ui.numbers.save_failed_msg", "Save failed - use a unique name (letters, numbers, spaces, - or _; not a built-in).");
					}
				}
				ImGuiMCP::SameLine(0.0f, -1.0f);
				if (ImGuiMCP::Button(I18n::Get("ui.numbers.reload_presets", "Reload presets"), { 0, 0 })) {
					Presets::Reload();
				}
				ImGuiMCP::SameLine(0.0f, -1.0f);
				if (ImGuiMCP::Button(I18n::Get("ui.common.open_folder", "Open folder"), { 0, 0 })) {
					OpenFolder("Data/SKSE/Plugins/FloatingDamageNG/Presets");
				}
				if (!saveMsg.empty()) {
					ImGuiMCP::TextDisabled("%s", saveMsg.c_str());
				} else {
					ImGuiMCP::TextDisabled("%s", I18n::Get("ui.tips.saved_to_presets", "Saved to Data/SKSE/Plugins/FloatingDamageNG/Presets - share the .json to trade effects."));
				}
			}

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.numbers.per_type_effects", "Per-type effects"), 0)) {
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.tips.per_type_effects", "Give a damage type its own motion and/or font (e.g. fire sprays, frost drifts). (global) uses the settings above; a font change applies on restart."));
				const auto& presets = Presets::All();
				std::vector<const char*> motionOpts{ I18n::Get("ui.common.global", "(global)") };
				for (const auto& p : presets) {
					motionOpts.push_back(p.name.c_str());
				}
				const auto& avail = Fonts::Available();
				std::vector<const char*> fontOpts{ I18n::Get("ui.common.global", "(global)") };
				for (const auto& f : avail) {
					fontOpts.push_back(f.first.c_str());
				}
				constexpr auto tblFlags = ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_BordersInnerH | ImGuiMCP::ImGuiTableFlags_SizingStretchProp;
				if (ImGuiMCP::BeginTable("##fdng_pertype", 3, tblFlags, { 0, 0 }, 0.0f)) {
					ImGuiMCP::TableSetupColumn(I18n::Get("ui.numbers.damage_type_col", "Damage type"), 0, 0.30f, 0);
					ImGuiMCP::TableSetupColumn(I18n::Get("ui.numbers.motion_col", "Motion"), 0, 0.35f, 0);
					ImGuiMCP::TableSetupColumn(I18n::Get("ui.numbers.font_restart_col", "Font (restart)"), 0, 0.35f, 0);
					ImGuiMCP::TableHeadersRow();
					for (std::size_t idx = 0; idx < kPerKindMeta.size(); ++idx) {
						const int i = static_cast<int>(idx);
						ImGuiMCP::TableNextRow(0, 0.0f);
						ImGuiMCP::TableSetColumnIndex(0);
						// Translate per-kind labels at display time
						ImGuiMCP::Text("%s", I18n::Get(kPerKindMeta[idx].label, kPerKindMeta[idx].label));

						ImGuiMCP::TableSetColumnIndex(1);
						ImGuiMCP::SetNextItemWidth(-1.0f);
						int mSel = 0;
						for (int j = 0; j < static_cast<int>(presets.size()); ++j) {
							if (presets[static_cast<std::size_t>(j)].name == s->motionByKind[idx]) {
								mSel = j + 1;
								break;
							}
						}
						const auto mId = std::format("##fdng_km{}", i);
						if (SearchableCombo(mId.c_str(), &mSel, motionOpts)) {
							s->motionByKind[idx] = mSel == 0 ? std::string{} : presets[static_cast<std::size_t>(mSel - 1)].name;
						}

						ImGuiMCP::TableSetColumnIndex(2);
						ImGuiMCP::SetNextItemWidth(-1.0f);
						int fSel = 0;
						for (int j = 0; j < static_cast<int>(avail.size()); ++j) {
							if (avail[static_cast<std::size_t>(j)].second == s->fontByKind[idx]) {
								fSel = j + 1;
								break;
							}
						}
						const auto fId = std::format("##fdng_kf{}", i);
						if (SearchableCombo(fId.c_str(), &fSel, fontOpts)) {
							s->fontByKind[idx] = fSel == 0 ? std::string{} : avail[static_cast<std::size_t>(fSel - 1)].second;
						}
					}
					ImGuiMCP::EndTable();
				}
			}

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.numbers.spawn_origin", "Spawn origin"), 0)) {
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.offset_up", "Offset up (game units)"), &s->originOffsetUp, -80.0f, 120.0f, "%.0f", 0);
				Tip(I18n::Get("ui.tips.spawn_origin", "Shift the spawn point relative to the target's head (~70 game units = 1 m). Use Live preview at the top to see it."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.offset_toward", "Offset toward you (game units)"), &s->originOffsetToward, -80.0f, 80.0f, "%.0f", 0);
				Tip(I18n::Get("ui.tips.spawn_origin", "Shift the spawn point relative to the target's head (~70 game units = 1 m). Use Live preview at the top to see it."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.offset_sideways", "Offset sideways (game units)"), &s->originOffsetSide, -80.0f, 80.0f, "%.0f", 0);
				Tip(I18n::Get("ui.tips.spawn_origin", "Shift the spawn point relative to the target's head (~70 game units = 1 m). Use Live preview at the top to see it."));
			}

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.numbers.thresholds", "Thresholds"), 0)) {
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.min_damage_to_show", "Min damage to show"), &s->minDamageToShow, 0.0f, 25.0f, "%.1f", 0);
				Tip(I18n::Get("ui.tips.min_damage_to_show", "Smaller ticks pool up until they cross this, then show as one number."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.min_heal_to_show", "Min heal to show"), &s->minHealToShow, 1.0f, 50.0f, "%.1f", 0);
				Tip(I18n::Get("ui.tips.min_heal_to_show", "Filters natural health regen; real heals accumulate past it quickly."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.merge_window", "Merge window (s)"), &s->dotAccumulationWindow, 0.1f, 2.0f, "%.2f", 0);
				Tip(I18n::Get("ui.tips.merge_window", "Repeat hits of the same type on one target within this window add into the existing number."));
			}

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.numbers.colors_and_style", "Colors and style"), 0)) {
				int style = static_cast<int>(s->originStyle);
				const char* styles[] = {
					I18n::Get("ui.numbers.style_colored_outline", "Colored outline"),
					I18n::Get("ui.numbers.style_underline", "Underline"),
					I18n::Get("ui.numbers.style_box", "Box"),
					I18n::Get("ui.numbers.style_none", "None (hide source)")
				};
				if (ImGuiMCP::Combo(I18n::Get("ui.numbers.origin_marker", "Origin marker"), &style, styles, 4, -1)) {
					s->originStyle = static_cast<OriginStyle>(style);
				}
				Tip(I18n::Get("ui.tips.origin_marker", "How a number shows whose fight it is. The number's own color always means the damage type; the marker uses the four 'Marker' colors below."));
				ImGuiMCP::SliderFloat(I18n::Get("ui.numbers.marker_thickness", "Marker thickness"), &s->styleThickness, 0.5f, 6.0f, "%.1f", 0);
				DrawStylePreview(s);
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.tips.preview_note", "Preview shows colors and marker style only (menu font, not your chosen game font)."));
				for (const auto& def : kColorTable) {
					ColorRow(def.uiLabel, s->*def.field);
				}
			}

			ImGuiMCP::EndDisabled();

			SaveRow(s);
		}

		// "Analytics & Debug" page: the combat-log capture module (whose data
		// the Combat Stats page browses) plus diagnostics.
		void __stdcall RenderAnalytics()
		{
			auto* s = Settings::GetSingleton();

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.analytics.analytics", "Analytics"), ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.combat_log", "Combat log"), &s->enableCombatLog);
				Tip(I18n::Get("ui.tips.combat_log", "Track each fight: per-combatant damage, healing, crits, time-to-die, DPS. Feeds the Combat Stats page."));
				ImGuiMCP::BeginDisabled(!s->enableCombatLog);
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.write_sessions_to_disk", "Write sessions to disk"), &s->writeLogToDisk);
				Tip(I18n::Get("ui.tips.write_sessions", "Appends session reports to FloatingDamageNG-combat.log next to your SKSE logs (rotated at 5 MB)."));
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.export_jsonl", "Export JSONL"), &s->exportJsonl);
				Tip(I18n::Get("ui.tips.export_jsonl", "One JSON object per session to FloatingDamageNG-sessions.jsonl - for pandas/jq/dashboards. Full drill-down data included."));
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.export_csv", "Export CSV"), &s->exportCsv);
				Tip(I18n::Get("ui.tips.export_csv", "One row per combatant per session to FloatingDamageNG-combatants.csv - for Excel/Sheets."));
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.live_dps_readout", "Live DPS readout"), &s->enableLiveDPSWindow);
				Tip(I18n::Get("ui.tips.live_dps_readout", "Small combat overlay: top-right on flat, head-locked HUD plane in VR."));
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.include_followers_in_reports", "Include followers in reports"), &s->logFollowerPerformance);
				if (ImGuiMCP::Checkbox(I18n::Get("ui.analytics.devbench_integration", "devbench integration"), &s->enableDevBench) && s->enableDevBench) {
					DevBench::Connect();
				}
				Tip(I18n::Get("ui.tips.devbench", "Registers a floatingdamage.stats tool with the devbench dev harness, exposing stats on its local port. For tooling; leave off otherwise."));
				if (s->enableDevBench) {
					if (DevBench::IsConnected()) {
						if (const int port = ReadDevBenchPort(); port > 0) {
							ImGuiMCP::Text(I18n::Get("ui.analytics.devbench_bound", "devbench host present (build %u), bound on 127.0.0.1:%d"), DevBench::HostBuild(), port);
						} else {
							ImGuiMCP::Text(I18n::Get("ui.analytics.devbench_pending", "devbench host present (build %u); port pending in runtime.json"), DevBench::HostBuild());
						}
						ImGuiMCP::TextDisabled("%s", I18n::Get("ui.analytics.devbench_tool_info", "Tool: floatingdamage.stats (MCP + REST); event: floatingdamage.sessionEnded"));
					} else {
						ImGuiMCP::TextDisabled("%s", I18n::Get("ui.analytics.devbench_not_detected", "devbench host not detected - install the devbench SKSE plugin."));
					}
				}
				ImGuiMCP::EndDisabled();
			}

			if (ImGuiMCP::CollapsingHeader(I18n::Get("ui.analytics.advanced", "Advanced"), 0)) {
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.debug_log", "Debug log"), &s->debugLog);
				Tip(I18n::Get("ui.tips.debug_log", "Writes per-event capture traces to FloatingDamageNG.log. For bug reports."));
				ImGuiMCP::Checkbox(I18n::Get("ui.analytics.delta_audit", "Delta audit"), &s->deltaAudit);
				Tip(I18n::Get("ui.tips.delta_audit", "Once a second, compares each fighter's observed health change against captured events and warns in the log about damage the mod missed."));
			}

			SaveRow(s);
		}

		bool NameMatchesFilter(const std::string& a_name, const char* a_filter)
		{
			if (!a_filter || !a_filter[0]) {
				return true;
			}
			const auto icontains = [](const std::string& hay, const char* needle) {
				const auto it = std::search(hay.begin(), hay.end(), needle, needle + std::strlen(needle),
					[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
				return it != hay.end();
			};
			return icontains(a_name, a_filter);
		}

		void DrawCombatantTable(const CombatLog::SessionSummary& a_session, const char* a_filter)
		{
			constexpr auto flags = ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_BordersInnerH |
			                       ImGuiMCP::ImGuiTableFlags_SizingStretchProp | ImGuiMCP::ImGuiTableFlags_Sortable;
			const auto id = std::format("##fdng_tbl{}", a_session.index);
			if (!ImGuiMCP::BeginTable(id.c_str(), 7, flags, { 0, 0 }, 0.0f)) {
				return;
			}
			ImGuiMCP::TableSetupColumn(I18n::Get("ui.stats.combatant", "Combatant"), 0, 0.30f, 0);
			ImGuiMCP::TableSetupColumn(I18n::Get("ui.stats.dealt", "Dealt"), ImGuiMCP::ImGuiTableColumnFlags_DefaultSort | ImGuiMCP::ImGuiTableColumnFlags_PreferSortDescending, 0.12f, 1);
			ImGuiMCP::TableSetupColumn(I18n::Get("ui.stats.dps", "DPS"), ImGuiMCP::ImGuiTableColumnFlags_PreferSortDescending, 0.10f, 2);
			ImGuiMCP::TableSetupColumn(I18n::Get("ui.stats.crit_pct", "Crit%"), ImGuiMCP::ImGuiTableColumnFlags_PreferSortDescending, 0.10f, 3);
			ImGuiMCP::TableSetupColumn(I18n::Get("ui.stats.taken", "Taken"), ImGuiMCP::ImGuiTableColumnFlags_PreferSortDescending, 0.12f, 4);
			ImGuiMCP::TableSetupColumn(I18n::Get("ui.stats.healed", "Healed"), ImGuiMCP::ImGuiTableColumnFlags_PreferSortDescending, 0.12f, 5);
			ImGuiMCP::TableSetupColumn(I18n::Get("ui.stats.fate", "Fate"), ImGuiMCP::ImGuiTableColumnFlags_NoSort, 0.14f, 6);
			ImGuiMCP::TableHeadersRow();

			const float duration = std::max(a_session.duration, 0.01f);

			std::vector<const CombatLog::CombatantSummary*> rows;
			rows.reserve(a_session.combatants.size());
			for (const auto& c : a_session.combatants) {
				if (NameMatchesFilter(c.name, a_filter)) {
					rows.push_back(&c);
				}
			}
			if (const auto* specs = ImGuiMCP::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
				const auto& spec = specs->Specs[0];
				const bool asc = spec.SortDirection == ImGuiMCP::ImGuiSortDirection_Ascending;
				std::sort(rows.begin(), rows.end(), [&](const auto* a, const auto* b) {
					const auto key = [&](const CombatLog::CombatantSummary& c) -> float {
						switch (spec.ColumnUserID) {
						case 1:
							return c.damageDealt;
						case 2:
							return c.damageDealt / duration;
						case 3:
							return c.hitsDealt > 0 ? static_cast<float>(c.critsDealt) / c.hitsDealt : 0.0f;
						case 4:
							return c.damageTaken;
						case 5:
							return c.healingReceived;
						default:
							return 0.0f;
						}
					};
					if (spec.ColumnUserID == 0) {
						const auto iless = [](const std::string& x, const std::string& y) {
							return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
								[](char l, char r) { return std::tolower(static_cast<unsigned char>(l)) < std::tolower(static_cast<unsigned char>(r)); });
						};
						return asc ? iless(a->name, b->name) : iless(b->name, a->name);
					}
					return asc ? key(*a) < key(*b) : key(*b) < key(*a);
				});
			}

			for (const auto* row : rows) {
				const auto& c = *row;
				ImGuiMCP::TableNextRow(0, 0.0f);
				ImGuiMCP::TableSetColumnIndex(0);
				ImGuiMCP::Text("%s%s", c.name.c_str(),
					c.isPlayer ? I18n::Get("ui.stats.you", " (you)") : c.isFollower ? I18n::Get("ui.stats.follower", " (follower)") :
														   (c.isHostileToPlayer ? "" : I18n::Get("ui.stats.neutral", " (neutral)")));
				ImGuiMCP::TableSetColumnIndex(1);
				ImGuiMCP::Text("%.0f", c.damageDealt);
				ImGuiMCP::TableSetColumnIndex(2);
				ImGuiMCP::Text("%.1f", c.damageDealt / duration);
				ImGuiMCP::TableSetColumnIndex(3);
				ImGuiMCP::Text("%d", c.hitsDealt > 0 ? (100 * c.critsDealt / c.hitsDealt) : 0);
				ImGuiMCP::TableSetColumnIndex(4);
				ImGuiMCP::Text("%.0f", c.damageTaken);
				ImGuiMCP::TableSetColumnIndex(5);
				if (c.healingReceived > 0.0f) {
					char healBuf[16];
					snprintf(healBuf, sizeof(healBuf),
						I18n::Get("ui.stats.healing_format", "+%.0f"), c.healingReceived);
					ImGuiMCP::Text("%s", healBuf);
				}
				ImGuiMCP::TableSetColumnIndex(6);
				if (c.died) {
					if (c.timeToDie >= 0.0f) {
						ImGuiMCP::Text(I18n::Get("ui.stats.died_with_time", "died (%.1fs)"), c.timeToDie);
					} else {
						ImGuiMCP::Text("%s", I18n::Get("ui.stats.died", "died"));
					}
				} else if (c.fled) {
					ImGuiMCP::TextDisabled("%s", I18n::Get("ui.stats.fled", "fled"));
				}
			}
			ImGuiMCP::EndTable();
		}

		void DrawMeterBars(const std::vector<CombatLog::BreakdownRow>& a_rows, bool a_kindColored, bool a_showMitigated)
		{
			if (a_rows.empty()) {
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.stats.none", "none"));
				return;
			}
			const auto settings = Settings::GetSingleton();
			float grand = 0.0f;
			for (const auto& r : a_rows) {
				grand += r.total;
			}
			auto* dl = ImGuiMCP::GetWindowDrawList();
			const float fontH = ImGuiMCP::GetFontSize();
			const float rowH = fontH + 6.0f;
			const float width = 540.0f;

			for (const auto& r : a_rows) {
				ImGuiMCP::ImVec2 base;
				ImGuiMCP::GetCursorScreenPos(&base);
				const float frac = grand > 0.0f ? r.total / grand : 0.0f;
				const auto rgb = a_kindColored ? KindRgb(*settings, r.kind) : settings->colorPhysical;
				ImGuiMCP::ImDrawListManager::AddRectFilled(dl, base, { base.x + width, base.y + rowH }, 0x40000000, 3.0f, 0);
				ImGuiMCP::ImDrawListManager::AddRectFilled(dl, base, { base.x + width * frac, base.y + rowH }, ToImCol(rgb, 0x78), 3.0f, 0);

				ImGuiMCP::ImDrawListManager::AddText(dl, { base.x + 6.0f, base.y + 3.0f }, ToImCol(0xFFFFFF), r.name.c_str());
				char rightBuf[128];
				const float pct = 100.0f * frac;
				snprintf(rightBuf, sizeof(rightBuf),
					I18n::Get("ui.stats.damage_bar", "%.0f  (%.0f%%)  %d hit%s"),
					r.total, pct, r.hits, r.hits == 1 ? "" : "s");
				std::string right = rightBuf;
				if (r.crits > 0) {
					char critBuf[32];
					snprintf(critBuf, sizeof(critBuf),
						I18n::Get("ui.stats.crit_bar", ", %d crit%s"),
						r.crits, r.crits == 1 ? "" : "s");
					right += critBuf;
				}
				if (a_showMitigated && r.mitigated > 0.0f && r.total + r.mitigated > 0.0f) {
					const float resistPct = 100.0f * r.mitigated / (r.total + r.mitigated);
					char resistBuf[32];
					snprintf(resistBuf, sizeof(resistBuf),
						I18n::Get("ui.stats.resisted_bar", "  [%.0f%% resisted]"), resistPct);
					right += resistBuf;
				}
				ImGuiMCP::ImVec2 rightSz;
				ImGuiMCP::CalcTextSize(&rightSz, right.c_str(), nullptr, false, -1.0f);
				ImGuiMCP::ImDrawListManager::AddText(dl, { base.x + width - rightSz.x - 6.0f, base.y + 3.0f }, ToImCol(0xE8E8E8), right.c_str());
				ImGuiMCP::Dummy({ width, rowH + 2.0f });
			}
		}

		void DrawSessionDrilldown(const CombatLog::SessionSummary& a_session)
		{
			if (a_session.combatants.empty()) {
				return;
			}
			static std::unordered_map<int, int> s_selected;
			if (s_selected.size() > 64) {
				s_selected.clear();
			}
			int& sel = s_selected[a_session.index];
			sel = std::clamp(sel, 0, static_cast<int>(a_session.combatants.size()) - 1);

			// Player's own name gets a "(you)" tag; owned here since it's not
			// stored on the summary (that's just the character name).
			std::vector<std::string> displayNames;
			displayNames.reserve(a_session.combatants.size());
			std::vector<const char*> names;
			names.reserve(a_session.combatants.size());
			for (const auto& c : a_session.combatants) {
				displayNames.push_back(c.isPlayer ? c.name + " (you)" : c.name);
			}
			for (const auto& n : displayNames) {
				names.push_back(n.c_str());
			}
			const auto comboID = std::format("Drill-down###fdng_drill{}", a_session.index);
			SearchableCombo(comboID.c_str(), &sel, names);

			const auto& c = a_session.combatants[static_cast<std::size_t>(sel)];

			ImGuiMCP::Text("%s", I18n::Get("ui.stats.damage_by_source", "Damage by source"));
			DrawMeterBars(c.bySource, true, true);
			ImGuiMCP::Text("%s", I18n::Get("ui.stats.damage_by_target", "Damage by target"));
			DrawMeterBars(c.byTarget, false, false);
			if (!c.takenByKind.empty()) {
				ImGuiMCP::Text("%s", I18n::Get("ui.stats.damage_taken_by_type", "Damage taken by type (resist profile)"));
				DrawMeterBars(c.takenByKind, true, true);
			}
			if (c.died) {
				ImGuiMCP::Text(I18n::Get("ui.stats.killed_by", "Killed by %s"), c.killedBy.c_str());
				for (const auto& line : c.deathRecap) {
					ImGuiMCP::BulletText("%s", line.c_str());
				}
			}
		}

		void __stdcall RenderStats()
		{
			const auto live = CombatLog::GetSingleton()->GetLiveStats();
			if (live.active) {
				ImGuiMCP::Text(I18n::Get("ui.stats.in_combat", "In combat: %.1fs | %.0f dmg | DPS %.1f real / %.1f active"),
					live.sessionSeconds, live.playerDamage, live.realDPS, live.activeDPS);
			} else if (!Settings::GetSingleton()->enableCombatLog) {
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.stats.combat_log_disabled", "Combat log is disabled - enable it in Settings > Analytics."));
			} else {
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.stats.not_in_combat", "Not in combat."));
			}
			ImGuiMCP::Separator();

			const auto history = CombatLog::GetSingleton()->GetHistory();
			if (history.empty()) {
				ImGuiMCP::TextDisabled("%s", I18n::Get("ui.stats.no_sessions_yet", "No finished combat sessions yet."));
				return;
			}

			static char s_filter[64]{};
			ImGuiMCP::InputTextWithHint("##fdng_filter", I18n::Get("ui.stats.filter_combatants", "Filter combatants..."), s_filter, sizeof(s_filter), 0, nullptr, nullptr);

			for (auto it = history.rbegin(); it != history.rend(); ++it) {
				const auto& s = *it;
				char headerBuf[256];
				snprintf(headerBuf, sizeof(headerBuf),
					I18n::Get("ui.stats.session_header", "#%d - %s @ %s - %.1fs, %.0f dmg###fdng_session%d"),
					s.index, s.startedAt.c_str(), s.location.c_str(), s.duration, s.playerDamage, s.index);
				const std::string header = headerBuf;
				if (!ImGuiMCP::CollapsingHeader(header.c_str(), 0)) {
					continue;
				}
				ImGuiMCP::Text(I18n::Get("ui.stats.player_dps", "Player DPS %.1f real / %.1f active"), s.realDPS, s.activeDPS);
				if (!s.dpsSamples.empty()) {
					char overlayBuf[64];
					snprintf(overlayBuf, sizeof(overlayBuf), I18n::Get("ui.stats.player_dps_peak", "player dps, peak %.0f"), s.peakDPS);
					ImGuiMCP::PlotLines("##dps", s.dpsSamples.data(), static_cast<int>(s.dpsSamples.size()),
						0, overlayBuf, 0.0f, FLT_MAX, { -1.0f, 80.0f }, sizeof(float));
				}
				DrawCombatantTable(s, s_filter);
				DrawSessionDrilldown(s);
			}
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::info("SKSE Menu Framework not installed; in-game config UI disabled.");
			return;
		}
		SKSEMenuFramework::SetSection(I18n::Get("_meta.mod_cn", "Floating Damage NG"));
		SKSEMenuFramework::AddSectionItem(I18n::Get("mcm.numbers", "Numbers"), RenderNumbers);
		SKSEMenuFramework::AddSectionItem(I18n::Get("mcm.analytics_debug", "Analytics & Debug"), RenderAnalytics);
		SKSEMenuFramework::AddSectionItem(I18n::Get("mcm.combat_stats", "Combat Stats"), RenderStats);
		logger::info("Registered SMF pages (framework v{:.1f}).", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
