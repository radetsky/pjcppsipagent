# Implementation Plan: PJSIP C++ Call Agent (PoC)

Audience: an implementing LLM (Sonnet-class) or a developer new to this repo.
Follow milestones strictly in order. Do not skip acceptance criteria.
Read `IDEA.md` in this directory and `../../docs/SIP-AGENT.md` before starting.

---

## 0. What we are building

A single headless Linux/macOS binary `pjcppagent` that:

1. Registers to a SIP server using credentials supplied by the platform user
   (server host/port, transport, username, password).
2. Places ONE outbound call, waits `answer_delay_seconds` (default 1) after
   answer, then plays a WAV file (8 kHz mono PCM16) or TTS-generated audio.
3. Records the inbound RTP stream to a WAV file.
4. After playback finishes, watches the inbound audio; if it stays silent for
   `silence_timeout_seconds` (default 10), hangs up.
5. Reports call lifecycle events and the final result.

Two frontends over one core library:

- **CLI mode** (`--mode cli`, default): one-shot call, parameters from CLI/ENV,
  WAV path or TTS text from STDIN, prints events and the recording path to
  STDOUT as JSON lines. This is the PoC debugging tool from `IDEA.md`.
- **Server mode** (`--mode server`): gRPC server implementing the
  `automatedcalls.callagent.v1.CallAgent` service (see `protos/call_agent.proto`,
  vendored in M1). `StreamAudio` returns `UNIMPLEMENTED` in the PoC.
  The process exits after 300 seconds without any RPC activity.

Out of scope for the PoC: real TTS (stub it, see M3), `StreamAudio`, TLS/SRTP,
multiple concurrent calls (max 1 call at a time; `Health` reports `BUSY`).

---

## 1. Target repository layout

Create exactly this layout inside `agents/pjcppagent/`:

```
pjcppagent/
├── CMakeLists.txt
├── PLAN.md                  # this file
├── IDEA.md                  # already exists, do not modify
├── README.md                # written in M9
├── .gitignore
├── .clang-format            # LLVM style, ColumnLimit 100
├── protos/
│   └── call_agent.proto     # vendored copy, see M1
├── src/
│   ├── main.cpp             # arg parsing, mode dispatch
│   ├── args.h / args.cpp    # Config struct + CLI/ENV parsing (no PJSIP deps)
│   ├── sip_agent.h / .cpp   # pjsua2 wrapper: Endpoint, Account, Call
│   ├── call_executor.h/.cpp # runs one call end-to-end, emits CallEvents
│   ├── silence.h / .cpp     # SilenceDetector: pure logic, no PJSIP deps
│   ├── events.h / .cpp      # internal event structs + JSON + proto mapping
│   ├── grpc_server.h / .cpp # CallAgent service impl + idle watchdog
│   └── tts_stub.h / .cpp    # text -> beep WAV placeholder
├── tests/
│   ├── unit/
│   │   ├── test_args.cpp
│   │   ├── test_silence.cpp
│   │   └── test_events.cpp
│   └── integration/
│       ├── conftest.py
│       ├── test_cli_mode.py
│       ├── test_grpc_mode.py
│       └── requirements.txt
├── testenv/
│   ├── sipp/                # every scenario also answers REGISTER via a method branch (see M6)
│   │   ├── uas_answer.xml         # answer -> await BYE; media via -rtp_echo
│   │   ├── uas_busy.xml           # 486 -> recv ACK
│   │   └── uas_no_answer.xml      # 180 forever, 200 + 487 on CANCEL
│   └── audio/
│       └── (generated fixtures, gitignored)
└── scripts/
    ├── gen_fixtures.sh      # creates test WAV files with ffmpeg
    ├── run_uas.sh           # starts a SIPp UAS scenario for manual testing
    └── run_integration.sh   # build -> fixtures -> pytest (SIPp spawned per test)
```

Rules for all C++ code:
- C++17, compile with `-Wall -Wextra -Werror`.
- Comments in English only.
- `args`, `silence`, `events` must NOT include any PJSIP or gRPC header —
  they are the unit-testable core.
