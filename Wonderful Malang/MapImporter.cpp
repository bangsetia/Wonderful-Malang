#include "MapImporter.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

MapImporter::MapImporter(ID3D11Device* device) : m_pd3dDevice(device) {}

MapImporter::~MapImporter() {}

namespace {
constexpr char kMbdSignature[4] = { 'M', 'B', 'D', 'X' };
constexpr uint32_t kMbdVersion = 1;
constexpr uint32_t kMaxSerializedItems = 1000000;
constexpr int32_t kMaxTerrainQuads = 4096;

template <typename T>
bool WriteValue(std::ofstream& file, const T& value)
{
    file.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return file.good();
}

template <typename T>
bool ReadValue(std::ifstream& file, T& value)
{
    file.read(reinterpret_cast<char*>(&value), sizeof(T));
    return file.good();
}

bool WriteVector(std::ofstream& file, const Vector3D& value)
{
    return WriteValue(file, value.x) && WriteValue(file, value.y) && WriteValue(file, value.z);
}

bool ReadVector(std::ifstream& file, Vector3D& value)
{
    return ReadValue(file, value.x) && ReadValue(file, value.y) && ReadValue(file, value.z);
}

bool ValidVector(const Vector3D& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool WriteSide(std::ofstream& file, const RoadSideProperties& side)
{
    const uint8_t invertRailing = side.invertRailing ? 1u : 0u;
    return WriteValue(file, side.heightOffset) &&
        WriteValue(file, side.modelDistance) &&
        WriteValue(file, side.width) &&
        WriteValue(file, side.modelIndex) &&
        WriteValue(file, side.railingModelIndex) &&
        WriteValue(file, invertRailing) &&
        WriteValue(file, side.terrainShaderIndex) &&
        WriteValue(file, side.terrainQuadCount) &&
        WriteValue(file, side.terrainProfileIndex) &&
        WriteValue(file, side.vegetationIndices[0]) &&
        WriteValue(file, side.vegetationIndices[1]) &&
        WriteValue(file, side.vegetationIndices[2]);
}

bool ReadSide(std::ifstream& file, RoadSideProperties& side)
{
    uint8_t invertRailing = 0;
    if (!ReadValue(file, side.heightOffset) ||
        !ReadValue(file, side.modelDistance) ||
        !ReadValue(file, side.width) ||
        !ReadValue(file, side.modelIndex) ||
        !ReadValue(file, side.railingModelIndex) ||
        !ReadValue(file, invertRailing) ||
        !ReadValue(file, side.terrainShaderIndex) ||
        !ReadValue(file, side.terrainQuadCount) ||
        !ReadValue(file, side.terrainProfileIndex) ||
        !ReadValue(file, side.vegetationIndices[0]) ||
        !ReadValue(file, side.vegetationIndices[1]) ||
        !ReadValue(file, side.vegetationIndices[2])) return false;
    if (invertRailing > 1u) return false;
    side.invertRailing = invertRailing != 0u;
    return std::isfinite(side.heightOffset) && std::isfinite(side.modelDistance) &&
        std::isfinite(side.width) && side.width >= 0.0f &&
        side.terrainQuadCount >= 0 && side.terrainQuadCount <= kMaxTerrainQuads;
}

bool WriteRoadProperties(std::ofstream& file, const RoadProperties& properties)
{
    const uint8_t flags = static_cast<uint8_t>((properties.leftHandTraffic ? 1u : 0u) |
        (properties.collisionEnabled ? 2u : 0u) |
        (properties.terrainCollisionEnabled ? 4u : 0u));
    return WriteValue(file, properties.roadTypeIndex) &&
        WriteValue(file, properties.roadMaterialIndex) &&
        WriteValue(file, flags) &&
        WriteSide(file, properties.left) && WriteSide(file, properties.right);
}

bool ReadRoadProperties(std::ifstream& file, RoadProperties& properties)
{
    uint8_t flags = 0;
    if (!ReadValue(file, properties.roadTypeIndex) ||
        !ReadValue(file, properties.roadMaterialIndex) ||
        !ReadValue(file, flags) || !ReadSide(file, properties.left) ||
        !ReadSide(file, properties.right)) return false;
    if ((flags & ~7u) != 0u) return false;
    properties.leftHandTraffic = (flags & 1u) != 0u;
    properties.collisionEnabled = (flags & 2u) != 0u;
    properties.terrainCollisionEnabled = (flags & 4u) != 0u;
    return true;
}

bool ValidRoad(const RoadSegment& road)
{
    return road.id != 0 && road.id != std::numeric_limits<uint32_t>::max() &&
        road.startNodeId != 0 && road.endNodeId != 0 &&
        road.startNodeId != std::numeric_limits<uint32_t>::max() &&
        road.endNodeId != std::numeric_limits<uint32_t>::max() &&
        std::isfinite(road.width) && road.width > 0.0f;
}

bool ValidSide(const RoadSideProperties& side)
{
    return std::isfinite(side.heightOffset) && std::isfinite(side.modelDistance) &&
        std::isfinite(side.width) && side.width >= 0.0f &&
        side.terrainQuadCount >= 0 && side.terrainQuadCount <= kMaxTerrainQuads;
}
}

bool MapImporter::SaveToMBD(const std::string& filePath, const RoadSystem& roadSystem) const
{
    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;

    const auto& nodes = roadSystem.GetNodes();
    const auto& roads = roadSystem.GetRoads();
    if (nodes.size() > kMaxSerializedItems || roads.size() > kMaxSerializedItems) return false;
    const uint32_t nodeCount = static_cast<uint32_t>(nodes.size());
    const uint32_t roadCount = static_cast<uint32_t>(roads.size());

    if (!file.write(kMbdSignature, sizeof(kMbdSignature)).good() ||
        !WriteValue(file, kMbdVersion) || !WriteValue(file, nodeCount) || !WriteValue(file, roadCount)) return false;

    for (const MapNode& node : nodes) {
        if (node.id == 0 || node.id == std::numeric_limits<uint32_t>::max() ||
            !ValidVector(node.position) || !ValidVector(node.forwardControl) ||
            !ValidVector(node.backwardControl) || !std::isfinite(node.rotationY)) return false;
        if (!WriteValue(file, node.id) || !WriteVector(file, node.position) ||
            !WriteVector(file, node.forwardControl) || !WriteVector(file, node.backwardControl) ||
            !WriteValue(file, node.rotationY)) return false;
    }

    for (const RoadSegment& road : roads) {
        if (!ValidRoad(road) || !ValidSide(road.properties.left) || !ValidSide(road.properties.right) ||
            roadSystem.GetNode(road.startNodeId) == nullptr || roadSystem.GetNode(road.endNodeId) == nullptr ||
            !WriteValue(file, road.id) ||
            !WriteValue(file, road.startNodeId) || !WriteValue(file, road.endNodeId) ||
            !WriteValue(file, road.width) || !WriteRoadProperties(file, road.properties)) return false;
    }
    return file.good();
}

bool MapImporter::LoadFromMBD(const std::string& filePath, RoadSystem& roadSystem) const
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    char signature[sizeof(kMbdSignature)] = {};
    uint32_t version = 0;
    uint32_t nodeCount = 0;
    uint32_t roadCount = 0;
    if (!file.read(signature, sizeof(signature)).good() ||
        !ReadValue(file, version) || !ReadValue(file, nodeCount) || !ReadValue(file, roadCount) ||
        !std::equal(std::begin(signature), std::end(signature), std::begin(kMbdSignature)) ||
        version != kMbdVersion || nodeCount > kMaxSerializedItems || roadCount > kMaxSerializedItems) return false;

