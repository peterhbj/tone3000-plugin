#include "FuzzGroup.h"

#include "core/Theme.h"

namespace t3k::ui {

namespace {

// The plate's knob-to-companion gap (Faceplate's kGroupGap).
constexpr int kGap = 10;
// Chrome boxes in a bottom-aligned row sit on the secondary-knob centreline.
constexpr int kChromeLift = theme::faceplateChromeLift(theme::kKnobSizeSecondary);

Knob::Options driveKnob() {
  Knob::Options o;
  o.label = "Fuzz";
  o.size = theme::kKnobSizeSecondary;
  o.thumb = Knob::Thumb::secondary;
  o.scale = &scales::tone();  // the pedal's 0-10, like the deck
  o.defaultValue = FuzzDeckPanel::defaults().drive;
  o.help = help::Key::fuzz;
  return o;
}

}  // namespace

FuzzGroup::FuzzGroup(Services& services)
    : services_(services),
      drive_(services.backend, "fuzzDrive", driveKnob()),
      power_(services.backend, "fuzzEnabled", help::Key::fuzzPower),
      deck_(services) {
  dim_.addAndMakeVisible(drive_);
  dim_.setOff(!power_.value(), false);
  power_.onValueChange = [this](bool on) { dim_.setOff(!on); };
  addAndMakeVisible(dim_);
  addAndMakeVisible(power_);

  // One gesture resets the whole pedal, deck included.
  drive_.onReset = [this] { FuzzDeckPanel::resetDeck(services_.backend); };
  // Touch-and-hold on the knob is the right-click of the platform.
  drive_.onLongPress = [this] { toggleDeck(); };

  setSize(kWidth, height());
}

FuzzGroup::~FuzzGroup() { deck_.close(); }

void FuzzGroup::visibilityChanged() {
  if (!isVisible()) deck_.close();
}

void FuzzGroup::mouseDown(const juce::MouseEvent& e) {
  if (isSecondaryPress(e)) toggleDeck();
}

void FuzzGroup::toggleDeck() {
  if (deck_.isOpen()) {
    deck_.close();
    return;
  }
  // Anchored to the knob: the panel's left edge tracks the knob's (the plate
  // leaves the deck's width clear to the right of the effects cluster).
  deck_.open(drive_, Popover::Align::left, FuzzDeckPanel::kGap, 0, Popover::Placement::above);
}

void FuzzGroup::resized() {
  const int baseline = getHeight() - Knob::kEditorOverflow;  // the label slot's bottom edge
  dim_.setBounds(0, 0, theme::kKnobSizeSecondary, getHeight());
  drive_.setTopLeftPosition(0, 0);
  power_.setTopLeftPosition(theme::kKnobSizeSecondary + kGap, baseline - theme::kIconBoxSize + kChromeLift);
}

}  // namespace t3k::ui
