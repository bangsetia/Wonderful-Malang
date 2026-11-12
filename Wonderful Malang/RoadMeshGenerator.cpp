#include "RoadSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr float kEpsilon = 0.00001f;
constexpr float kPi = 3.14159265358979323846f;

float Dot(const Vector3D& left, const Vector3D& right)
{
	return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vector3D Cross(const Vector3D& left, const Vector3D& right)
{
	return {
		left.y * right.z - left.z * right.y,
		left.z * right.x - left.x * right.z,
		left.x * right.y - left.y * right.x
	};
}

float LengthSquared(const Vector3D& value)
{
	return Dot(value, value);
}

Vector3D Normalize(const Vector3D& value, const Vector3D& fallback)
{
	const float lengthSquared = LengthSquared(value);
	if (lengthSquared <= kEpsilon) return fallback;
	const float inverseLength = 1.0f / std::sqrt(lengthSquared);
	return value * inverseLength;
}

Vector3D ProjectOntoPlane(const Vector3D& value, const Vector3D& normal)
{
	return value - normal * Dot(value, normal);
}

Vector3D SelectReferenceUp(const Vector3D& tangent)
{
	const Vector3D worldUp = { 0.0f, 1.0f, 0.0f };
	if (std::abs(Dot(tangent, worldUp)) < 0.985f) return worldUp;
	return { 1.0f, 0.0f, 0.0f };
}

bool CreateDynamicBuffer(ID3D11Device* device, UINT byteWidth, UINT bindFlags,
	Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, UINT& capacity)
{
	if (device == nullptr || byteWidth == 0) return false;
	if (buffer && capacity >= byteWidth) return true;

	D3D11_BUFFER_DESC description = {};
	description.Usage = D3D11_USAGE_DYNAMIC;
	description.ByteWidth = byteWidth;
	description.BindFlags = bindFlags;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	Microsoft::WRL::ComPtr<ID3D11Buffer> replacement;
	if (FAILED(device->CreateBuffer(&description, nullptr, replacement.GetAddressOf()))) return false;
	buffer = replacement;
	capacity = byteWidth;
	return true;
}

bool UploadBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
	const void* source, size_t byteCount)
{
	if (context == nullptr || buffer == nullptr || source == nullptr || byteCount == 0) return false;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr) {
		return false;
	}
	std::memcpy(mapped.pData, source, byteCount);
	context->Unmap(buffer, 0);
	return true;
}
}

