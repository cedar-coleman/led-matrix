// Virtual 256x96 canvas -> position in the 12-panel HUB75 chain.
//
// The chain is one 768x32 strip as the driver sees it. Driver x 0 is the far end of the
// chain: the first pixel clocked out is pushed through every panel, so it lands in the
// panel furthest from the board. The panel the board plugs into gets driver x 704..767.
//
// Chain numbers on the wall, seen from the FRONT (1 = the panel the board plugs into):
//
//     row 1  [ 4 ][ 3 ][ 2 ][ 1 ]   upright, chain runs right to left
//     row 2  [ 5 ][ 6 ][ 7 ][ 8 ]   mounted upside down, chain runs left to right
//     row 3  [12 ][11 ][10 ][ 9 ]   upright, chain runs right to left
//
// This is the layout these panels fall into naturally: with a panel upright (arrows up),
// its input is on the left seen from behind, so the board ends up on the front top-right
// panel and every row-to-row ribbon is short. It is the library's CHAIN_TOP_RIGHT_DOWN,
// and the native test checks every pixel against that formula. Kept as plain arithmetic
// here (instead of the library's VirtualMatrixPanel, which needs GFX) so host tests run it.
#pragma once

#include <stdint.h>

namespace wall {
namespace render {

constexpr int kPanelW = 64;
constexpr int kPanelH = 32;
constexpr int kPanelCols = 4;
constexpr int kPanelRows = 3;
constexpr int kChainLength = kPanelCols * kPanelRows;

// 0-based chain position (0 = the panel the board plugs into) of the panel at grid
// column col, row row, both counted from the front top-left.
constexpr int chainIndex(int col, int row) {
  return row % 2 == 0 ? row * kPanelCols + (kPanelCols - 1 - col) : row * kPanelCols + col;
}

struct ChainXY {
  int16_t x;
  int16_t y;
};

constexpr ChainXY chainXY(int x, int y) {
  const int col = x / kPanelW, row = y / kPanelH;
  const int lx = x % kPanelW, ly = y % kPanelH;
  // Driver blocks count from the far end of the chain.
  const int base = (kChainLength - 1 - chainIndex(col, row)) * kPanelW;
  // Upside-down panels (row 2): rotate 180 degrees within the panel.
  return row % 2 == 0 ? ChainXY{static_cast<int16_t>(base + lx), static_cast<int16_t>(ly)}
                      : ChainXY{static_cast<int16_t>(base + kPanelW - 1 - lx), static_cast<int16_t>(kPanelH - 1 - ly)};
}

}  // namespace render
}  // namespace wall
