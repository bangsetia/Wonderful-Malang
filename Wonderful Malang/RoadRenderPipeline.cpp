#include "RoadRenderPipeline.h"
#include "SiiBridge.h"
#include "ZipVFS.h"

#include <algorithm>
#include <cctype>
#include <d3dcompiler.h>
#include <filesystem>
#include <cstring>
#include <sstream>

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
constexpr UINT kShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
constexpr UINT kMaxDrawIndexCount = 0xffffffffu;
constexpr float kMaxTargetDimension = 4294967040.0f;

bool SetError(std::string* error, const char* message)
{
	if (error) *error = message;
	return false;
}

std::string NormalizeVariantTag(std::string tag)
{
	const auto first = std::find_if_not(tag.begin(), tag.end(), [](unsigned char ch) {
		return std::isspace(ch) != 0;
	});
	tag.erase(tag.begin(), first);
	const auto last = std::find_if_not(tag.rbegin(), tag.rend(), [](unsigned char ch) {
		return std::isspace(ch) != 0;
	}).base();
	tag.erase(last, tag.end());
	std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char ch) {
		return static_cast<char>(std::toupper(ch));
	});
	return tag;
}

std::wstring ShaderPath(const std::wstring& directory, const wchar_t* fileName)
{
	const std::filesystem::path direct = std::filesystem::path(directory) / fileName;
	if (std::filesystem::exists(direct)) return direct.wstring();

	const std::filesystem::path projectRelative = std::filesystem::path(L"Wonderful Malang") / direct;
	if (std::filesystem::exists(projectRelative)) return projectRelative.wstring();

	return direct.wstring();
}

bool CompileShader(const std::wstring& path, const char* entryPoint, const char* profile,
	Microsoft::WRL::ComPtr<ID3DBlob>& bytecode, std::string* error)
{
	Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
	const HRESULT result = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entryPoint, profile, kShaderFlags, 0, bytecode.GetAddressOf(), diagnostics.GetAddressOf());
	if (FAILED(result)) {
		if (error) {
			if (diagnostics && diagnostics->GetBufferPointer()) {
				error->assign(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize());
			} else {
				*error = "Unable to compile shader: " + std::filesystem::path(path).string();
			}
		}
		return false;
	}
	return true;
}
}

bool RoadRenderPipeline::Initialize(ID3D11Device* device, const std::wstring& shaderDirectory,
	std::string* error)
{
	if (device == nullptr) return SetError(error, "D3D11 device is null.");
	Shutdown();
	m_renderReady = false;
	if (error) error->clear();
	m_device = device;
	if (!m_textureCache.Initialize(device, error)) {
		Shutdown();
		return false;
	}

	if (!CompileShaders(device, shaderDirectory, error) ||
		!CreateConstantBuffers(device, error) ||
		!CreateSampler(device, error) ||
		!CreatePipelineStates(device, error) ||
		!CreateFallbackTexture(device, error)) {
		Shutdown();
		return false;
	}
	return true;
}

bool RoadRenderPipeline::CompileShaders(ID3D11Device* device,
	const std::wstring& shaderDirectory, std::string* error)
{
	Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
	Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
	if (!CompileShader(ShaderPath(shaderDirectory, L"RoadVS.hlsl"), "main", "vs_5_0", vertexBytecode, error) ||
		!CompileShader(ShaderPath(shaderDirectory, L"RoadPS.hlsl"), "main", "ps_5_0", pixelBytecode, error)) {
		return false;
	}
	if (FAILED(device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
		nullptr, m_vertexShader.GetAddressOf()))) return SetError(error, "Unable to create road vertex shader.");
	if (FAILED(device->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
		nullptr, m_pixelShader.GetAddressOf()))) return SetError(error, "Unable to create road pixel shader.");

	static_assert(offsetof(RoadMeshVertex, position) == 0);
	static_assert(offsetof(RoadMeshVertex, normal) == sizeof(Vector3D));
	static_assert(offsetof(RoadMeshVertex, u) == sizeof(Vector3D) * 2);
	static_assert(offsetof(RoadMeshVertex, v) == offsetof(RoadMeshVertex, u) + sizeof(float));

	const D3D11_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(RoadMeshVertex, position)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(RoadMeshVertex, normal)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		// Vertex input uses TEXCOORD0; RoadVS forwards UV as pixel-shader TEXCOORD1.
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(RoadMeshVertex, u)), D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};
	if (FAILED(device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)),
		vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(), m_inputLayout.GetAddressOf()))) {
		return SetError(error, "Unable to create road input layout.");
	}
	return true;
}

