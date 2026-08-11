// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 FloatingDamageNG contributors. See COPYING and EXCEPTIONS.md.
//
// SKSE plugin entry point. Lifecycle:
//   kPostLoad     — load settings (before renderer init).
//   kPostPostLoad — install hooks (before BSGraphics::InitD3D runs) and
//                   perform the ImGuiVRHelper handshake (its listener is up
//                   by then regardless of load order).
//   kDataLoaded   — register game event sinks.

#include "pch.h"

#include "Capture.h"
#include "CombatLog.h"
#include "DevBench.h"
#include "Hooks.h"
#include "I18n.h"
#include "Renderer.h"
#include "Settings.h"
#include "UI.h"

namespace
{
	void OnSKSELifecycle(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			FDNG::I18n::Load();
			FDNG::Settings::GetSingleton()->Load();
			break;
		case SKSE::MessagingInterface::kPostPostLoad:
			FDNG::Hooks::Install();
			FDNG::Renderer::Install();
			FDNG::Renderer::Connect();
			FDNG::DevBench::Connect();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			FDNG::Capture::GetSingleton()->Register();
			FDNG::CombatLog::GetSingleton()->Register();
			FDNG::UI::Register();
			break;
		// An open session would be lost on quit-after-save or discarded by a
		// load; persist it at the last reliable checkpoints.
		case SKSE::MessagingInterface::kSaveGame:
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			FDNG::CombatLog::GetSingleton()->Flush();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);  // initializes logging via the bundled CommonLibSSE-NG default

	logger::info("FloatingDamageNG {} loading", FDNG_VERSION_STRING);

	SKSE::AllocTrampoline(64);

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSELifecycle)) {
		SKSE::stl::report_and_fail("failed to register SKSE lifecycle listener"sv);
	}

	logger::info("FloatingDamageNG loaded");
	return true;
}
