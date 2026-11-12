#pragma once

#include "RoadDdsTextureCache.h"
#include "RoadSystem.h"

#include <DirectXMath.h>
#include <cstddef>
#include <d3d11.h>
#include <string>
#include <vector>
#include <wrl/client.h>

enum class TipeVarian;

// Matches RoadVS.hlsl register(b0): row-major matrices consumed with row vectors.
struct RoadCameraConstants
{
	DirectX::XMMATRIX view;
	DirectX::XMMATRIX projection;
};

// Matches RoadPS.hlsl register(b1).
struct RoadLightConstants
{
	DirectX::XMFLOAT3 direction = { 0.5f, -1.0f, 0.2f };
	float ambientStrength = 0.2f;
	DirectX::XMFLOAT4 color = { 1.0f, 0.95f, 0.8f, 1.0f };
};

static_assert(sizeof(RoadCameraConstants) % 16 == 0);
static_assert(sizeof(RoadLightConstants) % 16 == 0);
static_assert(sizeof(RoadCameraConstants) == sizeof(DirectX::XMMATRIX) * 2);
static_assert(offsetof(RoadLightConstants, ambientStrength) == sizeof(DirectX::XMFLOAT3));
static_assert(offsetof(RoadLightConstants, color) == 16);

class RoadRenderPipeline
{
public:
	struct SubMeshVisual
	{
		std::string namaNode;
		std::string penandaVarian;
		bool isVisible = true;
		unsigned int totalIndex = 0;
		Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
		UINT vertexStride = sizeof(RoadMeshVertex);
	};

	bool Initialize(ID3D11Device* device, const std::wstring& shaderDirectory,
		std::string* error = nullptr);
	void Shutdown();
	void TerapkanFilterVarian(std::vector<SubMeshVisual>& daftarMesh, TipeVarian varianAktif);
	void RenderSubMeshVarian(const std::vector<SubMeshVisual>& daftarMesh,
		ID3D11DeviceContext* context);

	void SetLight(const RoadLightConstants& light) noexcept { m_light = light; }
	void SetTextureLogCallback(const RoadDdsTextureCache::LogCallback& callback) {
		m_textureLogCallback = callback;
		m_textureCache.SetLogCallback(callback);
	}
	bool LoadRoadTexture(const std::string& path, const ZipVFS& vfs,
		std::string* error = nullptr);

	void Render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
		const std::vector<RoadSegment>& roads, const RoadCameraConstants& camera,
		const std::vector<std::string>& terrainShaders, const ZipVFS& vfs,
		float viewportX, float viewportY, float viewportWidth, float viewportHeight,
		float targetWidth, float targetHeight);

private:
	bool CompileShaders(ID3D11Device* device, const std::wstring& shaderDirectory,
		std::string* error);
	bool CreateConstantBuffers(ID3D11Device* device, std::string* error);
	bool CreateSampler(ID3D11Device* device, std::string* error);
	bool CreatePipelineStates(ID3D11Device* device, std::string* error);
	bool CreateFallbackTexture(ID3D11Device* device, std::string* error);
	bool EnsureDepthBuffer(ID3D11Device* device, UINT width, UINT height);
	bool UploadBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
		const void* data, size_t byteCount) const;
	ID3D11ShaderResourceView* LoadTerrainTexture(const std::string& materialPath, const ZipVFS& vfs);

	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_cameraBuffer;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_lightBuffer;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_fallbackTexture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_fallbackView;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_roadTexture;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthView;
	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	RoadDdsTextureCache m_textureCache;
	RoadDdsTextureCache::LogCallback m_textureLogCallback;
	std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_terrainTextures;
	RoadLightConstants m_light;
	bool m_renderReady = false;
	UINT m_depthWidth = 0;
	UINT m_depthHeight = 0;
};