bool RoadSystem::GenerateRoadMesh(RoadSegment& road, int sampleCount)
{
	road.leftTerrainVertices.clear();
	road.leftTerrainIndices.clear();
	road.rightTerrainVertices.clear();
	road.rightTerrainIndices.clear();
	const MapNode* start = GetNode(road.startNodeId);
	const MapNode* end = GetNode(road.endNodeId);
	if (start == nullptr || end == nullptr) {
		road.meshVertices.clear();
		road.meshIndices.clear();
		return false;
	}

	sampleCount = std::clamp(sampleCount, 2, 4096);
	std::vector<Vector3D> samples(static_cast<size_t>(sampleCount));
	std::vector<Vector3D> tangents(samples.size());
	std::vector<Vector3D> normals(samples.size());
	std::vector<Vector3D> binormals(samples.size());
	std::vector<float> distances(samples.size(), 0.0f);

	for (int index = 0; index < sampleCount; ++index) {
		const float t = static_cast<float>(index) / static_cast<float>(sampleCount - 1);
		const Vector3D mapPosition = EngineMath::InterpolateSpline(
			start->position, start->rotationY, end->position, end->rotationY, t);
		samples[static_cast<size_t>(index)] = { mapPosition.x, mapPosition.z, mapPosition.y };
		if (index > 0) {
			distances[static_cast<size_t>(index)] = distances[static_cast<size_t>(index - 1)] +
				samples[static_cast<size_t>(index)].DistanceTo(samples[static_cast<size_t>(index - 1)]);
		}
	}

	for (int index = 0; index < sampleCount; ++index) {
		const int previous = std::max(index - 1, 0);
		const int next = std::min(index + 1, sampleCount - 1);
		const Vector3D derivative = samples[static_cast<size_t>(next)] - samples[static_cast<size_t>(previous)];
		tangents[static_cast<size_t>(index)] = Normalize(derivative,
			index > 0 ? tangents[static_cast<size_t>(index - 1)] : Vector3D(1.0f, 0.0f, 0.0f));

		if (index == 0) {
			const Vector3D reference = SelectReferenceUp(tangents[0]);
			normals[0] = Normalize(ProjectOntoPlane(reference, tangents[0]), { 0.0f, 1.0f, 0.0f });
		} else {
			const Vector3D transported = ProjectOntoPlane(normals[static_cast<size_t>(index - 1)],
				tangents[static_cast<size_t>(index)]);
			normals[static_cast<size_t>(index)] = Normalize(transported,
				Normalize(ProjectOntoPlane(SelectReferenceUp(tangents[static_cast<size_t>(index)]),
					tangents[static_cast<size_t>(index)]), { 0.0f, 1.0f, 0.0f }));
		}
		binormals[static_cast<size_t>(index)] = Normalize(
			Cross(tangents[static_cast<size_t>(index)], normals[static_cast<size_t>(index)]),
			{ 0.0f, 0.0f, 1.0f });
		normals[static_cast<size_t>(index)] = Normalize(
			Cross(binormals[static_cast<size_t>(index)], tangents[static_cast<size_t>(index)]),
			normals[static_cast<size_t>(index)]);
	}

	const float leftWidth = road.properties.left.width > kEpsilon
		? road.properties.left.width : std::max(road.width * 0.5f, kEpsilon);
	const float rightWidth = road.properties.right.width > kEpsilon
		? road.properties.right.width : std::max(road.width * 0.5f, kEpsilon);
	const int subdivisions = std::clamp(std::max(road.properties.left.terrainQuadCount,
		road.properties.right.terrainQuadCount), 1, 256);
	const int verticesPerSlice = subdivisions + 1;
	const size_t sliceCount = static_cast<size_t>(sampleCount);

	road.meshVertices.clear();
	road.meshIndices.clear();
	road.meshVertices.reserve(sliceCount * static_cast<size_t>(verticesPerSlice));
	road.meshIndices.reserve((sliceCount - 1) * static_cast<size_t>(subdivisions) * 6);

	const float totalDistance = std::max(distances.back(), kEpsilon);
	for (int slice = 0; slice < sampleCount; ++slice) {
		const float distanceV = distances[static_cast<size_t>(slice)] / std::max(road.width, kEpsilon);
		for (int column = 0; column <= subdivisions; ++column) {
			const float across = static_cast<float>(column) / static_cast<float>(subdivisions);
			const float signedOffset = -leftWidth + (leftWidth + rightWidth) * across;
			const float heightOffset = road.properties.left.heightOffset +
				(road.properties.right.heightOffset - road.properties.left.heightOffset) * across;
			RoadMeshVertex vertex;
			vertex.position = samples[static_cast<size_t>(slice)] +
				binormals[static_cast<size_t>(slice)] * signedOffset +
				normals[static_cast<size_t>(slice)] * heightOffset;
			vertex.normal = normals[static_cast<size_t>(slice)];
			vertex.u = across;
			vertex.v = distanceV;
			road.meshVertices.push_back(vertex);
		}
	}

	for (int slice = 0; slice < sampleCount - 1; ++slice) {
		for (int column = 0; column < subdivisions; ++column) {
			const uint32_t first = static_cast<uint32_t>(slice * verticesPerSlice + column);
			const uint32_t second = first + 1;
			const uint32_t nextFirst = first + static_cast<uint32_t>(verticesPerSlice);
			const uint32_t nextSecond = nextFirst + 1;
			road.meshIndices.push_back(first);
			road.meshIndices.push_back(second);
			road.meshIndices.push_back(nextFirst);
			road.meshIndices.push_back(second);
			road.meshIndices.push_back(nextSecond);
			road.meshIndices.push_back(nextFirst);
		}
	}

	const auto buildTerrainSide = [&](const RoadSideProperties& side, bool isLeft, float sideWidth,
		std::vector<RoadMeshVertex>& vertices, std::vector<uint32_t>& indices) {
		const int quadCount = std::clamp(side.terrainQuadCount, 0, 256);
		if (quadCount == 0) return;
		const int sideVerticesPerSlice = quadCount + 1;
		const TerrainProfileData* profile = side.terrainProfileIndex >= 0 &&
			side.terrainProfileIndex < static_cast<int>(m_terrainProfiles.size())
			? &m_terrainProfiles[static_cast<size_t>(side.terrainProfileIndex)] : nullptr;
		vertices.reserve(sliceCount * static_cast<size_t>(sideVerticesPerSlice));
		indices.reserve((sliceCount - 1) * static_cast<size_t>(quadCount) * 6);

		for (int slice = 0; slice < sampleCount; ++slice) {
			const size_t sample = static_cast<size_t>(slice);
			for (int column = 0; column <= quadCount; ++column) {
				const float across = static_cast<float>(column) / static_cast<float>(quadCount);
				float profileElevation = profile ? profile->coefficient * across : 0.0f;
				if (profile && !profile->elevations.empty()) {
					const float profilePosition = across * static_cast<float>(profile->elevations.size() - 1);
					const size_t lower = static_cast<size_t>(profilePosition);
					const size_t upper = std::min(lower + 1, profile->elevations.size() - 1);
					const float blend = profilePosition - static_cast<float>(lower);
					profileElevation = profile->elevations[lower] +
						(profile->elevations[upper] - profile->elevations[lower]) * blend;
				}
				const float outwardDistance = static_cast<float>(column) * 4.0f;
				const float signedOffset = isLeft ? -(sideWidth + outwardDistance) : sideWidth + outwardDistance;
				RoadMeshVertex vertex;
				vertex.position = samples[sample] + binormals[sample] * signedOffset;
				vertex.position.y += side.heightOffset + profileElevation;
				vertex.normal = normals[sample];
				vertex.u = static_cast<float>(column);
				vertex.v = distances[sample] * 0.1f;
				vertices.push_back(vertex);
			}
		}

		for (int slice = 0; slice < sampleCount - 1; ++slice) {
			for (int column = 0; column < quadCount; ++column) {
				const uint32_t first = static_cast<uint32_t>(slice * sideVerticesPerSlice + column);
				const uint32_t second = first + 1;
				const uint32_t nextFirst = first + static_cast<uint32_t>(sideVerticesPerSlice);
				const uint32_t nextSecond = nextFirst + 1;
				indices.push_back(first);
				indices.push_back(second);
				indices.push_back(nextFirst);
				indices.push_back(second);
				indices.push_back(nextSecond);
				indices.push_back(nextFirst);
			}
		}
	};
	buildTerrainSide(road.properties.left, true, leftWidth,
		road.leftTerrainVertices, road.leftTerrainIndices);
	buildTerrainSide(road.properties.right, false, rightWidth,
		road.rightTerrainVertices, road.rightTerrainIndices);
	return !road.meshVertices.empty() && !road.meshIndices.empty() && totalDistance > 0.0f;
}

