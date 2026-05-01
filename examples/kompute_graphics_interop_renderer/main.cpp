#include <ranges>
#include <cstdlib>

#include <tracy/Tracy.hpp>

#include "kompute_graphics_interop_renderer.hpp"
#include <mr-renderer/file_presenter.hpp>
#include <mr-renderer/window_presenter.hpp>

int main()
try {
  ZoneScoped;
  int frames_number = 1 << 14;

  if (std::getenv("MR_RENDERER_INTEROP_WINDOW") != nullptr) {
    mr::WindowPresenter presenter{256, 256};
    mr::KomputeGraphicsInteropRenderer renderer{presenter.vulkan_context(), 256, 256};
    for (auto&& frame : renderer.frames(presenter.targets()) | std::ranges::views::take(frames_number)) {
      FrameMarkNamed("interop_main_frame");
      presenter.present(std::move(frame));
    }
  } else {
    mr::KomputeGraphicsInteropRenderer renderer{256, 256};
    mr::FilePresenter presenter{"frames_out", 256, 256};
    for (auto&& frame : renderer.frames(presenter.targets()) | std::ranges::views::take(frames_number)) {
      FrameMarkNamed("interop_main_frame");
      presenter.present(std::move(frame));
    }
  }

  return 0;
} catch (...) {
  return 1;
}
