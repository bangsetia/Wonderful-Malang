#include "RoadDdsTextureCache.h"
#include "ZipVFS.h"
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace {

struct DdsPixelFormat {
	std::uint32_t size;
	std::uint32_t flags;
	std::uint32_t fourCC;
	std::uint32_t rgbBitCount;
	std::uint32_t redMask;
	std::uint32_t greenMask;
	std::uint32_t blueMask;
	std::uint32_t alphaMask;
};

struct DdsHeader {
	std::uint32_t size;
	std::uint32_t flags;
	std::uint32_t height;
	std::uint32_t width;
	std::uint32_t pitchOrLinearSize;
	std::uint32_t depth;
	std::uint32_t mipMapCount;
	std::uint32_t reserved1[11];
	DdsPixelFormat pixelFormat;
	std::uint32_t caps;
	std::uint32_t caps2;
	std::uint32_t caps3;
	std::uint32_t caps4;
	std::uint32_t reserved2;
};

static_assert(sizeof(DdsPixelFormat) == 32);
static_assert(sizeof(DdsHeader) == 124);

constexpr std::uint32_t kDdsMagic = 0x20534444;
constexpr std::uint32_t kDdsPixelFormatFourCC = 0x4;
constexpr std::uint32_t kDdsPixelFormatRgb = 0x40;

}

bool RoadDdsTextureCache::Initialize(ID3D11Device* device, std::string* error) {
	if (!device) {
		if (error)
			*error = "D3D11 device is null.";
		return false;
	}
	m_device = device;
	if (!CreateFallbackTexture(device)) {
		if (error)
			*error = "Unable to create fallback road texture.";
		return false;
	}
	if (error)
		error->clear();
	return true;
}

bool RoadDdsTextureCache::CreateFallbackTexture(ID3D11Device* device) {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = 1;
	desc.Height = 1;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	const std::uint8_t pixel[4] = { 255, 0, 255, 255 };
	D3D11_SUBRESOURCE_DATA data{};
	data.pSysMem = pixel;
	data.SysMemPitch = sizeof(pixel);

	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	if (FAILED(device->CreateTexture2D(&desc, &data, texture.GetAddressOf())))
		return false;
	return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, m_fallback.GetAddressOf()));
}

