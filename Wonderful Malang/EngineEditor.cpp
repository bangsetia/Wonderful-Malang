#include "EngineEditor.h"
#include "EngineEditor_3DRendering.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

bool EngineEditor::EnsureViewportRenderTarget(UINT width, UINT height)
{
    if (m_graphicsDevice == nullptr || width == 0 || height == 0)
        return false;
    if (m_viewportTexture && m_viewportRTV && m_viewportSRV &&
        m_viewportTexWidth == width && m_viewportTexHeight == height)
        return true;

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
    if (FAILED(m_graphicsDevice->CreateTexture2D(&textureDesc, nullptr, texture.GetAddressOf())) ||
        FAILED(m_graphicsDevice->CreateRenderTargetView(texture.Get(), nullptr, renderTargetView.GetAddressOf())) ||
        FAILED(m_graphicsDevice->CreateShaderResourceView(texture.Get(), nullptr, shaderResourceView.GetAddressOf())))
        return false;

    m_viewportTexture = std::move(texture);
    m_viewportRTV = std::move(renderTargetView);
    m_viewportSRV = std::move(shaderResourceView);
    m_viewportTexWidth = width;
    m_viewportTexHeight = height;
    return true;
}

bool EngineEditor::LoadAssetArchive(const std::string& archivePath, std::string* error)
{
    m_assetVfs.SetPhysicalRoot({});
    if (!m_assetVfs.Mount(archivePath, error))
        return false;
    std::string bridgeError;
    if (!m_siiBridge.Initialize(std::string(), &m_assetVfs, &bridgeError)) {
        if (error)
            *error = bridgeError;
        return false;
    }
    SynchronizeAssetCaches();
    LoadPreferredRoadMaterialTexture(nullptr);
    return true;
}

void EngineEditor::SynchronizeAssetCaches()
{
    const SiiAssetCache& assets = m_siiBridge.Assets();
    m_roadSystem.SetAssetDatabase(assets.roadLooks, assets.roadMaterialData);
    m_roadSystem.SetTerrainProfiles(assets.terrainProfileData);
    m_assetRegistry.roadTypes.clear();
    for (const RoadLookData& look : assets.roadLooks) {
        if (std::find(m_assetRegistry.roadTypes.begin(), m_assetRegistry.roadTypes.end(), look.name) ==
            m_assetRegistry.roadTypes.end())
            m_assetRegistry.roadTypes.push_back(look.name);
    }
    m_assetRegistry.roadMaterials = assets.roadMaterials;
    m_assetRegistry.models = assets.models;
    m_assetRegistry.railingModels = assets.railings;
    m_assetRegistry.terrainShaders = assets.terrainShaders;
    m_assetRegistry.terrainProfiles = assets.terrainProfiles;
    m_assetRegistry.vegetation = assets.vegetation;

    const std::string message = "Synchronized UI cache counts: RoadSystem roadLooks=" +
        std::to_string(m_roadSystem.GetRoadLooks().size()) + ", RoadSystem roadMaterials=" +
        std::to_string(m_roadSystem.GetRoadMaterials().size()) +
        "; AssetRegistry roadLooks=" + std::to_string(m_assetRegistry.roadTypes.size()) +
        ", roadMaterials=" + std::to_string(m_assetRegistry.roadMaterials.size()) +
        ", terrainShaders=" + std::to_string(m_assetRegistry.terrainShaders.size()) +
        ", terrainProfiles=" + std::to_string(m_assetRegistry.terrainProfiles.size()) +
        ", models=" + std::to_string(m_assetRegistry.models.size()) +
        ", railingModels=" + std::to_string(m_assetRegistry.railingModels.size()) +
        ", vegetation=" + std::to_string(m_assetRegistry.vegetation.size());
    if (!m_assetInitializationLog.empty())
        m_assetInitializationLog += '\n';
    m_assetInitializationLog += message;
    OutputDebugStringA(("[EngineEditor] " + message + "\n").c_str());
}