- Every PJSIP call goes through `sip_agent.cpp`; no `pjsua2.hpp` includes
  anywhere else except `call_executor.cpp`.

---

## 2. Milestones

### M0 — Skeleton and toolchain

Tasks:
1. Create the directory layout above (empty files where needed).
2. `.gitignore`: `build/`, `.cache/`, `testenv/audio/*.wav`,
   `tests/integration/.python-venv/`, `__pycache__/`, `*.o`.
3. Install dependencies. No Docker is needed anywhere in this project.
   - macOS: `brew install cmake pjproject grpc protobuf pkg-config ffmpeg sipp`
   - Debian/Ubuntu (droplet/CI):
     `apt-get install -y cmake g++ pkg-config libpjproject-dev protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev ffmpeg sipp`
   - SIPp >= 3.6 is required for the M6/M8 test environment; verify that
     `sipp -v` mentions `PCAP` in the version string (v3.7.3 confirmed working).
   - If the distro's pjproject is older than 2.13, build it from source with
     `./configure --disable-video --disable-sound && make dep && make && make install`
     (`--disable-sound` is fine: we use the null audio device + files only).
4. Root `CMakeLists.txt`:
   - `find_package(PkgConfig REQUIRED)` + `pkg_check_modules(PJPROJECT REQUIRED libpjproject)`.
   - `find_package(Protobuf REQUIRED)` and `find_package(gRPC CONFIG QUIET)`;
     if gRPC CONFIG is not found, fall back to `pkg_check_modules(GRPC REQUIRED grpc++)`.
   - `FetchContent` for: `CLI11` (v2.4.x, header-only, arg parsing),
     `nlohmann_json` (v3.11.x, JSON event output), `googletest` (v1.14+).
   - Targets: `agent_core` (static lib: args, silence, events, tts_stub),
     `pjcppagent` (main + sip_agent + call_executor + grpc_server),
     `unit_tests` (gtest, links `agent_core` only).
5. `src/main.cpp`: prints `pjcppagent 0.1.0` and exits 0 (placeholder).

Acceptance criteria:
- `cmake -S . -B build && cmake --build build -j` succeeds on macOS and Linux.
- `./build/pjcppagent --version` prints the version.
- `ctest --test-dir build` runs (zero tests is OK at this point).

### M1 — Vendor the proto and generate code

Tasks:
1. Copy `../../project/automated_calls/protos/call_agent.proto` to
   `protos/call_agent.proto` **unchanged**. Add a header comment:
   `// Vendored from automated-calls main repo. Do not edit here; sync manually.`
2. CMake: generate C++ sources with `protoc` + `grpc_cpp_plugin` into
   `${CMAKE_BINARY_DIR}/gen/`. Use `add_custom_command` keyed on the .proto
   file, or `protobuf_generate` if available. Create an object/static lib
   `proto_gen` from the generated files; link it into `pjcppagent`.
3. Smoke check: `main.cpp` constructs a `automatedcalls::callagent::v1::CallRequest`,
   sets `call_id`, and logs it (proves codegen + linking works). Remove in M2.

Acceptance criteria:
- Build succeeds; generated files land under `build/gen/`.
- `protoc --version` >= 3.15 documented in README notes (M9).

### M2 — Config parsing and CLI surface

Define `struct Config` in `args.h` (plain data, no deps):

```
mode            : enum { CLI, SERVER }         (--mode, default cli)
sip_host        : string   (--sip-host,  env AGENT_SIP_HOST,  required in cli mode)
sip_port        : uint16   (--sip-port,  env AGENT_SIP_PORT,  default 5060)
sip_transport   : enum UDP/TCP/TLS (--sip-transport, default udp)
sip_user        : string   (--sip-user,  env AGENT_SIP_USER,  required in cli mode)
sip_pass        : string   (--sip-pass,  env AGENT_SIP_PASS,  required in cli mode)
destination     : string   (--dest, required in cli mode; E.164 or extension)
caller_id       : string   (--caller-id, optional)
answer_delay_s  : uint32   (--answer-delay, default 1)
answer_timeout_s: uint32   (--answer-timeout, default 30; max ringing time
                            before the agent cancels the call, see M3)
silence_s       : uint32   (--silence-timeout, default 10)
record          : bool     (--record, default true)
record_dir      : string   (--record-dir, default "./recordings")
grpc_listen     : string   (--grpc-listen, default "0.0.0.0:50051", server mode)
idle_shutdown_s : uint32   (--idle-shutdown, default 300, server mode)
log_level       : int      (--log-level, 0..5, default 3; passed to PJSIP)
```

