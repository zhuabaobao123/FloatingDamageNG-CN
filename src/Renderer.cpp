// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#include "Renderer.h"

#include "CombatLog.h"
#include "Fonts.h"
#include "I18n.h"
#include "ImGuiVRHelperClientSDK.h"
#include "NumberManager.h"
#include "Settings.h"
#include "StyleMetrics.h"

namespace FDNG::Renderer
{
	namespace
	{
		ImGuiVRHelperPluginAPI::Client g_vrClient;
		ImGuiVRHelperPluginAPI::Client g_vrHudClient;  // head-locked HUD plane (live DPS readout)

		std::atomic<bool> g_initialized{ false };
		ID3D11Device* g_d3dDevice = nullptr;
		ID3D11DeviceContext* g_d3dContext = nullptr;

		// Reused frame buffers (no steady-state allocation).
		std::vector<ResolvedNumber> g_resolved;
		std::vector<ImGuiVRHelperPluginAPI::WorldQuad> g_quads;

		// Base atlas px for numbers; the user scales it via fBaseFontPixels.
		float BaseFontPx() { return Settings::GetSingleton()->baseFontPixels; }
		constexpr float kPanelMarginPx = 16.0f;
		constexpr float kPanelGapPx = 12.0f;  // enough to prevent quad UV bleed; smaller = more panel capacity
		// World size of one panel pixel on a billboard (matches FloatingSubtitles' tuning).
		constexpr float kWorldMetersPerPanelPixel = 0.0016875f;
		// Quads are physically sized, so beyond the reference distance their
		// angular size drops below HMD readability (an NPC number is ~5 cm -
		// arc-minutes at a 20 m brawl). They grow with distance, capped so a
		// far skirmish reads as a hint rather than a billboard wall; the
		// reference distance and cap are the user's distanceRefMeters /
		// vrDistanceMaxBoost (flat uses the same reference with a shrink floor).
		// NPC in-view test margin beyond the screen edges, so head/camera
		// motion does not pop numbers at the periphery.
		constexpr float kFrustumMargin = 0.3f;

		ImU32 KindColor(const Number& a_n, float a_alpha)
		{
			const auto settings = Settings::GetSingleton();
			// Crit/blocked styling overrides the kind hue.
			const std::uint32_t rgb = a_n.flags.critical ? settings->colorCritical :
			                          a_n.flags.blocked  ? settings->colorBlocked :
			                                               KindRgb(*settings, a_n.kind);
			const auto a = static_cast<std::uint8_t>(std::clamp(a_alpha, 0.0f, 1.0f) * 255.0f);
			return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
		}

		bool PlayerInFirstPerson()
		{
			const auto camera = RE::PlayerCamera::GetSingleton();
			return camera && camera->IsInFirstPerson();
		}

