#include "GalaxyXROscCodec.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#endif

namespace {

struct Options {
	bool enabled = false;
	bool once = false;
	std::string destination = "127.0.0.1";
	std::uint16_t port = 9000;
	double rateHz = 60.0;
};

void Usage(){
	std::cout
		<< "GalaxyXRFaceOscAdapter --enable [--destination PRIVATE_IPV4]"
		<< " [--port 1..65535] [--rate-hz 1..120] [--once]\n"
		<< "Emits project-defined /galaxyxr/face/frame/v1; it is not Valve FB2"
		<< " or a standard VRCFaceTracking schema.\n";
}

bool ParseUnsigned(const char* text, unsigned long& value){
	if(!text || *text == '\0'){
		return false;
	}
	char* end = nullptr;
	errno = 0;
	value = std::strtoul(text, &end, 10);
	return errno == 0 && end && *end == '\0';
}

bool ParseOptions(int argc, char** argv, Options& options){
	for(int index = 1; index < argc; ++index){
		const std::string argument = argv[index];
		if(argument == "--enable"){
			options.enabled = true;
		}else if(argument == "--once"){
			options.once = true;
		}else if((argument == "--destination" ||
			argument == "--port" ||
			argument == "--rate-hz") &&
			index + 1 < argc){
			const char* value = argv[++index];
			if(argument == "--destination"){
				options.destination = value;
			}else if(argument == "--port"){
				unsigned long parsed = 0;
				if(!ParseUnsigned(value, parsed) || parsed == 0 || parsed > 65535){
					return false;
				}
				options.port = static_cast<std::uint16_t>(parsed);
			}else{
				char* end = nullptr;
				errno = 0;
				const double parsed = std::strtod(value, &end);
				if(errno != 0 || !end || *end != '\0' ||
					!std::isfinite(parsed) || parsed < 1.0 || parsed > 120.0){
					return false;
				}
				options.rateHz = parsed;
			}
		}else{
			return false;
		}
	}
	return true;
}

#ifdef _WIN32
std::atomic<bool> keepRunning{true};

BOOL WINAPI HandleConsoleSignal(DWORD signal){
	if(signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
		signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT){
		keepRunning.store(false, std::memory_order_release);
		return TRUE;
	}
	return FALSE;
}

bool IsPrivateOrLoopback(std::uint32_t networkAddress){
	const std::uint32_t address = ntohl(networkAddress);
	return (address & 0xFF000000U) == 0x7F000000U ||
		(address & 0xFF000000U) == 0x0A000000U ||
		(address & 0xFFF00000U) == 0xAC100000U ||
		(address & 0xFFFF0000U) == 0xC0A80000U;
}

std::int64_t ReadGeneration(
	const galaxyxr::FaceSharedMemoryV1* mapped){
	return InterlockedCompareExchange64(
		reinterpret_cast<volatile LONG64*>(
			const_cast<std::int64_t*>(&mapped->generation)),
		0,
		0);
}

bool ReadStableSnapshot(
	const galaxyxr::FaceSharedMemoryV1* mapped,
	galaxyxr::FaceSharedMemoryV1& snapshot){
	for(int attempt = 0; attempt < 4; ++attempt){
		const std::int64_t before = ReadGeneration(mapped);
		if(before == 0 || (before & 1) != 0){
			continue;
		}
		MemoryBarrier();
		std::memcpy(&snapshot, mapped, sizeof(snapshot));
		MemoryBarrier();
		const std::int64_t after = ReadGeneration(mapped);
		if(before == after && (after & 1) == 0){
			return true;
		}
	}
	return false;
}
#endif

} // namespace

int main(int argc, char** argv){
	Options options;
	if(!ParseOptions(argc, argv, options)){
		Usage();
		return 2;
	}
	if(!options.enabled){
		std::cerr << "Adapter remains disabled; pass --enable explicitly.\n";
		return 2;
	}
#ifndef _WIN32
	std::cerr << "The shared-memory adapter is available only on Windows.\n";
	return 3;
#else
	IN_ADDR destinationAddress{};
	if(InetPtonA(AF_INET, options.destination.c_str(), &destinationAddress) != 1 ||
		!IsPrivateOrLoopback(destinationAddress.S_un.S_addr)){
		std::cerr << "Destination must be an explicit loopback or RFC1918 IPv4 address.\n";
		return 2;
	}
	WSADATA winsock{};
	if(WSAStartup(MAKEWORD(2, 2), &winsock) != 0){
		std::cerr << "WSAStartup failed.\n";
		return 3;
	}
	const SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(socketHandle == INVALID_SOCKET){
		WSACleanup();
		return 3;
	}
	HANDLE mapping = OpenFileMappingW(
		FILE_MAP_READ,
		FALSE,
		galaxyxr::FaceSharedMemoryName);
	if(!mapping){
		std::cerr << "Galaxy XR face mapping is unavailable; start an authenticated host session first.\n";
		closesocket(socketHandle);
		WSACleanup();
		return 3;
	}
	const auto* mapped = static_cast<const galaxyxr::FaceSharedMemoryV1*>(
		MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(galaxyxr::FaceSharedMemoryV1)));
	if(!mapped){
		CloseHandle(mapping);
		closesocket(socketHandle);
		WSACleanup();
		return 3;
	}
	sockaddr_in destination{};
	destination.sin_family = AF_INET;
	destination.sin_addr = destinationAddress;
	destination.sin_port = htons(options.port);
	SetConsoleCtrlHandler(HandleConsoleSignal, TRUE);
	const auto interval = std::chrono::duration<double>(1.0 / options.rateHz);
	std::int64_t lastGeneration = std::numeric_limits<std::int64_t>::min();
	std::array<std::uint8_t, galaxyxr::SessionIdBytes> lastSessionId{};
	bool haveLastSession = false;
	std::cout
		<< "Forwarding lossless project OSC frames to "
		<< options.destination << ':' << options.port
		<< " at <= " << options.rateHz << " Hz. Raw biometric values are not logged.\n";
	while(keepRunning.load(std::memory_order_acquire)){
		const auto started = std::chrono::steady_clock::now();
		galaxyxr::FaceSharedMemoryV1 snapshot{};
		if(ReadStableSnapshot(mapped, snapshot)){
			const bool sessionChanged =
				!haveLastSession ||
				!std::equal(
					std::begin(snapshot.sessionId),
					std::end(snapshot.sessionId),
					lastSessionId.begin());
			if(snapshot.generation == lastGeneration && !sessionChanged){
				std::this_thread::sleep_until(started + interval);
				continue;
			}
			std::vector<std::uint8_t> packet;
			if(galaxyxr::osc::EncodeFaceFrame(snapshot, packet)){
				const int sent = sendto(
					socketHandle,
					reinterpret_cast<const char*>(packet.data()),
					static_cast<int>(packet.size()),
					0,
					reinterpret_cast<const sockaddr*>(&destination),
					sizeof(destination));
				if(sent != static_cast<int>(packet.size())){
					std::cerr << "OSC send failed with error " << WSAGetLastError() << ".\n";
					break;
				}
				lastGeneration = snapshot.generation;
				std::copy(
					std::begin(snapshot.sessionId),
					std::end(snapshot.sessionId),
					lastSessionId.begin());
				haveLastSession = true;
				if(options.once){
					break;
				}
			}
		}
		std::this_thread::sleep_until(started + interval);
	}
	UnmapViewOfFile(mapped);
	CloseHandle(mapping);
	closesocket(socketHandle);
	WSACleanup();
	return 0;
#endif
}
