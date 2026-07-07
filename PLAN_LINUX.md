# Linux Verification Plan

## Goal
Build and test `pjcppagent` on Linux to verify DoD items that cannot be
checked on macOS (AppleClang ASan lacks leak detection).

## Prerequisites

```bash
# Debian / Ubuntu (adjust for your distro)
sudo apt-get update
sudo apt-get install -y cmake g++ pkg-config libpjproject-dev \
  protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev \
  ffmpeg sipp valgrind
```

If the distro's pjproject is older than 2.13, build from source:

```bash
wget https://www.pjsip.org/release/2.15/pjproject-2.15.tar.bz2
tar xf pjproject-2.15.tar.bz2 && cd pjproject-2.15
./configure --disable-video --disable-sound
make dep && make -j$(nproc) && sudo make install
```

Verify: `sipp -v` must mention `PCAP` in the version string.

## Step 1 — Clean build with -Werror

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
```

- [ ] Build succeeds with zero warnings
- [ ] No `-Werror`-related failures
- [ ] Binary exists: `./build/pjcppagent`

## Step 2 — Unit tests (44)

```bash
./build/unit_tests
```

- [ ] All 44 pass, no tests touch the network or PJSIP

## Step 3 — Proto integrity

```bash
diff <(tail -n +2 protos/call_agent.proto) ../../project/automated_calls/protos/call_agent.proto
```

The upstream path may differ — the point is to confirm `call_agent.proto`
matches the authoritative source except for the vendored-note comment on
line 1. If the file doesn't exist upstream, note it and move on.

- [ ] Proto is identical (except vendor comment) or upstream file is absent

## Step 4 — No password in logs

```bash
AGENT_SIP_PASS=supersecret ./build/pjcppagent --mode server \
  --grpc-listen 127.0.0.1:51999 &
PID=$!
sleep 3
kill $PID 2>/dev/null; wait $PID 2>/dev/null
```

- [ ] `./build/pjcppagent` stderr contains **no** occurrence of `supersecret`

## Step 5 — Valgrind leak check

```bash
# Start a SIPp UAS
sipp -sf testenv/sipp/uas_answer.xml -i 127.0.0.1 -p 5080 \
     -mi 127.0.0.1 -mp 6000 -rtp_echo -bg -nostdin -trace_err

# Run the agent under Valgrind
mkdir -p /tmp/vg_test
valgrind --leak-check=full --show-leak-kinds=definite,possible \
  --errors-for-leak-kinds=definite,possible \
  --exit-on-first-error=yes --error-exitcode=42 \
  ./build/pjcppagent --sip-host 127.0.0.1 --sip-port 5080 \
  --sip-user agent --dest 100 --answer-timeout 8 --silence-timeout 3 \
  --record-dir /tmp/vg_test <<< "text:hi" \
  2>/tmp/valgrind_report.txt
VG_EXIT=$?

# Clean up UAS
kill $(lsof -ti :5080) 2>/dev/null; wait 2>/dev/null

echo "Exit code: $VG_EXIT"
echo "=== Valgrind report ==="
cat /tmp/valgrind_report.txt
```

### Interpreting results

- **PJSIP one-time allocations** are acceptable — PJSIP allocates pools on
  `libInit()` that are never freed. These show as "still reachable" at exit.
- **Definite losses from our code** (`src/*.cpp`) are unacceptable.
- **Possible losses** should be investigated but are often false positives
  from PJSIP's pool allocator.

Expected outcome: `VG_EXIT` should be 0 (no errors), or if PJSIP one-time
allocations trigger `--errors-for-leak-kinds=definite`, re-run without
`--errors-for-leak-kinds=definite` and manually check that only
"still reachable" blocks point to PJSIP (`pjsip_*`, `pj_*`, `pjsua_*`).

- [ ] No definite losses from our code (`src/*.cpp` symbols)

## Step 6 — Integration tests (8)

```bash
# Build if not already done
cmake --build build -j$(nproc)

# Generate audio fixtures
bash scripts/gen_fixtures.sh

# Setup Python venv
python3 -m venv tests/integration/.python-venv
source tests/integration/.python-venv/bin/activate
pip install -r tests/integration/requirements.txt

# Run tests
python3 -m pytest tests/integration -v --tb=short
```

- [ ] All 8 integration tests pass

## Summary

Check off and return:

| Item | Status |
|------|--------|
| `-Werror` clean build | ✓ / ✗ |
| 44 unit tests pass | ✓ / ✗ |
| Proto integrity | ✓ / ✗ |
| No password in logs | ✓ / ✗ |
| Valgrind clean | ✓ / ✗ |
| 8 integration tests pass | ✓ / ✗ |
