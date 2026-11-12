# Copilot Instructions

## Project Guidelines
- For DirectXTK in this vcpkg workspace, include DDS loading headers as <directxtk/DDSTextureLoader.h>; if unavailable, prefer an internal DDS loader limited to mip level 0 with desc.MipLevels=1 and support for DXT1/DXT3/DXT5/RGBA8. Keep detailed .mat → .tobj → .dds loading logs in m_assetInitializationLog.