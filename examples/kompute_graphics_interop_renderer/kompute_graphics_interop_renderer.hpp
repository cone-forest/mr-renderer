#pragma once

#include <cstdint>
#include <memory>

#include <mr-renderer/renderer.hpp>

namespace mr {
  struct KomputeGraphicsInteropRenderer : IRenderer {
    KomputeGraphicsInteropRenderer(uint32_t width, uint32_t height);
    ~KomputeGraphicsInteropRenderer() override;

    KomputeGraphicsInteropRenderer(const KomputeGraphicsInteropRenderer&) = delete;
    KomputeGraphicsInteropRenderer& operator=(const KomputeGraphicsInteropRenderer&) = delete;
    KomputeGraphicsInteropRenderer(KomputeGraphicsInteropRenderer&&) noexcept;
    KomputeGraphicsInteropRenderer& operator=(KomputeGraphicsInteropRenderer&&) noexcept;

    coro::generator<Frame> frames() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace mr
