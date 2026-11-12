#include "EngineCameraPipeline.h"
#include "RoadSystem.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

bool EngineCameraPipeline::Initialize(ID3D11Device* device) {
    if (device == nullptr) {
        return false;
    }

    Shutdown();

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth = sizeof(MatrixConstantBuffer);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    HRESULT hr = device->CreateBuffer(&desc, nullptr, &m_constantBuffer);
    if (FAILED(hr)) {
        m_constantBuffer = nullptr;
        return false;
    }

    return true;
}

void EngineCameraPipeline::FocusOnWorldCoordinates(float targetX, float targetZ) {
    m_cameraTarget = DirectX::XMFLOAT3(targetX, 0.0f, targetZ);
    m_cameraPosition = DirectX::XMFLOAT3(targetX, 35.0f, targetZ - 60.0f);

    const DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&m_cameraTarget),
            DirectX::XMLoadFloat3(&m_cameraPosition)));
    DirectX::XMStoreFloat3(&m_cameraDirection, direction);
    m_pitchRadians = std::asin(std::clamp(m_cameraDirection.y, -1.0f, 1.0f));
    m_yawRadians = std::atan2(m_cameraDirection.x, m_cameraDirection.z);
}

void EngineCameraPipeline::FocusOnRoad(const RoadSystem& roadSystem) {
    const auto& nodes = roadSystem.GetNodes();
    if (nodes.empty()) return;

    const Vector3D& roadStartPos = nodes.front().position;
    m_cameraTarget = DirectX::XMFLOAT3(roadStartPos.x, 0.0f, roadStartPos.y);
    m_cameraPosition = DirectX::XMFLOAT3(
        roadStartPos.x,
        25.0f,
        roadStartPos.y - 60.0f);

    const DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&m_cameraTarget),
            DirectX::XMLoadFloat3(&m_cameraPosition)));
    DirectX::XMStoreFloat3(&m_cameraDirection, direction);
    m_pitchRadians = std::asin(std::clamp(m_cameraDirection.y, -1.0f, 1.0f));
    m_yawRadians = std::atan2(m_cameraDirection.x, m_cameraDirection.z);
}

void EngineCameraPipeline::Update(float deltaTime, bool controlsEnabled) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!controlsEnabled || io.WantTextInput || deltaTime <= 0.0f) {
        return;
    }

    const float frameTime = std::min(deltaTime, 0.1f);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        constexpr float mouseSensitivity = 0.0035f;
        m_yawRadians += io.MouseDelta.x * mouseSensitivity;
        m_pitchRadians = std::clamp(m_pitchRadians - io.MouseDelta.y * mouseSensitivity,
            DirectX::XMConvertToRadians(-85.0f), DirectX::XMConvertToRadians(85.0f));
    }
    const float cosPitch = std::cos(m_pitchRadians);
    const DirectX::XMFLOAT3 lookDirection = {
        std::sin(m_yawRadians) * cosPitch,
        std::sin(m_pitchRadians),
        std::cos(m_yawRadians) * cosPitch
    };
    const DirectX::XMFLOAT3 rightDirection = {
        std::cos(m_yawRadians),
        0.0f,
        -std::sin(m_yawRadians)
    };
    m_cameraDirection = lookDirection;

    DirectX::XMVECTOR position = DirectX::XMLoadFloat3(&m_cameraPosition);
    const DirectX::XMVECTOR look = DirectX::XMLoadFloat3(&lookDirection);
    const DirectX::XMVECTOR right = DirectX::XMLoadFloat3(&rightDirection);
    const float distance = m_moveSpeed * frameTime;

    if (ImGui::IsKeyDown(ImGuiKey_W)) position = DirectX::XMVectorAdd(position, DirectX::XMVectorScale(look, distance));
    if (ImGui::IsKeyDown(ImGuiKey_S)) position = DirectX::XMVectorSubtract(position, DirectX::XMVectorScale(look, distance));
    if (ImGui::IsKeyDown(ImGuiKey_A)) position = DirectX::XMVectorSubtract(position, DirectX::XMVectorScale(right, distance));
    if (ImGui::IsKeyDown(ImGuiKey_D)) position = DirectX::XMVectorAdd(position, DirectX::XMVectorScale(right, distance));

    DirectX::XMStoreFloat3(&m_cameraPosition, position);
    m_cameraPosition.y = std::max(m_cameraPosition.y, 1.0f);
    DirectX::XMStoreFloat3(&m_cameraTarget,
        DirectX::XMVectorAdd(DirectX::XMLoadFloat3(&m_cameraPosition), look));
}

DirectX::XMMATRIX EngineCameraPipeline::GetViewMatrix() const {
    return DirectX::XMMatrixLookAtLH(
        DirectX::XMLoadFloat3(&m_cameraPosition),
        DirectX::XMLoadFloat3(&m_cameraTarget),
        DirectX::XMLoadFloat3(&m_upVector));
}

DirectX::XMMATRIX EngineCameraPipeline::GetProjectionMatrix(float aspectRatio) const {
    return DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(45.0f), aspectRatio, 0.1f, 5000.0f);
}

void EngineCameraPipeline::UpdatePipelineContext(ID3D11DeviceContext* context, float viewportWidth, float viewportHeight) {
    if (m_constantBuffer == nullptr || context == nullptr || viewportWidth <= 0.0f || viewportHeight <= 0.0f) {
        return;
    }

    const float aspectRatio = viewportWidth / viewportHeight;
    const DirectX::XMMATRIX view = GetViewMatrix();
    const DirectX::XMMATRIX proj = GetProjectionMatrix(aspectRatio);

    D3D11_MAPPED_SUBRESOURCE mappedResource = {};
    if (SUCCEEDED(context->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource)) &&
        mappedResource.pData != nullptr) {
        MatrixConstantBuffer* dataPtr = reinterpret_cast<MatrixConstantBuffer*>(mappedResource.pData);
        dataPtr->viewMatrix = DirectX::XMMatrixTranspose(view);
        dataPtr->projectionMatrix = DirectX::XMMatrixTranspose(proj);
        context->Unmap(m_constantBuffer, 0);
    }

    context->VSSetConstantBuffers(0, 1, &m_constantBuffer);
}

void EngineCameraPipeline::Shutdown() {
    if (m_constantBuffer) {
        m_constantBuffer->Release();
        m_constantBuffer = nullptr;
    }
}
