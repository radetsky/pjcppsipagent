"""CLI mode: run the binary as a subprocess, feed it STDIN, parse JSON
lines from stdout, assert correct exit code and event sequence."""

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def test_cli_mode(proto_stubs, sipp_uas):
    pb2, _ = proto_stubs  # noqa — just for protobuf module availability

    addr = sipp_uas("answer")
    host, port = addr

    record_dir = "/tmp/pjtest_cli_rec"
    os.makedirs(record_dir, exist_ok=True)

    proc = subprocess.Popen(
        [str(REPO / "build" / "pjcppagent"),
         f"--sip-host={host}", f"--sip-port={port}",
         "--sip-user=agent",
         "--dest=100",
         "--silence-timeout=3",
         f"--record-dir={record_dir}"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={"AGENT_SIP_PASS": "agentpass", **os.environ},
        text=True,
    )

    stdout, stderr = proc.communicate(input="text:hello world", timeout=45)
    # PJSIP logs may interleave with JSON on stdout; only parse JSON lines.
    lines = [l for l in stdout.split("\n") if l.strip()]
    events = [json.loads(l) for l in lines if l.startswith("{")]

    # Event sequence assertions
    state_seq = [ev for ev in events if ev.get("event") == "state"]
    state_names = [ev["state"] for ev in state_seq]
    result_ev = next((ev for ev in events if ev.get("event") == "result"), None)
    rec_ev = next((ev for ev in events if ev.get("event") == "recording_ready"), None)

    assert result_ev is not None, f"No result event in:\n{lines}"
    assert result_ev["status"] == "ANSWERED", \
        f"Expected ANSWERED, got {result_ev['status']}: {lines}"
    # PJSIP reports local hangup of a connected call as 603 Decline.
    assert result_ev["sip_code"] == 603, \
        f"Expected 603 (local hangup), got {result_ev['sip_code']}"
    assert proc.returncode == 0, \
        f"CLI exit code {proc.returncode}, stderr={stderr[:500]}"

    assert "ANSWERED" in state_names, f"ANSWERED not in states: {state_names}"
    assert "PLAYING" in state_names, f"PLAYING not in states: {state_names}"
    assert "PLAYED" in state_names, f"PLAYED not in states: {state_names}"
    assert "SILENCE_DETECTED" in state_names, \
        f"SILENCE_DETECTED not in states: {state_names}"

    assert rec_ev is not None, "Missing recording_ready event"
    assert rec_ev["duration"] >= 3

    # Billing seconds in result
    assert result_ev["billing_seconds"] >= 3
