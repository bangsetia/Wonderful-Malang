#include "RoadSystem.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>

namespace {

std::string_view Trim(std::string_view value)
{
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
		value.remove_prefix(1);
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
		value.remove_suffix(1);
	return value;
}

std::string_view WithoutComment(std::string_view line)
{
	bool quoted = false;
	for (size_t index = 0; index < line.size(); ++index) {
		if (line[index] == '"')
			quoted = !quoted;
		else if (line[index] == '#' && !quoted)
			return line.substr(0, index);
	}
	return line;
}

std::string_view QuotedValue(std::string_view value)
{
	const size_t firstQuote = value.find('"');
	if (firstQuote == std::string_view::npos)
		return {};
	const size_t lastQuote = value.find('"', firstQuote + 1);
	if (lastQuote == std::string_view::npos)
		return {};
	return value.substr(firstQuote + 1, lastQuote - firstQuote - 1);
}

} // namespace

RoadSystem::RoadSystem()
	: m_nextNodeId(1), m_nextRoadId(1), m_selectedNodeId(0)
{
}

RoadSystem::~RoadSystem() = default;

void RoadSystem::LoadAssetDatabase(const std::filesystem::path& rootPath)
{
	const std::filesystem::path assetPath = rootPath / "def" / "world" / "road.sii";
	std::ifstream input(assetPath, std::ios::binary);
	if (!input) {
		m_roadLooks.clear();
		m_roadMaterials.clear();
		return;
	}

	const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	std::vector<RoadLookData> roadLooks;
	std::vector<RoadMaterialData> roadMaterials;
	RoadLookData currentLook;
	bool inRoadLook = false;
	const auto finishRoadLook = [&]() {
		if (inRoadLook && !currentLook.token.empty() && !currentLook.name.empty())
			roadLooks.push_back(std::move(currentLook));
		currentLook = {};
		inRoadLook = false;
	};

	for (size_t lineStart = 0; lineStart < text.size();) {
		const size_t lineEnd = text.find('\n', lineStart);
		const size_t length = lineEnd == std::string::npos ? text.size() - lineStart : lineEnd - lineStart;
		const std::string_view line = Trim(WithoutComment(std::string_view(text).substr(lineStart, length)));
		lineStart = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
		if (line.empty())
			continue;
		if (inRoadLook && line.find('}') != std::string_view::npos &&
			line.find(':') == std::string_view::npos) {
			finishRoadLook();
			continue;
		}

		const size_t colon = line.find(':');
		if (colon == std::string_view::npos)
			continue;
		const std::string_view key = Trim(line.substr(0, colon));
		const std::string_view value = Trim(line.substr(colon + 1));

		if (key.size() > 14 && key.substr(0, 14) == "road_material[") {
			const std::string_view path = QuotedValue(value);
			if (!path.empty())
				roadMaterials.push_back({ std::string(path) });
			continue;
		}

		if (key == "road_look" && line.find('{', colon + 1) != std::string_view::npos) {
			finishRoadLook();
			const size_t blockStart = value.find('{');
			const std::string_view token = Trim(value.substr(0, blockStart));
			if (!token.empty()) {
				currentLook.token.assign(token);
				inRoadLook = true;
			}
			continue;
		}

		if (inRoadLook && key == "name") {
			const std::string_view name = QuotedValue(value);
			if (!name.empty())
				currentLook.name.assign(name);
		}
		if (inRoadLook && line.find('}') != std::string_view::npos)
			finishRoadLook();
	}
	finishRoadLook();

	m_roadLooks = std::move(roadLooks);
	m_roadMaterials = std::move(roadMaterials);
}

void RoadSystem::SetAssetDatabase(const std::vector<RoadLookData>& roadLooks,
	const std::vector<RoadMaterialData>& roadMaterials)
{
	m_roadLooks = roadLooks;
	m_roadMaterials = roadMaterials;
}

void RoadSystem::SetTerrainProfiles(const std::vector<TerrainProfileData>& terrainProfiles)
{
	m_terrainProfiles = terrainProfiles;
}

const MapNode* RoadSystem::GetNode(uint32_t nodeId) const
{
	for (const MapNode& node : m_nodes) {
		if (node.id == nodeId) return &node;
	}
	return nullptr;
}

