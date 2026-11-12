#pragma once
#include "EngineCameraPipeline.h"
#include "RoadRenderPipeline.h"
#include "RoadSystem.h"
#include "SiiBridge.h"
#include "ZipVFS.h"

#include <string>
#include <wrl/client.h>

void FocusCameraToFirstSpline(Camera3D& camera, const RoadSystem& roadSystem);

void RenderRoadItemPropertiesWindow(RoadSegment& activeRoad, const SiiAssetRegistry& registry,
    const RoadSystem& roadSystem);

// Deklarasi Kelas Utama UI Editor
class EngineEditor
{
public:
    bool Initialize(ID3D11Device* device);
    void Shutdown();
    void Render3DScene(ID3D11DeviceContext* context);
    void Update();
    void RenderUI(ID3D11DeviceContext* context = nullptr, ID3D11RenderTargetView* target = nullptr,
        float targetWidth = 0.0f, float targetHeight = 0.0f);
    bool LoadAssetArchive(const std::string& archivePath, std::string* error = nullptr);
    ZipVFS& GetAssetVFS() { return m_assetVfs; }
    SiiBridge& GetSiiBridge() { return m_siiBridge; }
    SiiAssetRegistry& GetAssetRegistry() { return m_assetRegistry; }

private:
    void SynchronizeAssetCaches();
    void LogAssetInitializationMessage(const std::string& message);
    bool LoadRoadMaterialTexture(const std::string& materialPath, std::string* error = nullptr);
    bool LoadPreferredRoadMaterialTexture(std::string* error = nullptr);
    bool EnsureViewportRenderTarget(UINT width, UINT height);

    EngineCameraPipeline m_cameraPipeline;
    RoadRenderPipeline m_roadPipeline;
    ID3D11Device* m_graphicsDevice = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_viewportTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_viewportRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_viewportSRV;
    UINT m_viewportTexWidth = 0;
    UINT m_viewportTexHeight = 0;

    // Instansiasi State Kamera Global
    EngineCamera m_camera;
    Camera3D m_camera3D;
    EditorMode m_editorMode = SELECT_MODE;
    int m_activeMoveNodeId = -1;

    // Koordinat internal Viewport untuk proteksi koordinat
    float m_viewportX = 0.0f;
    float m_viewportY = 0.0f;
    float m_viewportWidth = 0.0f;
    float m_viewportHeight = 0.0f;

    // State Machine Sekuensial Shortcut (1/2/3/4 lalu E)
    int m_pendingNumberKey = 0;
    float m_sequenceTimer = 0.0f;

    // State Buka/Tutup Jendela UI ImGui
    bool m_showMenuShortcut = false;
    bool m_showMaterialDistanceReplacement = false;
    bool m_showPreviewMap = false;
    bool m_showRebuildProgress = false;
    bool m_showEditorSetting = false;
    bool m_showSelectStamp = false;
    bool m_showFind = false;
    bool m_showSignItemDefaultProperties = false;
    bool m_showImportMBD = false;
    bool m_showNodeProperties = false;
    bool m_showRoadItemProperties = false;
    bool m_showShortcut1E = false;
    bool m_showShortcut2E = false;
    bool m_showShortcut3E = false;
    bool m_showShortcut4E = false;
    bool m_showEditorLog = true; // Buka otomatis saat startup untuk deteksi error
    // Database Penampung Seluruh Node di Peta
    // Pemasangan Komponen Mandiri RoadSystem
// Instansiasi Modul Mandiri (Micro-Component)
    RoadSystem m_roadSystem;
    SiiAssetRegistry m_assetRegistry;
    ZipVFS m_assetVfs;
    SiiBridge m_siiBridge;
    std::string m_assetInitializationLog;
    std::string m_loadedRoadMaterialPath;



};
