// tests/test_notchcontroller.cpp
#include <gtest/gtest.h>
#include "app/NotchController.h"

namespace {
class FakeClock : public ClockSource {
public:
    double nowMs() const override { return ms_; }
    void advance (double m) { ms_ += m; }
private:
    double ms_ = 1000.0;
};

struct Harness {
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    NotchController controller { tap, commands, clock };
};
}

TEST (NotchControllerValidation, RejectsBadParamsWithoutTouchingModelOrQueue)
{
    Harness h;
    EXPECT_FALSE (h.controller.setNotch (0, 0, 24000.0, 30.0, -12.0, NotchController::Origin::Detector)); // >= Nyquist of 48k
    EXPECT_FALSE (h.controller.setNotch (0, 0, 1000.0,  0.0, -12.0, NotchController::Origin::Detector));  // Q <= 0
    EXPECT_FALSE (h.controller.setNotch (0, 0, 1000.0, 30.0,  +3.0, NotchController::Origin::Detector));  // depth > 0
    EXPECT_FALSE (h.controller.setNotch (2, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));  // channel OOR
    EXPECT_FALSE (h.controller.setNotch (0, 16, 1000.0, 30.0, -12.0, NotchController::Origin::Detector)); // index OOR
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
}

TEST (NotchControllerCommands, SetNotchRecordsModelAndEnqueues)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (1, 3, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.runOnce();  // flush outbox
    ASSERT_EQ (h.commands.getAvailableRead(), 1u);
    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    EXPECT_EQ (cmd.channel, 1);
    EXPECT_EQ (cmd.index, 3);
    EXPECT_FLOAT_EQ (cmd.frequency, 1000.0f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
}

TEST (NotchControllerCommands, ClearEmitsClearCommandOnlyWhenActive)
{
    Harness h;
    h.controller.clearNotch (0, 5);   // nothing active -> nothing queued
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
    ASSERT_TRUE (h.controller.setNotch (0, 5, 800.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.clearNotch (0, 5);
    h.controller.runOnce();
    ASSERT_EQ (h.commands.getAvailableRead(), 2u);
    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Clear);
    EXPECT_EQ (cmd.index, 5);
}

TEST (NotchControllerCommands, FullQueueDelaysButNeverLoses)
{
    Harness h;
    for (int i = 0; i < 140; ++i)  // 140 > capacity 128
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 500.0 + i, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.runOnce();
    EXPECT_GT (h.controller.retryCount(), 0u);
    NotchCommand batch[160];
    const auto first  = h.commands.read (batch, 160);
    h.controller.runOnce();          // retry the unsent tail
    const auto second = h.commands.read (batch + first, 160);
    EXPECT_EQ (first + second, 140u);  // nothing lost
}

TEST (NotchControllerAutoRelease, NotchSurvivesDeadTapAfter30s)   // D-06
{
    Harness h;  // tap never fed => dead forever
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.clock.advance (31000.0);
    h.controller.runOnce();

    NotchCommand cmd {};
    std::size_t sets = 0, clears = 0;
    while (h.commands.read (&cmd, 1) == 1)
        (cmd.type == NotchCommandType::Clear ? clears : sets)++;
    EXPECT_EQ (sets, 1u);
    EXPECT_EQ (clears, 0u);
}

TEST (NotchControllerLiveClock, AlternatingEmptyPollsTrackWallTime)  // half-speed bug guard
{
    Harness h;
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 400; ++i) {
        if (i % 2 == 0)                       // every OTHER poll delivers data
            h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    EXPECT_NEAR (h.controller.liveMsForTest(), 2000.0, 60.0);
}

TEST (NotchControllerAutoRelease, LiveTapReleasesAfter30s)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 7000; ++i) {          // 7000 * 5 ms = 35 s of live audio
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    h.controller.runOnce();                   // make sure everything is flushed

    NotchCommand cmd {};
    bool sawClearOfSlot0 = false;
    while (h.commands.read (&cmd, 1) == 1)
        if (cmd.type == NotchCommandType::Clear && cmd.channel == 0 && cmd.index == 0)
            sawClearOfSlot0 = true;
    EXPECT_TRUE (sawClearOfSlot0);
}
