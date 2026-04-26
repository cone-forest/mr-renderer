#pragma once

#include "presenter.hpp"

namespace mr {
  struct WindowPresenter : IPresenter {
    void present(Frame frame) override;
  };
}
