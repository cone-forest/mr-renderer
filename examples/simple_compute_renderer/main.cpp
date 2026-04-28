#include <utility>

#include <tracy/Tracy.hpp>

#include "simple_compute_renderer.hpp"
#include <mr-renderer/file_presenter.hpp>

int main()
{
  ZoneScoped;
  mr::SimpleComputeRenderer renderer{256, 256};
  mr::FilePresenter presenter{"frames_out"};

  int frames_number = 3;
  for (auto&& frame : renderer.frames()) {
    FrameMarkNamed("simple_compute_main_frame");
    presenter.present(std::move(frame));
    if (frames_number-- == 0) {
      break;
    }
  }

  return 0;
}