bool EngineEditor::LoadRoadMaterialTexture(const std::string& materialPath, std::string* error)
{
    const auto readBytes = [this](const std::string& path) {
        return m_assetVfs.ReadFileFromMemory(path);
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
            if (slash != std::string::npos)
                reference = ownerPath.substr(0, slash + 1) + reference;
        }
        return ZipVFS::NormalizePath(reference);
    };

    const std::string normalizedMaterial = ZipVFS::NormalizePath(materialPath);
    LogAssetInitializationMessage("Road texture .mat requested: " + materialPath + " (normalized: " + normalizedMaterial + ")");
    const std::vector<std::uint8_t> materialBytes = readBytes(normalizedMaterial);
    LogAssetInitializationMessage("Road texture .mat " + normalizedMaterial + ": " +
        (materialBytes.empty() ? "not found/readable" : "found, bytes=" + std::to_string(materialBytes.size())));
    if (materialBytes.empty()) {
        const std::string message = "Unable to read road material: " + normalizedMaterial;
        if (error) *error = message;
        LogAssetInitializationMessage("[ERROR] " + message);
        return false;
    }
    const std::string materialText(materialBytes.begin(), materialBytes.end());
    std::string tobjReference;
    std::istringstream materialLines(materialText);
    std::string line;
    while (std::getline(materialLines, line)) {
        if (line.find("texture") == std::string::npos)
            continue;
        const std::size_t firstQuote = line.find('"');
        const std::size_t lastQuote = firstQuote == std::string::npos
            ? std::string::npos : line.find('"', firstQuote + 1);
        if (lastQuote != std::string::npos) {
            tobjReference = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            break;
        }
    }
    if (tobjReference.empty()) {
        const std::string message = "No TOBJ texture reference in road material: " + normalizedMaterial;
        if (error) *error = message;
        LogAssetInitializationMessage("[ERROR] " + message);
        return false;
    }

    const std::string tobjPath = resolveReference(normalizedMaterial, tobjReference);
    LogAssetInitializationMessage("Road texture .tobj extracted from " + normalizedMaterial + ": " + tobjPath);
    const std::vector<std::uint8_t> tobjBytes = readBytes(tobjPath);
    LogAssetInitializationMessage("Road texture .tobj " + tobjPath + ": " +
        (tobjBytes.empty() ? "not found/readable" : "found, bytes=" + std::to_string(tobjBytes.size())));
    if (tobjBytes.empty()) {
        const std::string message = "Unable to read road TOBJ: " + tobjPath;
        if (error) *error = message;
        LogAssetInitializationMessage("[ERROR] " + message);
        return false;
    }
    std::string ddsPath = ZipVFS::ExtractDdsPathFromTobjBytes(tobjBytes);
    if (ddsPath.empty()) {
        const std::string message = "No DDS path in road TOBJ: " + tobjPath;
        if (error) *error = message;
        LogAssetInitializationMessage("[ERROR] " + message);
        return false;
    }
    if (ddsPath.front() != '/')
        ddsPath = resolveReference(tobjPath, ddsPath);
    else
        ddsPath = ZipVFS::NormalizePath(ddsPath);
    LogAssetInitializationMessage("Road texture .dds extracted from " + tobjPath + ": " + ddsPath);
    if (!m_roadPipeline.LoadRoadTexture(ddsPath, m_assetVfs, error)) {
        LogAssetInitializationMessage("[ERROR] Road texture load failed for " + ddsPath +
            (error && !error->empty() ? ": " + *error : std::string()));
        return false;
    }
    m_loadedRoadMaterialPath = normalizedMaterial;
    LogAssetInitializationMessage("Road material texture loaded: " + normalizedMaterial + " -> " + ddsPath);
    return true;
}

bool EngineEditor::LoadPreferredRoadMaterialTexture(std::string* error)
{
    const std::vector<RoadMaterialData>& materials = m_roadSystem.GetRoadMaterials();
    if (materials.empty()) {
        const std::string message = "No road material is available for initial texture loading.";
        if (error) *error = message;
        LogAssetInitializationMessage("[WARNING] " + message);
        return false;
    }

    const auto isAsphalt = [](const RoadMaterialData& material) {
        std::string searchable = material.name + " " + material.path;
        std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return searchable.find("asphalt") != std::string::npos;
    };
    const auto asphalt = std::find_if(materials.begin(), materials.end(), isAsphalt);
    const RoadMaterialData& selected = asphalt != materials.end() ? *asphalt : materials.front();
    LogAssetInitializationMessage("Initial road material selected: " + selected.name + " (" + selected.path + ")");
    return LoadRoadMaterialTexture(selected.path, error);
}

void EngineEditor::LogAssetInitializationMessage(const std::string& message)
{
    if (!m_assetInitializationLog.empty())
        m_assetInitializationLog += '\n';
    m_assetInitializationLog += message;
    OutputDebugStringA(("[EngineEditor] " + message + "\n").c_str());
}

