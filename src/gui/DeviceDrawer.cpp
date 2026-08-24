#include "gui/DeviceDrawer.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
// Distinct from ModeRail's mode radio group so the two never interfere.
constexpr int kLayoutRadioGroupId = 2;

// Header cell widths the theme does not tokenise (they are drawer-local
// metrics, not shared design tokens): the PERFORMANCE button is wider than
// CLASSIC to fit its longer label, and the status badge gets a fixed slot.
constexpr float kPerformanceCellWidth = 148.0f;
constexpr float kBadgeCellWidth       = 120.0f;
} // namespace

DeviceDrawer::DeviceDrawer (DevicePanel& wrappedPanel)
    : wrapped_ (wrappedPanel)
{
    using namespace az::theme;

    // THE re-parent: the same DevicePanel instance moves under this drawer,
    // once, forever. Nothing here may destroy or recreate it -- its restart
    // hooks are wired by MainComponent and must survive every layout switch.
    addAndMakeVisible (wrapped_);

    // U+2699 GEAR, built from its code point rather than a literal so the
    // source file stays pure ASCII end to end.
    gearButton_.setButtonText (juce::String (juce::juce_wchar (0x2699)));
    gearButton_.onClick = [this] { toggleOpen(); };
    gearButton_.setColour (juce::TextButton::buttonColourId, panel);
    addAndMakeVisible (gearButton_);

    classicButton_.setClickingTogglesState (true);
    performanceButton_.setClickingTogglesState (true);
    classicButton_.setRadioGroupId (kLayoutRadioGroupId);
    performanceButton_.setRadioGroupId (kLayoutRadioGroupId);

    // Same guard shape as ModeRail's mode cells: the radio group deselecting
    // the previous button fires that button too, so each handler acts only
    // when its OWN button ended up ON.
    classicButton_.onClick = [p = &classicButton_, this]
    {
        if (! p->getToggleState())
            return;
        if (onLayoutSelected != nullptr)
            onLayoutSelected (ScreenLayout::Classic);
    };
    performanceButton_.onClick = [p = &performanceButton_, this]
    {
        if (! p->getToggleState())
            return;
        if (onLayoutSelected != nullptr)
            onLayoutSelected (ScreenLayout::Performance);
    };

    addAndMakeVisible (classicButton_);
    addAndMakeVisible (performanceButton_);

    presetsPlaceholder_.setJustificationType (juce::Justification::centredRight);
    presetsPlaceholder_.setColour (juce::Label::textColourId, dim);
    presetsPlaceholder_.setFont (baseFont());
    addAndMakeVisible (presetsPlaceholder_);

    applyLayoutMode (ScreenLayout::Performance);
}

void DeviceDrawer::applyLayoutMode (ScreenLayout layout)
{
    collapsible_ = (layout == ScreenLayout::Performance);
    gearButton_.setVisible (collapsible_);
    // Both layouts start OPEN: the device row and the routing table beneath
    // it are the first things a soundman needs, and an invisible drawer reads
    // as "the app lost my interface". L2 stays collapsible via the gear.
    open_ = true;

    wrapped_.setVisible (open_);
    resized();
}

void DeviceDrawer::setOpen (bool shouldOpen)
{
    if (! collapsible_)
        return;   // the L1 top bar has nothing to collapse

    if (open_ == shouldOpen)
        return;

    open_ = shouldOpen;
    wrapped_.setVisible (open_);
    resized();

    // The PARENT sizes this drawer from getPreferredHeight(), so only our own
    // resized() leaves the content squeezed out until some unrelated window
    // resize -- the parent must re-run its layout too.
    if (auto* parent = getParentComponent())
        parent->resized();
}

void DeviceDrawer::toggleOpen()
{
    setOpen (! open_);
}

void DeviceDrawer::setSelectedLayout (ScreenLayout layout)
{
    const auto noNotify = juce::dontSendNotification;

    if (layout == ScreenLayout::Classic)
    {
        classicButton_.setToggleState (true, noNotify);
        performanceButton_.setToggleState (false, noNotify);
    }
    else
    {
        classicButton_.setToggleState (false, noNotify);
        performanceButton_.setToggleState (true, noNotify);
    }
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
    return kHeaderHeight + (open_ ? kContentHeight : 0);
}

void DeviceDrawer::resized()
{
    using namespace az::theme;

    auto headerArea = getLocalBounds().removeFromTop (kHeaderHeight).reduced (gap, 0);

    juce::FlexBox header;
    header.flexDirection = juce::FlexBox::Direction::row;
    header.alignItems    = juce::FlexBox::AlignItems::center;

    constexpr float cellH = (float) touchTarget;

    header.items.add (juce::FlexItem (classicButton_)
                          .withWidth ((float) buttonCellWidth).withHeight (cellH)
                          .withMargin ({ 0.0f, 0.0f, 0.0f, (float) gap }));
    header.items.add (juce::FlexItem (performanceButton_)
                          .withWidth (kPerformanceCellWidth).withHeight (cellH)
                          .withMargin ({ 0.0f, 0.0f, 0.0f, (float) gap }));
    header.items.add (juce::FlexItem (presetsPlaceholder_).withFlex (1.0f));

    if (badge_ != nullptr)
        header.items.add (juce::FlexItem (*badge_)
                              .withWidth (kBadgeCellWidth).withHeight (cellH)
                              .withMargin ({ 0.0f, 0.0f, 0.0f, (float) gap }));

    if (collapsible_)
        header.items.add (juce::FlexItem (gearButton_)
                              .withWidth ((float) touchTarget).withHeight (cellH)
                              .withMargin ({ 0.0f, 0.0f, 0.0f, (float) gap }));

    header.performLayout (headerArea);

    // Content sits under the header. While closed there IS no content area --
    // assigning a leftover rect here would hand the panel geometry it must
    // not paint or hit-test into.
    if (open_)
        wrapped_.setBounds (getLocalBounds()
                                .removeFromTop (kHeaderHeight + kContentHeight)
                                .removeFromBottom (kContentHeight));
    else
        wrapped_.setBounds ({});
}

} // namespace gui