void RoadSystem::RebuildRoadGeometry(RoadSegment& road)
{
	road.vertexCache.clear();
	const MapNode* start = nullptr;
	const MapNode* end = nullptr;
	for (const MapNode& node : m_nodes) {
		if (node.id == road.startNodeId) start = &node;
		if (node.id == road.endNodeId) end = &node;
	}
	if (!start || !end) return;

	constexpr int steps = 20;
	const float halfWidth = road.width * 0.5f;
	road.vertexCache.reserve(steps + 1);
	for (int i = 0; i <= steps; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(steps);
		const float beforeT = static_cast<float>(std::max(i - 1, 0)) / static_cast<float>(steps);
		const float afterT = static_cast<float>(std::min(i + 1, steps)) / static_cast<float>(steps);
		const Vector3D mapCenter = EngineMath::InterpolateSpline(
			start->position, start->rotationY, end->position, end->rotationY, t);
		const Vector3D mapBefore = EngineMath::InterpolateSpline(
			start->position, start->rotationY, end->position, end->rotationY, beforeT);
		const Vector3D mapAfter = EngineMath::InterpolateSpline(
			start->position, start->rotationY, end->position, end->rotationY, afterT);
		const Vector3D center(mapCenter.x, mapCenter.z, mapCenter.y);
		const Vector3D before(mapBefore.x, mapBefore.z, mapBefore.y);
		const Vector3D after(mapAfter.x, mapAfter.z, mapAfter.y);
		const float tangentX = after.x - before.x;
		const float tangentZ = after.z - before.z;
		const float tangentLength = std::sqrt(tangentX * tangentX + tangentZ * tangentZ);
		const float normalX = tangentLength > 0.0001f ? -tangentZ / tangentLength : 0.0f;
		const float normalZ = tangentLength > 0.0001f ? tangentX / tangentLength : 0.0f;
		RoadVertex vertex;
		vertex.position = center;
		vertex.leftBorder = Vector3D(center.x + normalX * halfWidth, center.y, center.z + normalZ * halfWidth);
		vertex.rightBorder = Vector3D(center.x - normalX * halfWidth, center.y, center.z - normalZ * halfWidth);
		road.vertexCache.push_back(vertex);
	}
	GenerateRoadMesh(road);
}

void RoadSystem::CancelRoadPlacement()
{
	m_selectedNodeId = 0;
	m_placementStartNodeId = 0;
	for (MapNode& node : m_nodes) node.isSelected = false;
}

int RoadSystem::FindClosestNode(const Vector3D& targetPos, float maxDistance, int excludeNodeId) const
{
	if (maxDistance < 0.0f) return -1;
	const MapNode* closest = nullptr;
	float closestDistance = maxDistance;
	for (const MapNode& node : m_nodes) {
		if (excludeNodeId >= 0 && node.id == static_cast<uint32_t>(excludeNodeId)) continue;
		if (node.id > static_cast<uint32_t>(std::numeric_limits<int>::max())) continue;
		const float distance = targetPos.DistanceTo(node.position);
		if (distance <= closestDistance) {
			closest = &node;
			closestDistance = distance;
		}
	}
	return closest ? static_cast<int>(closest->id) : -1;
}

int RoadSystem::GetActivePlacementNodeId() const
{
	return m_selectedNodeId > 0 && m_selectedNodeId <= static_cast<uint32_t>(std::numeric_limits<int>::max())
		? static_cast<int>(m_selectedNodeId)
		: -1;
}