bool RoadRenderPipeline::CreateConstantBuffers(ID3D11Device* device, std::string* error)
{
	D3D11_BUFFER_DESC desc{};
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.ByteWidth = sizeof(RoadCameraConstants);
	if (FAILED(device->CreateBuffer(&desc, nullptr, m_cameraBuffer.GetAddressOf())))
		return SetError(error, "Unable to create road camera constant buffer.");

	desc.ByteWidth = sizeof(RoadLightConstants);
	if (FAILED(device->CreateBuffer(&desc, nullptr, m_lightBuffer.GetAddressOf())))
		return SetError(error, "Unable to create road light constant buffer.");
	return true;
}

bool RoadRenderPipeline::CreateSampler(ID3D11Device* device, std::string* error)
{
	D3D11_SAMPLER_DESC desc{};
	desc.Filter = D3D11_FILTER_ANISOTROPIC;
	desc.MaxAnisotropy = 8;
	desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MinLOD = 0.0f;
	desc.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(device->CreateSamplerState(&desc, m_sampler.GetAddressOf())))
		return SetError(error, "Unable to create road sampler.");
	return true;
}

bool RoadRenderPipeline::CreatePipelineStates(ID3D11Device* device, std::string* error)
{
	D3D11_RASTERIZER_DESC rasterizer{};
	rasterizer.FillMode = D3D11_FILL_SOLID;
	rasterizer.CullMode = D3D11_CULL_NONE;
	rasterizer.DepthClipEnable = TRUE;
	if (FAILED(device->CreateRasterizerState(&rasterizer, m_rasterizerState.GetAddressOf())))
		return SetError(error, "Unable to create road rasterizer state.");

	D3D11_DEPTH_STENCIL_DESC depth{};
	depth.DepthEnable = TRUE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	if (FAILED(device->CreateDepthStencilState(&depth, m_depthState.GetAddressOf())))
		return SetError(error, "Unable to create road depth state.");

	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0].BlendEnable = FALSE;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(device->CreateBlendState(&blend, m_blendState.GetAddressOf())))
		return SetError(error, "Unable to create road blend state.");
	return true;
}

bool RoadRenderPipeline::CreateFallbackTexture(ID3D11Device* device, std::string* error)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = 1;
	desc.Height = 1;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const UINT pixel = 0xffb8b8b8u;
	D3D11_SUBRESOURCE_DATA data{};
	data.pSysMem = &pixel;
	data.SysMemPitch = sizeof(pixel);
	if (FAILED(device->CreateTexture2D(&desc, &data, m_fallbackTexture.GetAddressOf())) ||
		FAILED(device->CreateShaderResourceView(m_fallbackTexture.Get(), nullptr, m_fallbackView.GetAddressOf())))
		return SetError(error, "Unable to create fallback road texture.");
	return true;
}

bool RoadRenderPipeline::LoadRoadTexture(const std::string& path, const ZipVFS& vfs,
	std::string* error)
{
	if (!m_device) return SetError(error, "Road renderer is not initialized.");
	std::string textureError;
	ID3D11ShaderResourceView* view = m_textureCache.GetOrLoad(path, vfs, &textureError);
	m_roadTexture = view;
	if (error) *error = textureError;
	return view != nullptr && textureError.empty();
}

