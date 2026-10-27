#pragma once

// Puts the game's frame-stepped animation on game time: life textures, which regenerate every fifth rendered frame (6
// times a second at the game's own 30), at 6 times a second of game time; scrolling meshes (the main menu's waterfall
// among them), which move their texture coordinates a fixed step per frame, by that step times the frame's game time
// over 1/30 s; and the wind, which advances by the world's frame time, so a paused world (the main menu) no longer
// freezes it. Call once the game's code is unpacked.
void installAnimationTiming();