STDIN protocol for CLI mode (exactly as in IDEA.md): the first line of STDIN
is either `wav:<absolute path to wav file>` or `text:<utf-8 text for TTS>`.
Parse it into `audio_source` (a `std::variant`-like tagged struct in Config).

Rules:
- CLI flag beats ENV var; ENV var beats default.
- Validation errors go to STDERR, exit code 2.
- Passwords must never be logged (assert this in code review and tests).

Acceptance criteria:
- `unit_tests` contains `test_args.cpp` covering: defaults, ENV fallback,
  CLI-over-ENV precedence, missing required arg -> error, STDIN line parsing
  (`wav:`, `text:`, garbage -> error).

### M3 — Core SIP: register, call, play, record (CLI mode works)

This is the largest milestone. Implement `sip_agent` + `call_executor`.

`SipAgent` (owns PJSIP lifetime):
1. `pj::Endpoint`: `libCreate()`, `libInit(EpConfig)`, create UDP transport
   (`PJSIP_TRANSPORT_UDP`, port 0 = ephemeral), `libStart()`.
   - Set `epConfig.logConfig.level = config.log_level`.
   - **Immediately after libStart: `ep.audDevManager().setNullDev();`**
     The agent is headless; without the null device pjsua2 tries to open a
     sound card and crashes/hangs on servers.
2. Account: `AccountConfig.idUri = "sip:<user>@<host>"`,
   `regConfig.registrarUri = "sip:<host>:<port>;transport=<udp|tcp>"`,
   `sipConfig.authCreds` with `AuthCredInfo("digest", "*", user, 0, pass)`.
   Subclass `pj::Account`, override `onRegState` -> push
   `REGISTERING/REGISTERED` internal events (or FAILED with SIP code).
   Registration timeout: 15 s -> `CallResult{FAILED, sip_code, "registration timeout"}`.
3. Call: subclass `pj::Call`.
   - `makeCall("sip:<dest>@<host>:<port>", CallOpParam)` after REGISTERED.
   - If `caller_id` is set, add SIP headers `P-Asserted-Identity` and set
     `AccountConfig.idUri` display part accordingly (best effort; document
     that CallerID honoring is provider-dependent).
   - `onCallState`: map `PJSIP_INV_STATE_CALLING->DIALING`,
     `EARLY->RINGING`, `CONFIRMED->ANSWERED`,
     `DISCONNECTED->` final result (see mapping table below).
   - `onCallMediaState`: when audio media is ACTIVE, get
     `AudioMedia am = getAudioMedia(-1)` and wire the conference bridge:
     recorder first, then (after `answer_delay_s`, via a timer) the player.
     Wiring: `recorder`: `am.startTransmit(rec)`; `player`: `player.startTransmit(am)`.
4. Playback: `pj::AudioMediaPlayer` with `PJMEDIA_FILE_NO_LOOP`.
   Override `onEof2()` -> push internal `PLAYED` event.
   **Never destroy the player inside `onEof2`** (PJSIP forbids it);
   set a flag / schedule cleanup on the executor thread.
5. Recording: `pj::AudioMediaRecorder`, file
   `<record_dir>/<call_id>.wav` (create dir if missing). Start at media-ACTIVE
   so we capture everything the callee says, including before playback.
6. TTS stub (`tts_stub`): for `text:` input generate
   `<record_dir>/<call_id>_tts.wav` — 8 kHz mono PCM16, 0.5 s of 440 Hz sine
   per word, 0.2 s pause between words (deterministic, testable). Real TTS
   is a later integration; keep the interface `std::string synth(text, outdir)`.

