#include "ZipVFS.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

#pragma pack(push, 1)
struct ZipLocalHeader {
    std::uint32_t signature;
    std::uint16_t versionNeeded;
    std::uint16_t flags;
    std::uint16_t compressionMethod;
    std::uint16_t modifiedTime;
    std::uint16_t modifiedDate;
    std::uint32_t crc32;
    std::uint32_t compressedSize;
    std::uint32_t uncompressedSize;
    std::uint16_t fileNameLength;
    std::uint16_t extraFieldLength;
};

struct ZipCentralDirectoryEntry {
    std::uint32_t signature;
    std::uint16_t versionMadeBy;
    std::uint16_t versionNeeded;
    std::uint16_t flags;
    std::uint16_t compressionMethod;
    std::uint16_t modifiedTime;
    std::uint16_t modifiedDate;
    std::uint32_t crc32;
    std::uint32_t compressedSize;
    std::uint32_t uncompressedSize;
    std::uint16_t fileNameLength;
    std::uint16_t extraFieldLength;
    std::uint16_t commentLength;
    std::uint16_t diskNumberStart;
    std::uint16_t internalAttributes;
    std::uint32_t externalAttributes;
    std::uint32_t localHeaderOffset;
};

struct ZipEndOfCentralDirectory {
    std::uint32_t signature;
    std::uint16_t diskNumber;
    std::uint16_t centralDirectoryDisk;
    std::uint16_t entriesOnDisk;
    std::uint16_t entryCount;
    std::uint32_t centralDirectorySize;
    std::uint32_t centralDirectoryOffset;
    std::uint16_t commentLength;
};
#pragma pack(pop)

constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr std::uint32_t kCentralDirectorySignature = 0x02014b50;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50;
constexpr std::uint16_t kCompressionStored = 0;
constexpr std::uint16_t kEncryptedFlag = 1u;
constexpr std::uint16_t kZip64Sentinel16 = 0xffff;
constexpr std::uint32_t kZip64Sentinel32 = 0xffffffffu;
constexpr std::size_t kMaxZipComment = 65535;

struct EntryRecord {
    std::uint32_t localHeaderOffset = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t crc32 = 0;
    std::uint16_t flags = 0;
    std::uint16_t compressionMethod = 0;
};

std::uint16_t ReadU16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadU32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool HasBytes(std::uint64_t totalSize, std::uint64_t offset, std::uint64_t count) {
    return offset <= totalSize && count <= totalSize - offset;
}

bool IsSafeRelativePath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos)
        return false;

    std::size_t componentStart = 0;
    while (componentStart < path.size()) {
        const std::size_t componentEnd = path.find('/', componentStart);
        const std::size_t count = componentEnd == std::string::npos
            ? path.size() - componentStart
            : componentEnd - componentStart;
        const std::string_view component(path.data() + componentStart, count);
        if (component == "..")
            return false;
        if (componentEnd == std::string::npos)
            break;
        componentStart = componentEnd + 1;
    }
    return true;
}

bool HasExtension(const std::string& path, const char* extension) {
    const std::size_t extensionLength = std::char_traits<char>::length(extension);
    if (path.size() < extensionLength)
        return false;
    for (std::size_t i = 0; i < extensionLength; ++i) {
        if (std::tolower(static_cast<unsigned char>(path[path.size() - extensionLength + i])) !=
            std::tolower(static_cast<unsigned char>(extension[i])))
            return false;
    }
    return true;
}

std::uint32_t CalculateCrc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void SetError(std::string* error, const char* message) {
    if (error)
        *error = message;
}

} // namespace

struct ZipVFS::InternalZipState {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const std::uint8_t* mappedBytes = nullptr;
    std::uint64_t mappedSize = 0;
    std::unordered_map<std::string, EntryRecord> entries;

