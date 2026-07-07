"""Agent dials SIPp UAS that rings forever; agent times out (--answer-timeout 8)
and sends CANCEL → 487 Request Terminated → NO_ANSWER."""

import grpc


def test_no_answer(proto_stubs, sipp_uas, agent_server, make_request):
    pb2, pb2_grpc = proto_stubs

    addr = sipp_uas("no_answer")
    grpc_port = agent_server(extra_args=["--answer-timeout", "8"])

    channel = grpc.insecure_channel(f"127.0.0.1:{grpc_port}")
    stub = pb2_grpc.CallAgentStub(channel)

    req = make_request(addr, tts_text="hello")

    events = list(stub.ExecuteCall(req, timeout=25))
    result_ev = next(ev for ev in events if ev.HasField("result"))
    recording_ev = [ev for ev in events if ev.HasField("recording_ready")]

    assert result_ev.result.status == pb2.CallResult.NO_ANSWER, \
        f"Expected NO_ANSWER, got {result_ev.result.status}"
    # 487 Request Terminated (from the CANCEL the agent sent after timeout)
    assert result_ev.result.sip_code == 487, \
        f"Expected sip_code 487, got {result_ev.result.sip_code}"
    assert not recording_ev, "Unexpected recording_ready on no-answer call"
