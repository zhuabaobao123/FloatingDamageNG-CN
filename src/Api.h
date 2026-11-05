// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.

#pragma once

namespace FDNG::Api
{
	// Answer other plugins' kMessage_GetInterface handshake with the versioned
	// public interface (api/FloatingDamageNGAPI.h). Call at kPostLoad, not
	// earlier: SKSEVR attaches a null-sender listener only to plugins already
	// loaded, so clients that load later would never be answered.
	void RegisterHandshake();
}
