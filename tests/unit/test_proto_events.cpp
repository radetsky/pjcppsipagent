#include <gtest/gtest.h>
#include "events.h"
#include <google/protobuf/timestamp.pb.h>
#include "call_agent.pb.h"
#include <chrono>

namespace v1 = automatedcalls::callagent::v1;

// ---------------------------------------------------------------------------
// Replicate the mapping helpers from grpc_server.cpp
// ---------------------------------------------------------------------------

static uint64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static void setTimestamp(google::protobuf::Timestamp* ts, uint64_t ms) {
    ts->set_seconds(static_cast<int64_t>(ms / 1000));
    ts->set_nanos(static_cast<int32_t>((ms % 1000) * 1000000));
}

static v1::StateChange::State toProtoState(CallStatus s) {
    switch (s) {
        case CallStatus::REGISTERING: return v1::StateChange::REGISTERING;
        case CallStatus::REGISTERED:  return v1::StateChange::REGISTERED;
        case CallStatus::DIALING:     return v1::StateChange::DIALING;
        case CallStatus::RINGING:     return v1::StateChange::RINGING;
        case CallStatus::ANSWERED:    return v1::StateChange::ANSWERED;
        case CallStatus::PLAYING:     return v1::StateChange::PLAYING;
        case CallStatus::PLAYED:      return v1::StateChange::PLAYED;
        case CallStatus::HANGING_UP:  return v1::StateChange::HANGING_UP;
        default:                      return v1::StateChange::STATE_UNSPECIFIED;
    }
}

static v1::CallResult::Status toProtoResultStatus(CallStatus s) {
    switch (s) {
        case CallStatus::ANSWERED:  return v1::CallResult::ANSWERED;
        case CallStatus::BUSY:      return v1::CallResult::BUSY;
        case CallStatus::NO_ANSWER: return v1::CallResult::NO_ANSWER;
        case CallStatus::CANCELLED: return v1::CallResult::CANCELLED;
        default:                    return v1::CallResult::FAILED;
    }
}