		// Small live DPS readout; lingers for the configured fade
		// window after combat ends.
		void DrawLiveDPSWindow()
		{
			const auto settings = Settings::GetSingleton();
			if (!settings->enableCombatLog || !settings->enableLiveDPSWindow) {
				return;
			}
			const auto stats = CombatLog::GetSingleton()->GetLiveStats();
			if (!stats.active && stats.secondsSinceEnd > settings->postCombatWindowFadeSeconds) {
				return;
			}

			const float alpha = stats.active ? 0.85f : 0.85f * (1.0f - stats.secondsSinceEnd / settings->postCombatWindowFadeSeconds);
			const auto displaySize = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(ImVec2(displaySize.x - 20.0f, 20.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
			ImGui::SetNextWindowBgAlpha(alpha * 0.4f);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
			if (ImGui::Begin("##FDNGDPS", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
						ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
				char dpsBuf1[64], dpsBuf2[64];
				snprintf(dpsBuf1, sizeof(dpsBuf1),
					I18n::Get("ui.stats.dps_overlay_line1", "%.0f dmg  %.1fs"),
					stats.playerDamage, stats.sessionSeconds);
				ImGui::Text("%s", dpsBuf1);
				snprintf(dpsBuf2, sizeof(dpsBuf2),
					I18n::Get("ui.stats.dps_overlay_line2", "DPS %.1f real / %.1f active"),
					stats.realDPS, stats.activeDPS);
				ImGui::Text("%s", dpsBuf2);
			}
			ImGui::End();
			ImGui::PopStyleVar();
		}

		// The origin marker color encodes who's involved (relationship
		// palette; user-themable) - the fill hue is reserved for the damage
		// kind, so origin must not repurpose it.
		ImU32 RgbWithAlpha(std::uint32_t a_rgb, std::uint8_t a_alpha)
		{
			return IM_COL32((a_rgb >> 16) & 0xFF, (a_rgb >> 8) & 0xFF, a_rgb & 0xFF, a_alpha);
		}

		ImU32 OriginColor(OriginTier a_origin, std::uint8_t a_alpha)
		{
			const auto settings = Settings::GetSingleton();
			switch (a_origin) {
			case OriginTier::kPlayerVictim:
				return RgbWithAlpha(settings->colorOriginTaken, a_alpha);
			case OriginTier::kFollower:
				return RgbWithAlpha(settings->colorOriginFollower, a_alpha);
			case OriginTier::kNPC:
				return RgbWithAlpha(settings->colorOriginNPC, a_alpha);
			default:
				return RgbWithAlpha(settings->colorOriginPlayer, a_alpha);
			}
		}

		// Text with an 8-direction outline ring; a_thickness 0 degrades to a
		// plain drop shadow.
		void AddOutlinedText(ImDrawList* a_drawList, ImFont* a_font, float a_size, ImVec2 a_pos, ImU32 a_color, ImU32 a_outline, float a_thickness, const char* a_text)
		{
			if (a_thickness > 0.0f) {
				for (const auto& o : kRingOffsets) {
					a_drawList->AddText(a_font, a_size, ImVec2(a_pos.x + o[0] * a_thickness, a_pos.y + o[1] * a_thickness), a_outline, a_text);
				}
			} else {
				a_drawList->AddText(a_font, a_size, ImVec2(a_pos.x + 2.0f, a_pos.y + 2.0f), a_outline, a_text);
			}
			a_drawList->AddText(a_font, a_size, a_pos, a_color, a_text);
		}

		// Squash-and-stretch scale for one number: stretch vertically while it
		// travels fast (from the motion profile at its current age), easing to
		// 1:1 as it slows. (1,1) when disabled.
		ImVec2 StretchScale(const Number& a_n)
		{
			const auto settings = Settings::GetSingleton();
			if (!settings->squashStretch) {
				return { 1.0f, 1.0f };
			}
			const auto& m = a_n.motion;  // per-kind or global, resolved at spawn
			const float t = a_n.age * settings->globalSpeedMultiplier;
			const float vy = m.riseSpeed + m.riseAccel * t;
			const float vlat = m.lateralDamping > 0.001f ? m.lateralSpeed * std::exp(-m.lateralDamping * t) : m.lateralSpeed;
			const float speed = std::sqrt(vy * vy + vlat * vlat) * settings->globalSpeedMultiplier;
			constexpr float kRefSpeed = 130.0f;
			const float k = std::clamp(speed / kRefSpeed, 0.0f, 1.0f) * settings->stretchIntensity;
			return { 1.0f - 0.30f * k, 1.0f + 0.55f * k };  // squash x, stretch y
		}

		// Draw one number (text + optional mitigation subtext). Returns the
		// block extent drawn (including origin marker + any stretch headroom).
		ImVec2 DrawNumberBlock(ImDrawList* a_drawList, const ResolvedNumber& a_rn, ImVec2 a_topLeft, float a_fontPx)
		{
			const auto& n = *a_rn.number;
			ImFont* font = Fonts::ForKind(n.kind);  // per-kind override or the default
			const float mainPx = a_fontPx;
			const float subPx = a_fontPx * SubtextRatio(n.amount, n.mitigated);

			const ImVec2 mainSz = font->CalcTextSizeA(mainPx, FLT_MAX, 0.0f, n.text);
			ImVec2 blockSz = mainSz;
			ImVec2 subSz{ 0.0f, 0.0f };
			if (n.subtext[0] != '\0') {
				subSz = font->CalcTextSizeA(subPx, FLT_MAX, 0.0f, n.subtext);
				blockSz.x = std::max(blockSz.x, subSz.x);
				blockSz.y += subSz.y;
			}

			const auto settings = Settings::GetSingleton();
			const auto alpha = static_cast<std::uint8_t>(std::clamp(a_rn.alpha, 0.0f, 1.0f) * 255.0f);
			const ImU32 color = KindColor(n, a_rn.alpha);
			const ImU32 origin = OriginColor(n.origin, alpha);
			const float thickness = settings->styleThickness;

			// kOutline colors the glyph ring with the origin; kNone keeps the
			// same full-thickness ring but black (readable, no source shown);
			// underline/box use a thin black ring plus a separate marker shape.
			const bool ringStyle = settings->originStyle == OriginStyle::kOutline || settings->originStyle == OriginStyle::kNone;
			const ImU32 textOutline = settings->originStyle == OriginStyle::kOutline ? origin : IM_COL32(0, 0, 0, alpha);
			const float textThickness = ringStyle ? thickness : 1.0f;
			const auto metrics = ComputeStyleMetrics(settings->originStyle, thickness);

			// Base block, then reserve stretch headroom so the VR quad (which
			// samples this exact rect) never clips the enlarged pixels. The
			// content is drawn centered in the reserved rect and the emitted
			// vertices are scaled about that center afterward.
			const ImVec2 baseExtent{ blockSz.x + 2.0f * metrics.padX, metrics.padTop + blockSz.y + metrics.padBottom };
			const ImVec2 st = StretchScale(n);
			const ImVec2 extent{ baseExtent.x * std::max(st.x, 1.0f), baseExtent.y * std::max(st.y, 1.0f) };
			const ImVec2 shift{ (extent.x - baseExtent.x) * 0.5f, (extent.y - baseExtent.y) * 0.5f };
			const ImVec2 origin2{ a_topLeft.x + shift.x, a_topLeft.y + shift.y };  // base top-left inside reserved rect
			const ImVec2 content{ origin2.x + metrics.padX, origin2.y + metrics.padTop };

			const int vtxStart = a_drawList->VtxBuffer.Size;

			AddOutlinedText(a_drawList, font, mainPx,
				ImVec2(content.x + (blockSz.x - mainSz.x) * 0.5f, content.y), color, textOutline, textThickness, n.text);
			if (n.subtext[0] != '\0') {
				// Partial mitigation reads in the element's color too, dimmed.
				const ImU32 subColor = KindColor(n, a_rn.alpha * 0.8f);
				AddOutlinedText(a_drawList, font, subPx,
					ImVec2(content.x + (blockSz.x - subSz.x) * 0.5f, content.y + mainSz.y), subColor, textOutline, textThickness, n.subtext);
			}

			switch (settings->originStyle) {
			case OriginStyle::kUnderline:
				a_drawList->AddRectFilled(
					ImVec2(content.x, content.y + blockSz.y + kUnderlineGap),
					ImVec2(content.x + blockSz.x, content.y + blockSz.y + kUnderlineGap + thickness), origin);
				break;
			case OriginStyle::kBox:
				a_drawList->AddRect(
					ImVec2(origin2.x + thickness * 0.5f, origin2.y + thickness * 0.5f),
					ImVec2(origin2.x + blockSz.x + 2.0f * metrics.padX - thickness * 0.5f, origin2.y + metrics.padTop + blockSz.y + metrics.padBottom - thickness * 0.5f),
					origin, 3.0f, 0, thickness);
				break;
			default:
				break;
			}

			if (st.x != 1.0f || st.y != 1.0f) {
				// Anisotropic scale about the reserved-rect center (ImGui text
				// draws uniformly, so squash-stretch is a post-transform).
				const ImVec2 c{ a_topLeft.x + extent.x * 0.5f, a_topLeft.y + extent.y * 0.5f };
				for (int i = vtxStart; i < a_drawList->VtxBuffer.Size; ++i) {
					auto& v = a_drawList->VtxBuffer[i].pos;
					v.x = c.x + (v.x - c.x) * st.x;
					v.y = c.y + (v.y - c.y) * st.y;
				}
			}
			return extent;
		}

		// Shared camera pass (flat and VR - the VR camera tracks the HMD):
		// fills each resolved number's normalized screen coords, whether it
		// projects in front of the camera, and - for NPC-on-NPC numbers - an
		// in-view flag with a 30% margin so head/camera motion doesn't pop
		// numbers at the edge. Player-relevant numbers always count as in
		// view: the renderers re-anchor them in first person, so their
		// pre-anchor position must not demote them.
		void ProjectResolved()
		{
			const auto camera = RE::Main::WorldRootCamera();
			std::size_t projectedCount = 0;
			for (auto& rn : g_resolved) {
				float x = 0.0f, y = 0.0f, z = -1.0f;
				rn.projected = camera && camera->WorldPtToScreenPt3(rn.worldPos, x, y, z, 1e-5f) && z > 0.0f;
				rn.screenX = x;
				rn.screenY = y;
				rn.inView = rn.number->origin != OriginTier::kNPC ||
				            (rn.projected && x > -kFrustumMargin && x < 1.0f + kFrustumMargin && y > -kFrustumMargin && y < 1.0f + kFrustumMargin);
				projectedCount += rn.projected ? 1 : 0;
			}
			// If nothing projects the camera matrix is unusable this frame;
			// don't let a bad projection demote every NPC number.
			if (projectedCount == 0) {
				for (auto& rn : g_resolved) {
					rn.inView = true;
				}
			}
		}

		// VR: lay each number into its own sub-rect of the helper panel and
		// record a world-space billboard pointing at that sub-rect.
		void DrawWorldQuadFrame()
		{
			g_quads.clear();

			const ImVec2 panelSize = ImGui::GetIO().DisplaySize;
			if (panelSize.x <= 0.0f || panelSize.y <= 0.0f) {
				return;
			}

			ImGui::SetNextWindowPos({ 0.0f, 0.0f });
			ImGui::SetNextWindowSize(panelSize);
			ImGui::Begin("##FDNG", nullptr,
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
			auto* drawList = ImGui::GetWindowDrawList();

			const auto settings = Settings::GetSingleton();
			const bool firstPerson = PlayerInFirstPerson();

			// First person: re-anchor player-received numbers ~1 m ahead at
			// chest height (fresh heading each frame) instead of on the head,
			// which would sit on the camera.
			const auto player = RE::PlayerCharacter::GetSingleton();
			RE::NiPoint3 firstPersonAnchor;
			if (firstPerson && player) {
				const float heading = player->GetAngleZ();
				firstPersonAnchor = player->GetPosition();
				firstPersonAnchor.x += std::sin(heading) * 70.0f;
				firstPersonAnchor.y += std::cos(heading) * 70.0f;
				firstPersonAnchor.z += 100.0f;
			}

			// Panel space is finite and busy battles overflow it, so pack in
			// priority order: numbers actually in view first, then the
			// player's own numbers, then NPC-on-NPC nearest-first.
			// Out-of-view numbers still pack while space remains - they only
			// lose when something visible needs the room.
			const auto anchorPos = player ? player->GetPosition() : RE::NiPoint3{};
			std::sort(g_resolved.begin(), g_resolved.end(), [&](const ResolvedNumber& a, const ResolvedNumber& b) {
				if (a.inView != b.inView) {
					return a.inView;
				}
				const bool aNPC = a.number->origin == OriginTier::kNPC;
				const bool bNPC = b.number->origin == OriginTier::kNPC;
				if (aNPC != bNPC) {
					return bNPC;
				}
				if (!aNPC) {
					return false;  // player-relevant numbers keep pool order
				}
				return anchorPos.GetSquaredDistance(a.worldPos) < anchorPos.GetSquaredDistance(b.worldPos);
			});

			// Simple shelf packer across the panel.
			float penX = kPanelMarginPx;
			float penY = kPanelMarginPx;
			float rowH = 0.0f;
			std::size_t dropped = 0;
			std::size_t index = 0;

			for (const auto& rn : g_resolved) {
				++index;
				RE::NiPoint3 worldPos = rn.worldPos;
				if (firstPerson && rn.number->origin == OriginTier::kPlayerVictim) {
					if (!settings->showFirstPersonNumbers || !player) {
						continue;
					}
					worldPos = firstPersonAnchor + (rn.worldPos - rn.number->anchor);  // keep spread + kinematic motion
				}
				// Full alpha in the panel; the fade is baked into the text color,
				// so keep the drawn pixels and the quad in sync by drawing as-is.
				const float fontPx = BaseFontPx() * rn.scale;
				const auto metrics = ComputeStyleMetrics(settings->originStyle, settings->styleThickness);
				// Measure with the SAME font the drawer uses (per-kind override
				// or default) so the reserved quad matches the drawn glyphs.
				ImFont* qFont = Fonts::ForKind(rn.number->kind);
				const ImVec2 estimate = qFont->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, rn.number->text);
				// The subtext can be wider than the main text; reserve for it (at
				// the SAME dynamic ratio the drawer uses) or neighboring quads
				// sample each other's pixels.
				float blockW = estimate.x;
				float blockH = estimate.y;
				if (rn.number->subtext[0] != '\0') {
					const float subPx = fontPx * SubtextRatio(rn.number->amount, rn.number->mitigated);
					const ImVec2 subEstimate = qFont->CalcTextSizeA(subPx, FLT_MAX, 0.0f, rn.number->subtext);
					blockW = std::max(blockW, subEstimate.x);
					blockH += subEstimate.y;
				}
				blockW += 2.0f * metrics.padX + 8.0f;
				blockH += metrics.padTop + metrics.padBottom;

				if (penX + blockW > panelSize.x - kPanelMarginPx) {
					penX = kPanelMarginPx;
					penY += rowH + kPanelGapPx;
					rowH = 0.0f;
				}
				if (penY + blockH > panelSize.y - kPanelMarginPx) {
					// Panel full. Everything not yet packed is lower priority;
					// stop here rather than backfill smaller blocks around it.
					dropped = g_resolved.size() - (index - 1);
					break;
				}

				const ImVec2 blockSz = DrawNumberBlock(drawList, rn, ImVec2(penX, penY), fontPx);
				rowH = std::max(rowH, blockSz.y);

				const float distMeters = anchorPos.GetDistance(worldPos) * kGameUnitToMeter;
				const float distanceBoost = std::clamp(distMeters / settings->distanceRefMeters, 1.0f, settings->vrDistanceMaxBoost);
				const float heightMeters = blockSz.y * kWorldMetersPerPanelPixel * distanceBoost;

				RE::NiPoint3 quadPos = worldPos;
				quadPos.z += 0.5f * heightMeters / kGameUnitToMeter;  // pos is the quad center

				ImGuiVRHelperPluginAPI::WorldQuad quad{};
				quad.u0 = penX / panelSize.x;
				quad.v0 = penY / panelSize.y;
				quad.u1 = std::min((penX + blockSz.x) / panelSize.x, 1.0f);
				quad.v1 = std::min((penY + blockSz.y) / panelSize.y, 1.0f);
				quad.pos[0] = quadPos.x;
				quad.pos[1] = quadPos.y;
				quad.pos[2] = quadPos.z;
				quad.height_m = heightMeters;
				g_quads.push_back(quad);

				penX += blockW + kPanelGapPx;
			}

			if (Settings::GetSingleton()->debugLog) {
				if (dropped > 0) {
					logger::debug("VR panel full ({}x{}): {} of {} numbers dropped", panelSize.x, panelSize.y, dropped, g_resolved.size());
				}
				// 1 Hz stage trace: what reached the packer and what the first
				// NPC quad looks like (pos/size), to pinpoint display losses.
				static std::chrono::steady_clock::time_point lastTrace;
				const auto now = std::chrono::steady_clock::now();
				if (now - lastTrace > std::chrono::seconds(1) && !g_resolved.empty()) {
					lastTrace = now;
					std::size_t npc = 0;
					for (const auto& rn : g_resolved) {
						npc += rn.number->origin == OriginTier::kNPC ? 1 : 0;
					}
					logger::debug("VR pack: resolved={} npc={} quads={} dropped={} panel={}x{}",
						g_resolved.size(), npc, g_quads.size(), dropped, panelSize.x, panelSize.y);
					for (const auto& rn : g_resolved) {
						if (rn.number->origin == OriginTier::kNPC) {
							logger::debug("VR pack: first NPC '{}' scale={:.2f} alpha={:.2f} inView={} pos=({:.0f},{:.0f},{:.0f})",
								rn.number->text, rn.scale, rn.alpha, rn.inView, rn.worldPos.x, rn.worldPos.y, rn.worldPos.z);
							break;
						}
					}
				}
			}

			ImGui::End();
		}

		// Flat: draw each number at its projected screen position.
		void DrawFlatFrame()
		{
			ImGui::SetNextWindowPos({ 0.0f, 0.0f });
			ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
			ImGui::Begin("##FDNG", nullptr,
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
			auto* drawList = ImGui::GetWindowDrawList();

			const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto playerPos = player ? player->GetPosition() : RE::NiPoint3{};

			const auto settings = Settings::GetSingleton();
			const bool firstPerson = PlayerInFirstPerson();

			for (const auto& rn : g_resolved) {
				ImVec2 screenPos;
				float fontPx;
				if (firstPerson && rn.number->origin == OriginTier::kPlayerVictim) {
					// First person: player numbers pin to a configurable screen
					// spot (they'd otherwise sit on the camera), keeping the
					// vertical component of their motion.
					if (!settings->showFirstPersonNumbers) {
						continue;
					}
					const float risePx = (rn.worldPos.z - rn.number->anchor.z) * 0.75f;
					screenPos = ImVec2(displaySize.x * settings->firstPersonX,
						displaySize.y * settings->firstPersonY - risePx);
					fontPx = BaseFontPx() * rn.scale;
				} else {
					if (!rn.projected) {
						continue;
					}
					screenPos = ImVec2(displaySize.x * rn.screenX, displaySize.y * (1.0f - rn.screenY));

					// Perspective size: full size within the reference distance,
					// shrinking past it but floored so distant (ranged) hits stay
					// legible.
					const float distMeters = std::max(playerPos.GetDistance(rn.worldPos) * kGameUnitToMeter, 0.1f);
					const float perspective = std::clamp(settings->distanceRefMeters / distMeters, settings->flatDistanceMinScale, 1.25f);
					fontPx = BaseFontPx() * rn.scale * perspective;
				}

				// Center using the same font the drawer picks per kind.
				const ImVec2 sz = Fonts::ForKind(rn.number->kind)->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, rn.number->text);
				DrawNumberBlock(drawList, rn, ImVec2(screenPos.x - sz.x * 0.5f, screenPos.y - sz.y), fontPx);
			}

			ImGui::End();
		}

		struct CreateD3DAndSwapChain
		{
			static void thunk()
			{
				func();

				const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
				if (!renderer) {
					return;
				}
				const auto swapChain = reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain);
				if (!swapChain) {
					logger::error("couldn't find swapChain");
					return;
				}
				DXGI_SWAP_CHAIN_DESC desc{};
				if (FAILED(swapChain->GetDesc(std::addressof(desc)))) {
					logger::error("IDXGISwapChain::GetDesc failed.");
					return;
				}

				const auto device = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
				const auto context = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);

				if (REL::Module::IsVR()) {
					// VR renders through the helper's world quads; keep the
					// device/context for RenderHud.
					g_d3dDevice = device;
					g_d3dContext = context;
					g_initialized.store(true);
					logger::info("D3D captured - VR world-quad rendering via ImGuiVRHelper.");
				} else {
					ImGui::CreateContext();
					auto& io = ImGui::GetIO();
					io.IniFilename = nullptr;

					if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
						logger::error("ImGui initialization failed (Win32).");
						return;
					}
					if (!ImGui_ImplDX11_Init(device, context)) {
						logger::error("ImGui initialization failed (DX11).");
						return;
					}
					Fonts::Load();
					g_initialized.store(true);
					logger::info("ImGui initialized (flat).");
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// RE::HUDMenu::PostDisplay - per-frame draw entry point.
		struct PostDisplay
		{
			static void thunk(RE::IMenu* a_menu)
			{
				func(a_menu);

				if (!g_initialized.load()) {
					return;
				}

				// Drain the raw hook queue on the main thread - the hooks fire
				// on job/script threads and only capture POD.
				Capture::GetSingleton()->ProcessQueued();
				CombatLog::GetSingleton()->Tick();
				Capture::GetSingleton()->AuditTick();
				NumberManager::GetSingleton()->PreviewTick();

				if (REL::Module::IsVR()) {
					if (!WorldQuadActive()) {
						return;
					}

					NumberManager::GetSingleton()->Update(g_resolved);
					ProjectResolved();
					if (g_resolved.empty()) {
						g_vrClient.SubmitWorldQuads(nullptr, 0);  // clear stale billboards
																  // Still run RenderHud so the panel clears its last pixels.
					}

					static const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
					const ImVec2 displaySize{ static_cast<float>(screenSize.width), static_cast<float>(screenSize.height) };

					g_vrClient.RenderHud(g_d3dDevice, g_d3dContext, displaySize, []() {
						GImGui->NavWindowingTarget = nullptr;
						DrawWorldQuadFrame();
					});

					if (!g_resolved.empty()) {
						g_vrClient.SubmitWorldQuads(g_quads.data(), g_quads.size());
					}

					// The head-locked HUD plane renders every frame it exists;
					// DrawLiveDPSWindow gates itself on the settings, leaving
					// the panel transparent when disabled.
					if (g_vrHudClient.IsConnected()) {
						g_vrHudClient.RenderHud(g_d3dDevice, g_d3dContext, displaySize, []() {
							GImGui->NavWindowingTarget = nullptr;
							DrawLiveDPSWindow();
						});
					}
				} else {
					NumberManager::GetSingleton()->Update(g_resolved);
					ProjectResolved();

					ImGui_ImplDX11_NewFrame();
					ImGui_ImplWin32_NewFrame();
					{
						// Render at the game's real resolution (upscaler-safe).
						static const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
						auto& io = ImGui::GetIO();
						io.DisplaySize.x = static_cast<float>(screenSize.width);
						io.DisplaySize.y = static_cast<float>(screenSize.height);
					}
					ImGui::NewFrame();
					if (!g_resolved.empty()) {
						DrawFlatFrame();
					}
					DrawLiveDPSWindow();
					ImGui::EndFrame();
					ImGui::Render();
					ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static inline std::size_t idx{ 0x6 };
		};
	}

	bool WorldQuadActive()
	{
		return REL::Module::IsVR() && g_vrClient.IsConnected() && g_vrClient.HasWorldQuads();
	}

	void Connect()
	{
		if (!REL::Module::IsVR()) {
			return;
		}
		if (!g_vrClient.Connect("FloatingDamageNG", FDNG_VERSION_STRING, ImGuiVRHelperPluginAPI::kClientFlag_WorldQuad)) {
			logger::warn("ImGuiVRHelper not found or registration failed - floating damage disabled in VR.");
			return;
		}
		logger::info("Connected to ImGuiVRHelper (world quads supported: {}).", g_vrClient.HasWorldQuads());

		// Load the damage font into the helper-owned HUD context when it is
		// created, so panel text rasterizes from real TTF outlines.
		g_vrClient.SetHudStyleCallback([]() { Fonts::Load(); });

		// Second client for head-locked flat HUD content (the live DPS
		// readout): the world-quad client's panel is consumed as a quad
		// atlas, so anything meant for a vanilla-HUD-style plane needs its
		// own kClientFlag_HUDMode panel.
		if (g_vrHudClient.Connect("FloatingDamageNG-HUD", FDNG_VERSION_STRING, ImGuiVRHelperPluginAPI::kClientFlag_HUDMode)) {
			// Each client owns a private HUD context; without the callback the
			// readout would rasterize in ImGui's 13 px embedded font.
			g_vrHudClient.SetHudStyleCallback([]() { Fonts::Load(); });
		} else {
			logger::warn("HUD-mode client registration failed - live DPS readout unavailable in VR.");
		}
	}

	void Install()
	{
		g_resolved.reserve(NumberManager::kPoolCapacity);
		g_quads.reserve(NumberManager::kPoolCapacity);

		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(75595, 77226), OFFSET(0x9, 0x275) };  // BSGraphics::InitD3D
		stl::write_thunk_call<CreateD3DAndSwapChain>(target.address());

		stl::write_vfunc<RE::HUDMenu, PostDisplay>();
		logger::info("Renderer hooks installed.");
	}
}
