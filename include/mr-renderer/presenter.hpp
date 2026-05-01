#pragma once

#include <coro/generator.hpp>

#include "frame.hpp"
#include "target.hpp"

namespace mr {
  struct IPresenter {
    constexpr IPresenter() noexcept = default;
    constexpr IPresenter(const IPresenter&) noexcept = default;
    constexpr IPresenter& operator=(const IPresenter&) noexcept = default;
    constexpr IPresenter(IPresenter&&) noexcept = default;
    constexpr IPresenter& operator=(IPresenter&&) noexcept = default;

    virtual coro::generator<Target> targets() = 0;
    virtual void present(Frame frame) = 0;
    virtual ~IPresenter() = default;
  };
}