ID3D11ShaderResourceView* RoadRenderPipeline::LoadTerrainTexture(const std::string& materialPath,
	const ZipVFS& vfs)
{
	const std::string normalizedMaterial = ZipVFS::NormalizePath(materialPath);
	const auto cached = m_terrainTextures.find(normalizedMaterial);
	if (cached != m_terrainTextures.end()) return cached->second.Get();
	const auto log = [this](const std::string& message) {
		if (m_textureLogCallback) m_textureLogCallback("[TerrainTexture] " + message);
	};
	const auto resolveReference = [](const std::string& ownerPath, std::string reference) {
		while (!reference.empty() && std::isspace(static_cast<unsigned char>(reference.front())))
			reference.erase(reference.begin());
		while (!reference.empty() && std::isspace(static_cast<unsigned char>(reference.back())))
			reference.pop_back();
		if (!reference.empty() && reference.front() == '/')
			reference.erase(reference.begin());
		else if (reference.rfind("material/", 0) != 0 && reference.rfind("system/", 0) != 0) {
			const std::size_t slash = ownerPath.find_last_of('/');
			if (slash != std::string::npos) reference = ownerPath.substr(0, slash + 1) + reference;
		}
		return ZipVFS::NormalizePath(reference);
	};

	log(".mat requested: " + materialPath + " (normalized: " + normalizedMaterial + ")");
	const std::vector<std::uint8_t> materialBytes = vfs.ReadFileFromMemory(normalizedMaterial);
	log(".mat " + normalizedMaterial + ": " +
		(materialBytes.empty() ? "not found/readable" : "found, bytes=" + std::to_string(materialBytes.size())));
	std::string textureReference;
	if (!materialBytes.empty()) {
		const std::string materialText(materialBytes.begin(), materialBytes.end());
		std::istringstream lines(materialText);
		std::string line;
		while (std::getline(lines, line)) {
			if (line.find("texture") == std::string::npos) continue;
			const std::size_t firstQuote = line.find('"');
			const std::size_t lastQuote = firstQuote == std::string::npos
				? std::string::npos : line.find('"', firstQuote + 1);
			if (lastQuote != std::string::npos) {
				textureReference = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
				break;
			}
		}
	}

	std::string ddsPath;
	if (textureReference.empty()) {
		log("[ERROR] No TOBJ texture reference in .mat " + normalizedMaterial);
	} else {
		const std::string tobjPath = resolveReference(normalizedMaterial, textureReference);
		log(".tobj extracted from " + normalizedMaterial + ": " + tobjPath);
		const std::vector<std::uint8_t> tobjBytes = vfs.ReadFileFromMemory(tobjPath);
		log(".tobj " + tobjPath + ": " +
			(tobjBytes.empty() ? "not found/readable" : "found, bytes=" + std::to_string(tobjBytes.size())));
		if (!tobjBytes.empty()) {
			ddsPath = ZipVFS::ExtractDdsPathFromTobjBytes(tobjBytes);
			if (!ddsPath.empty()) ddsPath = resolveReference(tobjPath, ddsPath);
		}
		if (ddsPath.empty()) log("[ERROR] No DDS path in .tobj " + tobjPath);
		else log(".dds extracted from " + tobjPath + ": " + ddsPath);
	}

	ID3D11ShaderResourceView* view = m_fallbackView.Get();
	if (!ddsPath.empty()) {
		std::string textureError;
		view = m_textureCache.GetOrLoad(ddsPath, vfs, &textureError);
		if (!textureError.empty()) log("[ERROR] .dds load failed: " + textureError);
	}
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> retainedView = view;
	return m_terrainTextures.emplace(normalizedMaterial, std::move(retainedView)).first->second.Get();
}

bool RoadRenderPipeline::EnsureDepthBuffer(ID3D11Device* device, UINT width, UINT height)
{
	if (device == nullptr || width == 0 || height == 0) return false;
	if (m_depthTexture && m_depthWidth == width && m_depthHeight == height) return true;
	m_depthTexture.Reset();
	m_depthView.Reset();
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, m_depthTexture.GetAddressOf())) ||
		FAILED(device->CreateDepthStencilView(m_depthTexture.Get(), nullptr, m_depthView.GetAddressOf()))) {
		return false;
	}
	m_depthWidth = width;
	m_depthHeight = height;
	return true;
}

