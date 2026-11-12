#include <cstdint>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>

#ifndef NATIVEBRIDGE_USE_REAL_MODEL_PARSER
#define NATIVEBRIDGE_USE_REAL_MODEL_PARSER 0
#endif

#if NATIVEBRIDGE_USE_REAL_MODEL_PARSER
#include "ModelBinary.h"
#else
namespace sem {

struct ModelAsset {};

inline bool DeserializeModel(const std::uint8_t* data, std::size_t size,
	ModelAsset&, std::string& err) {
	if (data == nullptr || size == 0) {
		err = "Input model buffer is empty.";
		return false;
	}

	err.clear();
	return true;
}

}
#endif

struct NativeModelFileHeader {
	std::uint32_t formatVersion;
	std::uint32_t importerVersion;
	std::uint32_t chunkCount;
	std::uint64_t sourceHash;
};

static_assert(sizeof(NativeModelFileHeader) == 24);
static_assert(offsetof(NativeModelFileHeader, sourceHash) == 16);

namespace {

std::mutex g_bridgeMutex;
NativeModelFileHeader g_modelHeader{};
std::vector<std::uint8_t> g_chunkData;

void LogNativeMessage(const char* message) noexcept {
	OutputDebugStringA("[NativeBridge] ");
	OutputDebugStringA(message);
	OutputDebugStringA("\n");
}

void LogNativeError(const std::string& error) noexcept {
	OutputDebugStringA("[NativeBridge] Model load failed: ");
	OutputDebugStringA(error.c_str());
	OutputDebugStringA("\n");
}

void SubmitModelToRenderer(const sem::ModelAsset&) noexcept {
}

}

extern "C" {

__declspec(dllexport) int __cdecl NativeBridge_SetModelFileHeader(
	const NativeModelFileHeader* header) noexcept {
	if (header == nullptr)
		return 0;

	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	g_modelHeader = *header;
	return 1;
}

__declspec(dllexport) int __cdecl NativeBridge_GetModelFileHeader(
	NativeModelFileHeader* header) noexcept {
	if (header == nullptr)
		return 0;

	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	*header = g_modelHeader;
	return 1;
}

__declspec(dllexport) int __cdecl NativeBridge_SetChunkData(
	const std::uint8_t* data, std::uint32_t size) noexcept {
	if (data == nullptr || size == 0)
		return 0;

	try {
		std::vector<std::uint8_t> copy(data, data + size);
		std::lock_guard<std::mutex> lock(g_bridgeMutex);
		g_chunkData = std::move(copy);
		return 1;
	}
	catch (...) {
		return 0;
	}
}

__declspec(dllexport) const std::uint8_t* __cdecl NativeBridge_GetChunkData(
	std::uint32_t* size) noexcept {
	if (size == nullptr)
		return nullptr;

	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	*size = static_cast<std::uint32_t>(g_chunkData.size());
	return g_chunkData.empty() ? nullptr : g_chunkData.data();
}

__declspec(dllexport) void __cdecl NativeBridge_ClearChunkData() noexcept {
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	std::vector<std::uint8_t>().swap(g_chunkData);
}

__declspec(dllexport) bool __cdecl NativeBridge_LoadModelFromMemory(
	const std::uint8_t* buffer, std::int64_t size) noexcept {
	if (buffer == nullptr || size <= 0) {
		LogNativeMessage("Input buffer is null or empty.");
		return false;
	}

	if (static_cast<std::uint64_t>(size) >
		static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
		LogNativeMessage("Input buffer is too large for this platform.");
		return false;
	}

	try {
		sem::ModelAsset model;
		std::string err;
		if (!sem::DeserializeModel(buffer, static_cast<std::size_t>(size), model, err)) {
			LogNativeError(err);
			return false;
		}

		SubmitModelToRenderer(model);
		LogNativeMessage("Model loaded successfully.");
		return true;
	}
	catch (const std::exception& exception) {
		LogNativeMessage(exception.what());
	}
	catch (...) {
		LogNativeMessage("Unknown native exception while loading the model.");
	}

	return false;
}

}
