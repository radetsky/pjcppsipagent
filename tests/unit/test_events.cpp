#include <gtest/gtest.h>
#include "events.h"
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// Disconnect mapping table (PLAN.md M3)
// ---------------------------------------------------------------------------

TEST(EventsTest, SipCodeMappingTable) {
    EXPECT_EQ(sipCodeToCallStatus(200), CallStatus::ANSWERED);
    EXPECT_EQ(sipCodeToCallStatus(486), CallStatus::BUSY);
    EXPECT_EQ(sipCodeToCallStatus(600), CallStatus::BUSY);
    EXPECT_EQ(sipCodeToCallStatus(603), CallStatus::BUSY);
    EXPECT_EQ(sipCodeToCallStatus(408), CallStatus::NO_ANSWER);
    EXPECT_EQ(sipCodeToCallStatus(480), CallStatus::NO_ANSWER);
    EXPECT_EQ(sipCodeToCallStatus(487), CallStatus::NO_ANSWER);
    EXPECT_EQ(sipCodeToCallStatus(404), CallStatus::FAILED);
    EXPECT_EQ(sipCodeToCallStatus(500), CallStatus::FAILED);
    EXPECT_EQ(sipCodeToCallStatus(0),   CallStatus::FAILED);
}

TEST(EventsTest, ExitCodes) {
    EXPECT_EQ(statusToExitCode(CallStatus::ANSWERED),  0);
    EXPECT_EQ(statusToExitCode(CallStatus::BUSY),      3);
    EXPECT_EQ(statusToExitCode(CallStatus::NO_ANSWER), 4);
    EXPECT_EQ(statusToExitCode(CallStatus::FAILED),    5);
    EXPECT_EQ(statusToExitCode(CallStatus::CANCELLED), 6);
}

// ---------------------------------------------------------------------------
// PJSIP state constants (regression: INCOMING=2 was missing, shifting all
// values after it and breaking EARLY/CONFIRMED/DISCONNECTED matching)
// ---------------------------------------------------------------------------

TEST(EventsTest, InvStateValuesMatchPjsip) {
    EXPECT_EQ(INV_STATE_NULL, 0);
    EXPECT_EQ(INV_STATE_CALLING, 1);
    EXPECT_EQ(INV_STATE_INCOMING, 2);
    EXPECT_EQ(INV_STATE_EARLY, 3);
    EXPECT_EQ(INV_STATE_CONNECTING, 4);
    EXPECT_EQ(INV_STATE_CONFIRMED, 5);
    EXPECT_EQ(INV_STATE_DISCONNECTED, 6);
}

// ---------------------------------------------------------------------------
// JSON serialization: state vs result discrimination
// ---------------------------------------------------------------------------

TEST(EventsTest, AnsweredIsStateUnlessFlaggedResult) {
    AgentEvent state_ev{CallStatus::ANSWERED, "cid", "", 0, 0, ""};
    auto js = nlohmann::json::parse(state_ev.toJson());
    EXPECT_EQ(js["event"], "state");
    EXPECT_EQ(js["state"], "ANSWERED");

    AgentEvent result_ev{CallStatus::ANSWERED, "cid", "/r.wav", 12, 200, "", true};
    auto jr = nlohmann::json::parse(result_ev.toJson());
    EXPECT_EQ(jr["event"], "result");
    EXPECT_EQ(jr["status"], "ANSWERED");
    EXPECT_EQ(jr["billing_seconds"], 12);
    EXPECT_EQ(jr["recording"], "/r.wav");
}

TEST(EventsTest, FinalStatusesAlwaysResults) {
    for (auto s : {CallStatus::BUSY, CallStatus::NO_ANSWER,
                   CallStatus::CANCELLED, CallStatus::FAILED}) {
        AgentEvent ev{s, "cid", "", 0, 486, "x"};
        auto j = nlohmann::json::parse(ev.toJson());
        EXPECT_EQ(j["event"], "result") << callStatusToString(s);
    }
}

TEST(EventsTest, StateEventFields) {
    AgentEvent ev{CallStatus::RINGING, "cid", "", 0, 0, ""};
    auto j = nlohmann::json::parse(ev.toJson());
    EXPECT_EQ(j["event"], "state");
    EXPECT_EQ(j["state"], "RINGING");
    EXPECT_EQ(j["call_id"], "cid");
    EXPECT_TRUE(j.contains("ts"));
}

TEST(EventsTest, RecordingReadyEvent) {
    AgentEvent ev{CallStatus::RECORDING_READY, "cid", "/rec/a.wav", 7, 0, ""};
    auto j = nlohmann::json::parse(ev.toJson());
    EXPECT_EQ(j["event"], "recording_ready");
    EXPECT_EQ(j["uri"], "/rec/a.wav");
    EXPECT_EQ(j["duration"], 7);
}
