// Virtual 256x96 canvas -> position in the 12-panel HUB75 chain.
//
// The chain is one 768x32 strip as the driver sees it: chain panel k (0 = the panel
// whose input is wired to the board) covers driver x 64k..64k+63. On the wall:
//
//     row 1  [ 1 ][ 2 ][ 3 ][ 4 ]   upright, left to right
//     row 2  [ 8 ][ 7 ][ 6 ][ 5 ]   mounted upside down, chain runs right to left
//     row 3  [ 9 ][10 ][11 ][12 ]   upright, left to right
//
// This replaces the library's VirtualMatrixPanel. For three rows its CHAIN_TOP_LEFT_DOWN
// treats rows 1 and 3 as the inverted ones, which is not this wall, and keeping the map
// as plain arithmetic here means the host tests check it instead of the wall.
#pragma once

#include <stdint.h>

namespace wall {
namespace render {

constexpr int kPanelW = 64;
constexpr int kPanelH = 32;
constexpr int kPanelCols = 4;
constexpr int kPanelRows = 3;
constexpr int kChainLength = kPanelCols * kPanelRows;

// 0-based chain position of the panel at grid column col, row row.
constexpr int chainIndex(int col, int row) {
  return row % 2 == 0 ? row * kPanelCols + col : row * kPanelCols + (kPanelCols - 1 - col);
}

struct ChainXY {
  int16_t x;
  int16_t y;
};

constexpr ChainXY chainXY(int x, int y) {
  const int col = x / kPanelW, row = y / kPanelH;
  const int lx = x % kPanelW, ly = y % kPanelH;
  const int base = chainIndex(col, row) * kPanelW;
  // Upside-down panels: rotate 180 degrees within the panel.
  return row % 2 == 0 ? ChainXY{static_cast<int16_t>(base + lx), static_cast<int16_t>(ly)}
                      : ChainXY{static_cast<int16_t>(base + kPanelW - 1 - lx), static_cast<int16_t>(kPanelH - 1 - ly)};
}

}  // namespace render
}  // namespace wall