void RoadSystem::LinkNodes(int nodeIdA, int nodeIdB)
{
	if (nodeIdA < 0 || nodeIdB < 0 || nodeIdA == nodeIdB) return;
	const uint32_t removedId = static_cast<uint32_t>(nodeIdA);
	const uint32_t keptId = static_cast<uint32_t>(nodeIdB);
	MapNode* removed = nullptr;
	MapNode* kept = nullptr;
	for (MapNode& node : m_nodes) {
		if (node.id == removedId) removed = &node;
		if (node.id == keptId) kept = &node;
	}
	if (!removed || !kept) return;

	const float angleA = removed->rotationY * 0.017453292519943295f;
	const float angleB = kept->rotationY * 0.017453292519943295f;
	const float meanX = std::cos(angleA) + std::cos(angleB);
	const float meanY = std::sin(angleA) + std::sin(angleB);
	if (std::abs(meanX) + std::abs(meanY) > 0.0001f) {
		kept->rotationY = std::atan2(meanY, meanX) * 57.29577951308232f;
	}

	for (auto it = m_roads.begin(); it != m_roads.end();) {
		bool changed = false;
		if (it->startNodeId == removedId) {
			it->startNodeId = keptId;
			changed = true;
		}
		if (it->endNodeId == removedId) {
			it->endNodeId = keptId;
			changed = true;
		}
		if (it->startNodeId == it->endNodeId) {
			it = m_roads.erase(it);
		} else {
			if (changed || it->startNodeId == keptId || it->endNodeId == keptId) {
				RebuildRoadGeometry(*it);
			}
			++it;
		}
	}
	for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
		if (it->id == removedId) {
			m_nodes.erase(it);
			break;
		}
	}
	if (m_placementStartNodeId == removedId) m_placementStartNodeId = keptId;
	if (m_selectedNodeId == removedId || m_selectedNodeId == keptId) {
		m_selectedNodeId = 0;
		m_placementStartNodeId = 0;
	}
}

int RoadSystem::TranslateNode(int nodeId, const Vector3D& worldDelta)
{
	if (nodeId < 0) return -1;
	const uint32_t id = static_cast<uint32_t>(nodeId);
	MapNode* moved = nullptr;
	for (MapNode& node : m_nodes) {
		if (node.id == id) {
			moved = &node;
			break;
		}
	}
	if (!moved) return -1;
	moved->position.x += worldDelta.x;
	moved->position.y += worldDelta.y;
	moved->position.z += worldDelta.z;
	for (RoadSegment& road : m_roads) {
		if (road.startNodeId == id || road.endNodeId == id) RebuildRoadGeometry(road);
	}
	const int snapTarget = FindClosestNode(moved->position, 1.75f, nodeId);
	if (snapTarget >= 0) {
		LinkNodes(nodeId, snapTarget);
		return snapTarget;
	}
	return nodeId;
}

void RoadSystem::SetNodeSelected(int nodeId, bool selected)
{
	for (MapNode& node : m_nodes) {
		if (selected && node.id != static_cast<uint32_t>(nodeId)) node.isSelected = false;
		else if (node.id == static_cast<uint32_t>(nodeId)) node.isSelected = selected;
	}
}

void RoadSystem::HandleInput(EngineCamera& camera, EditorMode mode,
	const ImVec2& viewportPosition, const ImVec2& viewportSize)
{
	constexpr float snapDistance = 1.75f;
	ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_Z)) {
		if (!m_roads.empty()) m_roads.pop_back();
		if (!m_nodes.empty()) m_nodes.pop_back();
		m_selectedNodeId = m_nodes.empty() ? 0 : m_nodes.back().id;
		m_placementStartNodeId = m_nodes.empty() ? 0 : m_nodes.front().id;
		return;
	}

	const ImVec2 mouse = io.MousePos;
	const bool mouseInViewport = mouse.x >= viewportPosition.x &&
		mouse.x <= viewportPosition.x + viewportSize.x &&
		mouse.y >= viewportPosition.y &&
		mouse.y <= viewportPosition.y + viewportSize.y;
	if (!mouseInViewport) return;

	if (!camera.is3DMode && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
		camera.x += io.MouseDelta.x;
		camera.y += io.MouseDelta.y;
	}

	const float zoom = std::max(camera.zoom, 0.01f);
	const Vector3D worldPosition(
		(mouse.x - viewportPosition.x - camera.x) / zoom,
		(mouse.y - viewportPosition.y - camera.y) / zoom,
		0.0f);

	if (mode == ROAD_PLACEMENT_MODE && !camera.is3DMode &&
		ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		MapNode* snapNode = nullptr;
		float nearestDistance = snapDistance;
		for (MapNode& node : m_nodes) {
			const float distance = worldPosition.DistanceTo(node.position);
			if (distance <= nearestDistance) {
				nearestDistance = distance;
				snapNode = &node;
			}
		}

		if (snapNode) {
			if (m_selectedNodeId == 0) {
				m_selectedNodeId = snapNode->id;
				m_placementStartNodeId = snapNode->id;
			} else if (m_selectedNodeId != snapNode->id) {
				if (snapNode->id == m_placementStartNodeId) {
					LinkNodes(static_cast<int>(m_selectedNodeId), static_cast<int>(snapNode->id));
				} else {
					RoadSegment road;
					road.id = m_nextRoadId++;
					road.startNodeId = m_selectedNodeId;
					road.endNodeId = snapNode->id;
					m_roads.push_back(road);
					RebuildRoadGeometry(m_roads.back());
					m_selectedNodeId = snapNode->id;
				}
			}
		} else {
			MapNode node;
			node.id = m_nextNodeId++;
			node.position = worldPosition;

			MapNode* previous = nullptr;
			for (MapNode& existing : m_nodes) {
				if (existing.id == m_selectedNodeId) {
					previous = &existing;
					break;
				}
			}
			if (previous) {
				const float dx = node.position.x - previous->position.x;
				const float dy = node.position.y - previous->position.y;
				const float length = std::sqrt(dx * dx + dy * dy);
				if (length > 0.0001f) {
					const float outgoingX = dx / length;
					const float outgoingY = dy / length;
					previous->rotationY = std::atan2(outgoingX, -outgoingY) * 180.0f / 3.14159265358979323846f;
					node.rotationY = previous->rotationY;
				}
			}
			m_nodes.push_back(node);
			if (m_selectedNodeId == 0) {
				m_placementStartNodeId = node.id;
			} else {
				RoadSegment road;
				road.id = m_nextRoadId++;
				road.startNodeId = m_selectedNodeId;
				road.endNodeId = node.id;
				m_roads.push_back(road);
				RebuildRoadGeometry(m_roads.back());
			}
			m_selectedNodeId = node.id;
		}
	}

}

