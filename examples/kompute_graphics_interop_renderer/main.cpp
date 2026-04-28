#include <utility>

#include <tracy/Tracy.hpp>

#include "kompute_graphics_interop_renderer.hpp"
#include <mr-renderer/file_presenter.hpp>

int main()
{
  ZoneScoped;
  mr::KomputeGraphicsInteropRenderer renderer{256, 256};
  mr::FilePresenter presenter{"frames_out"};

  int frames_number = 3;
  for (auto&& frame : renderer.frames()) {
    FrameMarkNamed("interop_main_frame");
    presenter.present(std::move(frame));
    if (frames_number-- == 0) {
      break;
    }
  }

  return 0;
}
