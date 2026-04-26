#pragma once

#include "frame.hpp"

namespace mr {
  struct IPresenter {
    constexpr IPresenter() noexcept = default;
    constexpr IPresenter(const IPresenter&) noexcept = default;
    constexpr IPresenter& operator=(const IPresenter&) noexcept = default;
    constexpr IPresenter(IPresenter&&) noexcept = default;
    constexpr IPresenter& operator=(IPresenter&&) noexcept = default;

    virtual void present(Frame frame) = 0;
    virtual ~IPresenter() = default;
  };
}
