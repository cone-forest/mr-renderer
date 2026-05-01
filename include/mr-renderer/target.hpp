#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>

#include <vulkan/vulkan.hpp>

namespace mr {
  struct VulkanContext;

  /// Non-owning {height × width × 4} RGBA float raster (row-major). Mirrors mdspan ergonomics without
  /// requiring a freestanding `<mdspan>` header from libstdc++ on all toolchain setups.
  struct RgbaFloatRasterView {
    float* data = nullptr;
    std::size_t height = 0;
    std::size_t width = 0;

    [[nodiscard]] constexpr std::size_t extent(std::size_t dim) const noexcept
    {
      return dim == 0 ? height : dim == 1 ? width : 4zu;
    }

    constexpr float& operator[](std::size_t y, std::size_t x, std::size_t c) const noexcept
    {
      return data[(y * width + x) * 4zu + c];
    }
  };

  struct CpuTarget {
    uint32_t presenter_slot_id = UINT32_MAX;
    uint64_t presenter_generation = 0;
    RgbaFloatRasterView pixels{};
  };

  struct GpuTarget {
    const VulkanContext* context = nullptr;
    vk::Image image{};
    vk::ImageView view{};
    vk::Format format = vk::Format::eUndefined;
    vk::Extent2D extent{};
    uint32_t slot_id = UINT32_MAX;
    uint64_t generation = 0;
    vk::Semaphore acquire_semaphore{};
    vk::PipelineStageFlags acquire_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::Semaphore render_finished_semaphore{};
  };

  using TargetPayload = std::variant<CpuTarget, GpuTarget>;

  struct Target {
    uint32_t index = 0;
    TargetPayload payload{};
  };

} // namespace mr