bool RoadRenderPipeline::UploadBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
	const void* data, size_t byteCount) const
{
	if (context == nullptr || buffer == nullptr || data == nullptr || byteCount == 0) return false;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	if (mapped.pData == nullptr) {
		context->Unmap(buffer, 0);
		return false;
	}
	std::memcpy(mapped.pData, data, byteCount);
	context->Unmap(buffer, 0);
	return true;
}

void RoadRenderPipeline::Render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
	const std::vector<RoadSegment>& roads, const RoadCameraConstants& camera,
	const std::vector<std::string>& terrainShaders, const ZipVFS& vfs,
	float viewportX, float viewportY, float viewportWidth, float viewportHeight,
	float targetWidth, float targetHeight)
{
	if (!context || !target || !m_device || !m_vertexShader || !m_pixelShader || !m_inputLayout ||
		!m_cameraBuffer || !m_lightBuffer || viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
		targetWidth <= 0.0f || targetHeight <= 0.0f) return;
	if (targetWidth > kMaxTargetDimension || targetHeight > kMaxTargetDimension) return;
	const UINT width = static_cast<UINT>(targetWidth);
	const UINT height = static_cast<UINT>(targetHeight);
	if (!EnsureDepthBuffer(m_device.Get(), width, height) ||
		!UploadBuffer(context, m_cameraBuffer.Get(), &camera, sizeof(camera))) return;

	RoadLightConstants light = m_light;
	const DirectX::XMVECTOR normalized = DirectX::XMVector3Normalize(
		DirectX::XMLoadFloat3(&light.direction));
	DirectX::XMStoreFloat3(&light.direction, normalized);
	if (!UploadBuffer(context, m_lightBuffer.Get(), &light, sizeof(light))) return;

	ID3D11RenderTargetView* renderTarget = target;
	context->OMSetRenderTargets(1, &renderTarget, m_depthView.Get());
	D3D11_VIEWPORT viewport{};
	viewport.TopLeftX = viewportX;
	viewport.TopLeftY = viewportY;
	viewport.Width = viewportWidth;
	viewport.Height = viewportHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);
	context->ClearDepthStencilView(m_depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	context->RSSetState(m_rasterizerState.Get());
	context->OMSetDepthStencilState(m_depthState.Get(), 0);
	context->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffffu);
	context->IASetInputLayout(m_inputLayout.Get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
	context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
	ID3D11Buffer* cameraBuffer = m_cameraBuffer.Get();
	ID3D11Buffer* lightBuffer = m_lightBuffer.Get();
	context->VSSetConstantBuffers(0, 1, &cameraBuffer);
	context->PSSetConstantBuffers(1, 1, &lightBuffer);
	ID3D11SamplerState* sampler = m_sampler.Get();
	context->PSSetSamplers(0, 1, &sampler);
	ID3D11ShaderResourceView* texture = m_roadTexture ? m_roadTexture.Get() : m_fallbackView.Get();
	context->PSSetShaderResources(0, 1, &texture);
	m_renderReady = true;

	for (const RoadSegment& road : roads) {
		const UINT stride = sizeof(RoadMeshVertex);
		const UINT offset = 0;
		if (road.vertexBuffer && road.indexBuffer && !road.meshIndices.empty() &&
			road.meshIndices.size() <= static_cast<size_t>(kMaxDrawIndexCount)) {
			ID3D11Buffer* vertexBuffer = road.vertexBuffer.Get();
			context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
			context->IASetIndexBuffer(road.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
			ID3D11ShaderResourceView* roadTexture = m_roadTexture ? m_roadTexture.Get() : m_fallbackView.Get();
			context->PSSetShaderResources(0, 1, &roadTexture);
			context->DrawIndexed(static_cast<UINT>(road.meshIndices.size()), 0, 0);
		}

		const auto drawTerrain = [&](int shaderIndex, const std::vector<RoadMeshVertex>& vertices,
			const std::vector<uint32_t>& indices, ID3D11Buffer* terrainVertexBuffer,
			ID3D11Buffer* terrainIndexBuffer) {
			if (shaderIndex < 0 || shaderIndex >= static_cast<int>(terrainShaders.size()) ||
				vertices.empty() || indices.empty() || !terrainVertexBuffer || !terrainIndexBuffer ||
				indices.size() > static_cast<size_t>(kMaxDrawIndexCount)) return;
			ID3D11ShaderResourceView* terrainTexture = LoadTerrainTexture(
				terrainShaders[static_cast<size_t>(shaderIndex)], vfs);
			if (terrainTexture == nullptr) terrainTexture = m_fallbackView.Get();
			context->PSSetShaderResources(0, 1, &terrainTexture);
			context->IASetVertexBuffers(0, 1, &terrainVertexBuffer, &stride, &offset);
			context->IASetIndexBuffer(terrainIndexBuffer, DXGI_FORMAT_R32_UINT, 0);
			context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
		};
		drawTerrain(road.properties.left.terrainShaderIndex, road.leftTerrainVertices,
			road.leftTerrainIndices, road.leftTerrainVertexBuffer.Get(), road.leftTerrainIndexBuffer.Get());
		drawTerrain(road.properties.right.terrainShaderIndex, road.rightTerrainVertices,
			road.rightTerrainIndices, road.rightTerrainVertexBuffer.Get(), road.rightTerrainIndexBuffer.Get());
	}
}

void RoadRenderPipeline::TerapkanFilterVarian(std::vector<SubMeshVisual>& daftarMesh,
	TipeVarian varianAktif)
{
	char activeTag = '\0';
	switch (varianAktif) {
	case TipeVarian::Exclusive: activeTag = 'A'; break;
	case TipeVarian::Standard: activeTag = 'B'; break;
	case TipeVarian::Custom: activeTag = 'C'; break;
	default: break;
	}

	for (SubMeshVisual& mesh : daftarMesh) {
		const std::string tag = NormalizeVariantTag(mesh.penandaVarian);
		mesh.isVisible = tag == "ABC" || (tag.size() == 1 && tag[0] == activeTag);
	}
}

void RoadRenderPipeline::RenderSubMeshVarian(const std::vector<SubMeshVisual>& daftarMesh,
	ID3D11DeviceContext* context)
{
	if (context == nullptr || !m_renderReady || !m_vertexShader || !m_pixelShader ||
		!m_inputLayout || !m_cameraBuffer || !m_lightBuffer) return;

	context->IASetInputLayout(m_inputLayout.Get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
	context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
	ID3D11Buffer* cameraBuffer = m_cameraBuffer.Get();
	ID3D11Buffer* lightBuffer = m_lightBuffer.Get();
	context->VSSetConstantBuffers(0, 1, &cameraBuffer);
	context->PSSetConstantBuffers(1, 1, &lightBuffer);
	ID3D11SamplerState* sampler = m_sampler.Get();
	context->PSSetSamplers(0, 1, &sampler);
	ID3D11ShaderResourceView* texture = m_roadTexture ? m_roadTexture.Get() : m_fallbackView.Get();
	context->PSSetShaderResources(0, 1, &texture);

	for (const SubMeshVisual& mesh : daftarMesh) {
		if (!mesh.isVisible || mesh.totalIndex == 0 || mesh.vertexStride == 0 ||
			!mesh.vertexBuffer || !mesh.indexBuffer) continue;

		const UINT offset = 0;
		ID3D11Buffer* vertexBuffer = mesh.vertexBuffer.Get();
		context->IASetVertexBuffers(0, 1, &vertexBuffer, &mesh.vertexStride, &offset);
		context->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
		context->DrawIndexed(mesh.totalIndex, 0, 0);
	}
}

void RoadRenderPipeline::Shutdown()
{
	m_renderReady = false;
	m_textureCache.Clear();
	m_terrainTextures.clear();
	m_vertexShader.Reset();
	m_pixelShader.Reset();
	m_inputLayout.Reset();
	m_cameraBuffer.Reset();
	m_lightBuffer.Reset();
	m_sampler.Reset();
	m_rasterizerState.Reset();
	m_depthState.Reset();
	m_blendState.Reset();
	m_fallbackTexture.Reset();
	m_fallbackView.Reset();
	m_roadTexture.Reset();
	m_depthTexture.Reset();
	m_depthView.Reset();
	m_device.Reset();
	m_depthWidth = 0;
	m_depthHeight = 0;
}