**Threading model (critical, read twice):**
- pjsua2 callbacks (`onRegState`, `onCallState`, `onCallMediaState`, `onEof2`)
  arrive on PJSIP internal threads. Do NOT block in them and do NOT call
  gRPC from them. They only push `InternalEvent` into a thread-safe queue
  (`std::mutex` + `std::condition_variable` + `std::deque`).
- `CallExecutor::run()` is the single consumer: a state machine that pops
  events, drives timers (answer delay, registration timeout, answer timeout,
  max call duration 600 s hard cap), calls PJSIP actions, and emits `CallEvent`s to a
  sink callback (`std::function<void(const AgentEvent&)>`).
- Any thread that touches PJSIP APIs and was not created by PJSIP must first
  call `Endpoint::instance().libRegisterThread("name")`. This includes the
  executor thread and (later, M5) gRPC handler threads.
- Shutdown order (reverse of creation): hangup call -> delete Call object ->
  account `shutdown()` -> delete Account -> `ep.libDestroy()`. Getting this
  wrong segfaults; wrap in RAII within `SipAgent`.

Disconnect mapping table (implement in `events.cpp`, unit-test it):

| SIP code / situation                  | CallResult.status | notes                    |
|---------------------------------------|-------------------|--------------------------|
| answered then hung up (any side)      | ANSWERED          | billing_seconds counted  |
| 486, 600, 603                         | BUSY              |                          |
| 408, 480, 487 (our cancel on timeout) | NO_ANSWER         |                          |
| client cancelled (M5 RPC cancel)      | CANCELLED         |                          |
| anything else (4xx/5xx/6xx, network)  | FAILED            | keep sip_code + reason   |

`billing_seconds` = whole seconds between CONFIRMED and DISCONNECTED, rounded up.

Answer timeout (the agent-initiated NO_ANSWER path): if the call has not
reached CONFIRMED within `answer_timeout_s` after `makeCall`, the executor
calls `hangup()` (PJSIP sends CANCEL), the remote replies 487, and the
mapping table yields NO_ANSWER. Do not rely on the far end sending 408/480.
Distinction: agent's own timeout -> NO_ANSWER; a gRPC client cancel (M5)
-> CANCELLED. Note the wait loop in `CallExecutor::run()` must use a
deadline, not an unbounded wait.

CLI mode output: one JSON object per line to STDOUT via `events.cpp`
(`nlohmann::json`), e.g.
`{"event":"state","state":"ANSWERED","call_id":"...","ts":"2026-07-05T12:00:00Z"}`
and finally
`{"event":"result","status":"ANSWERED","billing_seconds":12,"recording":"/abs/path.wav"}`.
Exit code: 0 for ANSWERED, 3 BUSY, 4 NO_ANSWER, 5 FAILED, 6 CANCELLED.

Acceptance criteria (manual, against the M6 SIPp UAS —
`./scripts/run_uas.sh answer 5080` — or any lab SIP box):
- `echo "wav:/tmp/hello.wav" | ./build/pjcppagent --sip-host 127.0.0.1 --sip-port 5080 --dest 100`
  registers, calls, plays, records, and prints the result JSON.
- The recording file exists, is valid WAV (`ffprobe` shows pcm_s16le 8000 Hz mono).

### M4 — Silence detection and auto-hangup

`SilenceDetector` (pure logic in `silence.h`, no PJSIP):

```cpp
class SilenceDetector {
 public:
  SilenceDetector(unsigned threshold /*0..255*/, unsigned timeout_ms);
  void arm();                       // called on PLAYED event
  // feed one sample; returns true exactly once when silence timeout is reached
  bool feed(unsigned level /*0..255*/, uint64_t now_ms);
 private:
  ...
};
```

Semantics: inactive until `arm()`. After arming, any `level >= threshold`
resets the countdown; `timeout_ms` of continuous sub-threshold levels ->
`feed` returns true once, then the detector deactivates.
Default threshold: 10 (empirical; make it a hidden `--silence-threshold` flag).

