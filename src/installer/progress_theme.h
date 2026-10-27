#pragma once

#include <rex/ui/progress_window.h>

namespace nocturne {

// The start screen's crimson and gold, for every startup progress window.
inline rex::ui::ProgressWindowTheme ProgressTheme() {
  rex::ui::ProgressWindowTheme theme;
  theme.background[0] = 0x2A;
  theme.background[1] = 0x05;
  theme.background[2] = 0x0C;
  theme.bar_fill[0] = 0xD4;
  theme.bar_fill[1] = 0xA8;
  theme.bar_fill[2] = 0x4A;
  theme.bar_frame[0] = 0x5A;
  theme.bar_frame[1] = 0x0E;
  theme.bar_frame[2] = 0x18;
  return theme;
}

}  // namespace nocturne
