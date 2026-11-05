// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 FloatingDamageNG contributors. See api/COPYING.LESSER.
//
// FloatingDamageNG public API.
//
// Clients vendor api/FloatingDamageNGAPI.h and api/FloatingDamageNGAPI.cpp
// (source form; they must compile inside the client so the SKSE messaging
// types share the client's CommonLib). At SKSE's kPostPostLoad, call
// GetFloatingDamageNGInterface001() to obtain a pointer to FloatingDamageNG's
// API. kPostPostLoad (not kPostLoad) is the safe point: it fires after every
// plugin's kPostLoad, so FloatingDamageNG's messaging listener is registered
// regardless of load order. The handshake is retryable. Subsequent calls go
// through the returned vtable - SKSE messaging is only used for the handshake.
//
// Handshake pattern follows ImGuiVRHelperAPI.h.
//
// What the API offers:
//   * SpawnPopup  - show custom text at a world position, styled and animated
//                   like a native FloatingDamageNG number (VR world quad or
//                   flat HUD, per the user's settings). The caller supplies
//                   world coordinates; FloatingDamageNG does the projection.
//   * ClaimHit    - mark FloatingDamageNG's own popup for a hit as handled,
//                   so a mod that shows its own feedback for that hit does
//                   not get a duplicate.
//
// Threading: both calls are safe from any thread. Nothing here touches the
// engine's actor state.

#pragma once

#include <cstdint>

#include <SKSE/SKSE.h>

namespace FloatingDamageNGPluginAPI
{
	/// SKSE plugin name (filename without extension) used for messaging dispatch.
	constexpr const auto kPluginName = "FloatingDamageNG";

	/// Handshake message exchanged between client and FloatingDamageNG.
	struct Message
	{
		enum : uint32_t
		{
			kMessage_GetInterface = 0x4FD1A7C3u  // randomly generated, fixed
		};

		/// Filled in by FloatingDamageNG. Returns the requested interface
		/// revision, or nullptr if the installed build doesn't support it.
		void* (*GetApiFunction)(uint32_t revisionNumber) = nullptr;
	};

	struct IFloatingDamageNGInterface001;

	/// Handshake. Call at kPostPostLoad (retryable - a null result before
	/// FloatingDamageNG's listener registers is not fatal; call again). Returns
	/// nullptr if FloatingDamageNG isn't installed or predates revision 001.
	IFloatingDamageNGInterface001* GetFloatingDamageNGInterface001();

	/// Which damage-kind palette entry (color, font, motion preset) the popup
	/// borrows. Values are stable; new styles append.
	enum class PopupStyle : uint32_t
	{
		kPhysical = 0,
		kFire,
		kFrost,
		kShock,
		kPoison,
		kMagic,
		kHealing,
		kCritical,  // the user's critical-hit color and enlarged size
	};

	/// Who the popup is "about"; selects the origin marker and the crowd
	/// attenuation tier (follower/NPC popups are smaller and distance-culled).
	enum class PopupOrigin : uint32_t
	{
		kPlayer = 0,    // the player dealt it
		kFollower,      // a teammate dealt it
		kNPC,           // NPC-on-NPC
		kPlayerVictim,  // the player received it
	};

	enum PopupFlags : uint32_t
	{
		kPopup_None = 0,
		kPopup_UseColor = 1u << 0,  // use PopupRequest::colorRGB instead of the style's color
	};

	/// Request to show one popup. `size` must be set to sizeof(PopupRequest):
	/// later revisions append fields, and FloatingDamageNG reads only the
	/// prefix the caller declared, so an older client keeps working.
	struct PopupRequest
	{
		uint32_t size = sizeof(PopupRequest);
		uint32_t flags = kPopup_None;  // PopupFlags
		const char* text = nullptr;    // UTF-8, copied; truncated to 27 bytes. Required.
		float x = 0.0f;                // world-space position (game units), fixed
		float y = 0.0f;                // for the popup's lifetime, e.g. the
		float z = 0.0f;                // hit contact point
		uint32_t victimFormID = 0;     // optional; groups rapid popups on one target so
									   // they fan out instead of overlapping
		PopupStyle style = PopupStyle::kPhysical;
		PopupOrigin origin = PopupOrigin::kPlayer;
		uint32_t colorRGB = 0;         // 0xRRGGBB; honored with kPopup_UseColor
		float scale = 1.0f;            // multiplies the popup's size; clamped to [0.25, 4]
		float lifetimeSeconds = 0.0f;  // 0 = the user's configured lifetime; clamped to [0.2, 10]
	};

	/// What ClaimHit suppresses. Bitmask.
	enum HitClaimFlags : uint32_t
	{
		/// Drop FloatingDamageNG's popup for the hit entirely. Combat analytics
		/// still records the hit; only the on-screen number is withheld.
		kClaim_SuppressPopup = 1u << 0,
		/// Keep the damage number but drop its locational decorations (the
		/// HEADSHOT-style tag and the implied-multiplier subtext), for mods that
		/// show those themselves.
		kClaim_SuppressLocation = 1u << 1,
	};

	/// Versioned interface. Future revisions extend by inheritance:
	///   struct IFloatingDamageNGInterface002 : IFloatingDamageNGInterface001 { ... };
	/// GetApiFunction returns the highest revision the installed build
	/// supports, or nullptr for revisions newer than itself.
	struct IFloatingDamageNGInterface001
	{
		/// FloatingDamageNG build number; informational. Key behavior off the
		/// interface revision, not this.
		virtual uint32_t GetBuildNumber() = 0;

		/// Queue a popup. Returns true if accepted. Returns false when the
		/// request is malformed (null text, bad size) or the user has switched
		/// floating numbers off, in which case the caller may fall back to its
		/// own rendering.
		virtual bool SpawnPopup(const PopupRequest* request) = 0;

		/// Claim FloatingDamageNG's popup for the next weapon hit on
		/// `victimFormID`. Call once per hit, from the code that decides the
		/// hit's outcome - before or after FloatingDamageNG sees the hit, as
		/// long as it is within about half a second of it. A claim applies to
		/// one hit and expires on its own; an unconsumed claim never affects a
		/// later hit. `flags` is a HitClaimFlags bitmask. Returns false if the
		/// request is invalid.
		virtual bool ClaimHit(uint32_t victimFormID, uint32_t flags) = 0;
	};
}
