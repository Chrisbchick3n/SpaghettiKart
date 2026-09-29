#pragma once

#include <libultraship/libultraship.h>

namespace GameUI {
class NetplayWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    ~NetplayWindow() override = default;

  protected:
    void InitElement() override {};
    void DrawElement() override;
    void UpdateElement() override {};
};
} // namespace GameUI
