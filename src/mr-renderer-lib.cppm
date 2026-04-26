module;

#include <mr-renderer/file_presenter.hpp>
#include <mr-renderer/forward_renderer.hpp>
#include <mr-renderer/frame.hpp>
#include <mr-renderer/presenter.hpp>
#include <mr-renderer/renderer.hpp>
#include <mr-renderer/window_presenter.hpp>

export module mr.renderer.lib;

export namespace mr {
  using ::mr::FilePresenter;
  using ::mr::ForwardRenderer;
  using ::mr::Frame;
  using ::mr::IPresenter;
  using ::mr::IRenderer;
  using ::mr::WindowPresenter;
}
