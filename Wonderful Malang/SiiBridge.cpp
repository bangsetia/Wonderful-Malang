#include "SiiBridge.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cstdlib>
#include <cmath>

namespace {

constexpr std::size_t kMaxSiiBytes = 32u * 1024u * 1024u;

std::string NormalizeSlashes(std::string value) {
	std::replace(value.begin(), value.end(), '\\', '/');
	return value;
}

bool IsCandidateReference(const std::string& value) {
	if (value.empty() || value.size() > 512 || value.find("..") != std::string::npos)
		return false;
	if (value.find('/') == std::string::npos && value.find('.') == std::string::npos)
		return false;
	return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
		return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == '\\';
	});
}

std::string Trim(std::string value) {
	const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
		return std::isspace(ch) != 0;
	});
	value.erase(value.begin(), first);
	const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
		return std::isspace(ch) != 0;
	}).base();
	value.erase(last, value.end());
	return value;
}

std::string QuotedValue(const std::string& value) {
	const std::size_t first = value.find('"');
	if (first == std::string::npos)
		return {};
	const std::size_t last = value.find('"', first + 1);
	return last == std::string::npos ? std::string() : value.substr(first + 1, last - first - 1);
}

std::vector<std::string> ParseRoadLookReferences(const std::string& text) {
	std::vector<std::string> tokens;
	std::unordered_set<std::string> unique;
	std::istringstream input(text);
	std::string line;
	while (std::getline(input, line)) {
		const std::size_t comment = line.find('#');
		if (comment != std::string::npos)
			line.resize(comment);
		const std::string trimmed = Trim(line);
		if (trimmed.substr(0, 10) != "road_look[")
			continue;
		const std::size_t colon = trimmed.find(':');
		if (colon == std::string::npos)
			continue;
		const std::string token = Trim(std::string(trimmed.substr(colon + 1)));
		if (!token.empty() && unique.insert(token).second)
			tokens.push_back(token);
	}
	return tokens;
}

struct SiiUnit {
	std::string type;
	std::string token;
	std::unordered_map<std::string, std::vector<std::string>> values;
};

std::vector<std::string> PropertyValues(const SiiUnit& unit, const std::string& key) {
	const auto found = unit.values.find(key);
	return found == unit.values.end() ? std::vector<std::string>() : found->second;
}

std::string FirstProperty(const SiiUnit& unit, const std::string& key) {
	const std::vector<std::string> values = PropertyValues(unit, key);
	return values.empty() ? std::string() : values.front();
}

std::vector<float> ParseFloatValues(const std::vector<std::string>& values) {
	std::vector<float> result;
	for (const std::string& value : values) {
		const char* cursor = value.c_str();
		while (*cursor != '\0') {
			char* end = nullptr;
			const float parsed = std::strtof(cursor, &end);
			if (end != cursor) {
				if (std::isfinite(parsed)) result.push_back(parsed);
				cursor = end;
			} else {
				++cursor;
			}
		}
	}
	return result;
}

std::vector<SiiUnit> ParseUnits(const std::string& text, const std::string& unitPrefix) {
	std::vector<SiiUnit> units;
	std::istringstream input(text);
	std::string line;
	SiiUnit current;
	bool inUnit = false;
	while (std::getline(input, line)) {
		bool quoted = false;
		for (std::size_t i = 0; i < line.size(); ++i) {
			if (line[i] == '"')
				quoted = !quoted;
			else if (line[i] == '#' && !quoted) {
				line.resize(i);
				break;
			}
		}
		const std::string trimmed = Trim(line);
		if (trimmed.empty())
			continue;
		if (!inUnit) {
			const std::size_t colon = trimmed.find(':');
			const std::size_t brace = trimmed.find('{');
			if (colon == std::string::npos || brace == std::string::npos || brace < colon)
				continue;
			current.type = Trim(trimmed.substr(0, colon));
			if (current.type != unitPrefix)
				continue;
			current.token = Trim(trimmed.substr(colon + 1, brace - colon - 1));
			inUnit = true;
			continue;
		}
		if (trimmed == "}") {
			units.push_back(std::move(current));
			current = {};
			inUnit = false;
			continue;
		}
		const std::size_t colon = trimmed.find(':');
		if (colon != std::string::npos) {
			const std::string key = Trim(trimmed.substr(0, colon));
			const std::string value = Trim(trimmed.substr(colon + 1));
			const std::size_t quote = value.find('"');
			const std::string parsedValue = quote == std::string::npos ? Trim(value) : QuotedValue(value);
			if (!key.empty() && !parsedValue.empty())
				current.values[key].push_back(parsedValue);
		}
		if (trimmed.find('}') != std::string::npos) {
			units.push_back(std::move(current));
			current = {};
			inUnit = false;
		}
	}
	if (inUnit)
		units.push_back(std::move(current));
	return units;
}

