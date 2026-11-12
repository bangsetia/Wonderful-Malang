#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <string>
#include <vector>
#include <d3d11.h>
#include <directxmath.h>
#include "RoadSystem.h"

// Struktur Vertex standar untuk DirectX 11
struct MapVertex {
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT4 Color;
};

// Kelas Importer Peta ala Engine Editor
class MapImporter {
private:
    ID3D11Device* m_pd3dDevice;

    // Titik acuan jangkar (Anchor GPS Alun-Alun Malang sebagai koordinat 0,0,0 game)
    const double MAP_CENTER_LAT = -7.9826;
    const double MAP_CENTER_LON = 112.6308;

    // Fungsi matematika internal untuk mengubah Lat/Lon GPS ke X,Z Dunia Game
    void ConvertGPSToWorld(double lat, double lon, float& outX, float& outZ);

public:
    MapImporter(ID3D11Device* device);
    ~MapImporter();

    // Fungsi utama untuk mengimpor file .shp jalan raya
    bool ImportRoadShapefile(const std::string& shpPath,
        std::vector<MapVertex>& outVertexBufferData,
        std::vector<unsigned int>& outIndexBufferData);

    bool SaveToMBD(const std::string& filePath, const RoadSystem& roadSystem) const;
    bool LoadFromMBD(const std::string& filePath, RoadSystem& roadSystem) const;
};
