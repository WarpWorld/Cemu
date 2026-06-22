// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "Cafe/CrowdControl.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "Common/socket.h"
#if !BOOST_OS_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/MMU/MMU.h"
#include "util/helpers/helpers.h"

// Wire protocol (mirrors the Dolphin Crowd Control bridge):
//
//   Request:  u32 frame_size | u32 request_id | u8 opcode | body...
//   Response: u32 frame_size | u32 request_id | u8 result | body...
//
// Header fields are little-endian; memory contents are raw guest bytes
// (big-endian for typed Wii U values). frame_size counts everything after the
// frame_size field itself. Addresses are guest effective addresses
// (e.g. 0x10000000+ data area, 0x01800000 codecave where graphics-pack
// patches and the BOTW mailbox live).

namespace CrowdControl
{
enum class Opcode : uint8
{
	Ping = 0x00,
	ReadMemory = 0x01,
	WriteMemory = 0x02,
	FreezeMemory = 0x03,
	UnfreezeMemory = 0x04,
	UnfreezeAll = 0x05,
	InvalidateJitCache = 0x06,
	DisplayMessage = 0x07,
	GetGameInfo = 0x08,
	SetInvertedControls = 0x0B,
	SetSwappedButtons = 0x0C,
	CompareSwap = 0x0D,
	SetInvertedCamera = 0x0E,
};

enum class Result : uint8
{
	Ok = 0x00,
	InvalidAddress = 0x01,
	InvalidRequest = 0x02,
	EmulatorNotRunning = 0x03,
	UnknownOpcode = 0x04,
	OperationFailed = 0x05,
};

constexpr uint32 MIN_FRAME_SIZE = 5; // u32 request_id + u8 opcode
constexpr uint32 MAX_FRAME_SIZE = 0x0800'0000 + 64;

static std::thread s_server_thread;
static std::atomic<bool> s_running = false;
static std::atomic<ConnectionStatus> s_connection_status = ConnectionStatus::Inactive;

static std::mutex s_freeze_mutex;
static std::map<uint32, std::vector<uint8>> s_freezes;

static std::atomic<bool> s_invert_controls = false;
static std::atomic<bool> s_swap_buttons = false;
static std::atomic<bool> s_invert_camera = false;

bool AreControlsInverted()
{
	return s_invert_controls.load(std::memory_order_relaxed);
}

bool AreButtonsSwapped()
{
	return s_swap_buttons.load(std::memory_order_relaxed);
}

bool IsCameraInverted()
{
	return s_invert_camera.load(std::memory_order_relaxed);
}

// Resolves a guest address range to host memory. Accesses are intentionally
// unsynchronized with the PPC cores; this matches the semantics packs were
// built against with ReadProcessMemory/WriteProcessMemory.
static uint8* ResolveEmulatedRange(uint32 address, uint32 size)
{
	if (size == 0 || !CafeSystem::IsTitleRunning())
		return nullptr;
	if (!memory_isAddressRangeAccessible(address, size))
		return nullptr;
	return memory_base + address;
}

static void ApplyFreezes()
{
	std::lock_guard lock(s_freeze_mutex);
	for (const auto& [address, bytes] : s_freezes)
	{
		uint8* range = ResolveEmulatedRange(address, (uint32)bytes.size());
		if (range)
			std::memcpy(range, bytes.data(), bytes.size());
	}
}

static void ResetEffectState()
{
	std::lock_guard lock(s_freeze_mutex);
	s_freezes.clear();
	s_invert_controls = false;
	s_swap_buttons = false;
	s_invert_camera = false;
}

template<typename T>
static T ReadLE(const uint8* data)
{
	T value;
	std::memcpy(&value, data, sizeof(T));
	return value; // all supported hosts are little-endian
}

template<typename T>
static void AppendLE(std::vector<uint8>& buffer, T value)
{
	const auto* bytes = reinterpret_cast<const uint8*>(&value);
	buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

struct Response
{
	Result result = Result::Ok;
	std::vector<uint8> body;
};

static Response HandleRequest(Opcode opcode, std::span<const uint8> body)
{
	Response response;

	switch (opcode)
	{
	case Opcode::Ping:
	{
		AppendLE<uint32>(response.body, PROTOCOL_VERSION);
		break;
	}

	case Opcode::ReadMemory:
	{
		if (body.size() != 8)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		const uint32 address = ReadLE<uint32>(body.data());
		const uint32 size = ReadLE<uint32>(body.data() + 4);
		const uint8* range = ResolveEmulatedRange(address, size);
		if (!range)
		{
			response.result = Result::InvalidAddress;
			break;
		}
		response.body.assign(range, range + size);
		break;
	}

	case Opcode::WriteMemory:
	{
		if (body.size() < 5)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		const uint32 address = ReadLE<uint32>(body.data());
		const uint32 size = (uint32)(body.size() - 4);
		uint8* range = ResolveEmulatedRange(address, size);
		if (!range)
		{
			response.result = Result::InvalidAddress;
			break;
		}
		std::memcpy(range, body.data() + 4, size);
		break;
	}

	case Opcode::FreezeMemory:
	{
		if (body.size() < 5 || body.size() > 12)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		const uint32 address = ReadLE<uint32>(body.data());
		std::lock_guard lock(s_freeze_mutex);
		s_freezes[address] = std::vector<uint8>(body.begin() + 4, body.end());
		break;
	}

	case Opcode::UnfreezeMemory:
	{
		if (body.size() != 4)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		const uint32 address = ReadLE<uint32>(body.data());
		std::lock_guard lock(s_freeze_mutex);
		s_freezes.erase(address);
		break;
	}

	case Opcode::UnfreezeAll:
	{
		ResetEffectState();
		break;
	}

	case Opcode::InvalidateJitCache:
	{
		if (!body.empty() && body.size() != 8)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		if (!CafeSystem::IsTitleRunning())
		{
			response.result = Result::EmulatorNotRunning;
			break;
		}
		if (body.empty())
		{
			PPCRecompiler_invalidateRange(0, 0xFFFFFFFF);
		}
		else
		{
			const uint32 address = ReadLE<uint32>(body.data());
			const uint32 size = ReadLE<uint32>(body.data() + 4);
			PPCRecompiler_invalidateRange(address, address + size);
		}
		break;
	}

	case Opcode::DisplayMessage:
	{
		if (body.size() < 4)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		const uint32 time_in_ms = ReadLE<uint32>(body.data());
		std::string message(reinterpret_cast<const char*>(body.data()) + 4, body.size() - 4);
		LatteOverlay_pushNotification(message, (sint32)time_in_ms);
		break;
	}

	case Opcode::GetGameInfo:
	{
		const bool running = CafeSystem::IsTitleRunning();
		AppendLE<uint8>(response.body, running ? 2 : 0); // matches Dolphin's Core::State::Running
		AppendLE<uint64>(response.body, running ? CafeSystem::GetForegroundTitleId() : 0);
		AppendLE<uint16>(response.body, running ? CafeSystem::GetForegroundTitleVersion() : 0);
		if (running)
		{
			const std::string name = CafeSystem::GetForegroundTitleName();
			response.body.insert(response.body.end(), name.begin(), name.end());
		}
		break;
	}

	case Opcode::SetInvertedControls:
	{
		if (body.size() != 1)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		s_invert_controls = body[0] != 0;
		break;
	}

	case Opcode::SetSwappedButtons:
	{
		if (body.size() != 1)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		s_swap_buttons = body[0] != 0;
		break;
	}

	case Opcode::SetInvertedCamera:
	{
		if (body.size() != 1)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		// Crowd Control "invert camera" effect: mirror the rendered TV view.
		s_invert_camera = body[0] != 0;
		break;
	}

	case Opcode::CompareSwap:
	{
		if (body.size() != 6)
		{
			response.result = Result::InvalidRequest;
			break;
		}
		const uint32 address = ReadLE<uint32>(body.data());
		const uint8 test = body[4];
		const uint8 new_val = body[5];
		uint8* range = ResolveEmulatedRange(address, 1);
		if (!range)
		{
			response.result = Result::InvalidAddress;
			break;
		}
		const uint8 orig = *range;
		response.body.push_back(orig);
		if (orig == test)
			*range = new_val;
		break;
	}

	default:
		response.result = Result::UnknownOpcode;
		break;
	}

	return response;
}

static bool SendAll(SOCKET sock, const uint8* data, size_t size)
{
	size_t sent = 0;
	while (sent < size)
	{
		const auto result = send(sock, reinterpret_cast<const char*>(data) + sent, (int)(size - sent), 0);
		if (result <= 0)
			return false;
		sent += (size_t)result;
	}
	return true;
}

// Consumes complete frames from rx_buffer, dispatching each request and sending its
// response. Returns false on a protocol violation (caller should drop the connection).
static bool ProcessReceiveBuffer(SOCKET client, std::vector<uint8>& rx_buffer)
{
	while (rx_buffer.size() >= sizeof(uint32))
	{
		const uint32 frame_size = ReadLE<uint32>(rx_buffer.data());
		if (frame_size < MIN_FRAME_SIZE || frame_size > MAX_FRAME_SIZE)
		{
			cemuLog_log(LogType::Force, "Crowd Control: bad frame size 0x{:x}, dropping client", frame_size);
			return false;
		}
		if (rx_buffer.size() < sizeof(uint32) + frame_size)
			return true; // incomplete frame; wait for more data

		const uint8* frame = rx_buffer.data() + sizeof(uint32);
		const uint32 request_id = ReadLE<uint32>(frame);
		const Opcode opcode = static_cast<Opcode>(frame[4]);
		const std::span<const uint8> body(frame + MIN_FRAME_SIZE, frame_size - MIN_FRAME_SIZE);

		const Response response = HandleRequest(opcode, body);

		std::vector<uint8> out;
		out.reserve(sizeof(uint32) + MIN_FRAME_SIZE + response.body.size());
		AppendLE<uint32>(out, MIN_FRAME_SIZE + (uint32)response.body.size());
		AppendLE<uint32>(out, request_id);
		AppendLE<uint8>(out, static_cast<uint8>(response.result));
		out.insert(out.end(), response.body.begin(), response.body.end());

		if (!SendAll(client, out.data(), out.size()))
			return false;

		rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + sizeof(uint32) + frame_size);
	}
	return true;
}

static void ServerThread(uint16 port)
{
	SetThreadName("CrowdControl");

	const SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == INVALID_SOCKET)
	{
		cemuLog_log(LogType::Force, "Crowd Control: failed to create server socket");
		return;
	}

