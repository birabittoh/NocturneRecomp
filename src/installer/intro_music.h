#pragma once

namespace nocturne {

// The start screen's looping music. Start and Stop bracket the screen; the
// toggle only silences it, so turning it back on resumes where it was.
//
// No track ships yet, so IntroMusicAvailable() is false and the screen hides
// its music toggle. Dropping one in means embedding it and filling these in
// (see the Eternal Sonata: Reprise intro_music.cpp this mirrors).
bool IntroMusicAvailable();
void StartIntroMusic();
void StopIntroMusic();
bool IntroMusicEnabled();
void SetIntroMusicEnabled(bool enabled);

}  // namespace nocturne