    try {
        std::vector<MapNode> nodes;
        std::vector<RoadSegment> roads;
        nodes.resize(nodeCount);
        roads.resize(roadCount);
        for (MapNode& node : nodes) {
            if (!ReadValue(file, node.id) || !ReadVector(file, node.position) ||
                !ReadVector(file, node.forwardControl) || !ReadVector(file, node.backwardControl) ||
                !ReadValue(file, node.rotationY) || node.id == 0 || !ValidVector(node.position) ||
                !ValidVector(node.forwardControl) || !ValidVector(node.backwardControl) ||
                !std::isfinite(node.rotationY) || node.id == std::numeric_limits<uint32_t>::max()) return false;
        }
        for (size_t index = 0; index < nodes.size(); ++index) {
            for (size_t other = index + 1; other < nodes.size(); ++other) {
                if (nodes[index].id == nodes[other].id) return false;
            }
        }
        for (RoadSegment& road : roads) {
            if (!ReadValue(file, road.id) || !ReadValue(file, road.startNodeId) ||
                !ReadValue(file, road.endNodeId) || !ReadValue(file, road.width) ||
                !ReadRoadProperties(file, road.properties) || !ValidRoad(road)) return false;
            bool startExists = false;
            bool endExists = false;
            for (const MapNode& node : nodes) {
                startExists |= node.id == road.startNodeId;
                endExists |= node.id == road.endNodeId;
            }
            if (!startExists || !endExists) return false;
        }
        if (!file.good()) return false;

        roadSystem.m_nodes = std::move(nodes);
        roadSystem.m_roads = std::move(roads);
        roadSystem.m_selectedNodeId = 0;
        roadSystem.m_placementStartNodeId = 0;
        roadSystem.m_nextNodeId = 1;
        roadSystem.m_nextRoadId = 1;
        for (RoadSegment& road : roadSystem.m_roads) {
            roadSystem.m_nextRoadId = std::max(roadSystem.m_nextRoadId, road.id + 1);
            roadSystem.RebuildRoadGeometry(road);
        }
        for (const MapNode& node : roadSystem.m_nodes) {
            roadSystem.m_nextNodeId = std::max(roadSystem.m_nextNodeId, node.id + 1);
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

// Proyeksi Mercator Sederhana untuk konversi koordinat Bumi ke 3D Dunia Game
void MapImporter::ConvertGPSToWorld(double lat, double lon, float& outX, float& outZ) {
    const double EarthRadius = 6378137.0; // Radius bumi dalam meter

    // Hitung posisi relatif terhadap titik tengah Alun-Alun Malang
    double originX = EarthRadius * MAP_CENTER_LON * (M_PI / 180.0);
    double originZ = EarthRadius * log(tan((90.0 + MAP_CENTER_LAT) * (M_PI / 360.0)));

    double currentX = EarthRadius * lon * (M_PI / 180.0);
    double currentZ = EarthRadius * log(tan((90.0 + lat) * (M_PI / 360.0)));

    // Output koordinat meter dalam game (Skala 1:1 meter)
    outX = static_cast<float>(currentX - originX);
    outZ = static_cast<float>(currentZ - originZ);
}

bool MapImporter::ImportRoadShapefile(const std::string& shpPath,
    std::vector<MapVertex>& outVertexBufferData,
    std::vector<unsigned int>& outIndexBufferData)
{
    // Membuka file biner SHP Data Peta Jawa
    std::ifstream file(shpPath, std::ios::binary);
    if (!file.is_open()) {
        return false; // File tidak ditemukan atau gagal diload
    }

    // [LOGIKAPARSER] Membaca Header berkas ESRI Shapefile
    // Secara spesifik membaca record geometri jalan (LineString/PolyLine)
    unsigned int vertexIndexOffset = 0;

    // Simulasi pembacaan data koordinat record jalan
    // AI lokal Anda (Qwen 1.5B) nanti akan mengisi loop ekstraksi biner GDAL di sini

    file.close();
    return true;
}