    ~InternalZipState() {
        if (mappedBytes)
            UnmapViewOfFile(mappedBytes);
        if (mapping)
            CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
};

ZipVFS::ZipVFS() = default;
ZipVFS::~ZipVFS() = default;

bool ZipVFS::Mount(const std::string& archivePath, std::string* error) {
    Unmount();
    if (archivePath.empty() || archivePath.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        SetError(error, "Archive path is empty or too long.");
        return false;
    }

    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        archivePath.data(), static_cast<int>(archivePath.size()), nullptr, 0);
    if (wideLength <= 0) {
        SetError(error, "Archive path is not valid UTF-8.");
        return false;
    }

    std::wstring widePath(static_cast<std::size_t>(wideLength), L'\\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, archivePath.data(),
        static_cast<int>(archivePath.size()), widePath.data(), wideLength) != wideLength) {
        SetError(error, "Unable to convert archive path to UTF-16.");
        return false;
    }

    auto candidate = std::make_unique<InternalZipState>();
    candidate->file = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (candidate->file == INVALID_HANDLE_VALUE) {
        SetError(error, "Unable to open ZIP archive.");
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(candidate->file, &fileSize) || fileSize.QuadPart <= 0) {
        SetError(error, "Unable to read ZIP archive size or archive is empty.");
        return false;
    }
    candidate->mappedSize = static_cast<std::uint64_t>(fileSize.QuadPart);
    if (candidate->mappedSize > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)())) {
        SetError(error, "ZIP archive is too large to map in this process.");
        return false;
    }

    candidate->mapping = CreateFileMappingW(candidate->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!candidate->mapping) {
        SetError(error, "CreateFileMappingW failed for ZIP archive.");
        return false;
    }
    candidate->mappedBytes = static_cast<const std::uint8_t*>(
        MapViewOfFile(candidate->mapping, FILE_MAP_READ, 0, 0, 0));
    if (!candidate->mappedBytes) {
        SetError(error, "MapViewOfFile failed for ZIP archive.");
        return false;
    }

    if (candidate->mappedSize < sizeof(ZipEndOfCentralDirectory)) {
        SetError(error, "ZIP archive is too small to contain an end record.");
        return false;
    }

    const std::uint64_t searchLength = (std::min<std::uint64_t>)(candidate->mappedSize,
        sizeof(ZipEndOfCentralDirectory) + kMaxZipComment);
    const std::uint64_t searchStart = candidate->mappedSize - searchLength;
    std::uint64_t eocdOffset = candidate->mappedSize;

    for (std::uint64_t offset = candidate->mappedSize - sizeof(ZipEndOfCentralDirectory);;) {
        if (ReadU32(candidate->mappedBytes + offset) == kEndOfCentralDirectorySignature) {
            const std::uint16_t commentLength = ReadU16(candidate->mappedBytes + offset + 20);
            if (HasBytes(candidate->mappedSize, offset,
                sizeof(ZipEndOfCentralDirectory) + static_cast<std::uint64_t>(commentLength))) {
                eocdOffset = offset;
                break;
            }
        }
        if (offset == searchStart)
            break;
        --offset;
    }

    if (eocdOffset == candidate->mappedSize) {
        SetError(error, "ZIP end-of-central-directory record was not found.");
        return false;
    }

    const std::uint8_t* eocd = candidate->mappedBytes + eocdOffset;
    const std::uint16_t diskNumber = ReadU16(eocd + 4);
    const std::uint16_t centralDirectoryDisk = ReadU16(eocd + 6);
    const std::uint16_t entriesOnDisk = ReadU16(eocd + 8);
    const std::uint16_t entryCount = ReadU16(eocd + 10);
    const std::uint32_t centralDirectorySize = ReadU32(eocd + 12);
    const std::uint32_t centralDirectoryOffset = ReadU32(eocd + 16);

    if (diskNumber != 0 || centralDirectoryDisk != 0 || entriesOnDisk != entryCount) {
        SetError(error, "Multi-disk ZIP archives are not supported.");
        return false;
    }
    if (entryCount == kZip64Sentinel16 || centralDirectorySize == kZip64Sentinel32 ||
        centralDirectoryOffset == kZip64Sentinel32) {
        SetError(error, "ZIP64 archives are not supported.");
        return false;
    }
    if (!HasBytes(candidate->mappedSize, centralDirectoryOffset, centralDirectorySize) ||
        static_cast<std::uint64_t>(centralDirectoryOffset) + centralDirectorySize > eocdOffset) {
        SetError(error, "ZIP central directory is out of archive bounds.");
        return false;
    }

    std::uint64_t cursor = centralDirectoryOffset;
    const std::uint64_t directoryEnd = cursor + centralDirectorySize;
    for (std::uint32_t index = 0; index < entryCount; ++index) {
        if (!HasBytes(directoryEnd, cursor, sizeof(ZipCentralDirectoryEntry)) ||
            ReadU32(candidate->mappedBytes + cursor) != kCentralDirectorySignature) {
            SetError(error, "Malformed ZIP central directory entry.");
            return false;
        }

        const std::uint8_t* bytes = candidate->mappedBytes + cursor;
        const std::uint16_t flags = ReadU16(bytes + 8);
        const std::uint16_t method = ReadU16(bytes + 10);
        const std::uint32_t crc = ReadU32(bytes + 16);
        const std::uint32_t compressedSize = ReadU32(bytes + 20);
        const std::uint32_t uncompressedSize = ReadU32(bytes + 24);
        const std::uint16_t nameLength = ReadU16(bytes + 28);
        const std::uint16_t extraLength = ReadU16(bytes + 30);
        const std::uint16_t commentLength = ReadU16(bytes + 32);
        const std::uint16_t diskStart = ReadU16(bytes + 34);
        const std::uint32_t localOffset = ReadU32(bytes + 42);
        const std::uint64_t recordSize = sizeof(ZipCentralDirectoryEntry) +
            static_cast<std::uint64_t>(nameLength) + extraLength + commentLength;

        if (!HasBytes(directoryEnd, cursor, recordSize)) {
            SetError(error, "ZIP central directory entry exceeds its bounds.");
            return false;
        }
        if (diskStart != 0 || compressedSize == kZip64Sentinel32 ||
            uncompressedSize == kZip64Sentinel32 || localOffset == kZip64Sentinel32) {
            SetError(error, "Multi-disk or ZIP64 entry is not supported.");
            return false;
        }

        std::string name(reinterpret_cast<const char*>(bytes + sizeof(ZipCentralDirectoryEntry)), nameLength);
        if (!name.empty() && name.back() != '/') {
            std::string normalized = NormalizePath(name);
            if (!normalized.empty()) {
                candidate->entries.emplace(std::move(normalized), EntryRecord{
                    localOffset, compressedSize, uncompressedSize, crc, flags, method
                });
            }
        }
        cursor += recordSize;
    }

    m_state = std::move(candidate);
    if (error)
        error->clear();
    return true;
}

