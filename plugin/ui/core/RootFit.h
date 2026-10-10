// How the shell fits the design-space root into the box it is given: one
// uniform scale (the tighter of width and height), and a layout width that
// grows past design::kWidth when the box is wider than the design aspect,
// so the root stretches to fill it instead of letterboxing. Shared by
// NativeEditor and the testbed's ScaledHost.
#pragma once

#include <algorithm>
#include <cmath>

#include "Design.h"

namespace t3k::ui::design {

struct RootFit {
  double scale = 1.0;
  int layoutWidth = kWidth;  // the root's width in design px
  int x = 0;                 // left offset in box px (non-zero only when narrower than kWidth fits)
};

// `scale` is the uniform scale to use (normally fitScale's); the layout width
// fills the box at that scale.
inline RootFit fitAt(int boxWidth, double scale) {
  RootFit fit;
  fit.scale = std::max(0.05, scale);
  fit.layoutWidth = std::max(kWidth, static_cast<int>(std::ceil(boxWidth / fit.scale - 1e-6)));
  fit.x = static_cast<int>(std::lround((boxWidth - fit.layoutWidth * fit.scale) / 2));
  return fit;
}

inline double fitScale(int boxWidth, int boxHeight, int designHeight) {
  return std::min(boxWidth / static_cast<double>(kWidth), boxHeight / static_cast<double>(designHeight));
}

}  // namespace t3k::ui::design
