#pragma once

#include "EngineMath.h"
#include "RoadSystem.h"
#include "imgui.h"

inline void Render3DHorizonAndGrid(ImDrawList* drawList, const ImVec2& canvasPos,
	const ImVec2& canvasSize, const Camera3D& camera, const float matVP[4][4])
{
	const ImVec2 canvasEnd(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);

	const ImColor gridColor(70, 90, 70, 120);
	constexpr float gridSize = 64.0f;
	constexpr int lineRange = 20;
	for (int i = -lineRange; i <= lineRange; ++i) {
		const Vector3D startZ = { i * gridSize, 0.0f, -lineRange * gridSize };
		const Vector3D endZ = { i * gridSize, 0.0f, lineRange * gridSize };
		ImVec2 startScreen;
		ImVec2 endScreen;
		if (RenderPipeline::ProjectWorldToScreen(startZ, matVP, canvasSize.x, canvasSize.y, startScreen) &&
			RenderPipeline::ProjectWorldToScreen(endZ, matVP, canvasSize.x, canvasSize.y, endScreen)) {
			drawList->AddLine(ImVec2(canvasPos.x + startScreen.x, canvasPos.y + startScreen.y),
				ImVec2(canvasPos.x + endScreen.x, canvasPos.y + endScreen.y), gridColor, 1.2f);
		}

		const Vector3D startX = { -lineRange * gridSize, 0.0f, i * gridSize };
		const Vector3D endX = { lineRange * gridSize, 0.0f, i * gridSize };
		if (RenderPipeline::ProjectWorldToScreen(startX, matVP, canvasSize.x, canvasSize.y, startScreen) &&
			RenderPipeline::ProjectWorldToScreen(endX, matVP, canvasSize.x, canvasSize.y, endScreen)) {
			drawList->AddLine(ImVec2(canvasPos.x + startScreen.x, canvasPos.y + startScreen.y),
				ImVec2(canvasPos.x + endScreen.x, canvasPos.y + endScreen.y), gridColor, 1.2f);
		}
	}

}

inline void RenderProjectedSplines(ImDrawList* drawList, const ImVec2& canvasPos,
	const ImVec2& canvasSize, const RoadSystem& roadSystem, const float matVP[4][4])
{
	const auto& nodes = roadSystem.GetNodes();
	const auto& roads = roadSystem.GetRoads();
	for (const MapNode& node : nodes) {
		ImVec2 screenPosition;
		if (RenderPipeline::ProjectWorldToScreen(node.position, matVP, canvasSize.x, canvasSize.y, screenPosition)) {
			drawList->AddCircleFilled(ImVec2(canvasPos.x + screenPosition.x,
				canvasPos.y + screenPosition.y), 5.0f, ImColor(255, 255, 0, 255));
		}
	}

	for (const RoadSegment& road : roads) {
		const MapNode* start = roadSystem.GetNode(road.startNodeId);
		const MapNode* end = roadSystem.GetNode(road.endNodeId);
		if (!start || !end) continue;

		constexpr int segments = 20;
		ImVec2 previousScreen;
		bool previousValid = false;
		for (int i = 0; i <= segments; ++i) {
			const float t = static_cast<float>(i) / static_cast<float>(segments);
			const Vector3D interpolated = start->position + (end->position - start->position) * t;
			ImVec2 currentScreen;
			const bool currentValid = RenderPipeline::ProjectWorldToScreen(interpolated, matVP,
				canvasSize.x, canvasSize.y, currentScreen);
			if (currentValid && previousValid) {
				drawList->AddLine(ImVec2(canvasPos.x + previousScreen.x, canvasPos.y + previousScreen.y),
					ImVec2(canvasPos.x + currentScreen.x, canvasPos.y + currentScreen.y),
					ImColor(50, 205, 50, 255), 3.0f);
			}
			previousScreen = currentScreen;
			previousValid = currentValid;
		}
	}
}
