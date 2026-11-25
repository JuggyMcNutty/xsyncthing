#pragma once

namespace xst::progress {

// Opens the GPU and display and shows the progress state at 720p from a drawing thread of its own, until stop_screen.
// The work runs on the caller's thread meanwhile. Shows the last lines of the title's log too.
bool start_screen();
void stop_screen();

} // namespace xst::progress
