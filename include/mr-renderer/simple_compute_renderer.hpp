#pragma once

#include <cstdint>

#include "renderer.hpp"

namespace mr {
  struct SimpleComputeRenderer : IRenderer {
    SimpleComputeRenderer(uint32_t width, uint32_t height);

    coro::generator<Frame> frames() override;

  private:
    uint32_t width_;
    uint32_t height_;
  };
} // namespace mr
