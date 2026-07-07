"""Client cancels the ExecuteCall RPC after ANSWERED."""

import threading
import time

import grpc


def test_cancel(proto_stubs, sipp_uas, agent_server, make_request):
    pb2, pb2_grpc = proto_stubs

    addr = sipp_uas("answer")
    grpc_port = agent_server()

    channel = grpc.insecure_channel(f"127.0.0.1:{grpc_port}")
    stub = pb2_grpc.CallAgentStub(channel)

    req = make_request(addr, tts_text="hello world",
                       silence_timeout_seconds=60, record=False)

    events = []
    call_ready = threading.Event()
    call_ref = [None]

    def collect():
        call_obj = stub.ExecuteCall(req, timeout=60)
        call_ref[0] = call_obj
        call_ready.set()
        try:
            for ev in call_obj:
                events.append(ev)
        except grpc.RpcError:
            pass

    t = threading.Thread(target=collect, daemon=True)
    t.start()

    # Wait until we see ANSWERED, then cancel via call.cancel()
    deadline = time.time() + 20
    answered_at = None
    while time.time() < deadline:
        for ev in events:
            if ev.HasField("state_change") and \
               ev.state_change.state == pb2.StateChange.ANSWERED:
                answered_at = time.time()
                break
        if answered_at:
            break
        time.sleep(0.1)

    assert answered_at is not None, "Never saw ANSWERED event before cancel"

    # Cancel via gRPC call.cancel(). This sends cancellation to the server
    # which detects context->IsCancelled() and cancels the SIP call.
    call_ready.wait(timeout=5)
    call_ref[0].cancel()
    t.join(timeout=15)

    state_seq = [ev.state_change.state for ev in events if ev.HasField("state_change")]
    result_ev = next((ev for ev in events if ev.HasField("result")), None)

    # The call was answered — ANSWERED in state sequence
    assert pb2.StateChange.ANSWERED in state_seq, \
        f"ANSWERED not seen: {state_seq}"

    if result_ev is not None:
        assert result_ev.result.status in (
            pb2.CallResult.CANCELLED, pb2.CallResult.ANSWERED,
        ), f"Unexpected result status: {result_ev.result.status}"
