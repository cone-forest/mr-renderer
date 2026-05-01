#include <utility>

#include <tracy/Tracy.hpp>

#include "kompute_graphics_interop_renderer.hpp"
#include <mr-renderer/window_presenter.hpp>

int main()
{
  ZoneScoped;
  mr::KomputeGraphicsInteropRenderer renderer{256, 256};
  mr::WindowPresenter presenter;

  int frames_number = 1024;
  for (auto&& frame : renderer.frames()) {
    FrameMarkNamed("interop_main_frame");
    presenter.present(std::move(frame));
    if (frames_number-- == 0) {
      break;
    }
  }

  return 0;
}
