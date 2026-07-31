#pragma once

namespace sf
{

// Opens a window, drives it, and shuts it down -- with no audio engine, no
// plugins, and (under SDL's dummy video driver) no display. Returns 0 on
// success, non-zero with a reason on stderr.
//
// This is the only part of the UI a test can honestly assert on: that the
// platform comes up, that the frame loop runs, that a click on a preset row
// produces exactly the request the running board would act on, and that it
// all tears down again.
int RunUiSelfTest(int frames);

}  // namespace sf
