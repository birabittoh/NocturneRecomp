// nocturnerecomp - ReXGlue Recompiled Project
//
// The JP release's story movies (DATA/video/*.wmv), played on World's code.
// See movie_player.cpp.

#pragma once

#include <memory>

namespace rex {
class Runtime;
}
namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace nocturne {

// Lets the movie hooks read the pad (Start skips a movie). Call once, after
// the input system exists.
void BindMovies(rex::Runtime* runtime);

// The overlay that draws the playing movie over the game.
std::unique_ptr<rex::ui::ImGuiDialog> CreateMovieOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace nocturne
