#include "EngineEditor.h"
#include "imgui.h"
#include <Windows.h>
#include <filesystem>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>

void FocusCameraToFirstSpline(Camera3D& camera, const RoadSystem& roadSystem)
{
	const auto& nodes = roadSystem.GetNodes();
	if (nodes.empty()) return;

	const Vector3D firstNodePosition = nodes.front().position;
	camera.position.x = firstNodePosition.x;
	camera.position = Vector3D(firstNodePosition.x, 15.0f, firstNodePosition.y - 35.0f);
	const float directionX = firstNodePosition.x - camera.position.x;
	const float directionY = -camera.position.y;
	const float directionZ = firstNodePosition.y - camera.position.z;
	camera.yaw = std::atan2(directionX, directionZ);
	camera.pitch = std::atan2(directionY, std::sqrt(directionX * directionX + directionZ * directionZ));
}

bool EngineEditor::Initialize(ID3D11Device* device)
{
	m_graphicsDevice = device;
	m_assetInitializationLog.clear();
	const auto logAssetStatus = [this](const std::string& message) {
		if (!m_assetInitializationLog.empty())
			m_assetInitializationLog += '\n';
		m_assetInitializationLog += message;
		OutputDebugStringA(("[EngineEditor] " + message + "\n").c_str());
	};
	logAssetStatus("Asset initialization started; resolving the runtime asset root.");
	std::string error;
	if (!m_cameraPipeline.Initialize(device)) {
		logAssetStatus("[ERROR] Camera pipeline initialization failed.");
		return false;
	}
	m_roadPipeline.SetTextureLogCallback([this](const std::string& message) {
		LogAssetInitializationMessage(message);
	});
	if (!m_roadPipeline.Initialize(device, L".", &error)) {
		logAssetStatus("[ERROR] Road pipeline initialization failed" +
			(error.empty() ? std::string(".") : ": " + error));
		m_cameraPipeline.Shutdown();
		return false;
	}

	std::error_code currentPathError;
	const std::filesystem::path workingDirectory = std::filesystem::current_path(currentPathError);
	if (currentPathError) {
		logAssetStatus("[ERROR] Could not resolve the executable working directory: " + currentPathError.message());
		return false;
	}
	const std::array<std::filesystem::path, 6> assetCandidates = {
		std::filesystem::path("asset"),
		std::filesystem::path("../asset"),
		std::filesystem::path("../../asset"),
		std::filesystem::path("../../Wonderful Malang/asset"),
		std::filesystem::path("../../../Wonderful Malang/asset"),
		std::filesystem::path("C:/Users/boime/source/repos/Wonderful Malang/Wonderful Malang/asset")
	};
	std::filesystem::path assetPath;
	for (const std::filesystem::path& candidate : assetCandidates) {
		const std::filesystem::path candidatePath = workingDirectory / candidate;
		std::error_code existsError;
		if (std::filesystem::exists(candidatePath, existsError) && !existsError &&
			std::filesystem::is_directory(candidatePath, existsError) && !existsError) {
			assetPath = std::filesystem::weakly_canonical(candidatePath, existsError);
			if (existsError)
				assetPath = candidatePath.lexically_normal();
			break;
		}
	}
	for (const std::filesystem::path& candidate : assetCandidates) {
		const std::filesystem::path candidatePath = workingDirectory / candidate;
		std::error_code existsError;
		logAssetStatus("Asset path candidate " + candidatePath.string() + ": exists=" +
			(std::filesystem::exists(candidatePath, existsError) && !existsError ? "yes" : "no"));
	}
	const auto initializeArchive = [this, &error, &logAssetStatus](const std::filesystem::path& archivePath) {
		m_assetVfs.SetPhysicalRoot({});
		error.clear();
		if (!m_assetVfs.Mount(archivePath.string(), &error)) {
			logAssetStatus("[ERROR] Could not mount asset archive " + archivePath.string() +
				(error.empty() ? std::string() : ": " + error));
			return false;
		}
		logAssetStatus("Using packed assets from " + archivePath.string());
		error.clear();
		if (!m_siiBridge.Initialize(std::string(), &m_assetVfs, &error)) {
			logAssetStatus("[ERROR] SiiBridge initialization failed for the mounted archive" +
				(error.empty() ? std::string() : ": " + error));
			return false;
		}
		if (!error.empty())
			logAssetStatus("[WARNING] Asset cache loaded with missing or unreadable SII files: " + error);
		SynchronizeAssetCaches();
		const SiiAssetCache& assets = m_siiBridge.Assets();
		const std::size_t parsedItems = assets.roadLooks.size() + assets.roadMaterialData.size() +
			assets.models.size() + assets.railings.size() + assets.terrainShaders.size() +
			assets.terrainProfiles.size() + assets.vegetation.size();
		logAssetStatus("Loaded asset path: " + archivePath.string() +
			"; road.sii exists=" +
			(std::filesystem::exists(archivePath.parent_path() / "def/world/road.sii") ?
				std::string("yes") : std::string("no")) +
			"; road.sii opened=" + (m_siiBridge.RoadFileLoaded() ? std::string("yes") : std::string("no")) +
			", bytes=" + std::to_string(m_siiBridge.RoadFileSizeBytes()) +
			"; total parsed dropdown items=" + std::to_string(parsedItems) +
			"; files=" + std::to_string(m_assetVfs.ListFiles().size()) +
			" files; road looks=" + std::to_string(assets.roadLooks.size()) +
			", road materials=" + std::to_string(assets.roadMaterialData.size()) +
			", models=" + std::to_string(assets.models.size()) +
			", railings=" + std::to_string(assets.railings.size()) +
			", terrain shaders=" + std::to_string(assets.terrainShaders.size()) +
			", terrain profiles=" + std::to_string(assets.terrainProfiles.size()) +
			", vegetation=" + std::to_string(assets.vegetation.size()));
		return true;
	};

	if (!assetPath.empty()) {
		m_assetVfs.Unmount();
		m_assetVfs.SetPhysicalRoot(assetPath);
		logAssetStatus("Using loose assets from " + assetPath.string());
		error.clear();
		if (!m_siiBridge.Initialize(assetPath.string(), &m_assetVfs, &error)) {
			logAssetStatus("[ERROR] SiiBridge initialization failed for " + assetPath.string() +
				(error.empty() ? std::string() : ": " + error));
		} else {
			if (!error.empty())
				logAssetStatus("[WARNING] Asset cache loaded with missing or unreadable SII files: " + error);
			SynchronizeAssetCaches();
			const SiiAssetCache& assets = m_siiBridge.Assets();
			const std::size_t parsedItems = assets.roadLooks.size() + assets.roadMaterialData.size() +
				assets.models.size() + assets.railings.size() + assets.terrainShaders.size() +
				assets.terrainProfiles.size() + assets.vegetation.size();
			logAssetStatus("Loaded asset path: " + assetPath.string() +
				"; exists=yes; road.sii exists=" +
				(std::filesystem::exists(assetPath / "def/world/road.sii") ? std::string("yes") : std::string("no")) +
				", opened=" + (m_siiBridge.RoadFileLoaded() ? std::string("yes") : std::string("no")) +
				", bytes=" + std::to_string(m_siiBridge.RoadFileSizeBytes()) +
				"; total parsed dropdown items=" + std::to_string(parsedItems) +
				"; files=" + std::to_string(m_assetVfs.ListFiles().size()) +
				" files; road looks=" + std::to_string(assets.roadLooks.size()) +
				", road materials=" + std::to_string(assets.roadMaterialData.size()) +
				", models=" + std::to_string(assets.models.size()) +
				", railings=" + std::to_string(assets.railings.size()) +
				", terrain shaders=" + std::to_string(assets.terrainShaders.size()) +
				", terrain profiles=" + std::to_string(assets.terrainProfiles.size()) +
				", vegetation=" + std::to_string(assets.vegetation.size()));
			if (m_roadSystem.GetRoadLooks().empty() || m_roadSystem.GetRoadMaterials().empty())
				logAssetStatus("[ERROR] Road property dropdown source vectors are empty after cache synchronization.");
		}
	} else {
		logAssetStatus("No loose asset directory found in the configured runtime path candidates.");
		if (!initializeArchive(workingDirectory / "asset.zip"))
			logAssetStatus("[WARNING] Continuing startup without loaded asset definitions.");
	}

	std::string textureError;
	if (!LoadPreferredRoadMaterialTexture(&textureError) && !textureError.empty())
		logAssetStatus("[WARNING] Could not load the initial road texture: " + textureError);
	return true;
}

void EngineEditor::Shutdown()
{
	m_cameraPipeline.Shutdown();
	m_roadPipeline.Shutdown();
	m_viewportSRV.Reset();
	m_viewportRTV.Reset();
	m_viewportTexture.Reset();
	m_viewportTexWidth = 0;
	m_viewportTexHeight = 0;
	m_graphicsDevice = nullptr;
}

void EngineEditor::Render3DScene(ID3D11DeviceContext* context)
{
	if (!ImGui::Begin("3D Scene"))
	{
		ImGui::End();
		return;
	}

	const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
	m_viewportWidth = contentRegion.x > 0.0f ? contentRegion.x : 0.0f;
	m_viewportHeight = contentRegion.y > 0.0f ? contentRegion.y : 0.0f;

	const ImVec2 windowPosition = ImGui::GetWindowPos();
	m_viewportX = windowPosition.x;
	m_viewportY = windowPosition.y;

	m_cameraPipeline.UpdatePipelineContext(context, m_viewportWidth, m_viewportHeight);
	for (RoadSegment& road : m_roadSystem.GetRoads()) {
		m_roadSystem.UpdateRoadMeshBuffers(road, m_graphicsDevice, context);
	}

	ImGui::End();
}
