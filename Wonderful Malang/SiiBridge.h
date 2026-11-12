#pragma once

#include "ZipVFS.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct SiiAssetCache {
	std::vector<RoadLookData> roadLooks;
	std::vector<RoadMaterialData> roadMaterialData;
	std::vector<std::string> roadProfiles;
	std::vector<std::string> roadMaterials;
	std::vector<std::string> terrainShaders;
	std::vector<std::string> railings;
	std::vector<std::string> models;
	std::vector<std::string> terrainProfiles;
	std::vector<TerrainProfileData> terrainProfileData;
	std::vector<std::string> vegetation;
};

enum class RoadCategory : int {
	Road = 0,
	CityRoad = 1,
	Terrain = 2
};

enum class RoadDistanceType : int {
	Near = 0,
	Medium = 1,
	Far = 2
};

enum class TipeVarian {
	Exclusive,
	Standard,
	Custom
};

struct VariabelBusSii {
	std::string namaKendaraan;
	TipeVarian varianAktif = TipeVarian::Exclusive;
};

struct RoadSideProfile {
	float heightOffset = 0.0f;
	bool shiftModels = false;
	float modelDistance = 0.0f;
	float modelOffset = 0.0f;
	int modelName = -1;
	bool invertRailing = false;
	float railingOffset = 0.0f;
	int railingName = -1;
	int terrainShader = -1;
	int quadCount = 0;
	float profileCoefficient = 1.0f;
	int terrainProfile = -1;
	std::array<int, 3> vegetation = { -1, -1, -1 };
	bool noDetailVegetation = false;
	bool vegetationCollision = false;
	int sidewalkMaterial = -1;
	int noisePower = 50;
	int transition = 8;
	float sidewalkSize = 0.0f;
};

struct RoadItemProperties {
	int roadType = -1;
	int roadMaterial = -1;
	RoadCategory category = RoadCategory::Road;
	bool halfRoadStep = false;
	bool noCollision = false;
	bool showInUiMap = true;
	bool noTraffic = false;
	bool noBoundary = false;
	bool noShadows = false;
	bool cracks = false;
	bool leftHandTraffic = false;
	RoadDistanceType distanceType = RoadDistanceType::Near;
	int centerMaterial = -1;
	int centerVegetation = -1;
	RoadSideProfile left;
	RoadSideProfile right;
};

class SiiBridge {
public:
	using TextLoader = std::function<bool(const std::string&, std::string&)>;

	SiiBridge() = default;

	bool Initialize(const std::string& basePath, const ZipVFS* zipVfs = nullptr,
		std::string* error = nullptr);
	bool Reload(std::string* error = nullptr);
	static bool BacaKonfigurasiBus(const std::string& pathSii, VariabelBusSii& dataOut);

	const SiiAssetCache& Assets() const noexcept { return m_assets; }
	const std::string& BasePath() const noexcept { return m_basePath; }
	bool IsInitialized() const noexcept { return m_initialized; }
	bool RoadFileLoaded() const noexcept { return m_roadFileLoaded; }
	std::size_t RoadFileSizeBytes() const noexcept { return m_roadFileSizeBytes; }

	static std::vector<std::string> Tokenize(std::string_view text);
	static std::vector<std::string> ExtractReferences(std::string_view text);

private:
	bool LoadText(const std::string& relativePath, std::string& text) const;
	bool ParseSiiFile(const std::string& relativePath, std::string& text, std::string* error) const;
	static void AddUnique(std::vector<std::string>& values, const std::string& value);
	static void AddFallback(std::vector<std::string>& values, const std::vector<std::string>& fallback);

	std::string m_basePath;
	const ZipVFS* m_zipVfs = nullptr;
	SiiAssetCache m_assets;
	bool m_initialized = false;
	bool m_roadFileLoaded = false;
	std::size_t m_roadFileSizeBytes = 0;
};
