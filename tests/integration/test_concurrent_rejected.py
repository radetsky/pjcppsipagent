"""Second ExecuteCall while the first is in progress gets ALREADY_EXISTS."""

import threading
import time

import grpc


def test_concurrent_rejected(proto_stubs, sipp_uas, agent_server, make_request):
    pb2, pb2_grpc = proto_stubs

    addr = sipp_uas("answer")
    grpc_port = agent_server()

    channel = grpc.insecure_channel(f"127.0.0.1:{grpc_port}")
    stub = pb2_grpc.CallAgentStub(channel)

    req1 = make_request(addr, tts_text="hello world", record=False)

    # Start first call in background
    first_events = []
    done = []

    def run_first():
        try:
            for ev in stub.ExecuteCall(req1, timeout=30):
                first_events.append(ev)
        except Exception:
            pass
        done.append(True)

    t = threading.Thread(target=run_first, daemon=True)
    t.start()
    time.sleep(2)  # let it register and start dialing

    # Second call — should be rejected
    req2 = make_request(addr, tts_text="second call", record=False)
    try:
        list(stub.ExecuteCall(req2, timeout=5))
        assert False, "Expected ALREADY_EXISTS"
    except grpc.RpcError as e:
        assert e.code() == grpc.StatusCode.ALREADY_EXISTS, \
            f"Expected ALREADY_EXISTS, got {e.code()}: {e.details()}"
