#pragma once

#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>
#include <kompute/Kompute.hpp>
#include <tracy/Tracy.hpp>

#if defined(TRACY_ENABLE)
#define MR_TRACY_ZONE ZoneScoped
#define MR_TRACY_ZONE_N(name) ZoneScopedN(name)
#define MR_TRACY_FRAME(name) FrameMarkNamed(name)
#else
#define MR_TRACY_ZONE
#define MR_TRACY_ZONE_N(name)
#define MR_TRACY_FRAME(name)
#endif

