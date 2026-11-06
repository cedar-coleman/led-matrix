// Firmware-only screens with no JS counterpart: messages and the bring-up test pattern.
#pragma once

#include <string>

#include "surface.h"

namespace wall {
namespace render {

// Two short lines centred on the wall in the wall font: double size where a line fits
// (21 characters), normal size otherwise, cut with '~' if even that is too long.
void renderMessage(Surface& s, const std::string& line1, const std::string& line2);

// Phase 0 bring-up: every panel shows its chain number (1 = wired to the board), an
// arrow pointing at the top of the wall and a border, in supply A's colour (cyan) or
// supply B's (orange). A panel mounted the wrong way shows an arrow pointing down; a
// panel in the wrong place shows the wrong number.
void renderTestPattern(Surface& s);

}  // namespace render
}  // namespace wall
