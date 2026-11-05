// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.
//
// Host side of the public API in api/FloatingDamageNGAPI.h. Everything here
// funnels into the same queues the native hooks use, so API popups obey the
// same pool, concurrency cap, and render path as native numbers.

#include "Api.h"

#include "Capture.h"
#include "FloatingDamageNGAPI.h"
#include "NumberManager.h"
#include "Settings.h"

namespace FDNG::Api
{
	namespace
	{
		namespace API = FloatingDamageNGPluginAPI;

		constexpr float kMinScale = 0.25f;
		constexpr float kMaxScale = 4.0f;
		constexpr float kMinLifetimeSeconds = 0.2f;
		constexpr float kMaxLifetimeSeconds = 10.0f;

		constexpr std::uint32_t kBuildNumber = FDNG_VERSION_MAJOR * 10000 + FDNG_VERSION_MINOR * 100 + FDNG_VERSION_PATCH;

		class Interface001 final : public API::IFloatingDamageNGInterface001
		{
		public:
			std::uint32_t GetBuildNumber() override { return kBuildNumber; }

			bool SpawnPopup(const API::PopupRequest* a_request) override
			{
				constexpr auto kMinSize = offsetof(API::PopupRequest, lifetimeSeconds) + sizeof(float);
				if (!a_request || a_request->size < kMinSize || !a_request->text || !a_request->text[0]) {
					return false;
				}
				if (!std::isfinite(a_request->x) || !std::isfinite(a_request->y) || !std::isfinite(a_request->z) ||
					!std::isfinite(a_request->scale) || !std::isfinite(a_request->lifetimeSeconds)) {
					logger::warn("API SpawnPopup rejected: non-finite position, scale, or lifetime");
					return false;
				}
				if (!Settings::GetSingleton()->enableFloatingDamage) {
					return false;
				}

				const auto style = std::to_underlying(a_request->style);
				const auto origin = std::to_underlying(a_request->origin);
				if (style > std::to_underlying(API::PopupStyle::kCritical) ||
					origin > std::to_underlying(API::PopupOrigin::kPlayerVictim)) {
					logger::warn("API SpawnPopup rejected: unknown style {} or origin {}", style, origin);
					return false;
				}

				DamageEvent event;
				event.victimID = a_request->victimFormID;
				event.anchor = { a_request->x, a_request->y, a_request->z };
				event.pinned = true;
				event.origin = static_cast<OriginTier>(origin);
				if (a_request->style == API::PopupStyle::kCritical) {
					event.flags.critical = true;
				} else {
					event.kind = static_cast<DamageKind>(style);  // PopupStyle 0..6 mirror DamageKind
				}
				std::snprintf(event.customText, sizeof(event.customText), "%s", a_request->text);
				event.useColor = (a_request->flags & API::kPopup_UseColor) != 0;
				event.colorRGB = a_request->colorRGB & 0xFFFFFF;
				event.scale = std::clamp(a_request->scale, kMinScale, kMaxScale);
				if (a_request->lifetimeSeconds > 0.0f) {
					event.lifetimeSeconds = std::clamp(a_request->lifetimeSeconds, kMinLifetimeSeconds, kMaxLifetimeSeconds);
				}

				NumberManager::GetSingleton()->Enqueue(event);
				return true;
			}

			bool ClaimHit(std::uint32_t a_victimFormID, std::uint32_t a_flags) override
			{
				constexpr auto kKnown = API::kClaim_SuppressPopup | API::kClaim_SuppressLocation;
				if (a_victimFormID == 0 || (a_flags & kKnown) == 0) {
					return false;
				}
				HitClaim claim;
				claim.suppressPopup = (a_flags & API::kClaim_SuppressPopup) != 0;
				claim.suppressLocation = (a_flags & API::kClaim_SuppressLocation) != 0;
				Capture::GetSingleton()->ClaimHit(a_victimFormID, claim);
				return true;
			}
		};

		void* GetApiFunction(std::uint32_t a_revision) noexcept
		{
			static Interface001 interface001;
			return a_revision == 1 ? static_cast<API::IFloatingDamageNGInterface001*>(&interface001) : nullptr;
		}

		void OnPluginMessage(SKSE::MessagingInterface::Message* a_msg)
		{
			if (a_msg && a_msg->type == API::Message::kMessage_GetInterface &&
				a_msg->data && a_msg->dataLen >= sizeof(API::Message*)) {
				static_cast<API::Message*>(a_msg->data)->GetApiFunction = &GetApiFunction;
				logger::info("API handshake from '{}'", a_msg->sender ? a_msg->sender : "<unknown>");
			}
		}
	}

	void RegisterHandshake()
	{
		const auto messaging = SKSE::GetMessagingInterface();
		if (!messaging || !messaging->RegisterListener(nullptr, OnPluginMessage)) {
			logger::warn("failed to register the public API handshake listener");
		}
	}
}
