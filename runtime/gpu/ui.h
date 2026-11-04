#pragma once

#include <filesystem>

#include <imgui.h>

namespace gpu {

ImTextureID LoadUiImage(const std::filesystem::path& path);

}  // namespace gpu
