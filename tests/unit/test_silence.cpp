#include <gtest/gtest.h>
#include "silence.h"

// ---------------------------------------------------------------------------
// Not armed -> never triggers
// ---------------------------------------------------------------------------

TEST(SilenceTest, NotArmedNeverTriggers) {
    SilenceDetector sd(10, 1000);
    // Without calling arm(), feed should never return true
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(sd.feed(0, i * 100));
    }
}

// ---------------------------------------------------------------------------
// Loud audio resets the window
// ---------------------------------------------------------------------------

TEST(SilenceTest, LoudAudioResetsWindow) {
    SilenceDetector sd(10, 1000);
    sd.arm();

    // 300ms of silence
    EXPECT_FALSE(sd.feed(0, 100));
    EXPECT_FALSE(sd.feed(0, 200));
    EXPECT_FALSE(sd.feed(0, 300));

    // Loud audio resets
    EXPECT_FALSE(sd.feed(255, 400));
    // Silence starts over from 500
    EXPECT_FALSE(sd.feed(0, 500));
    EXPECT_FALSE(sd.feed(0, 600));

    // After 1000ms from 500, should trigger at 1500
    EXPECT_FALSE(sd.feed(0, 1000));
    EXPECT_FALSE(sd.feed(0, 1300));
    EXPECT_TRUE(sd.feed(0, 1500));
}

// ---------------------------------------------------------------------------
// Exact boundary: 9.9s gap does NOT trigger with 10s timeout
// ---------------------------------------------------------------------------

TEST(SilenceTest, BoundaryJustBelowDoesNotTrigger) {
    SilenceDetector sd(10, 10000); // 10s timeout
    sd.arm();

    uint64_t t = 1000;
    // Feed silence at 900ms intervals for 9.9s total
    for (int i = 0; i < 11; ++i) {
        EXPECT_FALSE(sd.feed(0, t));
        t += 900;
    }
    // At this point ~9.9s of silence have passed -> should NOT trigger yet
    uint64_t now = t;
    EXPECT_FALSE(sd.feed(0, now + 50)); // 9.95s total
}

// ---------------------------------------------------------------------------
// Exact boundary: 10.0s gap DOES trigger with 10s timeout
// ---------------------------------------------------------------------------

TEST(SilenceTest, BoundaryExactTriggers) {
    SilenceDetector sd(10, 10000); // 10s timeout
    sd.arm();

    // First feed starts the silence timer
    EXPECT_FALSE(sd.feed(0, 0));
    // 10s later -> trigger
    EXPECT_TRUE(sd.feed(0, 10000));
}

// ---------------------------------------------------------------------------
// Triggers only once
// ---------------------------------------------------------------------------

TEST(SilenceTest, TriggersOnlyOnce) {
    SilenceDetector sd(10, 1000);
    sd.arm();

    // First feed starts the silence timer
    EXPECT_FALSE(sd.feed(0, 0));
    // Trigger at 1000ms
    EXPECT_TRUE(sd.feed(0, 1000));
    // Subsequent calls should return false (disarmed)
    EXPECT_FALSE(sd.feed(0, 2000));
    EXPECT_FALSE(sd.feed(0, 3000));
    EXPECT_FALSE(sd.feed(0, 4000));
}

// ---------------------------------------------------------------------------
// Re-arm works
// ---------------------------------------------------------------------------

TEST(SilenceTest, ReArmWorks) {
    SilenceDetector sd(10, 1000);
    sd.arm();

    // First trigger
    EXPECT_FALSE(sd.feed(0, 0));
    EXPECT_TRUE(sd.feed(0, 1000));
    EXPECT_FALSE(sd.feed(0, 2000)); // disarmed

    // Re-arm
    sd.arm();
    // Should trigger again after timeout
    EXPECT_FALSE(sd.feed(0, 3000));
    EXPECT_TRUE(sd.feed(0, 4000));
}

// ---------------------------------------------------------------------------
// Threshold: level just below threshold is silence, level >= threshold is audio
// ---------------------------------------------------------------------------

TEST(SilenceTest, ThresholdBoundary) {
    SilenceDetector sd(100, 1000);
    sd.arm();

    // Level 99 (below threshold) is silence -> accumulates
    EXPECT_FALSE(sd.feed(99, 100));
    EXPECT_FALSE(sd.feed(99, 200));
    // Level 100 (== threshold) is audio -> resets
    EXPECT_FALSE(sd.feed(100, 300));
    // Level 101 (> threshold) is also audio -> resets
    EXPECT_FALSE(sd.feed(101, 400));
    // Now silence from 500
    EXPECT_FALSE(sd.feed(99, 500));
    EXPECT_FALSE(sd.feed(99, 1000));
    EXPECT_TRUE(sd.feed(99, 1500));
}
