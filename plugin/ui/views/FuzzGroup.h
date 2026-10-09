// The faceplate's Fuzz Factory group: the Drive knob and its power switch,
// plus the advanced deck (FuzzDeckPanel) with all five of the pedal's knobs.
// Same shape and gestures as the gate and pitch groups to its left: the knob
// dims and goes inert while the effect is off, the power stays bright;
// right-click anywhere on the group (Ctrl-click on macOS, touch-and-hold on
// the knob) opens the deck above the plate; Alt/Option-click on the knob
// resets the whole pedal. Off is the default.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "FuzzDeckPanel.h"
#include "services/Services.h"
#include "widgets/DimGroup.h"
#include "widgets/ParamControls.h"
#include "widgets/SecondaryPress.h"

namespace t3k::ui {

class FuzzGroup : public juce::Component, public SecondaryPressTarget {
public:
  // Knob + the plate's knob-to-companion gap + the power box, like Pitch.
  static constexpr int kWidth = theme::kKnobSizeSecondary + 10 + theme::kIconBoxSize;
  static int height() { return Knob::heightFor(theme::kKnobSizeSecondary); }

  explicit FuzzGroup(Services& services);
  ~FuzzGroup() override;

  void resized() override;
  // The plate hides the group (Show Fuzz Factory view setting): its deck goes too.
  void visibilityChanged() override;
  void mouseDown(const juce::MouseEvent& e) override;
  void secondaryPress(const juce::MouseEvent&) override { toggleDeck(); }

private:
  void toggleDeck();

  Services& services_;
  DimGroup dim_;
  ParamKnob drive_;
  ParamPowerButton power_;
  FuzzDeckPanel deck_;
};

}  // namespace t3k::ui