Wiring in `call_executor`: after `PLAYED`, start a 100 ms periodic poll (PJSIP
timer or the executor loop's `wait_for`) reading
`call->getAudioMedia(-1).getRxLevel()` (0..255 from the conference bridge) and
feeding the detector. On trigger: emit `SILENCE_DETECTED{silence_seconds}`,
emit `HANGING_UP`, call `call->hangup()`. Result stays `ANSWERED`.

Also emit `RECORDING_READY{uri, duration}` right after the recorder is
stopped (on DISCONNECTED, before the final result event).

Acceptance criteria:
- `test_silence.cpp` covers: not armed -> never triggers; loud audio resets
  the window; exact boundary (9.9 s loud gap -> no trigger, 10.0 s -> trigger);
  triggers only once; re-arm works.
- Manual: call the M6 `answer` scenario (echoes the agent's audio, silent
  after playback) -> agent hangs up ~10 s after playback ends, events in order:
  `... PLAYED -> SILENCE_DETECTED -> HANGING_UP -> RECORDING_READY -> result`.

### M5 — gRPC server mode

Implement `grpc_server.cpp` with the **sync** gRPC API (simpler than async/
callback API and sufficient for 1 concurrent call):

1. `Health`: fill from agent state (`READY` / `BUSY` when a call is running /
   `SHUTTING_DOWN`), `active_calls` (0/1), `idle_seconds`, `agent_version`.
2. `ExecuteCall(CallRequest, ServerWriter<CallEvent>)`:
   - Reject with `ALREADY_EXISTS` if a call is in progress (single-call PoC).
   - Validate request (`call_id` non-empty, sip fields present, destination
     non-empty, exactly one audio_source) -> `INVALID_ARGUMENT` with details.
   - Map proto -> internal `Config`-like struct; `0` timeout fields mean
     defaults (1 s / 10 s) as documented in the proto. The vendored proto has
     no per-request answer timeout field; the process-wide `--answer-timeout`
     flag applies to every call the server executes.
   - Run `CallExecutor` with a sink that converts internal events to proto
     `CallEvent` (reuse `events.cpp` mapping; add `occurred_at` timestamps)
     and `writer->Write()`s them. `Write` happens on the RPC thread: the sink
     pushes into a queue consumed by the RPC handler, NOT called directly
     from the executor thread — this keeps gRPC and PJSIP threads apart.
   - Cancellation: poll `context->IsCancelled()` in the handler loop every
     200 ms; on cancel -> `executor.cancel()` (hangs up, result CANCELLED).
   - The stream always ends with the `result` event, then `Status::OK`
     (call-level failures are expressed in `CallResult.status`, not in the
     gRPC status; gRPC errors are reserved for protocol misuse).
3. `StreamAudio`: `return Status(grpc::StatusCode::UNIMPLEMENTED, "PoC");`
4. Idle watchdog: a thread wakes every 5 s; if
   `now - last_rpc_activity > idle_shutdown_s` and no active call ->
   `server->Shutdown()`, process exits 0. Every RPC (including Health)
   refreshes `last_rpc_activity`.
5. `--grpc-listen` uses `InsecureServerCredentials` for the PoC. Add a
   `TODO(security): mTLS or per-worker token (docs/SIP-AGENT.md)` comment.

Acceptance criteria:
- `grpcurl -plaintext -d '{}' localhost:50051 automatedcalls.callagent.v1.CallAgent/Health`
  returns `state: READY` (grpcurl needs `-import-path protos -proto call_agent.proto`).
- ExecuteCall from a Python client streams events in the documented order.
- Second concurrent ExecuteCall gets `ALREADY_EXISTS`.
- With `--idle-shutdown 10`, the process exits within ~15 s of the last RPC.

### M6 — Local test SIP environment (SIPp UAS, no Docker)

Docker-based Asterisk is deliberately NOT used: on macOS Docker's NAT breaks
RTP (no audio flows), which makes every media assertion fail. The test SIP
peer is SIPp (>= 3.6) running natively; it covers both roles the agent needs:
it answers the REGISTER (registrar stand-in) and terminates the test call
(UAS with scripted behaviour and real RTP).

Key SIPp facts (verified against v3.7.3 on this machine; they shape the
scenario structure):
- A scenario instance is keyed by Call-ID, and `-oocsf` (out-of-call
  scenarios) is REJECTED in server mode ("SIPp cannot use out-of-call
  scenarios when running in server mode"). Therefore every scenario handles
  REGISTER itself via a method branch — see the template below.
- The Homebrew build has NO working rtpstream (`sipp -v` says
  `TLS-PCAP-SHA256`, no `RTPSTREAM`): `<exec rtp_stream=...>` silently sends
  0 RTP packets. `<exec play_pcap_audio=...>` needs a raw IPv4 socket, i.e.
  root — unusable in tests. **The media mechanism is `-rtp_echo`**: SIPp
  echoes the agent's own RTP back, so the recording contains the played
  prompt and goes silent when playback ends (which is exactly what the
  silence-detection flow needs). Callee-driven "talks N seconds then goes
  silent" behaviour is covered by the SilenceDetector unit tests instead.
- `[media_ip]`/`[media_port]` in SDP are empty unless `-mi`/`-mp` are passed;
  PJSIP then tears the call down immediately after the 200.
- The un-REGISTER the agent sends on shutdown reuses the original REGISTER's
  Call-ID, so it lands on the already-finished instance and is logged as
  "Dead call ... (successful)" in the error log — benign, do not assert on it.

REGISTER-branch template (opening of EVERY scenario; verified working —
PJSIP requires the echoed Contact to consider registration successful):

```xml
<recv request=".*" regexp_match="true">
  <action>
    <ereg regexp="REGISTER" search_in="hdr" header="CSeq:"
          check_it="false" assign_to="isreg" />
  </action>
</recv>
<nop test="isreg" next="do_register" />
<!-- ... INVITE flow of the specific scenario, ending with next="done" ... -->
<label id="do_register" />
<send><!-- 200 OK echoing [last_Contact:], plus "Expires: 300" --></send>
<label id="done" />
<nop />
```

No digest challenge in the PoC: PJSIP only authenticates when challenged, so
the `agent/agentpass` credentials flow through unverified (optional stretch
goal: a variant that first sends a static 401 challenge and asserts an
`Authorization` header on the retry).

Call scenarios — the INVITE branch of each; every `200 OK` carries SDP with
`[media_ip]`/`[media_port]` and PCMU/8000:

| file              | behaviour                                                        |
|-------------------|------------------------------------------------------------------|
| uas_answer.xml    | 180 -> 200+SDP -> recv ACK -> await BYE (600 s) -> 200; media echoed via `-rtp_echo`. Serves the happy-path, cancel, and CLI tests |
| uas_busy.xml      | 486 Busy Here -> recv ACK                                        |
| uas_no_answer.xml | 180 Ringing, never answers -> recv CANCEL -> 200 + 487 to INVITE |

Launch command (used by `run_uas.sh` and the pytest fixture):

```
sipp -sf testenv/sipp/<scenario>.xml -i 127.0.0.1 -p <port> \
     -mi 127.0.0.1 -mp <rtp_port> -rtp_echo -bg -nostdin -trace_err
```

Do NOT use `-m`: with the REGISTER branch every registration is its own
SIPp "call", which skews the count. Stop SIPp with SIGUSR1 (graceful: exits
0 if no scenario deviations occurred) — that is what the pytest fixture
asserts on.

`scripts/run_uas.sh <scenario> <port>`: wraps the command above (serves
calls until killed) for manual CLI-mode testing; prints the
`--sip-host/--sip-port` values to use.

`scripts/gen_fixtures.sh` — WAV fixtures for the agent's player:
`ffmpeg -f lavfi -i "sine=frequency=440:duration=3" -ar 8000 -ac 1 -sample_fmt s16 testenv/audio/hello.wav`
(plus a 1 s and a 30 s variant).

Acceptance criteria (all verified live during development):
- `./scripts/run_uas.sh answer 5080` starts and stays up.
- A manual CLI-mode call against it completes the full happy path:
  REGISTERED -> ANSWERED -> PLAYED -> SILENCE_DETECTED -> hangup,
  result ANSWERED, exit code 0.
- The agent's recording contains the echoed 440 Hz prompt, not silence:
  `ffmpeg -i <rec>.wav -af volumedetect -f null -` reports a mean volume
  clearly above the noise floor (measured: ~-28 dB vs -91 dB for silence).

### M7 — Unit tests (GoogleTest)

Already partially written in M2/M4. Complete the set:

- `test_args.cpp` — see M2.
- `test_silence.cpp` — see M4.
- `test_events.cpp`:
  - SIP disconnect code -> `CallResult.Status` mapping table (every row).
  - JSON serialization of each event type (stable field names — these are a
    de-facto CLI API).
  - internal event -> proto `CallEvent` mapping: correct `oneof` case set,
    `call_id` propagated, timestamps non-zero.
- TTS stub determinism: same text -> byte-identical WAV; WAV header says
  8000 Hz mono s16le.

Acceptance criteria: `ctest --test-dir build --output-on-failure` — all green,
no test touches the network or PJSIP.

### M8 — Integration tests (pytest + grpcio against SIPp UAS)

Setup (`tests/integration/`):
- `requirements.txt`: `pytest`, `grpcio`, `grpcio-tools`, `protobuf`.
- Create venv at `tests/integration/.python-venv` (project convention),
  `pip install -r requirements.txt`.
- `conftest.py`:
  - fixture `sipp_uas` (factory, function-scoped): given a scenario name,
    allocates a unique UDP port per test (5081, 5082, ... via a counter),
    launches the M6 SIPp command as a subprocess and yields `(host, port)`.
    Teardown: send SIGUSR1 (graceful stop), wait up to 5 s, assert exit
    code 0 — a non-zero code means the SIP flow deviated from the scenario,
    which is itself a test failure. Keep `-trace_err` logs in the test
    tmpdir for debugging ("Dead call" un-REGISTER entries are benign).
  - session fixture `proto_stubs`: run `python -m grpc_tools.protoc` on
    `protos/call_agent.proto` into a temp dir, import stubs from there.
  - fixture `agent_server` (factory: accepts extra CLI flags — needed by
    test_no_answer `--answer-timeout 8` and test_idle_shutdown
    `--idle-shutdown 10`): launch `./build/pjcppagent --mode server
    --grpc-listen 127.0.0.1:<unique port> --record-dir <tmpdir>` as a
    subprocess, wait until Health answers READY (retry 20x every 0.5 s),
    teardown: kill.
  - helper `make_request(sipp_addr, wav|text, **overrides)` filling
    SipCredentials with `agent/agentpass@127.0.0.1:<port>` UDP, where
    `<port>` is the test's own SIPp instance. The dialled destination is
    arbitrary (use `100`) — the scenario, not the number, defines behaviour.

Test cases (each asserts the full ordered event sequence and the result):

| test                        | scenario      | expected                                             |
|-----------------------------|---------------|------------------------------------------------------|
| test_answered_full_flow     | uas_answer    | states ...ANSWERED..PLAYED, SILENCE_DETECTED, RECORDING_READY, result ANSWERED, billing_seconds >= 10, recording file exists, is 8k mono s16le and is not silent (echoed prompt) |
| test_busy                   | uas_busy      | result BUSY, sip_code 486, no RECORDING_READY        |
| test_no_answer              | uas_no_answer | agent started with `--answer-timeout 8`; client-side deadline 25 s as a safety net -> agent cancels, result NO_ANSWER, sip_code 487 |
| test_cancel                 | uas_answer    | cancel the RPC 3 s after ANSWERED -> agent hangs up; a follow-up Health shows READY within 5 s |
| test_concurrent_rejected    | uas_answer    | second ExecuteCall while first runs -> ALREADY_EXISTS (rejected by the agent before any SIP traffic; one SIPp instance suffices) |
| test_stream_audio_stub      | (none)        | StreamAudio -> UNIMPLEMENTED                          |
| test_cli_mode (subprocess)  | uas_answer    | run binary in cli mode, parse JSON lines from stdout, exit code 0 |
| test_idle_shutdown          | (none)        | agent with --idle-shutdown 10 exits by itself in <20 s |

WAV validation helper: read the RIFF header in Python (`wave` module), assert
`framerate == 8000, nchannels == 1, sampwidth == 2, nframes > 0`.

`scripts/run_integration.sh`: preflight (`command -v sipp ffmpeg`) -> build ->
gen_fixtures -> pytest. SIPp instances are spawned per test by the fixture;
nothing to start or stop globally. Must work from a clean checkout with only
CMake, SIPp, ffmpeg, and system deps installed.

Acceptance criteria: `./scripts/run_integration.sh` exits 0 locally.

### M9 — Documentation and definition of done

`README.md` must contain: prerequisites per OS (no Docker required), build
commands, CLI usage with a copy-pasteable example including how to start a
local SIPp UAS (`scripts/run_uas.sh`), gRPC usage (grpcurl example), how to
run unit and integration tests, the event/exit-code tables from M3, and a
"Known limitations" section (single call, no real TTS, insecure gRPC,
CallerID is provider-dependent, proto is vendored and synced manually, test
SIP peer is scripted SIPp scenarios rather than a real PBX).

Definition of done checklist (verify each item, do not self-certify):
- [ ] Clean build on Linux and macOS with `-Werror`.
- [ ] All unit tests pass; all integration tests pass via `run_integration.sh`.
- [ ] No password ever appears in logs at any `--log-level` (grep the logs in
      an integration test).
- [ ] Valgrind or ASan run of `test_cli_mode` scenario shows no leaks from our
      code (PJSIP one-time allocations are acceptable; document suppressions).
- [ ] `Health` idle_seconds resets on every RPC; watchdog never kills an
      in-progress call.
- [ ] Proto file identical to
      `../../project/automated_calls/protos/call_agent.proto` except for the
      leading vendor-note comment line added in M1. Verify:
      `diff <(tail -n +2 protos/call_agent.proto) ../../project/automated_calls/protos/call_agent.proto`

---

## 3. Common pitfalls (read before writing any PJSIP code)

1. **Null audio device**: call `setNullDev()` right after `libStart()` or the
   process dies on headless servers. This is the #1 PJSIP server-side mistake.
2. **Thread registration**: any std::thread calling PJSIP must run
   `libRegisterThread()` first, or PJSIP asserts and aborts.
3. **Never block or destroy objects inside pjsua2 callbacks** (`onCallState`,
   `onEof2`, ...). Push to the queue, return immediately. Destroy `Call` and
   `AudioMediaPlayer` objects only from the executor thread, and never inside
   the callback that reports their own event.
4. **Object lifetime order**: Call before Account before Endpoint. Deleting
   the Endpoint while a Call object exists is a guaranteed crash.
5. **WAV format**: PJSIP's file player wants PCM WAV; resample fixtures to
   8 kHz mono s16le (`ffmpeg -ar 8000 -ac 1 -sample_fmt s16`). A 44.1 kHz
   stereo file will play as garbage or fail to open.
6. **SIPp quirks** (all verified): `-oocsf` does not work in server mode —
   REGISTER must be handled by the method branch inside each scenario (M6
   template). SDP `[media_ip]`/`[media_port]` are empty without `-mi`/`-mp`
   on the command line, and PJSIP then drops the call right after the 200.
   `rtp_stream` silently sends nothing in the Homebrew build and
   `play_pcap_audio` needs root (raw sockets) — use `-rtp_echo` for media
   (M6). The shutdown un-REGISTER shows up as a benign
   "Dead call (successful)" log entry.
7. **grpcurl with a non-reflected server**: pass
   `-import-path protos -proto call_agent.proto`, or add gRPC reflection to
   the server (optional nice-to-have, not required).
8. **proto3 zero values**: `answer_delay_seconds == 0` means "use default 1",
   it does NOT mean "no delay". This is already documented in the proto —
   implement it exactly that way.
9. **Unreachable host / dead network**: `makeCall` to an unreachable host
   ends with DISCONNECTED carrying 503 (or PJSIP throws from `makeCall`
   itself — wrap it in try/catch). Both paths must produce
   `CallResult{FAILED, sip_code, reason}` and a clean shutdown, never a hang;
   the registration timeout (15 s) covers the dead-registrar case.

## 4. Suggested commit sequence

One commit per milestone (M0..M9), message format:
`M<n>: <short description>`. Do not push; the repo owner reviews and pushes
manually.
