# pjcppagent — PJSIP C++ Call Agent (PoC)

A single headless binary that places one outbound SIP call, plays a WAV file
or TTS-generated audio, records the inbound audio, and hangs up on silence.

Two frontends:

- **CLI mode** (default): one-shot call. Parameters from CLI flags / env vars,
  audio from STDIN (`text:hello` or `wav:/path/to/file.wav`). Events and result
  printed as JSON lines to stdout.
- **Server mode** (`--mode server`): gRPC server implementing the
  `automatedcalls.callagent.v1.CallAgent` service. Not yet production-ready
  (see Known limitations).

## Prerequisites

### macOS

```bash
brew install cmake pjproject grpc protobuf pkg-config ffmpeg sipp
```

### Debian / Ubuntu

```bash
apt-get install -y cmake g++ pkg-config libpjproject-dev \
  protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev ffmpeg sipp
```

If the distro's pjproject is older than 2.13, build from source:

```bash
./configure --disable-video --disable-sound
make dep && make -j && make install
```

SIPp must be >= 3.6 and built with PCAP support (`sipp -v` mentions
`TLS-PCAP`). The Homebrew build on macOS meets this. No Docker is required —
the test SIP peer is SIPp running natively.

## Build

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j
```

The binary is `build/pjcppagent`.

## CLI mode usage

Start a SIPp UAS that answers with RTP echo (the agent's own audio is echoed
back, so silence detection works):

```bash
./scripts/run_uas.sh answer 5080
```

In another terminal, place a call:

```bash
echo "text:Hello world" | ./build/pjcppagent \
  --sip-host 127.0.0.1 --sip-port 5080 \
  --sip-user agent --dest 100
```

The agent registers, calls `100@127.0.0.1:5080`, plays an 8 kHz beep
(one beep per word, 0.5 s each), records the echoed RTP, hangs up after 10 s
of silence, and prints JSON events to stdout:

```json
{"event":"state","state":"REGISTERING","call_id":"...","ts":"..."}
{"event":"state","state":"REGISTERED","call_id":"...","ts":"..."}
{"event":"state","state":"DIALING","call_id":"...","ts":"..."}
{"event":"state","state":"RINGING","call_id":"...","ts":"..."}
{"event":"state","state":"ANSWERED","call_id":"...","ts":"..."}
{"event":"state","state":"PLAYING","call_id":"...","ts":"..."}
{"event":"state","state":"PLAYED","call_id":"...","ts":"..."}
{"event":"state","state":"SILENCE_DETECTED","silence_seconds":3,"call_id":"...","ts":"..."}
{"event":"state","state":"HANGING_UP","call_id":"...","ts":"..."}
{"event":"recording_ready","event":"recording_ready","uri":"...","duration":6,"call_id":"...","ts":"..."}
{"event":"result","status":"ANSWERED","billing_seconds":6,"recording":"...","sip_code":603,"reason":"Decline","call_id":"...","ts":"..."}
```

The recording file is a valid 8 kHz mono PCM16 WAV.

## gRPC server mode

```bash
AGENT_SIP_PASS=secret ./build/pjcppagent --mode server \
  --grpc-listen 127.0.0.1:50051 --idle-shutdown 300
```

### Health

```bash
grpcurl -plaintext -import-path protos -proto call_agent.proto \
  localhost:50051 automatedcalls.callagent.v1.CallAgent/Health
```

### Execute a call

```python
import grpc
import call_agent_pb2 as pb2
import call_agent_pb2_grpc as pb2_grpc

channel = grpc.insecure_channel("127.0.0.1:50051")
stub = pb2_grpc.CallAgentStub(channel)

req = pb2.CallRequest()
req.call_id = "test-1"
req.destination = "100"
req.sip.username = "agent"
req.sip.password = "agentpass"
req.sip.server_host = "127.0.0.1"
req.sip.server_port = 5080
req.tts_text = "hello world"

for event in stub.ExecuteCall(req, timeout=60):
    print(event)
```

### StreamAudio

Returns `UNIMPLEMENTED` in this PoC.

## Events

### State sequence

| State              | Meaning                                                   |
|--------------------|-----------------------------------------------------------|
| REGISTERING        | Registering with the SIP server                            |
| REGISTERED         | Registration confirmed                                     |
| DIALING            | Calling the destination (INVITE sent)                      |
| RINGING            | Remote end is ringing (180 received)                       |
| ANSWERED           | Call is answered (200 OK with SDP)                         |
| PLAYING            | Playback of WAV/TTS audio in progress                      |
| PLAYED             | Playback finished, silence detector armed                  |
| SILENCE_DETECTED   | Silence timeout reached, about to hang up                  |
| HANGING_UP         | Hanging up the call                                        |

### Result mapping

| SIP code / situation                       | Result      | Notes                   |
|--------------------------------------------|-------------|-------------------------|
| Answered then hung up (any side)           | ANSWERED    | billing_seconds counted |
| 486, 600, 603                              | BUSY        |                         |
| 408, 480, 487 (our cancel on timeout)      | NO_ANSWER   |                         |
| Client cancelled (gRPC cancel)             | CANCELLED   |                         |
| Anything else (4xx/5xx/6xx, network)       | FAILED      | sip_code + reason       |

### Exit codes (CLI mode)

| Code | Status       |
|------|-------------|
| 0    | ANSWERED    |
| 3    | BUSY        |
| 4    | NO_ANSWER   |
| 5    | FAILED      |
| 6    | CANCELLED   |

## Tests

### Unit tests (44)

```bash
make test
```

Pure logic (args, silence detection, events, TTS stub, proto mapping) — no
network or PJSIP.

### Integration tests (8)

Preflight check (sipp / ffmpeg), build, generate audio fixtures, then pytest:

```bash
make integ
```

Or step by step:

```bash
./scripts/gen_fixtures.sh
python3 -m venv tests/integration/.python-venv
source tests/integration/.python-venv/bin/activate
pip install -r tests/integration/requirements.txt
python3 -m pytest tests/integration -v --tb=short
```

## Known limitations (PoC)

- **Single call**: the server rejects a second concurrent `ExecuteCall` with
  `ALREADY_EXISTS`.
- **No real TTS**: the TTS stub generates a 440 Hz beep per word. No AWS Polly /
  ElevenLabs / etc. integration.
- **Insecure gRPC**: uses `InsecureServerCredentials`. Add mTLS or per-worker
  token before production.
- **CallerID**: the `--caller-id` / `caller_id` field is sent as a SIP
  `P-Asserted-Identity` header. The carrier may ignore or override it.
- **Vendored proto**: `protos/call_agent.proto` is copied from the main
  `automated-calls` repo. Sync manually when the upstream changes.
- **Test SIP peer**: SIPp scenarios are scripted (`-rtp_echo`), not a real PBX.
  The agent has not been tested against Asterisk / FreeSWITCH / carrier SBCs.
- **StreamAudio**: not implemented — returns `UNIMPLEMENTED`.
- **No SRTP/TLS**: the agent uses UDP with no encryption.
- **macOS-only leak checking**: AppleClang ASan lacks `detect_leaks`; Valgrind
  is incompatible with macOS ≥ Ventura. On-disk ASan builds verify
  buffer-overflow / use-after-free safety (44/44 unit tests pass under ASan)
  but not memory leak freedom. Linux + Valgrind would be needed for a
  full leak check.
