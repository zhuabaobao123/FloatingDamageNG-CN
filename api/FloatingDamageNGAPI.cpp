// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 FloatingDamageNG contributors. See api/COPYING.LESSER.
//
// Client-side handshake stub. Compiled into the client mod's binary. Dispatches
// an SKSE message to FloatingDamageNG and caches the resulting interface
// pointer. The handshake is RETRYABLE: an early call can return null if
// FloatingDamageNG's listener isn't registered yet (plugin load-order race), so
// failure is NOT latched.

#include "FloatingDamageNGAPI.h"

namespace FloatingDamageNGPluginAPI
{
	namespace
	{
		IFloatingDamageNGInterface001* g_interface001 = nullptr;
	}

	IFloatingDamageNGInterface001* GetFloatingDamageNGInterface001()
	{
		if (g_interface001) {
			return g_interface001;
		}
		const auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			return nullptr;
		}
		Message msg{};
		messaging->Dispatch(Message::kMessage_GetInterface, static_cast<void*>(&msg), sizeof(Message*), kPluginName);
		if (!msg.GetApiFunction) {
			return nullptr;  // not installed, or not ready yet - safe to retry
		}
		g_interface001 = static_cast<IFloatingDamageNGInterface001*>(msg.GetApiFunction(1));
		return g_interface001;
	}
}
