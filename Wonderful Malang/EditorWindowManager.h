// --- EditorWindowManager.h ---
#pragma once

#include "SiiBridge.h"
#include "EngineMath.h"

#include <functional>

class RoadSystem;

struct EngineContext {
	SiiBridge* sii = nullptr;
	RoadSystem* roadSys = nullptr;
	Camera3D camera3D;
	RoadItemProperties roadProperties;
	RoadItemProperties roadDefaults;
	bool roadPropertiesOpen = false;
	bool requestRoadPropertiesClose = false;
	bool requestEditSigns = false;
	bool requestRemoveStamps = false;
	bool requestRoadRebuild = false;
	std::function<void(const RoadItemProperties&)> onRoadPropertiesAccepted;
	std::function<void()> onEditSigns;
	std::function<void()> onRemoveStamps;
};

class EditorWindowManager {
public:
	void DrawWorkspace(EngineContext& ctx);
	void DrawRoadItemProperties(EngineContext& ctx);
};

void DrawFreeFlight3DViewport(EngineContext& ctx);