static v1::CallEvent toProtoEvent(const AgentEvent& ev, const std::string& call_id,
                                  uint64_t started_at_ms) {
    v1::CallEvent out;
    out.set_call_id(call_id);
    uint64_t now = epochMs();
    setTimestamp(out.mutable_occurred_at(), now);

    bool is_final = ev.is_result ||
                    ev.status == CallStatus::BUSY ||
                    ev.status == CallStatus::NO_ANSWER ||
                    ev.status == CallStatus::CANCELLED ||
                    ev.status == CallStatus::FAILED;
    if (is_final) {
        auto* res = out.mutable_result();
        res->set_status(toProtoResultStatus(ev.status));
        res->set_billing_seconds(ev.billing_seconds);
        res->set_sip_code(static_cast<uint32_t>(ev.sip_code));
        res->set_error_message(ev.reason);
        res->set_recording_uri(ev.recording_path);
        if (started_at_ms > 0) setTimestamp(res->mutable_started_at(), started_at_ms);
        setTimestamp(res->mutable_ended_at(), now);
    } else if (ev.status == CallStatus::SILENCE_DETECTED) {
        out.mutable_silence_detected()->set_silence_seconds(ev.billing_seconds);
    } else if (ev.status == CallStatus::RECORDING_READY) {
        auto* rec = out.mutable_recording_ready();
        rec->set_recording_uri(ev.recording_path);
        rec->set_duration_seconds(ev.billing_seconds);
    } else {
        out.mutable_state_change()->set_state(toProtoState(ev.status));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(ProtoEventsTest, StateChangeSetsCorrectOneof) {
    AgentEvent ev{CallStatus::RINGING, "cid", "", 0, 0, ""};
    auto proto = toProtoEvent(ev, "call-001", 0);

    EXPECT_EQ(proto.call_id(), "call-001");
    EXPECT_TRUE(proto.has_occurred_at());
    EXPECT_GT(proto.occurred_at().seconds(), 0);

    // oneof case
    EXPECT_TRUE(proto.has_state_change());
    EXPECT_FALSE(proto.has_silence_detected());
    EXPECT_FALSE(proto.has_recording_ready());
    EXPECT_FALSE(proto.has_result());

    EXPECT_EQ(proto.state_change().state(), v1::StateChange::RINGING);
}

TEST(ProtoEventsTest, EveryStateMapsToCorrectProtoState) {
    struct { CallStatus s; v1::StateChange::State expected; } cases[] = {
        {CallStatus::REGISTERING, v1::StateChange::REGISTERING},
        {CallStatus::REGISTERED,  v1::StateChange::REGISTERED},
        {CallStatus::DIALING,     v1::StateChange::DIALING},
        {CallStatus::RINGING,     v1::StateChange::RINGING},
        {CallStatus::ANSWERED,    v1::StateChange::ANSWERED},
        {CallStatus::PLAYING,     v1::StateChange::PLAYING},
        {CallStatus::PLAYED,      v1::StateChange::PLAYED},
        {CallStatus::HANGING_UP,  v1::StateChange::HANGING_UP},
    };
    for (auto c : cases) {
        AgentEvent ev{c.s, "cid", "", 0, 0, ""};
        auto proto = toProtoEvent(ev, "cid", 0);
        ASSERT_TRUE(proto.has_state_change()) << callStatusToString(c.s);
        EXPECT_EQ(proto.state_change().state(), c.expected) << callStatusToString(c.s);
    }
}

TEST(ProtoEventsTest, DisconnectedMapsToUnspecifiedStateChange) {
    // DISCONNECTED is not a final/special event in the mapping — it falls
    // into the else branch and gets STATE_UNSPECIFIED.
    AgentEvent ev{CallStatus::DISCONNECTED, "cid", "", 0, 0, ""};
    auto proto = toProtoEvent(ev, "cid", 0);

    EXPECT_TRUE(proto.has_state_change());
    EXPECT_EQ(proto.state_change().state(), v1::StateChange::STATE_UNSPECIFIED);
    EXPECT_FALSE(proto.has_result());
    EXPECT_FALSE(proto.has_silence_detected());
    EXPECT_FALSE(proto.has_recording_ready());
}

TEST(ProtoEventsTest, FinalAndSpecialEventsDontBecomeStateChange) {
    for (auto s : {CallStatus::BUSY, CallStatus::NO_ANSWER, CallStatus::CANCELLED,
                   CallStatus::FAILED, CallStatus::SILENCE_DETECTED,
                   CallStatus::RECORDING_READY}) {
        AgentEvent ev{s, "cid", "", 0, 0, ""};
        auto proto = toProtoEvent(ev, "cid", 0);
        EXPECT_FALSE(proto.has_state_change()) << callStatusToString(s);
    }
}

TEST(ProtoEventsTest, SilenceDetectedSetsCorrectOneof) {
    AgentEvent ev{CallStatus::SILENCE_DETECTED, "cid", "", 10, 0, ""};
    auto proto = toProtoEvent(ev, "call-001", 0);

    EXPECT_TRUE(proto.has_silence_detected());
    EXPECT_FALSE(proto.has_state_change());
    EXPECT_FALSE(proto.has_recording_ready());
    EXPECT_FALSE(proto.has_result());

    EXPECT_EQ(proto.silence_detected().silence_seconds(), 10u);
}

TEST(ProtoEventsTest, RecordingReadySetsCorrectOneof) {
    AgentEvent ev{CallStatus::RECORDING_READY, "cid", "/rec/call.wav", 7, 0, ""};
    auto proto = toProtoEvent(ev, "call-001", 0);

    EXPECT_TRUE(proto.has_recording_ready());
    EXPECT_FALSE(proto.has_state_change());
    EXPECT_FALSE(proto.has_silence_detected());
    EXPECT_FALSE(proto.has_result());

    EXPECT_EQ(proto.recording_ready().recording_uri(), "/rec/call.wav");
    EXPECT_EQ(proto.recording_ready().duration_seconds(), 7u);
}

TEST(ProtoEventsTest, ResultSetsCorrectOneofWithAllFields) {
    AgentEvent ev{CallStatus::ANSWERED, "cid", "/rec/r.wav", 42, 200, "BYE", true};
    uint64_t started = epochMs() - 50000; // 50s ago
    auto proto = toProtoEvent(ev, "call-001", started);

    EXPECT_TRUE(proto.has_result());
    EXPECT_FALSE(proto.has_state_change());
    EXPECT_FALSE(proto.has_silence_detected());
    EXPECT_FALSE(proto.has_recording_ready());

    auto& res = proto.result();
    EXPECT_EQ(res.status(), v1::CallResult::ANSWERED);
    EXPECT_EQ(res.billing_seconds(), 42u);
    EXPECT_EQ(res.sip_code(), 200u);
    EXPECT_EQ(res.error_message(), "BYE");
    EXPECT_EQ(res.recording_uri(), "/rec/r.wav");

    // started_at < ended_at
    EXPECT_TRUE(res.has_started_at());
    EXPECT_TRUE(res.has_ended_at());
    EXPECT_LT(res.started_at().seconds(), res.ended_at().seconds());
}

TEST(ProtoEventsTest, EveryFinalStatusMapsCorrectly) {
    struct { CallStatus s; v1::CallResult::Status expected; } cases[] = {
        {CallStatus::ANSWERED,  v1::CallResult::ANSWERED},
        {CallStatus::BUSY,      v1::CallResult::BUSY},
        {CallStatus::NO_ANSWER, v1::CallResult::NO_ANSWER},
        {CallStatus::CANCELLED, v1::CallResult::CANCELLED},
        {CallStatus::FAILED,    v1::CallResult::FAILED},
    };
    for (auto c : cases) {
        AgentEvent ev{c.s, "cid", "", 0, 0, "", true};
        auto proto = toProtoEvent(ev, "cid", 0);
        ASSERT_TRUE(proto.has_result()) << callStatusToString(c.s);
        EXPECT_EQ(proto.result().status(), c.expected) << callStatusToString(c.s);
    }
}

TEST(ProtoEventsTest, ResultWithoutStartedAtOmitsStartField) {
    AgentEvent ev{CallStatus::BUSY, "cid", "", 0, 486, ""};
    auto proto = toProtoEvent(ev, "cid", 0);

    ASSERT_TRUE(proto.has_result());
    EXPECT_FALSE(proto.result().has_started_at()) << "no started_at when 0 passed";
    EXPECT_TRUE(proto.result().has_ended_at());
}

TEST(ProtoEventsTest, TimestampsAreReasonable) {
    uint64_t before = epochMs();
    AgentEvent ev{CallStatus::REGISTERING, "cid", "", 0, 0, ""};
    auto proto = toProtoEvent(ev, "cid", 0);
    uint64_t after = epochMs();

    uint64_t occurred = static_cast<uint64_t>(proto.occurred_at().seconds()) * 1000 +
                        proto.occurred_at().nanos() / 1000000;

    EXPECT_GE(occurred, before);
    EXPECT_LE(occurred, after);
}