std::string MaterialName(const std::string& path) {
	const std::size_t slash = path.find_last_of("/\\");
	const std::size_t begin = slash == std::string::npos ? 0 : slash + 1;
	const std::size_t dot = path.find_last_of('.');
	return path.substr(begin, dot == std::string::npos || dot < begin ? std::string::npos : dot - begin);
}

void ParseRoadDefinitionArrays(const std::string& text, std::vector<RoadMaterialData>& materials,
	std::vector<std::string>& materialNames) {
	std::istringstream input(text);
	std::string line;
	bool inRoadDefinition = false;
	while (std::getline(input, line)) {
		bool quoted = false;
		for (std::size_t index = 0; index < line.size(); ++index) {
			if (line[index] == '"') quoted = !quoted;
			else if (line[index] == '#' && !quoted) { line.resize(index); break; }
		}
		const std::string trimmed = Trim(line);
		if (!inRoadDefinition) {
			if (trimmed.find("road_def") == 0 && trimmed.find('{') != std::string::npos)
				inRoadDefinition = true;
			continue;
		}
		if (trimmed == "}") {
			inRoadDefinition = false;
			continue;
		}
		if (trimmed.find("road_material[") != 0 && trimmed.find("sidewalk_material[") != 0 &&
			trimmed.find("road_kerb_material[") != 0)
			continue;
		const std::string path = QuotedValue(trimmed);
		if (path.empty())
			continue;
		const std::string name = MaterialName(path);
		materials.push_back({ path, name });
		if (std::find(materialNames.begin(), materialNames.end(), path) == materialNames.end())
			materialNames.push_back(path);
	}
}

} // namespace

bool SiiBridge::BacaKonfigurasiBus(const std::string& pathSii, VariabelBusSii& dataOut) {
	std::ifstream file(pathSii);
	if (!file.is_open()) return false;

	const auto trim = [](std::string& value) {
		const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
			return std::isspace(ch) != 0;
		});
		value.erase(value.begin(), first);
		const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
			return std::isspace(ch) != 0;
		}).base();
		value.erase(last, value.end());
	};
	const auto lowercase = [](std::string& value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
	};

	TipeVarian parsedVariant = dataOut.varianAktif;
	bool foundVariant = false;
	std::string line;
	while (std::getline(file, line)) {
		bool quoted = false;
		for (std::size_t index = 0; index < line.size(); ++index) {
			if (line[index] == '"') quoted = !quoted;
			else if (line[index] == '#' && !quoted) {
				line.resize(index);
				break;
			}
		}

		trim(line);
		if (line.empty()) continue;
		const std::size_t colon = line.find(':');
		if (colon == std::string::npos) continue;

		std::string key = line.substr(0, colon);
		trim(key);
		lowercase(key);
		if (key != "variant") continue;

		std::string value = line.substr(colon + 1);
		trim(value);
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
			value = value.substr(1, value.size() - 2);
			trim(value);
		}
		lowercase(value);

		if (value == "exclusive") parsedVariant = TipeVarian::Exclusive;
		else if (value == "standard") parsedVariant = TipeVarian::Standard;
		else if (value == "custom") parsedVariant = TipeVarian::Custom;
		else continue;
		foundVariant = true;
	}

	if (!foundVariant || file.bad()) return false;
	dataOut.varianAktif = parsedVariant;
	return true;
}

bool SiiBridge::Initialize(const std::string& basePath, const ZipVFS* zipVfs, std::string* error) {
	if (basePath.empty() && zipVfs == nullptr) {
		if (error)
			*error = "Either a base path or mounted ZipVFS is required.";
		return false;
	}

	m_basePath = basePath;
	m_zipVfs = zipVfs;
	m_initialized = true;
	return Reload(error);
}

