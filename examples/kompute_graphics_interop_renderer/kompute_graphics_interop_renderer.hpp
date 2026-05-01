#pragma once

#include <cstdint>
#include <memory>

#include <mr-renderer/renderer.hpp>
#include <mr-renderer/vulkan_wrappers.hpp>

namespace mr {
  struct KomputeGraphicsInteropRenderer : IRenderer {
    explicit KomputeGraphicsInteropRenderer(uint32_t width, uint32_t height);
    KomputeGraphicsInteropRenderer(const VulkanContext& shared_context, uint32_t width, uint32_t height);
    ~KomputeGraphicsInteropRenderer() override;

    KomputeGraphicsInteropRenderer(const KomputeGraphicsInteropRenderer&) = delete;
    KomputeGraphicsInteropRenderer& operator=(const KomputeGraphicsInteropRenderer&) = delete;
    KomputeGraphicsInteropRenderer(KomputeGraphicsInteropRenderer&&) noexcept;
    KomputeGraphicsInteropRenderer& operator=(KomputeGraphicsInteropRenderer&&) noexcept;

    [[nodiscard]] const VulkanContext& vulkan_context() const;

    coro::generator<Frame> frames(coro::generator<Target> targets) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace mr