void ZipVFS::Unmount() {
    m_state.reset();
}

bool ZipVFS::IsMounted() const {
    return m_state != nullptr;
}

void ZipVFS::SetPhysicalRoot(const std::filesystem::path& rootPath) {
    m_physicalRoot = rootPath;
}

std::string ZipVFS::NormalizePath(const std::string& path) {
    std::string normalized;
    normalized.reserve(path.size());
    for (unsigned char ch : path) {
        normalized.push_back(ch == '\\' ? '/' : static_cast<char>(std::tolower(ch)));
    }
    while (!normalized.empty() && normalized.front() == '/')
        normalized.erase(normalized.begin());
    while (normalized.compare(0, 2, "./") == 0)
        normalized.erase(0, 2);
    return IsSafeRelativePath(normalized) ? normalized : std::string{};
}

std::vector<std::uint8_t> ZipVFS::ReadFileFromMemory(
    const std::string& path, std::string* error) const {
    const std::string normalized = NormalizePath(path);
    if (normalized.empty()) {
        SetError(error, "Invalid or unsafe virtual path.");
        return {};
    }
    if (!m_physicalRoot.empty()) {
        const std::filesystem::path physicalPath = m_physicalRoot / std::filesystem::path(normalized);
        std::ifstream input(physicalPath, std::ios::binary | std::ios::ate);
        if (input) {
            const std::streamoff size = input.tellg();
            if (size >= 0 && static_cast<std::uint64_t>(size) <=
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
                input.seekg(0, std::ios::beg);
                if (result.empty() || static_cast<bool>(input.read(
                    reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size())))) {
                    if (error)
                        error->clear();
                    return result;
                }
            }
        }
    }

    if (!m_state) {
        SetError(error, "File was not found in the physical asset root and no ZIP archive is mounted.");
        return {};
    }
    const auto found = m_state->entries.find(normalized);
    if (found == m_state->entries.end()) {
        SetError(error, "File was not found in the physical asset root or ZIP archive.");
        return {};
    }
    const EntryRecord& entry = found->second;
    if ((entry.flags & kEncryptedFlag) != 0) {
        SetError(error, "Encrypted ZIP entries are not supported.");
        return {};
    }
    if (entry.compressionMethod != kCompressionStored || entry.compressedSize != entry.uncompressedSize) {
        SetError(error, "Only stored ZIP entries are supported; compressed entries require DEFLATE support.");
        return {};
    }

    const std::uint64_t localOffset = entry.localHeaderOffset;
    if (!HasBytes(m_state->mappedSize, localOffset, sizeof(ZipLocalHeader)) ||
        ReadU32(m_state->mappedBytes + localOffset) != kLocalHeaderSignature) {
        SetError(error, "Invalid ZIP local file header.");
        return {};
    }
    const std::uint8_t* localHeader = m_state->mappedBytes + localOffset;
    const std::uint16_t localFlags = ReadU16(localHeader + 6);
    const std::uint16_t localMethod = ReadU16(localHeader + 8);
    const std::uint16_t nameLength = ReadU16(localHeader + 26);
    const std::uint16_t extraLength = ReadU16(localHeader + 28);
    if ((localFlags & kEncryptedFlag) != 0 || localMethod != entry.compressionMethod) {
        SetError(error, "Local and central ZIP headers disagree or entry is encrypted.");
        return {};
    }

    const std::uint64_t dataOffset = localOffset + sizeof(ZipLocalHeader) +
        static_cast<std::uint64_t>(nameLength) + extraLength;
    if (!HasBytes(m_state->mappedSize, dataOffset, entry.compressedSize)) {
        SetError(error, "ZIP entry data exceeds archive bounds.");
        return {};
    }
    const auto* data = m_state->mappedBytes + dataOffset;
    if (CalculateCrc32(data, entry.uncompressedSize) != entry.crc32) {
        SetError(error, "ZIP entry CRC validation failed.");
        return {};
    }

    std::vector<std::uint8_t> result(data, data + entry.uncompressedSize);
    if (error)
        error->clear();
    return result;
}

