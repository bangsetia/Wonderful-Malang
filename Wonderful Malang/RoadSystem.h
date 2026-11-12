#pragma once
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "EngineMath.h"
#include <array>
#include <d3d11.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

class MapImporter;

// Struktur Data Data Lokal Jalan & Node
enum MapRenderMode {
    MAP_HIDDEN = 0,
    MAP_CLOSE_RANGE = 1,
    MAP_FAR_RANGE = 2,
    MAP_MODE_COUNT = 3
};

struct RoadLookData {
    std::string token;
    std::string name;
    float roadSizeLeft = 0.0f;
    float roadSizeRight = 0.0f;
};

struct RoadMaterialData {
    std::string path;
    std::string name;
};

struct TerrainProfileData {
    std::string name;
    std::vector<float> elevations;
    float coefficient = 0.0f;
};

struct RoadMeshVertex {
    Vector3D position;
    Vector3D normal;
    float u = 0.0f;
    float v = 0.0f;
};

enum EditorMode {
    SELECT_MODE,
    MOVE_MODE,
    ROAD_PLACEMENT_MODE
};

struct EngineCamera {
    float x = 0.0f;
    float y = 0.0f;
    float z = 100.0f;
    float pitch = -90.0f;
    float yaw = 0.0f;
    float zoom = 1.0f;
    bool is3DMode = false;
    MapRenderMode mapMode = MAP_HIDDEN;
};

// Struktur Data Data Titik Poligon Jalan untuk Hardware Pipeline
struct RoadVertex {
    Vector3D position;      // Titik tengah jalan (X,Y,Z)
    Vector3D leftBorder;    // Titik batas kiri jalan (X,Y,Z)
    Vector3D rightBorder;   // Titik batas kanan jalan (X,Y,Z)
};

struct RoadSideProperties {
    float heightOffset = 0.0f;
    float modelDistance = 0.0f;
    float width = 10.0f;
    int modelIndex = -1;
    int railingModelIndex = -1;
    bool invertRailing = false;
    int terrainShaderIndex = -1;
    int terrainQuadCount = 0;
    int terrainProfileIndex = -1;
    std::array<int, 3> vegetationIndices = { -1, -1, -1 };
};

struct RoadProperties {
    int roadTypeIndex = -1;
    int roadMaterialIndex = -1;
    bool leftHandTraffic = false;
    bool collisionEnabled = true;
    bool terrainCollisionEnabled = true;
    RoadSideProperties left;
    RoadSideProperties right;
};

struct RoadSegment {
    uint32_t id = 0;
    uint32_t startNodeId = 0;
    uint32_t endNodeId = 0;
    float width = 20.0f;
    RoadProperties properties;
    std::vector<RoadVertex> vertexCache;
    std::vector<RoadMeshVertex> meshVertices;
    std::vector<uint32_t> meshIndices;
    std::vector<RoadMeshVertex> leftTerrainVertices;
    std::vector<uint32_t> leftTerrainIndices;
    std::vector<RoadMeshVertex> rightTerrainVertices;
    std::vector<uint32_t> rightTerrainIndices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> leftTerrainVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> leftTerrainIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> rightTerrainVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> rightTerrainIndexBuffer;
    UINT vertexBufferCapacity = 0;
    UINT indexBufferCapacity = 0;
    UINT leftTerrainVertexBufferCapacity = 0;
    UINT leftTerrainIndexBufferCapacity = 0;
    UINT rightTerrainVertexBufferCapacity = 0;
    UINT rightTerrainIndexBufferCapacity = 0;
};

struct SiiAssetRegistry {
    std::vector<std::string> roadTypes;
    std::vector<std::string> roadMaterials;
    std::vector<std::string> terrainShaders;
    std::vector<std::string> terrainProfiles;
    std::vector<std::string> models;
    std::vector<std::string> railingModels;
    std::vector<std::string> vegetation;
    std::shared_ptr<RoadProperties> defaultRoadProperties = std::make_shared<RoadProperties>();
    std::function<void(RoadSegment&)> rebuildRoadMesh;
    std::function<void(const std::string&)> loadRoadMaterialTexture;
    std::function<void(RoadSegment&)> openSignEditor;
    std::function<void()> closeRoadPropertiesWindow;
};

struct MapNode {
    uint32_t id = 0;
    Vector3D position;
    Vector3D forwardControl;
    Vector3D backwardControl;
    float rotationY = 0.0f;
    bool isSelected = false;
};

// Komponen Terisolasi Khusus Sistem Jalan (Anti-Bloat)
class RoadSystem {
public:
    RoadSystem();
    ~RoadSystem();

    void HandleInput(EngineCamera& camera, EditorMode mode,
        const struct ImVec2& viewportPosition, const struct ImVec2& viewportSize);
    void CancelRoadPlacement();
    void RebuildRoadGeometry(RoadSegment& road);
    bool GenerateRoadMesh(RoadSegment& road, int sampleCount = 32);
    bool UpdateRoadMeshBuffers(RoadSegment& road, ID3D11Device* device,
        ID3D11DeviceContext* context);
    int FindClosestNode(const Vector3D& targetPos, float maxDistance, int excludeNodeId) const;
    int GetActivePlacementNodeId() const;
    void LinkNodes(int nodeIdA, int nodeIdB);
    int TranslateNode(int nodeId, const Vector3D& worldDelta);
    void SetNodeSelected(int nodeId, bool selected);
    void RenderCanvas(struct ImDrawList* draw_list, const struct ImVec2& window_pos, EngineCamera& camera);
    void LoadAssetDatabase(const std::filesystem::path& rootPath);
    void SetAssetDatabase(const std::vector<RoadLookData>& roadLooks,
        const std::vector<RoadMaterialData>& roadMaterials);
    void SetTerrainProfiles(const std::vector<TerrainProfileData>& terrainProfiles);

    // Antarmuka Eksternal untuk UI
    size_t GetNodeCount() const { return m_nodes.size(); }
    size_t GetRoadCount() const { return m_roads.size(); }
    const std::vector<MapNode>& GetNodes() const { return m_nodes; }
    std::vector<RoadSegment>& GetRoads() { return m_roads; }
    const std::vector<RoadSegment>& GetRoads() const { return m_roads; }
    const std::vector<RoadLookData>& GetRoadLooks() const { return m_roadLooks; }
    const std::vector<RoadMaterialData>& GetRoadMaterials() const { return m_roadMaterials; }
    RoadSegment* GetRoad(size_t index) { return index < m_roads.size() ? &m_roads[index] : nullptr; }
    const MapNode* GetNode(uint32_t nodeId) const;

private:
    friend class MapImporter;

    std::vector<MapNode> m_nodes;
    std::vector<RoadSegment> m_roads;
    std::vector<RoadLookData> m_roadLooks;
    std::vector<RoadMaterialData> m_roadMaterials;
    std::vector<TerrainProfileData> m_terrainProfiles;

    uint32_t m_nextNodeId = 1;
    uint32_t m_nextRoadId = 1;
    uint32_t m_selectedNodeId = 0;
    uint32_t m_placementStartNodeId = 0;
};
