#include "gui/DeviceDrawer.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

DeviceDrawer::DeviceDrawer (DevicePanel& wrappedPanel)
    : wrapped_ (wrappedPanel)
{
    // THE re-parent: the same DevicePanel instance moves under this drawer,
    // once, forever. Nothing here may destroy or recreate it -- its restart
    // hooks are wired by MainComponent and must survive everything.
    addAndMakeVisible (wrapped_);
}

int DeviceDrawer::getPreferredHeight() const
{
    return kHeaderHeight + kContentHeight;
}

void DeviceDrawer::paint (juce::Graphics& g)
{
    using namespace az::theme;

    const auto header = getLocalBounds().removeFromTop (kHeaderHeight);

    drawCaption (g, "Interface", header, dim);
    drawEngravedDivider (g, header.withTrimmedBottom (spacing));
}

void DeviceDrawer::resized()
{
    // The drawer is always open: the content sits under the caption, forever.
    wrapped_.setBounds (getLocalBounds()
                            .removeFromTop (kHeaderHeight + kContentHeight)
                            .removeFromBottom (kContentHeight));
}

} // namespace gui