bool SiiBridge::Reload(std::string* error) {
	if (!m_initialized) {
		if (error)
			*error = "SiiBridge must be initialized explicitly before reload.";
		return false;
	}

	SiiAssetCache next;
	m_roadFileLoaded = false;
	m_roadFileSizeBytes = 0;
	std::string firstError;
	std::string roadText;
	if (ParseSiiFile("def/world/road.sii", roadText, &firstError)) {
		m_roadFileLoaded = true;
		m_roadFileSizeBytes = roadText.size();
		std::vector<RoadLookData> roadLooksByDefinition;
		for (const SiiUnit& unit : ParseUnits(roadText, "road_look")) {
			RoadLookData look;
			look.token = unit.token;
			look.name = FirstProperty(unit, "name");
			try {
				look.roadSizeLeft = std::stof(FirstProperty(unit, "road_size_left"));
				look.roadSizeRight = std::stof(FirstProperty(unit, "road_size_right"));
			} catch (...) {
				look.roadSizeLeft = 0.0f;
				look.roadSizeRight = 0.0f;
			}
			if (look.name.empty())
				look.name = look.token;
			if (!look.token.empty())
				roadLooksByDefinition.push_back(std::move(look));
		}
		for (const std::string& token : ParseRoadLookReferences(roadText)) {
			const auto found = std::find_if(roadLooksByDefinition.begin(), roadLooksByDefinition.end(),
				[&](const RoadLookData& look) { return look.token == token; });
			if (found != roadLooksByDefinition.end())
				next.roadLooks.push_back(*found);
		}
		for (const RoadLookData& look : roadLooksByDefinition)
			if (std::none_of(next.roadLooks.begin(), next.roadLooks.end(),
				[&](const RoadLookData& existing) { return existing.token == look.token; }))
				next.roadLooks.push_back(look);
		ParseRoadDefinitionArrays(roadText, next.roadMaterialData, next.roadMaterials);
	}
	if (ParseSiiFile("def/world/terrain_material.sii", roadText, &firstError)) {
		for (const SiiUnit& unit : ParseUnits(roadText, "terrain_materials")) {
			for (const std::string& path : PropertyValues(unit, "material[]"))
				AddUnique(next.terrainShaders, path);
			for (const auto& property : unit.values) {
				if (property.first.find("material[") == 0)
					for (const std::string& path : property.second)
						AddUnique(next.terrainShaders, path);
			}
		}
	}
	std::string fileText;
	if (ParseSiiFile("def/world/railing.sii", fileText, &firstError)) {
		for (const SiiUnit& unit : ParseUnits(fileText, "railing_model")) {
			const std::string model = FirstProperty(unit, "model_desc");
			AddUnique(next.railings, model.empty() ? unit.token : model);
		}
	}
	if (ParseSiiFile("def/world/building_scheme.sii", fileText, &firstError)) {
		for (const SiiUnit& unit : ParseUnits(fileText, "building_scheme")) {
			const std::string name = FirstProperty(unit, "name");
			AddUnique(next.models, name.empty() ? unit.token : name);
		}
	}
	if (ParseSiiFile("def/world/building_model.sii", fileText, &firstError)) {
		for (const SiiUnit& unit : ParseUnits(fileText, "building_model")) {
			const std::string model = FirstProperty(unit, "model_desc");
			AddUnique(next.models, model.empty() ? unit.token : model);
		}
	}
	if (ParseSiiFile("def/world/terrain_profile.sii", fileText, &firstError)) {
		for (const SiiUnit& unit : ParseUnits(fileText, "terrain_profile")) {
			const std::string name = FirstProperty(unit, "name");
			TerrainProfileData profile;
			profile.name = name.empty() ? unit.token : name;
			profile.elevations = ParseFloatValues(PropertyValues(unit, "elevation[]"));
			const std::vector<float> coefficientValues = ParseFloatValues(PropertyValues(unit, "coefficient"));
			if (profile.elevations.empty() && coefficientValues.size() > 1) {
				profile.elevations = coefficientValues;
			} else if (!coefficientValues.empty()) {
				profile.coefficient = coefficientValues.front();
			}
			if (profile.name.empty()) continue;
			if (std::find(next.terrainProfiles.begin(), next.terrainProfiles.end(), profile.name) ==
				next.terrainProfiles.end()) {
				next.terrainProfiles.push_back(profile.name);
				next.terrainProfileData.push_back(std::move(profile));
			}
		}
	}
	if (ParseSiiFile("def/world/vegetation.sii", fileText, &firstError)) {
		for (const SiiUnit& unit : ParseUnits(fileText, "vegetation_data")) {
			const std::string name = FirstProperty(unit, "name");
			const std::vector<std::string> models = PropertyValues(unit, "models[]");
			const std::vector<std::string> model = PropertyValues(unit, "model[]");
			const std::vector<std::string> textures = PropertyValues(unit, "texture");
			if (!name.empty()) AddUnique(next.vegetation, name);
			else if (!models.empty()) AddUnique(next.vegetation, models.front());
			else if (!model.empty()) AddUnique(next.vegetation, model.front());
			else if (!textures.empty()) AddUnique(next.vegetation, textures.front());
			else AddUnique(next.vegetation, unit.token);
		}
	}
	if (ParseSiiFile("def/world/detail_vegetation_def.sii", fileText, &firstError)) {
		for (const SiiUnit& unit : ParseUnits(fileText, "detail_vegetation_def")) {
			const std::vector<std::string> models = PropertyValues(unit, "model[]");
			const std::string texture = FirstProperty(unit, "texture");
			if (!models.empty()) AddUnique(next.vegetation, models.front());
			else if (!texture.empty()) AddUnique(next.vegetation, texture);
			else AddUnique(next.vegetation, unit.token);
		}
	}

	m_assets = std::move(next);
	if (error)
		*error = firstError;
	return true;
}