	const int on = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
		listen(listener, 1) != 0)
	{
		cemuLog_log(LogType::Force, "Crowd Control: failed to bind/listen on 127.0.0.1:{}", port);
		closesocket(listener);
		return;
	}

	cemuLog_log(LogType::Force, "Crowd Control: listening on 127.0.0.1:{}", port);
	s_connection_status = ConnectionStatus::Disconnected;

	SOCKET client = INVALID_SOCKET;
	std::vector<uint8> rx_buffer;

	const auto drop_client = [&]() {
		if (client != INVALID_SOCKET)
		{
			closesocket(client);
			client = INVALID_SOCKET;
		}
		rx_buffer.clear();
		// Don't leave stale freezes running if the Crowd Control app goes away.
		ResetEffectState();
		s_connection_status = ConnectionStatus::Disconnected;
	};

	while (s_running.load(std::memory_order_relaxed))
	{
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(listener, &read_fds);
		if (client != INVALID_SOCKET)
			FD_SET(client, &read_fds);

#if BOOST_OS_WINDOWS
		const int nfds = 0; // ignored by Winsock
#else
		const int nfds = (int)std::max(listener, client == INVALID_SOCKET ? listener : client) + 1;
#endif
		timeval timeout{};
		timeout.tv_usec = 50000;
		const int ready = select(nfds, &read_fds, nullptr, nullptr, &timeout);

		// Re-apply freezes on every wakeup (~50ms cadence). Cemu has no central
		// per-frame cheat pass to hook, so this approximates Dolphin's
		// once-per-frame freeze application.
		ApplyFreezes();

		if (ready <= 0)
			continue;

		if (FD_ISSET(listener, &read_fds))
		{
			const SOCKET new_client = accept(listener, nullptr, nullptr);
			if (new_client != INVALID_SOCKET)
			{
				// Only one client is served at a time; a new connection replaces the
				// old one so the Crowd Control app can always recover by reconnecting.
				drop_client();
				client = new_client;
				const int nodelay = 1;
				setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
				s_connection_status = ConnectionStatus::Connecting;
				cemuLog_log(LogType::Force, "Crowd Control: client connected");
			}
		}

		if (client != INVALID_SOCKET && FD_ISSET(client, &read_fds))
		{
			char buffer[8192];
			const auto received = recv(client, buffer, sizeof(buffer), 0);
			if (received <= 0)
			{
				cemuLog_log(LogType::Force, "Crowd Control: client disconnected");
				drop_client();
				continue;
			}
			rx_buffer.insert(rx_buffer.end(), buffer, buffer + received);
			s_connection_status = ConnectionStatus::Connected;
			if (!ProcessReceiveBuffer(client, rx_buffer))
				drop_client();
		}
	}

	drop_client();
	closesocket(listener);
	s_connection_status = ConnectionStatus::Inactive;
}

void Init(uint16 port)
{
	if (s_running || port == 0)
		return;

#if BOOST_OS_WINDOWS
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	ResetEffectState();
	s_running = true;
	s_server_thread = std::thread(ServerThread, port);
}

void Shutdown()
{
	if (!s_running)
		return;

	s_running = false;
	if (s_server_thread.joinable())
		s_server_thread.join();
	ResetEffectState();
	s_connection_status = ConnectionStatus::Inactive;
}

bool IsActive()
{
	return s_running;
}

ConnectionStatus GetConnectionStatus()
{
	return s_connection_status.load(std::memory_order_relaxed);
}

std::string GetTitleSuffix()
{
	switch (GetConnectionStatus())
	{
	case ConnectionStatus::Disconnected:
		return " [CC: waiting for client]";
	case ConnectionStatus::Connecting:
		return " [CC: connecting...]";
	case ConnectionStatus::Connected:
		return " [CC: connected]";
	default:
		return {};
	}
}
} // namespace CrowdControl