bool RoadSystem::UpdateRoadMeshBuffers(RoadSegment& road, ID3D11Device* device,
	ID3D11DeviceContext* context)
{
	if (road.meshVertices.empty() || road.meshIndices.empty() || device == nullptr || context == nullptr) {
		return false;
	}
	const auto uploadMesh = [device, context](const std::vector<RoadMeshVertex>& vertices,
		const std::vector<uint32_t>& indices, Microsoft::WRL::ComPtr<ID3D11Buffer>& vertexBuffer,
		Microsoft::WRL::ComPtr<ID3D11Buffer>& indexBuffer, UINT& vertexCapacity, UINT& indexCapacity) {
		if (vertices.empty() || indices.empty()) return true;
		const UINT vertexBytes = static_cast<UINT>(vertices.size() * sizeof(RoadMeshVertex));
		const UINT indexBytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
		if (!CreateDynamicBuffer(device, vertexBytes, D3D11_BIND_VERTEX_BUFFER, vertexBuffer, vertexCapacity) ||
			!CreateDynamicBuffer(device, indexBytes, D3D11_BIND_INDEX_BUFFER, indexBuffer, indexCapacity)) return false;
		return UploadBuffer(context, vertexBuffer.Get(), vertices.data(), vertexBytes) &&
			UploadBuffer(context, indexBuffer.Get(), indices.data(), indexBytes);
	};
	return uploadMesh(road.meshVertices, road.meshIndices, road.vertexBuffer, road.indexBuffer,
		road.vertexBufferCapacity, road.indexBufferCapacity) &&
		uploadMesh(road.leftTerrainVertices, road.leftTerrainIndices, road.leftTerrainVertexBuffer,
			road.leftTerrainIndexBuffer, road.leftTerrainVertexBufferCapacity, road.leftTerrainIndexBufferCapacity) &&
		uploadMesh(road.rightTerrainVertices, road.rightTerrainIndices, road.rightTerrainVertexBuffer,
			road.rightTerrainIndexBuffer, road.rightTerrainVertexBufferCapacity, road.rightTerrainIndexBufferCapacity);
}
