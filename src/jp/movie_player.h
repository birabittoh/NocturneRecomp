// nocturnerecomp - ReXGlue Recompiled Project
//
// The JP release's story movies (DATA/video/*.wmv), played on World's code.
// See movie_player.cpp.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rex {
class Runtime;
}

namespace nocturne {

// Lets the movie hooks read the pad (Start skips a movie). Call once, after
// the input system exists.
void BindMovies(rex::Runtime* runtime);

// The movie frame due now, for the native renderer, which draws it into the
// PS1 box of the guest's frame so the screen filter goes over it like over
// the game. `rgba` is null before the first frame is out (the box is black
// then); `serial` changes whenever the frame does.
struct MoviePicture {
  std::shared_ptr<const std::vector<uint8_t>> rgba;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t serial = 0;
};

// False when no movie is playing.
bool CurrentMoviePicture(MoviePicture& out);

}  // namespace nocturne