namespace {
bool RoadLookCombo(const char* id, int& selection, const std::vector<RoadLookData>& items)
{
    if (items.empty()) {
        selection = -1;
        ImGui::BeginDisabled();
    }
    else if (selection < 0 || selection >= static_cast<int>(items.size())) selection = 0;

    const char* preview = items.empty() ? "<empty>" : items[static_cast<size_t>(selection)].name.c_str();
    bool changed = false;
    if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightRegular)) {
        for (int index = 0; index < static_cast<int>(items.size()); ++index) {
            const bool selected = index == selection;
            if (ImGui::Selectable(items[static_cast<size_t>(index)].name.c_str(), selected)) {
                selection = index;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (items.empty()) ImGui::EndDisabled();
    return changed || ImGui::IsItemEdited();
}

bool RoadMaterialCombo(const char* id, int& selection, const std::vector<RoadMaterialData>& items)
{
    if (items.empty()) {
        selection = -1;
        ImGui::BeginDisabled();
    }
    else if (selection < 0 || selection >= static_cast<int>(items.size())) selection = 0;

    const std::string previewText = items.empty() ? "<empty>" :
        items[static_cast<size_t>(selection)].name + " (" + items[static_cast<size_t>(selection)].path + ")";
    const char* preview = previewText.c_str();
    bool changed = false;
    if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightRegular)) {
        for (int index = 0; index < static_cast<int>(items.size()); ++index) {
            const bool selected = index == selection;
            const RoadMaterialData& material = items[static_cast<size_t>(index)];
            const std::string label = material.name + " (" + material.path + ")";
            if (ImGui::Selectable(label.c_str(), selected)) {
                selection = index;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (items.empty()) ImGui::EndDisabled();
    return changed || ImGui::IsItemEdited();
}
bool RegistryCombo(const char* id, int& selection, const std::vector<std::string>& items)
{
    if (items.empty()) {
        selection = -1;
        ImGui::BeginDisabled();
    }
    else if (selection < 0 || selection >= static_cast<int>(items.size())) selection = 0;

    const char* preview = items.empty() ? "<empty>" : items[static_cast<size_t>(selection)].c_str();
    bool changed = false;
    if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightRegular)) {
        for (int index = 0; index < static_cast<int>(items.size()); ++index) {
            const bool selected = index == selection;
            if (ImGui::Selectable(items[static_cast<size_t>(index)].c_str(), selected)) {
                selection = index;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (items.empty()) ImGui::EndDisabled();
    return changed || ImGui::IsItemEdited();
}

void TriggerRoadMeshRebuild(RoadSegment& road, const SiiAssetRegistry& registry)
{
    if (registry.rebuildRoadMesh) registry.rebuildRoadMesh(road);
}

void DrawSideLabel(const char* label, float controlX)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(controlX);
}

bool DrawSideFloat(const char* label, const char* id, float& value, float controlX)
{
    DrawSideLabel(label, controlX);
    ImGui::PushItemWidth(120.0f);
    const bool changed = ImGui::InputFloat(id, &value, 0.1f, 1.0f, "%.2f");
    ImGui::PopItemWidth();
    return changed || ImGui::IsItemEdited();
}

bool DrawSideInt(const char* label, const char* id, int& value, float controlX)
{
    DrawSideLabel(label, controlX);
    ImGui::PushItemWidth(120.0f);
    const bool changed = ImGui::InputInt(id, &value);
    ImGui::PopItemWidth();
    return changed || ImGui::IsItemEdited();
}

bool DrawSideCombo(const char* label, const char* id, int& value,
    const std::vector<std::string>& items, float controlX)
{
    DrawSideLabel(label, controlX);
    ImGui::PushItemWidth(180.0f);
    const bool changed = RegistryCombo(id, value, items);
    ImGui::PopItemWidth();
    return changed || ImGui::IsItemEdited();
}

bool DrawSideCheck(const char* label, const char* id, bool& value, float controlX)
{
    DrawSideLabel(label, controlX);
    const bool changed = ImGui::Checkbox(id, &value);
    return changed || ImGui::IsItemEdited();
}

void DrawSideGroupHeader(const char* title)
{
    ImGui::Text("--- %s ----------------", title);
}

void DrawSideRows(RoadSideProperties& side, const char* sideId,
    const SiiAssetRegistry& registry, bool& edited)
{
    constexpr float controlX = 142.0f;
    const std::string prefix = std::string("##") + sideId;
    edited |= DrawSideFloat("Height Offset", ("HeightOffset" + prefix).c_str(), side.heightOffset, controlX);
    edited |= DrawSideFloat("Model Distance", ("ModelDistance" + prefix).c_str(), side.modelDistance, controlX);
    edited |= DrawSideCombo("Model", ("Model" + prefix).c_str(), side.modelIndex, registry.models, controlX);
    edited |= DrawSideCombo("Railing Model", ("RailingModel" + prefix).c_str(), side.railingModelIndex, registry.railingModels, controlX);
    edited |= DrawSideCheck("Invert Railing", ("InvertRailing" + prefix).c_str(), side.invertRailing, controlX);
    edited |= DrawSideCombo("Terrain Shader", ("TerrainShader" + prefix).c_str(), side.terrainShaderIndex, registry.terrainShaders, controlX);
    edited |= DrawSideInt("Terrain Quads", ("TerrainQuads" + prefix).c_str(), side.terrainQuadCount, controlX);
    if (side.terrainQuadCount < 0) side.terrainQuadCount = 0;
    edited |= DrawSideCombo("Terrain Profile", ("TerrainProfile" + prefix).c_str(), side.terrainProfileIndex, registry.terrainProfiles, controlX);
    for (size_t index = 0; index < side.vegetationIndices.size(); ++index) {
        const std::string label = "Vegetation " + std::to_string(index + 1);
        const std::string id = "Vegetation" + prefix + std::to_string(index);
        edited |= DrawSideCombo(label.c_str(), id.c_str(), side.vegetationIndices[index], registry.vegetation, controlX);
    }
}
}

void RenderRoadItemPropertiesWindow(RoadSegment& activeRoad, const SiiAssetRegistry& registry,
    const RoadSystem& roadSystem)
{
    static RoadProperties editSnapshot;
    static uint32_t snapshotRoadId = 0;
    static int snapshotFrame = -1;
    const int frame = ImGui::GetFrameCount();
    if (snapshotRoadId != activeRoad.id || snapshotFrame + 1 != frame) {
        editSnapshot = activeRoad.properties;
        snapshotRoadId = activeRoad.id;
    }
    snapshotFrame = frame;

    RoadProperties& properties = activeRoad.properties;
    bool edited = false;
    bool applyAndClose = false;
    bool cancel = false;

    ImGui::Text("GLOBAL ROAD SETTINGS");
    ImGui::Separator();
    ImGui::Text("Road Type / Look");
    ImGui::SameLine(180.0f);
    ImGui::PushItemWidth(180.0f);
    edited |= RoadLookCombo("##RoadType", properties.roadTypeIndex, roadSystem.GetRoadLooks());
    ImGui::PopItemWidth();
    ImGui::Text("Road Material");
    ImGui::SameLine(180.0f);
    ImGui::PushItemWidth(180.0f);
    const bool roadMaterialChanged = RoadMaterialCombo("##RoadMaterial", properties.roadMaterialIndex,
        roadSystem.GetRoadMaterials());
    edited |= roadMaterialChanged;
    const std::vector<RoadMaterialData>& materials = roadSystem.GetRoadMaterials();
    if (roadMaterialChanged && properties.roadMaterialIndex >= 0 &&
        properties.roadMaterialIndex < static_cast<int>(materials.size()) && registry.loadRoadMaterialTexture) {
        registry.loadRoadMaterialTexture(materials[static_cast<size_t>(properties.roadMaterialIndex)].path);
    }
    ImGui::PopItemWidth();
    if (ImGui::RadioButton("Left-hand traffic##trafficLeft", properties.leftHandTraffic)) {
        properties.leftHandTraffic = true;
        edited = true;
    }
    ImGui::SameLine(260.0f);
    if (ImGui::RadioButton("Right-hand traffic##trafficRight", !properties.leftHandTraffic)) {
        properties.leftHandTraffic = false;
        edited = true;
    }
    edited |= ImGui::Checkbox("Collision Enabled##collision", &properties.collisionEnabled) || ImGui::IsItemEdited();
    edited |= ImGui::Checkbox("Terrain Collision##terrainCollision", &properties.terrainCollisionEnabled) || ImGui::IsItemEdited();

    ImGui::Text("SIDE PARAMETERS");
    ImGui::Separator();
    if (ImGui::BeginTable("RoadSideParameters", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Left Side", ImGuiTableColumnFlags_WidthFixed, 305.0f);
        ImGui::TableSetupColumn("Right Side", ImGuiTableColumnFlags_WidthFixed, 305.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawSideGroupHeader("LEFT");
        ImGui::TableSetColumnIndex(1);
        DrawSideGroupHeader("RIGHT");
        for (int row = 0; row < 11; ++row) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const size_t leftVegetation = row >= 8 ? static_cast<size_t>(row - 8) : 0;
            if (row < 8) {
                if (row == 0) edited |= DrawSideFloat("Height Offset", "##HeightOffsetLeft", properties.left.heightOffset, 142.0f);
                if (row == 1) edited |= DrawSideFloat("Model Distance", "##ModelDistanceLeft", properties.left.modelDistance, 142.0f);
                if (row == 2) edited |= DrawSideCombo("Model", "##ModelLeft", properties.left.modelIndex, registry.models, 142.0f);
                if (row == 3) edited |= DrawSideCombo("Railing Model", "##RailingModelLeft", properties.left.railingModelIndex, registry.railingModels, 142.0f);
                if (row == 4) edited |= DrawSideCheck("Invert Railing", "##InvertRailingLeft", properties.left.invertRailing, 142.0f);
                if (row == 5) edited |= DrawSideCombo("Terrain Shader", "##TerrainShaderLeft", properties.left.terrainShaderIndex, registry.terrainShaders, 142.0f);
                if (row == 6) edited |= DrawSideInt("Terrain Quads", "##TerrainQuadsLeft", properties.left.terrainQuadCount, 142.0f);
                if (row == 7) edited |= DrawSideCombo("Terrain Profile", "##TerrainProfileLeft", properties.left.terrainProfileIndex, registry.terrainProfiles, 142.0f);
            } else {
                const std::string label = "Vegetation " + std::to_string(leftVegetation + 1);
                const std::string id = "##VegetationLeft" + std::to_string(leftVegetation);
                edited |= DrawSideCombo(label.c_str(), id.c_str(), properties.left.vegetationIndices[leftVegetation], registry.vegetation, 142.0f);
            }
            ImGui::TableSetColumnIndex(1);
            if (row < 8) {
                if (row == 0) edited |= DrawSideFloat("Height Offset", "##HeightOffsetRight", properties.right.heightOffset, 142.0f);
                if (row == 1) edited |= DrawSideFloat("Model Distance", "##ModelDistanceRight", properties.right.modelDistance, 142.0f);
                if (row == 2) edited |= DrawSideCombo("Model", "##ModelRight", properties.right.modelIndex, registry.models, 142.0f);
                if (row == 3) edited |= DrawSideCombo("Railing Model", "##RailingModelRight", properties.right.railingModelIndex, registry.railingModels, 142.0f);
                if (row == 4) edited |= DrawSideCheck("Invert Railing", "##InvertRailingRight", properties.right.invertRailing, 142.0f);
                if (row == 5) edited |= DrawSideCombo("Terrain Shader", "##TerrainShaderRight", properties.right.terrainShaderIndex, registry.terrainShaders, 142.0f);
                if (row == 6) edited |= DrawSideInt("Terrain Quads", "##TerrainQuadsRight", properties.right.terrainQuadCount, 142.0f);
                if (row == 7) edited |= DrawSideCombo("Terrain Profile", "##TerrainProfileRight", properties.right.terrainProfileIndex, registry.terrainProfiles, 142.0f);
            } else {
                const size_t rightVegetation = static_cast<size_t>(row - 8);
                const std::string label = "Vegetation " + std::to_string(rightVegetation + 1);
                const std::string id = "##VegetationRight" + std::to_string(rightVegetation);
                edited |= DrawSideCombo(label.c_str(), id.c_str(), properties.right.vegetationIndices[rightVegetation], registry.vegetation, 142.0f);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button("OK##roadProperties", ImVec2(72.0f, 0.0f))) applyAndClose = true;
    ImGui::SameLine(92.0f);
    if (ImGui::Button("Set as Defaults##roadProperties", ImVec2(140.0f, 0.0f)) && registry.defaultRoadProperties) {
        *registry.defaultRoadProperties = properties;
    }
    ImGui::SameLine(240.0f);
    if (ImGui::Button("Use Defaults##roadProperties", ImVec2(120.0f, 0.0f)) && registry.defaultRoadProperties) {
        properties = *registry.defaultRoadProperties;
        edited = true;
    }
    ImGui::SameLine(368.0f);
    if (ImGui::Button("Edit Signs##roadProperties", ImVec2(100.0f, 0.0f)) && registry.openSignEditor) {
        registry.openSignEditor(activeRoad);
    }
    ImGui::SameLine(476.0f);
    cancel = ImGui::Button("Cancel##roadProperties", ImVec2(72.0f, 0.0f));

    if (cancel) {
        properties = editSnapshot;
        TriggerRoadMeshRebuild(activeRoad, registry);
        snapshotRoadId = 0;
        if (registry.closeRoadPropertiesWindow) registry.closeRoadPropertiesWindow();
        return;
    }
    if (edited || applyAndClose) TriggerRoadMeshRebuild(activeRoad, registry);
    if (applyAndClose) {
        snapshotRoadId = 0;
        if (registry.closeRoadPropertiesWindow) registry.closeRoadPropertiesWindow();
    }
}

void EngineEditor::Update()
{
    ImGuiIO& io = ImGui::GetIO();
    m_cameraPipeline.Update(io.DeltaTime, m_camera.is3DMode);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_roadSystem.CancelRoadPlacement();
        m_editorMode = SELECT_MODE;
        if (m_activeMoveNodeId >= 0) m_roadSystem.SetNodeSelected(m_activeMoveNodeId, false);
        m_activeMoveNodeId = -1;
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_activeMoveNodeId >= 0) m_roadSystem.SetNodeSelected(m_activeMoveNodeId, false);
        m_activeMoveNodeId = -1;
    }

    // PROTEKSI CRITICAL: Abaikan shortcut jika user sedang mengetik di input field ImGui
    if (io.WantTextInput) return;

    if (ImGui::IsKeyPressed(ImGuiKey_M)) {
        if (m_editorMode == MOVE_MODE) {
            if (m_activeMoveNodeId >= 0) m_roadSystem.SetNodeSelected(m_activeMoveNodeId, false);
            m_activeMoveNodeId = -1;
            m_editorMode = SELECT_MODE;
        } else {
            m_roadSystem.CancelRoadPlacement();
            m_activeMoveNodeId = -1;
            m_editorMode = MOVE_MODE;
        }
    }

    // 1. Single Key Engine Shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) m_showMenuShortcut = !m_showMenuShortcut;
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) m_showMaterialDistanceReplacement = !m_showMaterialDistanceReplacement;
    if (ImGui::IsKeyPressed(ImGuiKey_C)) {
        m_camera.is3DMode = !m_camera.is3DMode;
        if (m_camera.is3DMode) {
            FocusCameraToFirstSpline(m_camera3D, m_roadSystem);
            m_cameraPipeline.FocusOnRoad(m_roadSystem);
            for (RoadSegment& road : m_roadSystem.GetRoads()) {
                if (road.meshVertices.empty()) m_roadSystem.GenerateRoadMesh(road);
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
        m_camera.mapMode = static_cast<MapRenderMode>((static_cast<int>(m_camera.mapMode) + 1) % MAP_MODE_COUNT);
        m_camera.is3DMode = m_camera.mapMode != MAP_HIDDEN;
        if (m_camera.is3DMode) {
            FocusCameraToFirstSpline(m_camera3D, m_roadSystem);
            m_cameraPipeline.FocusOnRoad(m_roadSystem);
            for (RoadSegment& road : m_roadSystem.GetRoads()) {
                if (road.meshVertices.empty()) m_roadSystem.GenerateRoadMesh(road);
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F7)) m_showRebuildProgress = !m_showRebuildProgress;
    if (ImGui::IsKeyPressed(ImGuiKey_F9)) m_showEditorSetting = !m_showEditorSetting;

    if (ImGui::IsKeyPressed(ImGuiKey_B))  m_showSelectStamp = !m_showSelectStamp;
    if (ImGui::IsKeyPressed(ImGuiKey_F))  m_showFind = !m_showFind;
    if (ImGui::IsKeyPressed(ImGuiKey_G))  m_showSignItemDefaultProperties = !m_showSignItemDefaultProperties;
    if (ImGui::IsKeyPressed(ImGuiKey_N))  m_showNodeProperties = !m_showNodeProperties;
    if (ImGui::IsKeyPressed(ImGuiKey_P))  m_showRoadItemProperties = !m_showRoadItemProperties;

    const float zoom = std::max(m_camera.zoom, 0.01f);
    const ImVec2 viewportPosition(m_viewportX, m_viewportY);
    const ImVec2 viewportSize(m_viewportWidth, m_viewportHeight);
    const ImVec2 mousePosition = io.MousePos;
    const bool mouseInViewport = mousePosition.x >= viewportPosition.x &&
        mousePosition.x <= viewportPosition.x + viewportSize.x &&
        mousePosition.y >= viewportPosition.y &&
        mousePosition.y <= viewportPosition.y + viewportSize.y;

    if (m_editorMode == MOVE_MODE && !m_camera.is3DMode && mouseInViewport &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Vector3D worldPosition(
            (mousePosition.x - viewportPosition.x - m_camera.x) / zoom,
            (mousePosition.y - viewportPosition.y - m_camera.y) / zoom,
            0.0f);
        m_activeMoveNodeId = m_roadSystem.FindClosestNode(worldPosition, 6.0f / zoom, -1);
        if (m_activeMoveNodeId >= 0) m_roadSystem.SetNodeSelected(m_activeMoveNodeId, true);
    }

    if (m_editorMode == MOVE_MODE && !m_camera.is3DMode && m_activeMoveNodeId >= 0 &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const Vector3D worldDelta(io.MouseDelta.x / zoom, io.MouseDelta.y / zoom, 0.0f);
        const int movedNodeId = m_roadSystem.TranslateNode(m_activeMoveNodeId, worldDelta);
        if (movedNodeId != m_activeMoveNodeId) {
            if (movedNodeId >= 0) m_roadSystem.SetNodeSelected(movedNodeId, false);
            m_activeMoveNodeId = -1;
        }
    }

    bool linkedPlacementDoubleClick = false;
    if (m_editorMode == ROAD_PLACEMENT_MODE && !m_camera.is3DMode && mouseInViewport &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const int activeNodeId = m_roadSystem.GetActivePlacementNodeId();
        const Vector3D worldPosition(
            (mousePosition.x - viewportPosition.x - m_camera.x) / zoom,
            (mousePosition.y - viewportPosition.y - m_camera.y) / zoom,
            0.0f);
        const int closestNodeId = activeNodeId >= 0
            ? m_roadSystem.FindClosestNode(worldPosition, 2.0f, activeNodeId)
            : -1;
        if (activeNodeId >= 0 && closestNodeId >= 0) {
            m_roadSystem.LinkNodes(activeNodeId, closestNodeId);
            m_editorMode = SELECT_MODE;
            linkedPlacementDoubleClick = true;
        }
    }

    if (!linkedPlacementDoubleClick) {
        m_roadSystem.HandleInput(m_camera, m_editorMode, viewportPosition, viewportSize);
        if (m_editorMode == ROAD_PLACEMENT_MODE &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            m_roadSystem.GetActivePlacementNodeId() < 0) {
            m_editorMode = SELECT_MODE;
        }
    }

    // Tombol 'I' untuk simulasi trigger Error Log
    if (ImGui::IsKeyPressed(ImGuiKey_I)) {
        m_showImportMBD = true;
        m_showEditorLog = true;
    }

    // 2. State Machine Sementara untuk Sequential Shortcuts (Ditekan bersamaan/berurutan)
    // Logika presisi deteksi sekuensial 1/2/3/4 + E akan disempurnakan di tahap berikutnya
    if (ImGui::IsKeyDown(ImGuiKey_1) && ImGui::IsKeyPressed(ImGuiKey_E)) {
        m_roadSystem.CancelRoadPlacement();
        m_activeMoveNodeId = -1;
        m_editorMode = ROAD_PLACEMENT_MODE;
    }
    if (ImGui::IsKeyDown(ImGuiKey_2) && ImGui::IsKeyPressed(ImGuiKey_E)) m_showShortcut2E = !m_showShortcut2E;
    if (ImGui::IsKeyDown(ImGuiKey_3) && ImGui::IsKeyPressed(ImGuiKey_E)) m_showShortcut3E = !m_showShortcut3E;
    if (ImGui::IsKeyDown(ImGuiKey_4) && ImGui::IsKeyPressed(ImGuiKey_E)) m_showShortcut4E = !m_showShortcut4E;
}

void EngineEditor::RenderUI(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
    float targetWidth, float targetHeight)
{
    // --- MENU BAR UTAMA (Dropdown Menu Terintegrasi) ---
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Import MBD", "I")) { m_showImportMBD = true; m_showEditorLog = true; }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {}
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows"))
        {
            ImGui::MenuItem("F1: Menu Shortcut Button", "F1", &m_showMenuShortcut);
            ImGui::MenuItem("F3: Material / Distance Replacement", "F3", &m_showMaterialDistanceReplacement);
            ImGui::MenuItem("Preview Map", nullptr, &m_showPreviewMap);
            ImGui::MenuItem("F7: Rebuild Progress", "F7", &m_showRebuildProgress);
            ImGui::MenuItem("F9: Editor Setting", "F9", &m_showEditorSetting);
            ImGui::Separator();
            ImGui::MenuItem("B: Select Stamp", "B", &m_showSelectStamp);
            ImGui::MenuItem("F: Find", "F", &m_showFind);
            ImGui::MenuItem("Editor Log", nullptr, &m_showEditorLog);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Properties"))
        {
            ImGui::MenuItem("G: Sign Item Default Properties", "G", &m_showSignItemDefaultProperties);
            ImGui::MenuItem("N: Node Properties", "N", &m_showNodeProperties);
            ImGui::MenuItem("P: Road Item Properties", "P", &m_showRoadItemProperties);
            ImGui::Separator();
            if (ImGui::MenuItem("1+E: Road Placement Mode", "1+E")) {
                m_roadSystem.CancelRoadPlacement();
                m_editorMode = ROAD_PLACEMENT_MODE;
            }
            ImGui::MenuItem("Road Item Default Properties", nullptr, &m_showShortcut1E);
            ImGui::MenuItem("2+E: Prefab Item Default Properties", "2+E", &m_showShortcut2E);
            ImGui::MenuItem("3+E: Buildings Item Road Properties", "3+E", &m_showShortcut3E);
            ImGui::MenuItem("4+E: Model Item Default Properties", "4+E", &m_showShortcut4E);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("MasterDockSpaceWindow", nullptr, window_flags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MyEngineDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    ImGui::Begin("Main Viewport Canvas", nullptr, ImGuiWindowFlags_NoScrollbar);
    const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 canvasEnd(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y);

    ImGuiIO& canvasIO = ImGui::GetIO();
    const bool mouseOverCanvas = canvasIO.MousePos.x >= canvasPosition.x &&
        canvasIO.MousePos.x <= canvasEnd.x &&
        canvasIO.MousePos.y >= canvasPosition.y &&
        canvasIO.MousePos.y <= canvasEnd.y;
    if (!m_camera.is3DMode && mouseOverCanvas && canvasIO.MouseWheel != 0.0f) {
        const float oldZoom = std::max(m_camera.zoom, 0.01f);
        const float newZoom = std::clamp(static_cast<float>(oldZoom * std::powf(1.1f, static_cast<float>(canvasIO.MouseWheel))), 0.1f, 8.0f);
        const float cursorX = canvasIO.MousePos.x - canvasPosition.x;
        const float cursorY = canvasIO.MousePos.y - canvasPosition.y;
        const float worldX = (cursorX - m_camera.x) / oldZoom;
        const float worldY = (cursorY - m_camera.y) / oldZoom;
        m_camera.zoom = newZoom;
        m_camera.x = cursorX - worldX * newZoom;
        m_camera.y = cursorY - worldY * newZoom;
    }

    m_viewportX = canvasPosition.x;
    m_viewportY = canvasPosition.y;
    m_viewportWidth = canvasSize.x;
    m_viewportHeight = canvasSize.y;

    if (!m_camera.is3DMode) {
        draw_list->AddRectFilled(canvasPosition, canvasEnd, IM_COL32(0x26, 0x26, 0x26, 0xFF));
    }

    if (!m_camera.is3DMode) {
        const float cellSize = 64.0f * std::max(m_camera.zoom, 0.01f);
        const ImU32 gridColor = IM_COL32(0x46, 0x5A, 0x46, 0xFF);
        float offsetX = std::fmod(m_camera.x, cellSize);
        float offsetY = std::fmod(m_camera.y, cellSize);
        if (offsetX < 0.0f) offsetX += cellSize;
        if (offsetY < 0.0f) offsetY += cellSize;

        draw_list->PushClipRect(canvasPosition, canvasEnd, true);
        for (float x = canvasPosition.x + offsetX; x <= canvasEnd.x; x += cellSize) {
            draw_list->AddLine(ImVec2(x, canvasPosition.y), ImVec2(x, canvasEnd.y), gridColor);
        }
        for (float y = canvasPosition.y + offsetY; y <= canvasEnd.y; y += cellSize) {
            draw_list->AddLine(ImVec2(canvasPosition.x, y), ImVec2(canvasEnd.x, y), gridColor);
        }
        draw_list->PopClipRect();

        draw_list->PushClipRect(canvasPosition, canvasEnd, true);
        m_roadSystem.RenderCanvas(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), m_camera);
        draw_list->PopClipRect();
    }
    else {
        const float aspectRatio = canvasSize.y > 0.0f ? canvasSize.x / canvasSize.y : 1.0f;
        const float skyBlue[4] = { 0.39f, 0.58f, 0.93f, 1.0f };
        const UINT viewportTextureWidth = static_cast<UINT>(canvasSize.x);
        const UINT viewportTextureHeight = static_cast<UINT>(canvasSize.y);
        if (context && target && canvasSize.x > 0.0f && canvasSize.y > 0.0f &&
            EnsureViewportRenderTarget(viewportTextureWidth, viewportTextureHeight)) {
            ID3D11RenderTargetView* viewportTarget = m_viewportRTV.Get();
            context->OMSetRenderTargets(1, &viewportTarget, nullptr);
            context->ClearRenderTargetView(viewportTarget, skyBlue);
            m_cameraPipeline.UpdatePipelineContext(context, canvasSize.x, canvasSize.y);
            for (RoadSegment& road : m_roadSystem.GetRoads()) {
                if (road.meshVertices.empty()) m_roadSystem.GenerateRoadMesh(road);
                m_roadSystem.UpdateRoadMeshBuffers(road, m_graphicsDevice, context);
            }

            RoadCameraConstants cameraConstants;
            cameraConstants.view = m_cameraPipeline.GetViewMatrix();
            cameraConstants.projection = m_cameraPipeline.GetProjectionMatrix(aspectRatio);
            m_roadPipeline.Render(context, viewportTarget, m_roadSystem.GetRoads(), cameraConstants,
                m_siiBridge.Assets().terrainShaders, m_assetVfs,
                0.0f, 0.0f, canvasSize.x, canvasSize.y, canvasSize.x, canvasSize.y);
            context->OMSetRenderTargets(1, &target, nullptr);
            ImGui::Image((ImTextureID)m_viewportSRV.Get(), canvasSize);
        }

    }

    if (m_camera.mapMode != MAP_HIDDEN) {
        const float desiredSize = m_camera.mapMode == MAP_CLOSE_RANGE ? 150.0f : 300.0f;
        constexpr float inset = 12.0f;
        const float availableSize = std::max(0.0f, std::min(canvasSize.x, canvasSize.y) - inset * 2.0f);
        const float minimapSize = std::min(desiredSize, availableSize);
        const ImVec2 minimapMin(canvasEnd.x - inset - minimapSize, canvasPosition.y + inset);
        const ImVec2 minimapMax(minimapMin.x + minimapSize, minimapMin.y + minimapSize);
        draw_list->AddRectFilled(minimapMin, minimapMax, IM_COL32(0x28, 0x28, 0x28, 0xE8));
        draw_list->AddRect(minimapMin, minimapMax, IM_COL32(0x12, 0x12, 0x12, 0xFF), 0.0f, 0, 2.0f);
    }

    ImGui::End();

    // --- ALOKASI WINDOWS KOSONG BERDASARKAN SHORTCUT ---
    if (m_showMenuShortcut) {
        ImGui::Begin("F1 Menu Shortcut Button", &m_showMenuShortcut);
        ImGui::Text("Data Kosong - Menu Kontrol Editor");
        ImGui::End();
    }
    if (m_showMaterialDistanceReplacement) {
        ImGui::Begin("F3 Material / Distance Replacement", &m_showMaterialDistanceReplacement);
        ImGui::Text("Data Kosong - Konfigurasi Material");
        ImGui::End();
    }
    if (m_showPreviewMap) {
        ImGui::Begin("F5 Preview Map", &m_showPreviewMap);
        ImGui::Text("Data Kosong - Map Viewport Preview");
        ImGui::End();
    }
    if (m_showRebuildProgress) {
        ImGui::Begin("F7 Rebuild Progress", &m_showRebuildProgress);
        ImGui::Text("Data Kosong - Progress Rekonstruksi Jalan...");
        ImGui::End();
    }
    if (m_showEditorSetting) {
        ImGui::Begin("F9 Editor Setting", &m_showEditorSetting);
        ImGui::Text("Data Kosong - Weather & Environment Options");
        ImGui::End();
    }
    if (m_showSelectStamp) {
        ImGui::Begin("B Select Stamp", &m_showSelectStamp);
        ImGui::Text("Data Kosong - Pilihan Stamp Aset");
        ImGui::End();
    }
    if (m_showFind) {
        ImGui::Begin("F Find", &m_showFind);
        ImGui::Text("Data Kosong - Pencarian Item");
        ImGui::End();
    }
    if (m_showSignItemDefaultProperties) {
        ImGui::Begin("G Sign Item Default Road Properties", &m_showSignItemDefaultProperties);
        ImGui::End();
    }
    if (m_showNodeProperties) {
        ImGui::Begin("N Node Properties", &m_showNodeProperties);
        ImGui::Text("Total Nodes: %d", (int)m_roadSystem.GetNodeCount());
        ImGui::End();
    }
    if (m_showRoadItemProperties) {
        const ImGuiStyle savedStyle = ImGui::GetStyle();
        ImGui::StyleColorsLight();
        ImGui::GetStyle().WindowRounding = 0.0f;
        ImGui::GetStyle().FrameRounding = 0.0f;
        ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_Always);
        const bool windowVisible = ImGui::Begin("P Road Item Properties", &m_showRoadItemProperties,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
        m_assetRegistry.closeRoadPropertiesWindow = [this]() { m_showRoadItemProperties = false; };
        if (windowVisible) {
            if (RoadSegment* activeRoad = m_roadSystem.GetRoad(0)) {
                m_assetRegistry.rebuildRoadMesh = [this, context](RoadSegment& road) {
                    const int materialIndex = road.properties.roadMaterialIndex;
                    const int roadTypeIndex = road.properties.roadTypeIndex;
                    for (RoadSegment& segment : m_roadSystem.GetRoads()) {
                        segment.properties.roadMaterialIndex = materialIndex;
                        segment.properties.roadTypeIndex = roadTypeIndex;
                        m_roadSystem.GenerateRoadMesh(segment);
                        if (context)
                            m_roadSystem.UpdateRoadMeshBuffers(segment, m_graphicsDevice, context);
                    }
                };
                m_assetRegistry.loadRoadMaterialTexture = [this](const std::string& materialPath) {
                    std::string textureError;
                    if (!LoadRoadMaterialTexture(materialPath, &textureError))
                        LogAssetInitializationMessage("[WARNING] Road material texture load failed: " + textureError);
                };
                RenderRoadItemPropertiesWindow(*activeRoad, m_assetRegistry, m_roadSystem);
            } else {
                ImGui::TextDisabled("Create a road segment to edit its properties.");
            }
        }
        ImGui::End();
        ImGui::GetStyle() = savedStyle;
    }
    if (m_showShortcut1E) {
        ImGui::Begin("1 E Road Item Default Properties", &m_showShortcut1E);
        ImGui::Text("Total Connected Roads: %d", (int)m_roadSystem.GetRoadCount());
        ImGui::End();
    }
    if (m_showShortcut2E) {
        ImGui::Begin("2 E Prefab Item Default Properties", &m_showShortcut2E);
        ImGui::End();
    }
    if (m_showShortcut3E) {
        ImGui::Begin("3 E untuk Buildings Item Road Properties", &m_showShortcut3E);
        ImGui::End();
    }
    if (m_showShortcut4E) {
        ImGui::Begin("4 E untuk Model Item Default Properties", &m_showShortcut4E);
        ImGui::End();
    }

    // --- ADDITIONAL WINDOW: EDITOR ERROR LOG ---
    if (m_showEditorLog) {
        ImGui::Begin("Editor Log", &m_showEditorLog);
        if (!m_assetInitializationLog.empty())
            ImGui::TextWrapped("%s", m_assetInitializationLog.c_str());
        if (m_showImportMBD) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[ERROR] Failed to load map data from .mbd file!");
        }
        else if (m_assetInitializationLog.empty()) {
            ImGui::Text("System Status: Ready. Press 'I' to simulate Save/Load Error Log.");
        }
        ImGui::End();
    }
}
