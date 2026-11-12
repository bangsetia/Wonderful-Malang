#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include "RoadSystem.h"

class ZipVFS {
public:
    ZipVFS();
    ~ZipVFS();

    ZipVFS(const ZipVFS&) = delete;
    ZipVFS& operator=(const ZipVFS&) = delete;

    bool Mount(const std::string& archivePath, std::string* error = nullptr);
    void Unmount();
    bool IsMounted() const;
    void SetPhysicalRoot(const std::filesystem::path& rootPath);
    const std::filesystem::path& PhysicalRoot() const noexcept { return m_physicalRoot; }

    static std::string NormalizePath(const std::string& path);
    std::vector<std::uint8_t> ReadFileFromMemory(const std::string& path, std::string* error = nullptr) const;
    std::vector<std::string> ListFiles(const std::string& directory = "") const;
    bool PopulateRegistry(SiiAssetRegistry& registry, std::string* error = nullptr) const;

    static std::vector<std::string> ParseMaterialDdsPaths(const std::string& materialText);
    static std::string ExtractDdsPathFromTobjBytes(const std::vector<std::uint8_t>& bytes);

private:
    struct InternalZipState;
    std::unique_ptr<InternalZipState> m_state;
    std::filesystem::path m_physicalRoot;
};
