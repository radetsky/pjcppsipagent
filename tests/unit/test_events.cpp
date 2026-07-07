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

TEST(EventsTest, SipCodeMappingEveryRecognisedCode) {
    // Document every SIP code the switch handles (regression safeguard)
    struct { int code; CallStatus expected; } cases[] = {
        {200, CallStatus::ANSWERED},
        {486, CallStatus::BUSY}, {600, CallStatus::BUSY}, {603, CallStatus::BUSY},
        {408, CallStatus::NO_ANSWER}, {480, CallStatus::NO_ANSWER}, {487, CallStatus::NO_ANSWER},
    };
    for (auto c : cases) {
        EXPECT_EQ(sipCodeToCallStatus(c.code), c.expected) << "code=" << c.code;
    }
}

TEST(EventsTest, SipCodeUnmappedCodesFallToFailed) {
    // All other SIP 4xx/5xx/6xx codes that are NOT in the explicit mapping
    for (int code : {401, 403, 404, 405, 406, 410, 412, 413, 414, 415, 416,
                     420, 421, 422, 423, 430, 433, 481, 482, 483, 484, 485,
                     488, 489, 490, 491, 493, 494, 500, 501, 502, 503, 504,
                     505, 513, 580, 604, 606}) {
        EXPECT_EQ(sipCodeToCallStatus(code), CallStatus::FAILED) << "code=" << code;
    }
}

TEST(EventsTest, ExitCodes) {
    EXPECT_EQ(statusToExitCode(CallStatus::ANSWERED),  0);
    EXPECT_EQ(statusToExitCode(CallStatus::BUSY),      3);
    EXPECT_EQ(statusToExitCode(CallStatus::NO_ANSWER), 4);
    EXPECT_EQ(statusToExitCode(CallStatus::FAILED),    5);
    EXPECT_EQ(statusToExitCode(CallStatus::CANCELLED), 6);
    EXPECT_EQ(statusToExitCode(CallStatus::REGISTERING), 1);
    EXPECT_EQ(statusToExitCode(CallStatus::DISCONNECTED), 1);
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

// ---------------------------------------------------------------------------
// Every state produces the correct JSON event type and stable field names
// ---------------------------------------------------------------------------

struct StateTestCase {
    CallStatus status;
    const char* expect_event;
    const char* expect_state_or_status; // "state" field for state events
    bool is_final;
};

TEST(EventsTest, AllStatesProduceCorrectJson) {
    StateTestCase cases[] = {
        {CallStatus::REGISTERING,      "state", "REGISTERING",      false},
        {CallStatus::REGISTERED,       "state", "REGISTERED",       false},
        {CallStatus::DIALING,          "state", "DIALING",          false},
        {CallStatus::RINGING,          "state", "RINGING",          false},
        {CallStatus::ANSWERED,         "state", "ANSWERED",         false},
        {CallStatus::PLAYING,          "state", "PLAYING",          false},
        {CallStatus::PLAYED,           "state", "PLAYED",           false},
        {CallStatus::SILENCE_DETECTED, "state", "SILENCE_DETECTED", false},
        {CallStatus::RECORDING_READY,  "recording_ready", nullptr,   false},
        {CallStatus::HANGING_UP,       "state", "HANGING_UP",       false},
        {CallStatus::DISCONNECTED,     "state", "DISCONNECTED",     false},
        {CallStatus::BUSY,             "result", "BUSY",             true},
        {CallStatus::NO_ANSWER,        "result", "NO_ANSWER",        true},
        {CallStatus::CANCELLED,        "result", "CANCELLED",        true},
        {CallStatus::FAILED,           "result", "FAILED",           true},
        {CallStatus::ANSWERED,         "result", "ANSWERED",         true}, // flagged
    };

    for (auto c : cases) {
        bool is_res = c.is_final;
        AgentEvent ev{c.status, "test-call", "/rec", 5, 486, "reason", is_res};
        auto j = nlohmann::json::parse(ev.toJson());

        EXPECT_EQ(j["event"], c.expect_event) << callStatusToString(c.status);
        EXPECT_EQ(j["call_id"], "test-call") << callStatusToString(c.status);
        EXPECT_TRUE(j.contains("ts")) << callStatusToString(c.status);
        EXPECT_FALSE(j["ts"].empty()) << callStatusToString(c.status);

        if (std::string(c.expect_event) == "state") {
            EXPECT_EQ(j["state"], c.expect_state_or_status) << callStatusToString(c.status);
        } else if (std::string(c.expect_event) == "result") {
            EXPECT_EQ(j["status"], c.expect_state_or_status) << callStatusToString(c.status);
            EXPECT_EQ(j["sip_code"], 486) << callStatusToString(c.status);
            EXPECT_EQ(j["reason"], "reason") << callStatusToString(c.status);
            EXPECT_EQ(j["billing_seconds"], 5) << callStatusToString(c.status);
            EXPECT_EQ(j["recording"], "/rec") << callStatusToString(c.status);
        } else if (std::string(c.expect_event) == "recording_ready") {
            EXPECT_EQ(j["uri"], "/rec") << callStatusToString(c.status);
            EXPECT_EQ(j["duration"], 5) << callStatusToString(c.status);
        }
    }
}

TEST(EventsTest, ResultEventOmitsEmptyFields) {
    AgentEvent ev{CallStatus::BUSY, "cid", "", 0, 486, ""};
    auto j = nlohmann::json::parse(ev.toJson());
    EXPECT_FALSE(j.contains("reason"));
    EXPECT_FALSE(j.contains("recording"));
}
