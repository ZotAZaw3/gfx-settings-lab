#pragma once
#include <memory>
#include "rhi/rhi.h"

struct GLFWwindow;

namespace gfx::rhi {
// Window must already have a current GL 4.5 core context.
std::unique_ptr<Device> createGLDevice(GLFWwindow* window);
#if defined(GFX_HAS_D3D11)
std::unique_ptr<Device> createD3D11Device(GLFWwindow* window);
#endif
}  // namespace gfx::rhi
