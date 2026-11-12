// ModelBinary.h - format biner khusus engine (.emdl): tulis, baca, validasi.
//
// Struktur file (little-endian):
//   Header : "EMDL" | u32 formatVersion | u32 importerVersion | u32 chunkCount | u64 sourceHash   (24 byte)
//   Chunk  : char id[4] | u64 payloadSize | payload...
//   Chunk dikenali: INFO VERT SKNV INDX SUBM MESH MATL IMGS NODE SKIN ANIM
//   Chunk tak dikenal dilewati -> file versi baru yang hanya menambah chunk tetap terbaca.
// Catatan: mengasumsikan CPU little-endian (x86/x64/ARM Windows) - cukup untuk target engine ini.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "AssetUtil.h"
#include "ModelAsset.h"

namespace sem {

struct ModelFileHeader {
    uint32_t formatVersion = 0;
    uint32_t importerVersion = 0;
    uint32_t chunkCount = 0;
    uint64_t sourceHash = 0;
};

// Serialisasi ke memori.
std::vector<uint8_t> SerializeModel(const ModelAsset& model);

// Deserialisasi dari memori (dengan validasi batas dan indeks). Mengembalikan false + pesan bila rusak.
bool DeserializeModel(const uint8_t* data, size_t size, ModelAsset& out, std::string& err);

// Simpan / muat file .emdl.
bool SaveModelFile(const ModelAsset& model, const fs::path& path, std::string& err);
bool LoadModelFile(const fs::path& path, ModelAsset& out, std::string& err);

// Baca cepat hanya header + chunk INFO (tanpa memuat geometri) untuk cek cache usang.
// Mengisi: header, nama, importerVersion, deps. Mengembalikan false bila file tidak ada / bukan EMDL.
bool PeekModelInfo(const fs::path& path, ModelFileHeader& header, std::vector<SourceDep>& deps);

// Pemeriksaan konsistensi (indeks, rentang). Dipanggil otomatis oleh DeserializeModel.
bool ValidateModel(const ModelAsset& model, std::string& err);

} // namespace sem
