#pragma once

#include <memory>

#include "presenter.hpp"

namespace mr {
  struct WindowPresenter : IPresenter {
    WindowPresenter();
    ~WindowPresenter() override;

    WindowPresenter(const WindowPresenter&) = delete;
    WindowPresenter& operator=(const WindowPresenter&) = delete;
    WindowPresenter(WindowPresenter&&) noexcept;
    WindowPresenter& operator=(WindowPresenter&&) noexcept;

    void present(Frame frame) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
