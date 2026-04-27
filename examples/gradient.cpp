#include <coroutine>
#include <utility>

import mr.renderer.lib;

int main()
{
  mr::KomputeGraphicsInteropRenderer renderer{256, 256};
  mr::FilePresenter presenter{"frames_out"};

  int frames_number = 3;
  for (auto&& frame : renderer.frames()) {
    presenter.present(std::move(frame));
    if (frames_number-- == 0) {
      break;
    }
  }

  return 0;
}