bool SiiBridge::LoadText(const std::string& relativePath, std::string& text) const {
	text.clear();
	const std::string normalized = NormalizeSlashes(relativePath);

	if (m_zipVfs) {
		std::string readError;
		const std::vector<std::uint8_t> bytes = m_zipVfs->ReadFileFromMemory(normalized, &readError);
		if (!bytes.empty() && bytes.size() <= kMaxSiiBytes) {
			text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			return true;
		}
	}

	if (m_basePath.empty())
		return false;

	const std::filesystem::path path = std::filesystem::path(m_basePath) / normalized;
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		return false;

	const std::streamoff size = input.tellg();
	if (size < 0 || static_cast<std::uint64_t>(size) > kMaxSiiBytes)
		return false;
	text.resize(static_cast<std::size_t>(size));
	input.seekg(0, std::ios::beg);
	return text.empty() || static_cast<bool>(input.read(text.data(), size));
}

bool SiiBridge::ParseSiiFile(const std::string& relativePath, std::string& text, std::string* error) const {
	text.clear();
	if (!LoadText(relativePath, text)) {
		if (error && error->empty())
			*error = "Unable to read " + relativePath;
		return false;
	}
	return true;
}

std::vector<std::string> SiiBridge::Tokenize(std::string_view text) {
	std::vector<std::string> tokens;
	std::string current;
	bool quoted = false;
	bool comment = false;

	for (std::size_t i = 0; i < text.size(); ++i) {
		const char ch = text[i];
		if (comment) {
			if (ch == '\n')
				comment = false;
			continue;
		}
		if (!quoted && ch == '#') {
			comment = true;
			continue;
		}
		if (ch == '"') {
			if (quoted && !current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
			quoted = !quoted;
			continue;
		}
		const bool separator = !quoted && (std::isspace(static_cast<unsigned char>(ch)) ||
			ch == '{' || ch == '}' || ch == '=' || ch == ';' || ch == ',' || ch == '(' || ch == ')');
		if (separator) {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
		} else {
			current.push_back(ch);
		}
	}
	if (!current.empty())
		tokens.push_back(std::move(current));
	return tokens;
}

std::vector<std::string> SiiBridge::ExtractReferences(std::string_view text) {
	std::vector<std::string> references;
	std::unordered_set<std::string> unique;
	for (const std::string& token : Tokenize(text)) {
		if (!IsCandidateReference(token))
			continue;
		if (unique.insert(token).second)
			references.push_back(token);
	}
	return references;
}

void SiiBridge::AddUnique(std::vector<std::string>& values, const std::string& value) {
	if (value.empty())
		return;
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(value);
}

void SiiBridge::AddFallback(std::vector<std::string>& values, const std::vector<std::string>& fallback) {
	for (const std::string& value : fallback)
		AddUnique(values, value);
}