std::vector<std::string> ZipVFS::ListFiles(const std::string& directory) const {
    std::vector<std::string> files;
    const std::string prefix = directory.empty() ? std::string{} : NormalizePath(directory);
    if (!directory.empty() && prefix.empty())
        return files;

    std::unordered_set<std::string> uniqueFiles;
    if (!m_physicalRoot.empty()) {
        std::error_code iterationError;
        for (std::filesystem::recursive_directory_iterator it(m_physicalRoot, iterationError), end;
            it != end; it.increment(iterationError)) {
            if (iterationError) {
                iterationError.clear();
                continue;
            }
            if (!it->is_regular_file(iterationError) || iterationError) {
                iterationError.clear();
                continue;
            }
            const std::filesystem::path relativePath = std::filesystem::relative(
                it->path(), m_physicalRoot, iterationError);
            if (iterationError) {
                iterationError.clear();
                continue;
            }
            const std::string normalized = NormalizePath(relativePath.generic_string());
            if (!normalized.empty() && (prefix.empty() || (normalized.size() > prefix.size() &&
                normalized.compare(0, prefix.size(), prefix) == 0 && normalized[prefix.size()] == '/')) &&
                uniqueFiles.insert(normalized).second)
                files.push_back(normalized);
        }
    }
    if (m_state) {
        files.reserve(files.size() + m_state->entries.size());
        for (const auto& entry : m_state->entries) {
            if ((prefix.empty() || (entry.first.size() > prefix.size() &&
                entry.first.compare(0, prefix.size(), prefix) == 0 && entry.first[prefix.size()] == '/')) &&
                uniqueFiles.insert(entry.first).second)
                files.push_back(entry.first);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> ZipVFS::ParseMaterialDdsPaths(const std::string& materialText) {
    std::vector<std::string> paths;
    std::unordered_set<std::string> unique;

    for (std::size_t cursor = 0; cursor + 4 <= materialText.size();) {
        const bool isDds = materialText[cursor] == '.' &&
            std::tolower(static_cast<unsigned char>(materialText[cursor + 1])) == 'd' &&
            std::tolower(static_cast<unsigned char>(materialText[cursor + 2])) == 'd' &&
            std::tolower(static_cast<unsigned char>(materialText[cursor + 3])) == 's';
        if (!isDds) {
            ++cursor;
            continue;
        }

        std::size_t start = cursor;
        while (start > 0) {
            const unsigned char ch = static_cast<unsigned char>(materialText[start - 1]);
            if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == '\\')
                --start;
            else
                break;
        }

        std::string path = NormalizePath(materialText.substr(start, cursor + 4 - start));
        if (!path.empty() && unique.insert(path).second)
            paths.push_back(std::move(path));
        cursor += 4;
    }
    return paths;
}

std::string ZipVFS::ExtractDdsPathFromTobjBytes(const std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t headerSize = 48;
    constexpr std::uint8_t signature[] = { 0x01, 0x0A, 0xB1, 0x70 };
    if (bytes.size() > headerSize &&
        std::equal(std::begin(signature), std::end(signature), bytes.begin())) {
        if (bytes.size() < headerSize)
            return {};
        const std::uint32_t pathLength = static_cast<std::uint32_t>(bytes[40]) |
            (static_cast<std::uint32_t>(bytes[41]) << 8) |
            (static_cast<std::uint32_t>(bytes[42]) << 16) |
            (static_cast<std::uint32_t>(bytes[43]) << 24);
        if (pathLength == 0 || static_cast<std::uint64_t>(headerSize) + pathLength > bytes.size())
            return {};
        return std::string(reinterpret_cast<const char*>(bytes.data() + headerSize), pathLength);
    }

    const std::string text(bytes.begin(), bytes.end());
    const std::vector<std::string> paths = ParseMaterialDdsPaths(text);
    return paths.empty() ? std::string() : paths.front();
}

bool ZipVFS::PopulateRegistry(SiiAssetRegistry& registry, std::string* error) const {
    if (!m_state) {
        SetError(error, "ZIP archive is not mounted.");
        return false;
    }

    SiiAssetRegistry parsed;
    std::unordered_set<std::string> materials;
    std::unordered_set<std::string> roadTypes;
    std::unordered_set<std::string> textures;

    for (const std::string& file : ListFiles()) {
        const bool isMaterial = HasExtension(file, ".mat");
        const bool isScript = HasExtension(file, ".sii");
        if (!isMaterial && !isScript)
            continue;

        const std::vector<std::uint8_t> bytes = ReadFileFromMemory(file);
        if (bytes.empty())
            continue;
        const std::string payload(bytes.begin(), bytes.end());
        const std::size_t slash = file.find_last_of('/');
        const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
        const std::size_t dot = file.find_last_of('.');
        const std::string name = file.substr(nameStart,
            dot == std::string::npos ? std::string::npos : dot - nameStart);

        if (isMaterial) {
            if (materials.insert(name).second)
                parsed.roadMaterials.push_back(name);
            for (std::string& texturePath : ParseMaterialDdsPaths(payload)) {
                if (textures.insert(texturePath).second)
                    parsed.terrainShaders.push_back(std::move(texturePath));
            }
        } else if (roadTypes.insert(name).second) {
            parsed.roadTypes.push_back(name);
        }
    }

    registry.roadTypes = std::move(parsed.roadTypes);
    registry.roadMaterials = std::move(parsed.roadMaterials);
    registry.terrainShaders = std::move(parsed.terrainShaders);
    if (error)
        error->clear();
    return true;
}
