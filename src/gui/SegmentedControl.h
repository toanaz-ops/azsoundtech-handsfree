// SegmentedControl -- one outlined box, several buttons butted together.
//
// This is the analyser toolbar's control, transcribed from the design study
// (docs/spec-ui-mockup.md section 3):
//
//     ┌────────┬──────────┬──────────┐
//     │  Line  │ 1/1 oct  │ 1/3 oct  │
//     └────────┴──────────┴──────────┘
//
// ONE border around the group, a hairline divider between segments, no radius
// on the inner corners and none on the individual buttons. The first cut drew
// each option as a separate rounded chip with a gap, which reads as several
// controls rather than one choice -- and is what the owner flagged.
//
// The segment TEXT is deliberately NOT the tracked uppercase legend face. It
// is the numeric face at 11 px in sentence case ("Line", "1/1 oct", "Avg
// off"), because these are display options sitting directly above the plot
// they modify and must not shout over it.
//
// The buttons stay real juce::Buttons -- keyboard focus, radio grouping and
// hit testing all come for free; this component only owns their geometry and
// paints the box around them.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace gui
{

class SegmentedControl : public juce::Component
{
public:
    // `labels` is the segment text, left to right. The group is mutually
    // exclusive; index 0 is selected until told otherwise.
    explicit SegmentedControl (const juce::StringArray& labels);

    // Fired only by a user click, with the newly selected index.
    std::function<void (int)> onSelected;

    // Lights one segment and clears the rest WITHOUT firing onSelected.
    void setSelectedIndex (int index);
    [[nodiscard]] int getSelectedIndex() const { return selected_; }

    // Width the group needs: every segment sized to the WIDEST label, so the
    // dividers land on a regular rhythm instead of wherever the text happens
    // to end. Asked for by the toolbar rather than assumed.
    [[nodiscard]] int getPreferredWidth() const;

    [[nodiscard]] int getNumSegments() const { return (int) buttons_.size(); }

    // TEST ACCESSOR ONLY -- drive a segment directly.
    [[nodiscard]] juce::Button& getSegmentForTest (int index)
    {
        return *buttons_[(std::size_t) index];
    }

    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;

    static constexpr int kSegmentPadding = 11;   // study: .seg button { padding: 4px 11px }

private:
    void reflect();

    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
    int selected_ = 0;
    int segmentWidth_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SegmentedControl)
};

} // namespace gui
