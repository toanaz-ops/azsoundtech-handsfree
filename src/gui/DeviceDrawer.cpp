#include "gui/DeviceDrawer.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
// Header cell width the theme does not tokenise (a drawer-local metric, not
// a shared design token): the fixed slot for the status badge.
constexpr float kBadgeCellWidth = 120.0f;
} // namespace

DeviceDrawer::DeviceDrawer (DevicePanel& wrappedPanel)
    : wrapped_ (wrappedPanel)
{
    using namespace az::theme;

    // THE re-parent: the same DevicePanel instance moves under this drawer,
    // once, forever. Nothing here may destroy or recreate it -- its restart
    // hooks are wired by MainComponent and must survive everything.
    addAndMakeVisible (wrapped_);

    presetsPlaceholder_.setJustificationType (juce::Justification::centredRight);
    presetsPlaceholder_.setColour (juce::Label::textColourId, dim);
    presetsPlaceholder_.setFont (baseFont());
    addAndMakeVisible (presetsPlaceholder_);
}

void DeviceDrawer::setStatusBadge (juce::Component* badgeOrNull)
{
    badge_ = badgeOrNull;
    if (badge_ != nullptr)
        addAndMakeVisible (*badge_);
    resized();
}

int DeviceDrawer::getPreferredHeight() const
{
    return kHeaderHeight + kContentHeight;
}

void DeviceDrawer::resized()
{
    using namespace az::theme;

    auto headerArea = getLocalBounds().removeFromTop (kHeaderHeight).reduced (gap, 0);

    juce::FlexBox header;
    header.flexDirection = juce::FlexBox::Direction::row;
    header.alignItems    = juce::FlexBox::AlignItems::center;

    constexpr float cellH = (float) touchTarget;

    header.items.add (juce::FlexItem (presetsPlaceholder_).withFlex (1.0f));

    if (badge_ != nullptr)
        header.items.add (juce::FlexItem (*badge_)
                              .withWidth (kBadgeCellWidth).withHeight (cellH)
                              .withMargin ({ 0.0f, 0.0f, 0.0f, (float) gap }));

    header.performLayout (headerArea);

    // The drawer is always open: the content sits under the header, forever.
    wrapped_.setBounds (getLocalBounds()
                            .removeFromTop (kHeaderHeight + kContentHeight)
                            .removeFromBottom (kContentHeight));
}

} // namespace gui
