"""pytest fixtures for pjcppagent integration tests."""

import os, re, subprocess, sys, tempfile, threading, time
from pathlib import Path
from typing import Iterator, Optional

import grpc
import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# ---------------------------------------------------------------------------
# Port pool
# ---------------------------------------------------------------------------

_next_sip_port = 5081
_next_rtp_port = 6001
_next_grpc_port = 50052
_port_lock = threading.Lock()


def _alloc_port(which: str) -> int:
    global _next_sip_port, _next_rtp_port, _next_grpc_port
    with _port_lock:
        if which == "sip":
            p = _next_sip_port
            _next_sip_port += 1
            return p
        elif which == "rtp":
            p = _next_rtp_port
            _next_rtp_port += 1
            return p
        else:  # grpc
            p = _next_grpc_port
            _next_grpc_port += 1
            return p


# ---------------------------------------------------------------------------
# Proto stubs (session-scoped — compile once)
# ---------------------------------------------------------------------------

def _compile_proto() -> str:
    """Compile call_agent.proto into a temp dir, return its path."""
    from grpc_tools import protoc

    out = tempfile.mkdtemp(prefix="pjcppagent_pb_")
    proto_file = str(REPO / "protos" / "call_agent.proto")
    proto_path = str(REPO / "protos")
    gtools = os.path.dirname(protoc.__file__)
    proto_include = os.path.join(gtools, "_proto")
    protoc.main([
        "protoc",
        f"-I{proto_path}",
        f"-I{proto_include}",
        f"--python_out={out}",
        f"--grpc_python_out={out}",
        proto_file,
    ])
    return out


@pytest.fixture(scope="session")
def proto_stubs():
    """Import and return (pb2, pb2_grpc) modules from compiled stubs."""
    out = _compile_proto()
    sys.path.insert(0, out)
    import call_agent_pb2 as pb2
    import call_agent_pb2_grpc as pb2_grpc
    yield pb2, pb2_grpc
    sys.path.remove(out)


# ---------------------------------------------------------------------------
# SIPp UAS fixture (function-scoped)
# ---------------------------------------------------------------------------

@pytest.fixture
def sipp_uas(proto_stubs) -> Iterator[callable]:
    """Factory fixture: call sipp_uas(scenario_name, [sip_port_override])
    to start a SIPp UAS. Yields a function that returns (host, sip_port).
    Teardown sends SIGUSR1 and asserts exit 0.

    Usage:
        addr = sipp_uas("answer")      # returns (host, port)
        addr = sipp_uas("busy", 5090)  # explicit port override
    """
    pb2, _ = proto_stubs  # noqa
    processes = []

    def _start(scenario: str, sip_port: Optional[int] = None,
               rtp_port: Optional[int] = None) -> tuple:
        sip = sip_port or _alloc_port("sip")
        rtp = rtp_port or _alloc_port("rtp")
        xml = REPO / "testenv" / "sipp" / f"uas_{scenario}.xml"
        if not xml.exists():
            raise FileNotFoundError(f"SIPp scenario not found: {xml}")

        # SIPp with -bg forks a child for each incoming call.
        # We start it and give it time to open the UDP port.
        proc = subprocess.Popen(
            ["sipp", "-sf", str(xml),
             "-i", "127.0.0.1", "-p", str(sip),
             "-mi", "127.0.0.1", "-mp", str(rtp),
             "-rtp_echo", "-nostdin",
             "-bg"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        # The -bg parent exits quickly after forking.
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        # Give the child a moment to open the UDP port
        time.sleep(0.5)
        processes.append((sip, rtp))
        return ("127.0.0.1", sip)

    try:
        yield _start
    finally:
        for sip, rtp in processes:
            # Find SIPp bg child by port
            try:
                r = subprocess.run(
                    ["lsof", "-ti", f":{sip}"],
                    capture_output=True, text=True, timeout=5,
                )
                if r.stdout.strip():
                    bg_pid = int(r.stdout.strip().split("\n")[0])
                    subprocess.run(
                        ["kill", "-SIGUSR1", str(bg_pid)],
                        capture_output=True, timeout=5,
                    )
                    subprocess.run(
                        ["kill", str(bg_pid)],
                        capture_output=True, timeout=5,
                    )
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Agent server fixture (function-scoped)
# ---------------------------------------------------------------------------

@pytest.fixture
def agent_server(proto_stubs) -> Iterator[callable]:
    """Factory fixture: call agent_server(extra_args=[]) to start a
    pjcppagent gRPC server.  Returns the grpc port.
    Teardown kills the process.
    """
    pb2, pb2_grpc = proto_stubs  # noqa
    processes = []

    def _start(extra_args: Optional[list] = None) -> int:
        port = _alloc_port("grpc")
        record_dir = tempfile.mkdtemp(prefix="pjcprec_")
        cmd = [
            str(REPO / "build" / "pjcppagent"),
            "--mode", "server",
            f"--grpc-listen=127.0.0.1:{port}",
            f"--record-dir={record_dir}",
            "--idle-shutdown", "30",
            * (extra_args or []),
        ]
        env = {**os.environ, "AGENT_SIP_PASS": "agentpass"}
        proc = subprocess.Popen(
            cmd, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        processes.append(proc)

        # Wait for Health to return READY (max 10s, retry every 0.5s)
        channel = grpc.insecure_channel(f"127.0.0.1:{port}")
        for _ in range(20):
            try:
                stub = pb2_grpc.CallAgentStub(channel)
                resp = stub.Health(pb2.HealthRequest(), timeout=2)
                if resp.state == pb2.HealthResponse.State.READY:
                    break
            except Exception:
                pass
            time.sleep(0.5)
        else:
            proc.kill()
            raise RuntimeError("Agent server did not become READY")

        return port

    try:
        yield _start
    finally:
        for proc in processes:
            proc.kill()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass

# ---------------------------------------------------------------------------
# make_request fixture
# ---------------------------------------------------------------------------


@pytest.fixture
def make_request(proto_stubs):
    """Factory fixture: returns a function that builds a CallRequest.

    Usage:
        req = make_request(sipp_addr, wav_path="...", tts_text="...",
                           **overrides)
    """
    pb2, _ = proto_stubs

    def _make(sipp_addr: tuple, *,
              wav_path: str = "", tts_text: str = "",
              **overrides) -> "pb2.CallRequest":
        host, port = sipp_addr
        req = pb2.CallRequest(**overrides)
        req.call_id = req.call_id or f"itest-{int(time.time())}"
        req.destination = req.destination or "100"
        req.sip.username = "agent"
        req.sip.password = "agentpass"
        req.sip.server_host = host
        req.sip.server_port = port
        req.sip.transport = pb2.SipCredentials.UDP
        if wav_path:
            req.wav_url = os.path.abspath(wav_path)
        elif tts_text:
            req.tts_text = tts_text
        else:
            req.tts_text = "hello world"
        return req

    return _make