void RoadSystem::RenderCanvas(ImDrawList* draw_list, const ImVec2& window_pos, EngineCamera& camera)
{
	if (!draw_list) return;
	for (const RoadSegment& road : m_roads) {
		const MapNode* start = nullptr;
		const MapNode* end = nullptr;
		for (const MapNode& node : m_nodes) {
			if (node.id == road.startNodeId) start = &node;
			if (node.id == road.endNodeId) end = &node;
		}
		if (!start || !end) continue;

		constexpr int steps = 20;
		constexpr float halfWidth = 20.0f;
		for (int i = 0; i < steps; ++i) {
			const float t0 = static_cast<float>(i) / steps;
			const float t1 = static_cast<float>(i + 1) / steps;
			const Vector3D p0 = EngineMath::InterpolateSpline(start->position, start->rotationY, end->position, end->rotationY, t0);
			const Vector3D p1 = EngineMath::InterpolateSpline(start->position, start->rotationY, end->position, end->rotationY, t1);
			const float dx = p1.x - p0.x;
			const float dy = p1.y - p0.y;
			const float length = std::sqrt(dx * dx + dy * dy);
			if (length <= 0.0001f) continue;
			const float nx = -dy / length * halfWidth;
			const float ny = dx / length * halfWidth;
			const float zoom = std::max(camera.zoom, 0.01f);
			const ImVec2 s0(window_pos.x + p0.x * zoom + camera.x, window_pos.y + p0.y * zoom + camera.y);
			const ImVec2 s1(window_pos.x + p1.x * zoom + camera.x, window_pos.y + p1.y * zoom + camera.y);
			draw_list->AddLine(s0, s1, 0xFF80C080, 4.0f);
			draw_list->AddLine(ImVec2(s0.x + nx * zoom, s0.y + ny * zoom), ImVec2(s1.x + nx * zoom, s1.y + ny * zoom), 0xFF505050, 1.5f);
			draw_list->AddLine(ImVec2(s0.x - nx * zoom, s0.y - ny * zoom), ImVec2(s1.x - nx * zoom, s1.y - ny * zoom), 0xFF505050, 1.5f);
		}
	}
	for (const MapNode& node : m_nodes) {
		const float zoom = std::max(camera.zoom, 0.01f);
		const ImVec2 center(window_pos.x + node.position.x * zoom + camera.x, window_pos.y + node.position.y * zoom + camera.y);
		const ImU32 color = node.id == 1 ? 0xFF0000FF :
			(node.isSelected || node.id == m_selectedNodeId) ? 0xFF00FF00 : 0xFF00FFFF;
		draw_list->AddRectFilled(ImVec2(center.x - 4.0f, center.y - 4.0f),
			ImVec2(center.x + 4.0f, center.y + 4.0f), color);
	}
}
