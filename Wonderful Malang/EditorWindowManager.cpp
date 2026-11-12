// --- EditorWindowManager.cpp ---
#include "EditorWindowManager.h"
#include "RoadSystem.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {

const std::vector<std::string>& EmptyAssets() {
	static const std::vector<std::string> empty;
	return empty;
}

const char* AssetName(const std::vector<std::string>& assets, int index) {
	if (index >= 0 && static_cast<std::size_t>(index) < assets.size())
		return assets[static_cast<std::size_t>(index)].c_str();
	return "None";
}

bool AssetCombo(
	const char* label,
	int& index,
	const std::vector<std::string>& assets) {
	if (assets.empty()) {
		index = -1;
		ImGui::TextDisabled("%s: no assets", label);
		return false;
	}

	if (index < 0 || static_cast<std::size_t>(index) >= assets.size())
		index = 0;
	bool changed = false;
	if (ImGui::BeginCombo(label, AssetName(assets, index))) {
		for (int assetIndex = 0;
			assetIndex < static_cast<int>(assets.size());
			++assetIndex) {
			const bool selected = assetIndex == index;
			if (ImGui::Selectable(
				assets[static_cast<std::size_t>(assetIndex)].c_str(),
				selected)) {
				index = assetIndex;
				changed = true;
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	return changed;
}

void DrawAssetField(
	const char* label,
	int& value,
	const std::vector<std::string>& assets) {
	ImGui::TextUnformatted(label);
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(-1.0f);
	AssetCombo((std::string("##") + label).c_str(), value, assets);
}

void DrawSideProfile(
	const char* label,
	RoadSideProfile& side,
	const EngineContext& ctx) {
	const SiiAssetCache* assets = ctx.sii != nullptr ? &ctx.sii->Assets() : nullptr;
	const std::vector<std::string>& models = assets != nullptr ? assets->models : EmptyAssets();
	const std::vector<std::string>& railings = assets != nullptr ? assets->railings : EmptyAssets();
	const std::vector<std::string>& shaders = assets != nullptr ? assets->terrainShaders : EmptyAssets();
	const std::vector<std::string>& profiles = assets != nullptr ? assets->terrainProfiles : EmptyAssets();
	const std::vector<std::string>& vegetation = assets != nullptr ? assets->vegetation : EmptyAssets();
	const std::vector<std::string>& materials = assets != nullptr ? assets->roadMaterials : EmptyAssets();

	ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s Profile", label);
	ImGui::Separator();
	ImGui::InputFloat("Height offset", &side.heightOffset, 0.0f, 0.0f, "%.4f");
	ImGui::Checkbox("Shift models", &side.shiftModels);
	ImGui::InputFloat("Model distance", &side.modelDistance, 0.0f, 0.0f, "%.4f");
	ImGui::InputFloat("Model offset", &side.modelOffset, 0.0f, 0.0f, "%.4f");
	DrawAssetField("Model", side.modelName, models);
	ImGui::Checkbox("Invert railing", &side.invertRailing);
	ImGui::InputFloat("Railing offset", &side.railingOffset, 0.0f, 0.0f, "%.4f");
	DrawAssetField("Railing", side.railingName, railings);
	DrawAssetField("Terrain shader", side.terrainShader, shaders);
	ImGui::InputInt("Quad count", &side.quadCount);
	ImGui::InputFloat("Profile coefficient", &side.profileCoefficient, 0.0f, 0.0f, "%.4f");
	DrawAssetField("Terrain profile", side.terrainProfile, profiles);

	for (int index = 0; index < 3; ++index) {
		const std::string id = "Vegetation " + std::to_string(index + 1);
		DrawAssetField(id.c_str(), side.vegetation[static_cast<std::size_t>(index)], vegetation);
	}

	ImGui::Checkbox("No detail vegetation", &side.noDetailVegetation);
	ImGui::Checkbox("Vegetation collision", &side.vegetationCollision);
	DrawAssetField("Sidewalk material", side.sidewalkMaterial, materials);
	ImGui::SliderInt("Noise power", &side.noisePower, 0, 100);
	ImGui::InputInt("Transition", &side.transition);
	ImGui::InputFloat("Sidewalk size", &side.sidewalkSize, 0.0f, 0.0f, "%.4f");
}

bool ProjectPoint(
	const Vector3D& point,
	const Camera3D& camera,
	const ImVec2& origin,
	const ImVec2& size,
	ImVec2& output) {
	const float yaw = camera.yaw * 0.0174532925f;
	const float pitch = camera.pitch * 0.0174532925f;
	const float cosYaw = std::cos(yaw);
	const float sinYaw = std::sin(yaw);
	const float cosPitch = std::cos(pitch);
	const float sinPitch = std::sin(pitch);

	const float dx = point.x - camera.pos[0];
	const float dy = point.y - camera.pos[1];
	const float dz = point.z - camera.pos[2];
	const float cameraX = -sinYaw * dx + cosYaw * dz;
	const float depth = cosYaw * dx + sinYaw * dz;
	const float cameraY = cosPitch * dy - sinPitch * depth;
	const float cameraZ = sinPitch * dy + cosPitch * depth;

	if (cameraZ <= 0.05f)
		return false;

	const ImVec2 center(
		origin.x + size.x * 0.5f,
		origin.y + size.y * 0.5f);
	constexpr float focalLength = 800.0f;
	output = ImVec2(
		center.x + cameraX * focalLength / cameraZ,
		center.y - cameraY * focalLength / cameraZ);
	return true;
}

} // namespace

void EditorWindowManager::DrawWorkspace(EngineContext& ctx) {
	DrawFreeFlight3DViewport(ctx);
	if (ctx.roadPropertiesOpen)
		DrawRoadItemProperties(ctx);
}

void EditorWindowManager::DrawRoadItemProperties(EngineContext& ctx) {
	ImGui::SetNextWindowSize(ImVec2(760.0f, 700.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(
		"P - Road Item Properties",
		&ctx.roadPropertiesOpen,
		ImGuiWindowFlags_MenuBar)) {
		ImGui::End();
		return;
	}

	RoadItemProperties& properties = ctx.roadProperties;
	const SiiAssetCache* assets = ctx.sii != nullptr ? &ctx.sii->Assets() : nullptr;
	const std::vector<std::string>& roadProfiles = assets != nullptr ? assets->roadProfiles : EmptyAssets();
	const std::vector<std::string>& roadMaterials = assets != nullptr ? assets->roadMaterials : EmptyAssets();
	const std::vector<std::string>& vegetation = assets != nullptr ? assets->vegetation : EmptyAssets();

	ImGui::TextUnformatted("Road type / look");
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(-1.0f);
	AssetCombo("##RoadType", properties.roadType, roadProfiles);
	ImGui::TextUnformatted("Road material");
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(-1.0f);
	AssetCombo("##RoadMaterial", properties.roadMaterial, roadMaterials);

	int category = static_cast<int>(properties.category);
	ImGui::TextUnformatted("Category");
	ImGui::SameLine(150.0f);
	ImGui::RadioButton("Road", &category, 0);
	ImGui::SameLine();
	ImGui::RadioButton("City road", &category, 1);
	ImGui::SameLine();
	ImGui::RadioButton("Terrain", &category, 2);
	properties.category = static_cast<RoadCategory>(std::clamp(category, 0, 2));

	ImGui::Checkbox("Half road step", &properties.halfRoadStep);
	ImGui::SameLine();
	ImGui::Checkbox("No collision", &properties.noCollision);
	ImGui::SameLine();
	ImGui::Checkbox("Show in UI map", &properties.showInUiMap);
	ImGui::Checkbox("No traffic", &properties.noTraffic);
	ImGui::SameLine();
	ImGui::Checkbox("No boundary", &properties.noBoundary);
	ImGui::SameLine();
	ImGui::Checkbox("No shadows", &properties.noShadows);
	ImGui::Checkbox("Cracks", &properties.cracks);
	ImGui::SameLine();
	ImGui::Checkbox("Left-hand traffic", &properties.leftHandTraffic);

	int distance = static_cast<int>(properties.distanceType);
	const char* distanceNames[] = { "Near", "Medium", "Far" };
	ImGui::Combo("Distance type", &distance, distanceNames, IM_ARRAYSIZE(distanceNames));
	properties.distanceType = static_cast<RoadDistanceType>(std::clamp(distance, 0, 2));

	DrawAssetField("Center material", properties.centerMaterial, roadMaterials);
	DrawAssetField("Center vegetation", properties.centerVegetation, vegetation);

	if (ImGui::CollapsingHeader("Left side", ImGuiTreeNodeFlags_DefaultOpen))
		DrawSideProfile("Left", properties.left, ctx);
	if (ImGui::CollapsingHeader("Right side", ImGuiTreeNodeFlags_DefaultOpen))
		DrawSideProfile("Right", properties.right, ctx);

	ImGui::Separator();
	if (ImGui::Button("Accept", ImVec2(90.0f, 28.0f))) {
		if (ctx.onRoadPropertiesAccepted)
			ctx.onRoadPropertiesAccepted(properties);
		ctx.requestRoadRebuild = true;
		ctx.roadPropertiesOpen = false;
	}
	ImGui::SameLine();
	if (ImGui::Button("Set as Defaults", ImVec2(130.0f, 28.0f)))
		ctx.roadDefaults = properties;
	ImGui::SameLine();
	if (ImGui::Button("Use Defaults", ImVec2(110.0f, 28.0f)))
		properties = ctx.roadDefaults;
	ImGui::SameLine();
	if (ImGui::Button("Edit Signs", ImVec2(100.0f, 28.0f))) {
		ctx.requestEditSigns = true;
		if (ctx.onEditSigns)
			ctx.onEditSigns();
	}
	ImGui::SameLine();
	if (ImGui::Button("Remove Stamps", ImVec2(120.0f, 28.0f))) {
		ctx.requestRemoveStamps = true;
		if (ctx.onRemoveStamps)
			ctx.onRemoveStamps();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(90.0f, 28.0f)))
		ctx.roadPropertiesOpen = false;

	ImGui::End();
}

void DrawFreeFlight3DViewport(EngineContext& ctx) {
	if (!ImGui::Begin(
		"Main Viewport Canvas",
		nullptr,
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoMove)) {
		ImGui::End();
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImVec2 windowPosition = ImGui::GetWindowPos();
	const ImVec2 cursorStart = ImGui::GetCursorStartPos();
	const ImVec2 origin(
		windowPosition.x + cursorStart.x,
		windowPosition.y + cursorStart.y);
	const ImVec2 available = ImGui::GetContentRegionAvail();
	const ImVec2 size(
		std::max(available.x, 1.0f),
		std::max(available.y, 1.0f));
	const ImVec2 center(
		origin.x + size.x * 0.5f,
		origin.y + size.y * 0.5f);

	if (ImGui::IsWindowHovered())
		ImGui::SetWindowFocus();

	Camera3D& camera = ctx.camera3D;
	const bool flightActive =
		ImGui::IsWindowHovered() &&
		ImGui::IsMouseDown(ImGuiMouseButton_Right);
	camera.isFlightActive = flightActive;

	if (flightActive) {
		ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		camera.yaw += io.MouseDelta.x * 0.15f;
		camera.pitch = std::clamp(
			camera.pitch - io.MouseDelta.y * 0.15f,
			-89.0f,
			89.0f);

		const float yaw = camera.yaw * 0.0174532925f;
		const float pitch = camera.pitch * 0.0174532925f;
		const float forward[3] = {
			std::cos(pitch) * std::cos(yaw),
			std::sin(pitch),
			std::cos(pitch) * std::sin(yaw)
		};
		const float right[3] = {
			-std::sin(yaw),
			0.0f,
			std::cos(yaw)
		};
		const float speed = io.KeyShift ? 2.0f : 0.5f;

		if (ImGui::IsKeyDown(ImGuiKey_W)) {
			camera.pos[0] += forward[0] * speed;
			camera.pos[1] += forward[1] * speed;
			camera.pos[2] += forward[2] * speed;
		}
		if (ImGui::IsKeyDown(ImGuiKey_S)) {
			camera.pos[0] -= forward[0] * speed;
			camera.pos[1] -= forward[1] * speed;
			camera.pos[2] -= forward[2] * speed;
		}
		if (ImGui::IsKeyDown(ImGuiKey_A)) {
			camera.pos[0] -= right[0] * speed;
			camera.pos[2] -= right[2] * speed;
		}
		if (ImGui::IsKeyDown(ImGuiKey_D)) {
			camera.pos[0] += right[0] * speed;
			camera.pos[2] += right[2] * speed;
		}
	}

	drawList->AddRectFilled(
		origin,
		ImVec2(origin.x + size.x, origin.y + size.y),
		IM_COL32(18, 22, 30, 255));
	drawList->AddLine(
		ImVec2(origin.x, center.y),
		ImVec2(origin.x + size.x, center.y),
		IM_COL32(75, 85, 100, 180),
		1.0f);

	for (float x = -100.0f; x <= 100.0f; x += 20.0f) {
		const Vector3D start{ x, 0.0f, -100.0f };
		const Vector3D end{ x, 0.0f, 100.0f };
		ImVec2 screenStart;
		ImVec2 screenEnd;
		if (ProjectPoint(start, camera, origin, size, screenStart) &&
			ProjectPoint(end, camera, origin, size, screenEnd)) {
			drawList->AddLine(
				screenStart,
				screenEnd,
				IM_COL32(55, 65, 80, 160),
				1.0f);
		}
	}
	for (float z = -100.0f; z <= 100.0f; z += 20.0f) {
		const Vector3D start{ -100.0f, 0.0f, z };
		const Vector3D end{ 100.0f, 0.0f, z };
		ImVec2 screenStart;
		ImVec2 screenEnd;
		if (ProjectPoint(start, camera, origin, size, screenStart) &&
			ProjectPoint(end, camera, origin, size, screenEnd)) {
			drawList->AddLine(
				screenStart,
				screenEnd,
				IM_COL32(55, 65, 80, 160),
				1.0f);
		}
	}

	if (ctx.roadSys != nullptr) {
		for (std::size_t roadIndex = 0;
			roadIndex < ctx.roadSys->GetRoadCount();
			++roadIndex) {
			RoadSegment* road = ctx.roadSys->GetRoad(roadIndex);
			if (road == nullptr || road->vertexCache.size() < 2)
				continue;

			for (std::size_t vertexIndex = 0;
				vertexIndex + 1 < road->vertexCache.size();
				++vertexIndex) {
				const RoadVertex& first = road->vertexCache[vertexIndex];
				const RoadVertex& second = road->vertexCache[vertexIndex + 1];
				ImVec2 screenFirst;
				ImVec2 screenSecond;
				if (!ProjectPoint(first.position, camera, origin, size, screenFirst) ||
					!ProjectPoint(second.position, camera, origin, size, screenSecond))
					continue;

				drawList->AddLine(
					screenFirst,
					screenSecond,
					IM_COL32(70, 70, 70, 255),
					7.0f);
				drawList->AddLine(
					screenFirst,
					screenSecond,
					IM_COL32(75, 205, 85, 255),
					3.0f);
			}
		}
	}

	ImGui::SetCursorScreenPos(ImVec2(origin.x + 15.0f, origin.y + 15.0f));
	if (ImGui::Button("[ SNAP CAMERA TO SPLINE ROAD ]", ImVec2(260.0f, 30.0f)) &&
		ctx.roadSys != nullptr && ctx.roadSys->GetRoadCount() > 0) {
		RoadSegment* firstRoad = ctx.roadSys->GetRoad(0);
		if (firstRoad != nullptr && !firstRoad->vertexCache.empty()) {
			const Vector3D& firstPosition = firstRoad->vertexCache[0].position;
			camera.pos[0] = firstPosition.x;
			camera.pos[1] = firstPosition.y + 150.0f;
			camera.pos[2] = firstPosition.z - 250.0f;
			camera.yaw = 90.0f;
			camera.pitch = -35.0f;
			for (std::size_t roadIndex = 0;
				roadIndex < ctx.roadSys->GetRoadCount();
				++roadIndex) {
				RoadSegment* road = ctx.roadSys->GetRoad(roadIndex);
				if (road != nullptr)
					ctx.roadSys->RebuildRoadGeometry(*road);
			}
		}
	}

	ImGui::End();
}