ID3D11ShaderResourceView* RoadDdsTextureCache::GetOrLoad(const std::string& virtualPath, const ZipVFS& vfs, std::string* error) {
	const auto log = [this](const std::string& message) {
		if (m_logCallback)
			m_logCallback("[RoadDdsTextureCache] " + message);
	};
	const std::string key = ZipVFS::NormalizePath(virtualPath);
	log("DDS requested: " + virtualPath + " (normalized: " + key + ")");
	const auto found = m_textures.find(key);
	if (found != m_textures.end()) {
		log("DDS cache hit: " + key);
		return found->second.Get();
	}

	if (!m_device) {
		if (error)
			*error = "Texture cache is not initialized.";
		log("[ERROR] Texture cache is not initialized for " + key + "; using pink fallback.");
		return m_fallback.Get();
	}

	const std::vector<std::uint8_t> bytes = vfs.ReadFileFromMemory(key);
	log("DDS " + key + ": " + (bytes.empty() ? "not found/readable" : "found, bytes=" + std::to_string(bytes.size())));
	if (bytes.empty()) {
		if (error)
			*error = "DDS asset is missing or empty: " + key;
		log("[ERROR] DDS asset is missing or empty: " + key + "; using pink fallback.");
		return m_fallback.Get();
	}

	const auto failToFallback = [this, &error, &log, &key](const std::string& message) {
		if (error)
			*error = message;
		log("[ERROR] " + message + "; using pink fallback.");
		return m_fallback.Get();
	};
	if (bytes.size() < 128 || std::memcmp(bytes.data(), &kDdsMagic, sizeof(kDdsMagic)) != 0)
		return failToFallback("Invalid or truncated DDS header: " + key);

	DdsHeader header{};
	std::memcpy(&header, bytes.data() + sizeof(kDdsMagic), sizeof(header));
	if (header.size != sizeof(DdsHeader) || header.pixelFormat.size != sizeof(DdsPixelFormat) ||
		header.width == 0 || header.height == 0)
		return failToFallback("Unsupported DDS header: " + key);

	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	std::uint32_t blockSize = 0;
	if (header.pixelFormat.flags & kDdsPixelFormatFourCC) {
		switch (header.pixelFormat.fourCC) {
		case 0x31545844: format = DXGI_FORMAT_BC1_UNORM; blockSize = 8; break;
		case 0x33545844: format = DXGI_FORMAT_BC2_UNORM; blockSize = 16; break;
		case 0x35545844: format = DXGI_FORMAT_BC3_UNORM; blockSize = 16; break;
		default: return failToFallback("Unsupported DDS compression format: " + key);
		}
	} else if ((header.pixelFormat.flags & kDdsPixelFormatRgb) && header.pixelFormat.rgbBitCount == 32) {
		if (header.pixelFormat.redMask == 0x000000ff && header.pixelFormat.greenMask == 0x0000ff00 &&
			header.pixelFormat.blueMask == 0x00ff0000 && header.pixelFormat.alphaMask == 0xff000000) {
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
		} else if (header.pixelFormat.redMask == 0x00ff0000 && header.pixelFormat.greenMask == 0x0000ff00 &&
			header.pixelFormat.blueMask == 0x000000ff && header.pixelFormat.alphaMask == 0xff000000) {
			format = DXGI_FORMAT_B8G8R8A8_UNORM;
		} else {
			return failToFallback("Unsupported DDS 32-bit pixel masks: " + key);
		}
	} else {
		return failToFallback("Unsupported DDS pixel format: " + key);
	}

	std::uint64_t rowPitch = blockSize != 0
		? static_cast<std::uint64_t>((header.width - 1) / 4 + 1) * blockSize
		: static_cast<std::uint64_t>(header.width) * 4;
	if (blockSize == 0 && header.pitchOrLinearSize > rowPitch)
		rowPitch = header.pitchOrLinearSize;
	const std::uint64_t rowCount = blockSize != 0 ? (header.height - 1) / 4 + 1 : header.height;
	if (rowCount != 0 && rowPitch > (std::numeric_limits<std::uint64_t>::max)() / rowCount)
		return failToFallback("DDS level-0 payload size overflows: " + key);
	const std::uint64_t levelZeroBytes = rowPitch * rowCount;
	constexpr std::size_t dataOffset = 128;
	if (rowPitch > (std::numeric_limits<UINT>::max)() || levelZeroBytes > bytes.size() - dataOffset)
		return failToFallback("DDS level-0 payload is truncated: " + key);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = header.width;
	desc.Height = header.height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA subresourceData{};
	subresourceData.pSysMem = bytes.data() + dataOffset;
	subresourceData.SysMemPitch = static_cast<UINT>(rowPitch);
	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
	HRESULT result = m_device->CreateTexture2D(&desc, &subresourceData, texture.GetAddressOf());
	if (SUCCEEDED(result))
		result = m_device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf());

	if (FAILED(result) || !view) {
		std::ostringstream resultText;
		resultText << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
		if (error)
			*error = "DDS GPU resource creation failed for " + key + " (HRESULT " + resultText.str() + ").";
		log("[ERROR] DDS GPU resource creation failed for " + key + " (HRESULT " + resultText.str() + "); using pink fallback.");
		return m_fallback.Get();
	}

	auto inserted = m_textures.emplace(key, std::move(view));
	if (error)
		error->clear();
	log("DDS loaded successfully: " + key);
	return inserted.first->second.Get();
}

void RoadDdsTextureCache::BindRoadMaterials(ID3D11DeviceContext* context,
	const std::vector<std::string>& materialTexturePaths, const ZipVFS& vfs, UINT firstSlot) {
	if (!context || materialTexturePaths.empty())
		return;

	for (std::size_t index = 0; index < materialTexturePaths.size(); ++index) {
		ID3D11ShaderResourceView* view = GetOrLoad(materialTexturePaths[index], vfs);
		const UINT slot = firstSlot + static_cast<UINT>(index);
		context->PSSetShaderResources(slot, 1, &view);
	}
}

void RoadDdsTextureCache::Clear() {
	m_textures.clear();
	m_fallback.Reset();
	m_device.Reset();
}
