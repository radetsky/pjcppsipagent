#!/usr/bin/env bash
# Starts one SIPp UAS scenario in the foreground for manual agent testing.
# Usage: run_uas.sh <scenario> <sip_port> [rtp_port]
#   scenario: answer | busy | no_answer
# Media is echoed back to the agent (-rtp_echo). Stop with Ctrl-C.
set -euo pipefail
cd "$(dirname "$0")/.."

SCENARIO="${1:?usage: run_uas.sh <scenario> <sip_port> [rtp_port]}"
PORT="${2:?usage: run_uas.sh <scenario> <sip_port> [rtp_port]}"
RTP_PORT="${3:-6000}"

FILE="testenv/sipp/uas_${SCENARIO#uas_}.xml"
[ -f "$FILE" ] || { echo "error: scenario file $FILE not found" >&2; exit 1; }
command -v sipp >/dev/null || { echo "error: sipp not installed" >&2; exit 1; }

if [ ! -f testenv/audio/hello.wav ]; then
    echo "audio fixtures missing; running scripts/gen_fixtures.sh"
    ./scripts/gen_fixtures.sh
fi

echo "SIPp UAS: $FILE on 127.0.0.1:$PORT (RTP echo on $RTP_PORT)"
echo "Point the agent at it, e.g.:"
echo "  echo \"wav:\$PWD/testenv/audio/hello.wav\" | AGENT_SIP_PASS=agentpass \\"
echo "    ./build/pjcppagent --sip-host 127.0.0.1 --sip-port $PORT --sip-user agent --dest 100"
exec sipp -sf "$FILE" -i 127.0.0.1 -p "$PORT" -mi 127.0.0.1 -mp "$RTP_PORT" \
          -rtp_echo -nostdin -trace_err
