"""Agent dials SIPp UAS that replies 486 Busy Here."""

import grpc


def test_busy(proto_stubs, sipp_uas, agent_server, make_request):
    pb2, pb2_grpc = proto_stubs

    addr = sipp_uas("busy")
    grpc_port = agent_server()

    channel = grpc.insecure_channel(f"127.0.0.1:{grpc_port}")
    stub = pb2_grpc.CallAgentStub(channel)

    req = make_request(addr, tts_text="hello")

    events = list(stub.ExecuteCall(req, timeout=30))
    result_ev = next(ev for ev in events if ev.HasField("result"))
    recording_ev = [ev for ev in events if ev.HasField("recording_ready")]

    assert result_ev.result.status == pb2.CallResult.BUSY, \
        f"Expected BUSY, got {result_ev.result.status}"
    assert result_ev.result.sip_code == 486, \
        f"Expected sip_code 486, got {result_ev.result.sip_code}"
    assert result_ev.result.billing_seconds == 0, \
        f"Expected 0 billing_seconds for busy, got {result_ev.result.billing_seconds}"
    # No recording for busy
    assert not recording_ev, "Unexpected recording_ready on busy call"
