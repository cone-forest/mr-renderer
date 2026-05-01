#pragma once

#include <coro/generator.hpp>

#include "frame.hpp"
#include "target.hpp"

namespace mr {
  struct IRenderer {
    constexpr IRenderer() noexcept = default;
    constexpr IRenderer(const IRenderer&) noexcept = default;
    constexpr IRenderer& operator=(const IRenderer&) noexcept = default;
    constexpr IRenderer(IRenderer&&) noexcept = default;
    constexpr IRenderer& operator=(IRenderer&&) noexcept = default;

    virtual coro::generator<Frame> frames(coro::generator<Target> targets) = 0;
    virtual ~IRenderer() = default;
  };
}
