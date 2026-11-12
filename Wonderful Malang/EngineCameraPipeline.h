#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d11.h>
#include <DirectXMath.h>

class RoadSystem;

struct alignas(16) MatrixConstantBuffer
{
    DirectX::XMMATRIX viewMatrix;
    DirectX::XMMATRIX projectionMatrix;
};

static_assert(alignof(MatrixConstantBuffer) >= 16, "MatrixConstantBuffer must be 16-byte aligned.");
static_assert(sizeof(MatrixConstantBuffer) % 16 == 0, "MatrixConstantBuffer must have a 16-byte size multiple.");

class EngineCameraPipeline
{
public:
    bool Initialize(ID3D11Device* device);
    void FocusOnWorldCoordinates(float targetX, float targetZ);
    void FocusOnRoad(const RoadSystem& roadSystem);
    void Update(float deltaTime, bool controlsEnabled = true);
    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix(float aspectRatio) const;
    void UpdatePipelineContext(ID3D11DeviceContext* context, float viewportWidth, float viewportHeight);
    void Shutdown();

private:
    ID3D11Buffer* m_constantBuffer = nullptr;
    DirectX::XMFLOAT3 m_cameraPosition = { 0.0f, 50.0f, -100.0f };
    DirectX::XMFLOAT3 m_cameraTarget = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_cameraDirection = { 0.0f, -0.4472136f, 0.8944272f };
    DirectX::XMFLOAT3 m_upVector = { 0.0f, 1.0f, 0.0f };
    float m_pitchRadians = -0.4636476f;
    float m_yawRadians = 0.0f;
    float m_moveSpeed = 35.0f;
};
#pragma once
