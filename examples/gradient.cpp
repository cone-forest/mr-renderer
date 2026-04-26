#if defined(MR_RENDERER_USE_MODULE)
import mr.renderer.lib;
#else
#include <mr-renderer/mr-renderer.hpp>
#endif

int main()
{
  mr::SimpleComputeRenderer renderer{256, 256};
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
