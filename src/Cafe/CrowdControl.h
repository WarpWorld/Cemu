// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Crowd Control TCP bridge.
//
// Exposes a localhost TCP server while a title is running so that the Crowd Control
// application can read/write emulated memory, register emulator-side memory freezes
// and display overlay notifications - without attaching to the Cemu process with
// ReadProcessMemory/WriteProcessMemory and scanning for guest RAM.
//
// The wire protocol is identical in framing to the Dolphin Crowd Control bridge
// (docs/CrowdControlTCP.md in the dolphin repository): little-endian framed
// requests/responses, guest effective addressing, raw big-endian memory contents.

#pragma once

#include <string>

namespace CrowdControl
{
	// Bump on breaking wire protocol changes.
	constexpr uint32 PROTOCOL_VERSION = 1;

	// Shown in the main window title while the TCP server is active.
	enum class ConnectionStatus
	{
		Inactive,     // Server not running (no title loaded or port disabled).
		Disconnected, // Listening, no client connected.
		Connecting,   // Client accepted, awaiting first request.
		Connected,    // Client connected and active.
	};

	// Starts/stops the TCP server. Called during title launch/shutdown.
	// A port of 0 disables the server.
	void Init(uint16 port);
	void Shutdown();
	bool IsActive();
	ConnectionStatus GetConnectionStatus();

	// Window-title suffix for the current connection status, e.g. " [CC: connected]".
	// Empty when the bridge is inactive.
	std::string GetTitleSuffix();

	// Controller mangling state, queried by the emulated VPAD input path.
	// Set via the SetInvertedControls/SetSwappedButtons opcodes; reset when the
	// client disconnects so a crashed Crowd Control app cannot leave them stuck.
	bool AreControlsInverted();
	bool AreButtonsSwapped();
	// Horizontally mirrors the TV framebuffer (view flip), not camera stick input.
	bool IsCameraInverted();
}
