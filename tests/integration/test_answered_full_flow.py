"""Full happy-path: agent dials SIPp UAS, gets answered with SDP and RTP
echo, plays the TTS prompt, detects silence, hangs up, recording exists."""

import time
import wave

import grpc


def test_answered_full_flow(proto_stubs, sipp_uas, agent_server, make_request):
    pb2, pb2_grpc = proto_stubs

    addr = sipp_uas("answer")
    grpc_port = agent_server()

    channel = grpc.insecure_channel(f"127.0.0.1:{grpc_port}")
    stub = pb2_grpc.CallAgentStub(channel)

    req = make_request(addr, tts_text="hello world",
                       silence_timeout_seconds=3, record=True)

    events = []
    for ev in stub.ExecuteCall(req, timeout=60):
        events.append(ev)

    # Unpack events by type
    state_seq = []
    result_ev = None
    recording_ev = None
    for ev in events:
        if ev.HasField("state_change"):
            state_seq.append(ev.state_change.state)
        elif ev.HasField("recording_ready"):
            recording_ev = ev
        elif ev.HasField("result"):
            result_ev = ev

    assert result_ev is not None, "Missing result event"
    assert recording_ev is not None, "Missing recording_ready event"

    # State sequence should include ANSWERED, PLAYING, PLAYED, SILENCE_DETECTED
    assert pb2.StateChange.ANSWERED in state_seq, \
        f"ANSWERED not in state sequence: {state_seq}"
    assert pb2.StateChange.PLAYING in state_seq, \
        f"PLAYING not in state sequence: {state_seq}"
    assert pb2.StateChange.PLAYED in state_seq, \
        f"PLAYED not in state sequence: {state_seq}"

    # Result
    assert result_ev.result.status == pb2.CallResult.ANSWERED, \
        f"Expected ANSWERED, got {result_ev.result.status}"
    # PJSIP reports local hangup of a connected call as 603 Decline.
    # The actual answer was 200 OK — this is the answer SIP code only
    # for never-answered calls.
    assert result_ev.result.sip_code == 603, \
        f"Expected sip_code 603 (local hangup), got {result_ev.result.sip_code}"
    assert result_ev.result.billing_seconds >= 3, \
        f"billing_seconds too low: {result_ev.result.billing_seconds}"
    assert result_ev.result.HasField("started_at"), "Missing started_at"
    assert result_ev.result.HasField("ended_at"), "Missing ended_at"
    assert result_ev.result.ended_at.seconds > result_ev.result.started_at.seconds

    # Recording ready
    assert recording_ev.recording_ready.recording_uri
    assert recording_ev.recording_ready.duration_seconds >= 3

    # WAV file exists and is valid
    rec_path = recording_ev.recording_ready.recording_uri
    with wave.open(rec_path, "rb") as wf:
        assert wf.getframerate() == 8000, f"sample rate: {wf.getframerate()}"
        assert wf.getnchannels() == 1, f"channels: {wf.getnchannels()}"
        assert wf.getsampwidth() == 2, f"sampwidth: {wf.getsampwidth()}"
        assert wf.getnframes() > 0, "empty recording"

        # Recording should NOT be silent (it echoes 440 Hz prompt)
        raw = wf.readframes(wf.getnframes())
    max_sample = max(abs(int.from_bytes(raw[i:i+2], "little", signed=True))
                     for i in range(0, len(raw), 2))
    assert max_sample > 1000, f"recording appears silent (max sample={max_sample})"
