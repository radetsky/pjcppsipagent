#!/usr/bin/env python3
"""Integration test for pjcppagent gRPC server."""

import os, sys, time, subprocess, threading

HERE = os.path.dirname(__file__)
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))

venv = "/tmp/pjtest_venv"
sys.path = [f"{venv}/lib/python3.14/site-packages"] + sys.path

from grpc_tools import protoc

# Generate Python stubs
proto_path = os.path.join(REPO, "protos")
out_path = "/tmp/pjtest_stubs"
os.makedirs(out_path, exist_ok=True)
grpc_tools_dir = os.path.dirname(protoc.__file__)
proto_include = os.path.join(grpc_tools_dir, "_proto")
protoc.main([
    "protoc",
    f"-I{proto_path}",
    f"-I{proto_include}",
    f"--python_out={out_path}",
    f"--grpc_python_out={out_path}",
    os.path.join(proto_path, "call_agent.proto"),
])

sys.path.insert(0, out_path)
import call_agent_pb2 as pb2
import call_agent_pb2_grpc as pb2_grpc
import grpc


def make_req(call_id="test-call", dest="1234", text="hello"):
    req = pb2.CallRequest(call_id=call_id, destination=dest, tts_text=text)
    req.sip.username = "user"
    req.sip.password = "pass"
    req.sip.server_host = "192.0.2.1"
    return req


def main():
    server = subprocess.Popen(
        [os.path.join(REPO, "build/pjcppagent"),
         "--mode", "server", "--idle-shutdown", "60"],
        env={"AGENT_SIP_PASS": "secret", **os.environ},
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(2)

    try:
        chan = grpc.insecure_channel("localhost:50051")
        grpc.channel_ready_future(chan).result(timeout=5)
        stub = pb2_grpc.CallAgentStub(chan)
        print("=== Connected ===")

        # 1. Health
        resp = stub.Health(pb2.HealthRequest())
        assert resp.state == pb2.HealthResponse.READY, f"Health: {resp.state}"
        assert resp.agent_version == "0.1.0"
        print("PASS test_health")

        # 2. ExecuteCall (run in bg thread, wait for first event)
        first_events = []
        def run_first_call():
            for ev in stub.ExecuteCall(make_req("call-1"), timeout=20):
                first_events.append(ev)
        t = threading.Thread(target=run_first_call, daemon=True)
        t.start()
        time.sleep(2)  # wait for REGISTERING event
        assert len(first_events) >= 1, "No initial events"
        assert first_events[0].HasField("state_change")
        assert first_events[0].state_change.state == pb2.StateChange.REGISTERING
        assert first_events[0].call_id == "call-1"
        print("PASS test_execute_call (first event REGISTERING)")

        # 3. ALREADY_EXISTS (concurrent with first call)
        try:
            list(stub.ExecuteCall(make_req("call-2", "5678"), timeout=5))
            print("FAIL: expected ALREADY_EXISTS")
            return 1
        except grpc.RpcError as e:
            assert e.code() == grpc.StatusCode.ALREADY_EXISTS, \
                f"Expected ALREADY_EXISTS, got {e.code()}"
            print("PASS test_already_exists")

        # 4. Wait for first call to finish, check result
        t.join()
        result_ev = [ev for ev in first_events if ev.HasField("result")]
        assert len(result_ev) == 1, f"Expected 1 result event, got {len(result_ev)}"
        assert result_ev[0].result.status == pb2.CallResult.FAILED
        assert "registration timeout" in result_ev[0].result.error_message
        print("PASS test_execute_call (result FAILED)")

        # 5. Post-call Health
        resp = stub.Health(pb2.HealthRequest())
        assert resp.state == pb2.HealthResponse.READY
        print("PASS post-call health")

        print("\n=== ALL PASS ===")
    except Exception as e:
        print(f"\nFAIL: {e}")
        return 1
    finally:
        server.terminate()
        server.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
