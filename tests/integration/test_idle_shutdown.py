"""With --idle-shutdown 10, the server exits by itself within ~20 s."""

import time
import subprocess
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def test_idle_shutdown(proto_stubs):
    pb2, _ = proto_stubs  # noqa — module availability

    start = time.time()
    proc = subprocess.Popen(
        [str(REPO / "build" / "pjcppagent"),
         "--mode", "server",
         "--grpc-listen=127.0.0.1:50599",
         "--idle-shutdown", "10"],
        env={"AGENT_SIP_PASS": "secret", **os.environ},
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    ret = proc.wait(timeout=25)
    elapsed = time.time() - start

    assert ret == 0, f"Server exited with code {ret}"
    # The idle watchdog polls every 5s, so actual exit is 10-15s after start.
    # Allow up to 20s for CI variance.
    assert elapsed < 20, f"Server took {elapsed:.0f}s to idle-shutdown"
