import mr.renderer.lib;

int main()
{
  mr::ForwardRenderer renderer{256, 256};
  mr::FilePresenter presenter{"frames_out"};

  for (auto&& frame : renderer.frames()) {
    presenter.present(std::move(frame));
  }

  return 0;
}
