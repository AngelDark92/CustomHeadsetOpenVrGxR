#include "GalaxyXRStatus.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace galaxyxr {
namespace {

bool ContainsInsensitive(const std::string& value, const char* needle){
	std::string lower(value);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character){
		return static_cast<char>(std::tolower(character));
	});
	return lower.find(needle) != std::string::npos;
}

} // namespace

GalaxyXRStatus::GalaxyXRStatus()
	: startupUnixMs(UnixTimeMs()),
	  startupMonotonicNs(MonotonicTimeNs()),
	  updatedUnixMs(startupUnixMs),
	  updatedMonotonicNs(startupMonotonicNs) {}

void GalaxyXRStatus::ConfigureFile(const std::string& path){
	std::lock_guard<std::mutex> lock(mutex);
	statusPath = path;
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::SetState(
	const std::string& name,
	const std::string& value){
	if(!IsSafeName(name)){
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	states[name] = value;
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::SetCounter(
	const std::string& name,
	std::uint64_t value){
	if(!IsSafeName(name)){
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	counters[name] = value;
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::SetMetric(
	const std::string& name,
	double value){
	if(!IsSafeName(name) || !std::isfinite(value)){
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	metrics[name] = value;
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::RemoveState(const std::string& name){
	std::lock_guard<std::mutex> lock(mutex);
	states.erase(name);
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::RemoveCounter(const std::string& name){
	std::lock_guard<std::mutex> lock(mutex);
	counters.erase(name);
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::RemoveMetric(const std::string& name){
	std::lock_guard<std::mutex> lock(mutex);
	metrics.erase(name);
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::RemoveByPrefix(const std::string& prefix){
	if(prefix.empty()){
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	auto eraseMatching = [&prefix](auto& values){
		for(auto entry = values.begin(); entry != values.end();){
			if(entry->first.rfind(prefix, 0) == 0){
				entry = values.erase(entry);
			}else{
				++entry;
			}
		}
	};
	eraseMatching(states);
	eraseMatching(counters);
	eraseMatching(metrics);
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

void GalaxyXRStatus::Transition(
	const std::string& stage,
	const std::string& state,
	const std::string& safeError){
	std::lock_guard<std::mutex> lock(mutex);
	transitionStage = stage;
	transitionState = state;
	lastError =
		safeError.empty() || IsSafeReasonCode(safeError)
			? safeError
			: "unsafe_reason_redacted";
	updatedUnixMs = UnixTimeMs();
	updatedMonotonicNs = MonotonicTimeNs();
}

std::string GalaxyXRStatus::Serialize() const{
	std::lock_guard<std::mutex> lock(mutex);
	std::ostringstream output;
	output << "{\n"
		<< "  \"schema_version\": 1,\n"
		<< "  \"startup_unix_ms\": " << startupUnixMs << ",\n"
		<< "  \"startup_monotonic_ns\": " << startupMonotonicNs << ",\n"
		<< "  \"updated_unix_ms\": " << updatedUnixMs << ",\n"
		<< "  \"updated_monotonic_ns\": " << updatedMonotonicNs << ",\n"
		<< "  \"heartbeat_sequence\": " << heartbeatSequence << ",\n"
		<< "  \"stale_after_ms\": 5000,\n"
		<< "  \"transition\": {\"stage\": \"" << Escape(transitionStage)
		<< "\", \"state\": \"" << Escape(transitionState)
		<< "\", \"last_error\": \"" << Escape(lastError) << "\"},\n";

	output << "  \"state\": {";
	bool first = true;
	for(const auto& entry : states){
		if(!first){ output << ','; }
		output << "\n    \"" << Escape(entry.first) << "\": \""
			<< Escape(entry.second) << '"';
		first = false;
	}
	if(!states.empty()){ output << '\n' << "  "; }
	output << "},\n";

	output << "  \"counters\": {";
	first = true;
	for(const auto& entry : counters){
		if(!first){ output << ','; }
		output << "\n    \"" << Escape(entry.first) << "\": " << entry.second;
		first = false;
	}
	if(!counters.empty()){ output << '\n' << "  "; }
	output << "},\n";

	output << "  \"metrics\": {";
	first = true;
	output << std::setprecision(10);
	for(const auto& entry : metrics){
		if(!first){ output << ','; }
		output << "\n    \"" << Escape(entry.first) << "\": " << entry.second;
		first = false;
	}
	if(!metrics.empty()){ output << '\n' << "  "; }
	output << "}\n}\n";
	return output.str();
}

bool GalaxyXRStatus::Flush() const{
	std::lock_guard<std::mutex> ioLock(ioMutex);
	std::string path;
	{
		std::lock_guard<std::mutex> lock(mutex);
		updatedUnixMs = UnixTimeMs();
		updatedMonotonicNs = MonotonicTimeNs();
		++heartbeatSequence;
		path = statusPath;
	}
	if(path.empty()){
		return false;
	}
	const std::filesystem::path destination(path);
	const std::filesystem::path temporary = destination.wstring() + L".tmp";
	std::error_code error;
	std::filesystem::create_directories(destination.parent_path(), error);
	if(error){
		return false;
	}
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if(!output){
			return false;
		}
		const std::string snapshot = Serialize();
		output.write(snapshot.data(), static_cast<std::streamsize>(snapshot.size()));
		output.flush();
		if(!output){
			return false;
		}
	}
#ifdef _WIN32
	if(MoveFileExW(
		temporary.c_str(),
		destination.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE){
		std::filesystem::remove(temporary, error);
		return false;
	}
#else
	std::filesystem::rename(temporary, destination, error);
	if(error){
		std::filesystem::remove(temporary, error);
		return false;
	}
#endif
	return true;
}

bool GalaxyXRStatus::IsSnapshotStale(
	std::int64_t updatedUnixMs,
	std::int64_t nowUnixMs,
	std::int64_t staleAfterMs){
	if(updatedUnixMs <= 0 || nowUnixMs < updatedUnixMs || staleAfterMs < 0){
		return true;
	}
	return nowUnixMs - updatedUnixMs > staleAfterMs;
}

bool GalaxyXRStatus::IsSafeName(const std::string& name){
	return !name.empty() &&
		!ContainsInsensitive(name, "secret") &&
		!ContainsInsensitive(name, "token") &&
		!ContainsInsensitive(name, "session_key") &&
		!ContainsInsensitive(name, "pairing_key") &&
		!ContainsInsensitive(name, "authentication_tag") &&
		!ContainsInsensitive(name, "biometric") &&
		!ContainsInsensitive(name, "weights");
}

bool GalaxyXRStatus::IsSafeReasonCode(const std::string& value){
	if(value.empty() || value.size() > 96){
		return false;
	}
	return std::all_of(value.begin(), value.end(), [](unsigned char character){
		return (character >= 'a' && character <= 'z') ||
			(character >= '0' && character <= '9') ||
			character == '_' ||
			character == '-' ||
			character == '.';
	});
}

std::string GalaxyXRStatus::Escape(const std::string& value){
	std::ostringstream output;
	for(unsigned char character : value){
		switch(character){
			case '"': output << "\\\""; break;
			case '\\': output << "\\\\"; break;
			case '\b': output << "\\b"; break;
			case '\f': output << "\\f"; break;
			case '\n': output << "\\n"; break;
			case '\r': output << "\\r"; break;
			case '\t': output << "\\t"; break;
			default:
				if(character < 0x20){
					output << "\\u"
						<< std::hex << std::setw(4) << std::setfill('0')
						<< static_cast<unsigned>(character)
						<< std::dec << std::setw(0);
				}else{
					output << static_cast<char>(character);
				}
				break;
		}
	}
	return output.str();
}

std::int64_t GalaxyXRStatus::UnixTimeMs(){
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t GalaxyXRStatus::MonotonicTimeNs(){
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace galaxyxr
