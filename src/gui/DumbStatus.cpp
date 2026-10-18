#include "gui/DumbStatus.h"

namespace gui
{

namespace
{
    juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }
}

DumbStatus dumbStatusFor (const DumbInputs& in)
{
    DumbStatus s;
    s.offOn       = in.bypassed;
    s.middleOn    = in.autoMode;
    s.leftEnabled = in.measureAllowed;

    // 1. A measurement owns the left button until it ends.
    if (in.phase == DumbInputs::Phase::Measuring)
    {
        s.lamp          = DumbStatus::Lamp::Cyan;
        s.pulse         = true;
        s.headline      = utf8 ("\xc4\x90\x61ng \xc4\x91o ph\xc3\xb2ng\xe2\x80\xa6");
        s.subline       = utf8 ("Loa s\xe1\xba\xbd ph\xc3\xa1t ti\xe1\xba\xbfng qu\xc3\xa9t. B\xe1\xba\xa5m D\xe1\xbb\xaaNG \xc4\x91\xe1\xbb\x83 ng\xe1\xbb\xabng ngay.");
        s.left          = DumbStatus::Left::Stop;
        s.leftEnabled   = true;
        s.middleEnabled = false;
        s.offEnabled    = false;
        return s;
    }

    // 2. Results wait for a decision; both left buttons become that decision.
    if (in.phase == DumbInputs::Phase::Results)
    {
        s.lamp     = DumbStatus::Lamp::Cyan;
        s.headline = in.resultCount > 0
                       ? utf8 ("T\xc3\xacm th\xe1\xba\xa5y ") + juce::String (in.resultCount)
                             + utf8 (" \xc4\x91i\xe1\xbb\x83m d\xe1\xbb\x85 h\xc3\xba")
                       : utf8 ("Kh\xc3\xb4ng th\xe1\xba\xa5y \xc4\x91i\xe1\xbb\x83m d\xe1\xbb\x85 h\xc3\xba");
        s.subline  = in.resultCount > 0
                       ? utf8 ("\xc3\x81P D\xe1\xbb\xa4NG \xc4\x91\xe1\xbb\x83 ch\xe1\xba\xb7n tr\xc6\xb0\xe1\xbb\x9b\x63, B\xe1\xbb\x8e \xc4\x91\xe1\xbb\x83 b\xe1\xbb\x8f qua.")
                       : utf8 ("B\xe1\xba\xa5m B\xe1\xbb\x8e \xc4\x91\xe1\xbb\x83 quay l\xe1\xba\xa1i.");
        s.left        = DumbStatus::Left::Apply;
        s.leftEnabled = in.resultCount > 0;
        s.middle      = DumbStatus::Middle::Dismiss;
        s.middleOn    = false;
        return s;
    }

    // 3. Applied: the report stays until BỎ (lane M Q20).
    if (in.phase == DumbInputs::Phase::Applied)
    {
        s.lamp        = DumbStatus::Lamp::Cyan;
        s.headline    = utf8 ("\xc4\x90\xc3\xa3 ch\xe1\xba\xb7n tr\xc6\xb0\xe1\xbb\x9b\x63 ")
                      + juce::String (in.resultCount) + utf8 (" \xc4\x91i\xe1\xbb\x83m");
        s.subline     = utf8 ("B\xe1\xba\xa5m B\xe1\xbb\x8e \xc4\x91\xe1\xbb\x83 \xc4\x91\xc3\xb3ng b\xc3\xa1o c\xc3\xa1o.");
        s.leftEnabled = false;
        s.middle      = DumbStatus::Middle::Dismiss;
        s.middleOn    = false;
        return s;
    }

    // 4. Nothing can be protected until the rig is set up. AFTER the three
    //    soundcheck rows, never before them (verification B-1): a run started
    //    in Geek on a slot other than 0 reads "not configured" here, and this
    //    row would hide DỪNG and leave TẮT live in the middle of a sweep. Only TẮT and the
    //    lamp (which opens the wizard) do anything.
    if (! in.configured)
    {
        s.lamp            = DumbStatus::Lamp::Grey;
        s.headline        = utf8 ("C\xe1\xba\xa7n c\xc3\xa0i \xc4\x91\xe1\xba\xb7t");
        s.subline         = utf8 ("Ch\xe1\xba\xa1m v\xc3\xa0o \xc4\x91\xc3\xa2y \xc4\x91\xe1\xbb\x83 ch\xe1\xbb\x8dn sound card, mic v\xc3\xa0 ng\xc3\xb5 ra.");
        s.lampOpensWizard = true;
        s.leftEnabled     = false;
        s.middleEnabled   = false;
        s.middleOn        = false;
        return s;
    }

    // 5. Bypassed: say plainly that nothing is protecting the room.
    if (in.bypassed)
    {
        s.lamp     = DumbStatus::Lamp::Grey;
        s.headline = utf8 ("\xc4\x90\x61ng T\xe1\xba\xae\x54");
        s.subline  = utf8 ("Kh\xc3\xb4ng ch\xe1\xbb\x91ng h\xc3\xba. B\xe1\xba\xa5m B\xe1\xba\xac\x54 CH\xe1\xbb\x90NG H\xc3\x9a \xc4\x91\xe1\xbb\x83 b\xe1\xba\xadt l\xe1\xba\xa1i.");
        return s;
    }

    // 6-10. Protecting (or passive Soundcheck). Worst news first.
    switch (in.risk)
    {
        case DumbInputs::Risk::Critical:
            s.lamp     = DumbStatus::Lamp::Red;
            s.headline = utf8 ("\xc4\x90\x61ng h\xc3\xba \xe2\x80\x94 \xc4\x91\x61ng x\xe1\xbb\xad l\xc3\xbd");
            s.subline  = utf8 ("N\xe1\xba\xbfu kh\xc3\xb4ng h\xe1\xba\xbft, h\xe1\xba\xa1 fader mic.");
            return s;

        case DumbInputs::Risk::Rising:
            s.lamp     = DumbStatus::Lamp::Amber;
            s.headline = utf8 ("S\xe1\xba\xafp h\xc3\xba");
            s.subline  = utf8 ("H\xe1\xbb\x87 th\xe1\xbb\x91ng \xc4\x91\x61ng theo d\xc3\xb5i.");
            return s;

        case DumbInputs::Risk::Unavailable:
            // No number from the detector (silence, a fresh reset): claiming
            // "safe" here would be a claim nothing measured.
            if (in.notchCount == 0)
            {
                s.lamp     = DumbStatus::Lamp::DimGreen;
                s.headline = utf8 ("\xc4\x90\x61ng ch\xe1\xbb\x91ng h\xc3\xba");
                s.subline  = utf8 ("Ch\xe1\xbb\x9d t\xc3\xadn hi\xe1\xbb\x87u t\xe1\xbb\xab mic.");
                return s;
            }
            break;

        case DumbInputs::Risk::Low:
            break;
    }

    s.lamp = DumbStatus::Lamp::Green;
    if (in.notchCount > 0)
    {
        s.headline = utf8 ("\xc4\x90\x61ng ch\xe1\xba\xb7n ") + juce::String (in.notchCount)
                   + utf8 (" \xc4\x91i\xe1\xbb\x83m");
        s.subline  = utf8 ("H\xe1\xbb\x87 th\xe1\xbb\x91ng \xc4\x91\x61ng ch\xe1\xbb\x91ng h\xc3\xba.");
    }
    else
    {
        s.headline = utf8 ("An to\xc3\xa0n");
        s.subline  = utf8 ("H\xe1\xbb\x87 th\xe1\xbb\x91ng \xc4\x91\x61ng ch\xe1\xbb\x91ng h\xc3\xba.");
    }
    return s;
}

} // namespace gui
