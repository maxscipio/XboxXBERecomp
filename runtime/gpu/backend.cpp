#include <cstdlib>
#include <cstring>

#include "gpu/backend.h"
#include "core/log.h"

namespace gpu {

std::unique_ptr<Backend> CreateBackend() {
    const char* pick = std::getenv("XBE_BACKEND");
    if (pick && !_stricmp(pick, "vulkan")) {
        LOGI("gpu: Vulkan backend");
        return CreateVulkanBackend();
    }
    LOGI("gpu: Direct3D 12 backend");
    return CreateD3D12Backend();
}

}  // namespace gpu
