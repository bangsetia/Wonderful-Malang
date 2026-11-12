#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class ZipVFS;

class RoadDdsTextureCache {
public:
	using LogCallback = std::function<void(const std::string&)>;

	bool Initialize(ID3D11Device* device, std::string* error = nullptr);
	ID3D11ShaderResourceView* GetOrLoad(const std::string& virtualPath, const ZipVFS& vfs, std::string* error = nullptr);
	void SetLogCallback(const LogCallback& callback) { m_logCallback = callback; }
	void BindRoadMaterials(ID3D11DeviceContext* context, const std::vector<std::string>& materialTexturePaths,
		const ZipVFS& vfs, UINT firstSlot = 0);
	void Clear();

private:
	bool CreateFallbackTexture(ID3D11Device* device);

	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_fallback;
	std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_textures;
	LogCallback m_logCallback;
};
