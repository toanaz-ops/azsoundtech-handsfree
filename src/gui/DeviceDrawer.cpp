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

    // The PRESET row. The buttons only REPORT the request; MainComponent owns
    // the chooser and the load/save. Guard the callback so an unwired drawer
    // (a bare component in a test) is a no-op rather than a crash.
    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);
    loadButton.onClick = [this] { if (onLoadRequested) onLoadRequested(); };
    saveButton.onClick = [this] { if (onSaveRequested) onSaveRequested(); };
}

int DeviceDrawer::getPreferredHeight() const
{
    return kHeaderHeight + kContentHeight + kPresetCaptionHeight + kPresetRowHeight;
}

void DeviceDrawer::paint (juce::Graphics& g)
{
    using namespace az::theme;

    auto area = getLocalBounds();

    const auto header = area.removeFromTop (kHeaderHeight);
    drawCaption (g, "Interface", header, dim);
    drawEngravedDivider (g, header.withTrimmedBottom (spacing));

    // Skip the wrapped device panel's band, then the PRESET caption.
    area.removeFromTop (kContentHeight);
    const auto presetHeader = area.removeFromTop (kPresetCaptionHeight);
    drawCaption (g, "Preset", presetHeader, dim);
    drawEngravedDivider (g, presetHeader.withTrimmedBottom (spacing));
}

void DeviceDrawer::resized()
{
    using namespace az::theme;

    // The drawer is always open: Interface caption, the device combos, then the
    // PRESET caption and its two buttons -- top to bottom, forever.
    auto area = getLocalBounds();

    area.removeFromTop (kHeaderHeight);
    wrapped_.setBounds (area.removeFromTop (kContentHeight));

    area.removeFromTop (kPresetCaptionHeight);
    auto row = area.removeFromTop (kPresetRowHeight);

    // Two equal buttons with the standard cell gap between them.
    const int half = (row.getWidth() - gap) / 2;
    loadButton.setBounds (row.removeFromLeft (half));
    row.removeFromLeft (gap);
    saveButton.setBounds (row);
}

} // namespace gui
