#pragma once

#include <coro/generator.hpp>
#include <cstdint>
#include <memory>

#include "presenter.hpp"

namespace mr {
  struct VulkanContext;

  struct WindowPresenter : IPresenter {
    explicit WindowPresenter(uint32_t width, uint32_t height);
    ~WindowPresenter() override;

    WindowPresenter(const WindowPresenter&) = delete;
    WindowPresenter& operator=(const WindowPresenter&) = delete;
    WindowPresenter(WindowPresenter&&) noexcept;
    WindowPresenter& operator=(WindowPresenter&&) noexcept;

    [[nodiscard]] const VulkanContext& vulkan_context() const;

    coro::generator<Target> targets() override;
    void present(Frame frame) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace mr
