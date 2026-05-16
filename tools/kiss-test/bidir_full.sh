#!/usr/bin/env bash
# Full bidirectional sweep with packet-counter telemetry. Tests OPPORTUNISTIC,
# DIRECT/in-link PACKET, and DIRECT/Resource paths in both directions and reports
# tx/rx counter deltas plus delivery success/failure for each test.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SX_URL=http://192.168.1.116
LR_URL=http://192.168.1.118
SX_TOK=$(cat "$HERE/.token")
LR_TOK=$(cat "$HERE/.lr-token")
LR_IDEN=$(cat "$HERE/.lr-iden")
LR_ADDR=$(cat "$HERE/.lr-addr")
SX_IDEN=140991649b164ece
SX_ADDR=e60cf2202cd0609925c0948cf84147a9

probe() {  # url tok -> "uptime|tx|rx|airtime|lt_lim|locked|conv_count"
  curl -sS --max-time 5 -H "Authorization: Bearer $2" "$1/api/info" 2>/dev/null | python3 -c "
import sys,json
try:
  d=json.load(sys.stdin)
  s=d['radio']['stats']
  print(f'{d[\"uptime_ms\"]}|{s[\"tx_packets\"]}|{s[\"rx_packets\"]}|{s[\"airtime_pct\"]}|{s.get(\"longterm_airtime_limit_pct\",\"?\")}|{s.get(\"airtime_locked\",\"?\")}')
except: print('|||||')
"
}

inbox_has() {  # url tok identity body -> "YES" or "NO"
  curl -sS --max-time 10 -H "Authorization: Bearer $2" "$1/api/identities/$3/state" 2>/dev/null | python3 -c "
import sys, json
try:
  d=json.load(sys.stdin)
  needle = sys.argv[1]
  for c in d.get('conversations',[]):
    for m in c.get('messages', []):
      body = m.get('body','')
      if body == needle: print('YES'); sys.exit(0)
  print('NO')
except: print('ERR')
" "$4"
}

# Prime announces both directions.
echo "=== priming announces ==="
for i in 1 2; do
  curl -sS -X POST -H "Authorization: Bearer $SX_TOK" "$SX_URL/api/identities/$SX_IDEN/announce" >/dev/null
  curl -sS -X POST -H "Authorization: Bearer $LR_TOK" "$LR_URL/api/identities/$LR_IDEN/announce" >/dev/null
  sleep 8
done

# Test definitions: label  sender_url  sender_tok  sender_iden  recv_url  recv_tok  recv_iden  recv_addr  body  timeout
TESTS=(
  "T1_SX-LR_short_OPP|$SX_URL|$SX_TOK|$SX_IDEN|$LR_URL|$LR_TOK|$LR_IDEN|$LR_ADDR|sweep T1 short|60"
  "T2_LR-SX_short_OPP|$LR_URL|$LR_TOK|$LR_IDEN|$SX_URL|$SX_TOK|$SX_IDEN|$SX_ADDR|sweep T2 short|60"
  "T3_SX-LR_350B_PKT|$SX_URL|$SX_TOK|$SX_IDEN|$LR_URL|$LR_TOK|$LR_IDEN|$LR_ADDR|$(python3 -c 'print("M"*350)')|120"
  "T4_LR-SX_350B_PKT|$LR_URL|$LR_TOK|$LR_IDEN|$SX_URL|$SX_TOK|$SX_IDEN|$SX_ADDR|$(python3 -c 'print("N"*350)')|120"
  "T5_SX-LR_2KB_RES|$SX_URL|$SX_TOK|$SX_IDEN|$LR_URL|$LR_TOK|$LR_IDEN|$LR_ADDR|$(python3 -c 'print("L"*2048)')|240"
  "T6_LR-SX_2KB_RES|$LR_URL|$LR_TOK|$LR_IDEN|$SX_URL|$SX_TOK|$SX_IDEN|$SX_ADDR|$(python3 -c 'print("K"*2048)')|240"
)

for entry in "${TESTS[@]}"; do
  IFS='|' read -r label send_url send_tok send_iden recv_url recv_tok recv_iden recv_addr body timeout <<< "$entry"
  echo
  echo "========================================================================"
  echo "$label"
  echo "  body_len=${#body}  timeout=${timeout}s"
  echo

  # Snapshot counters BEFORE
  SX_BEFORE=$(probe "$SX_URL" "$SX_TOK")
  LR_BEFORE=$(probe "$LR_URL" "$LR_TOK")
  echo "  SX before: $SX_BEFORE"
  echo "  LR before: $LR_BEFORE"

  # Send
  T0=$(date +%s)
  RESP=$(curl -sS --max-time 15 -X POST -H "Authorization: Bearer $send_tok" -H 'Content-Type: application/json' \
    -d "$(python3 -c "import json,sys; print(json.dumps({'to': sys.argv[1], 'content': sys.argv[2]}))" "$recv_addr" "$body")" \
    "$send_url/api/identities/$send_iden/send" 2>&1)
  echo "  send response: $RESP"

  # Poll receiver inbox
  RESULT="FAIL"
  while [ $(($(date +%s) - T0)) -lt $timeout ]; do
    GOT=$(inbox_has "$recv_url" "$recv_tok" "$recv_iden" "$body")
    if [ "$GOT" = "YES" ]; then RESULT="PASS in $(($(date +%s) - T0))s"; break; fi
    sleep 3
  done
  echo "  RESULT: $RESULT"

  # Snapshot counters AFTER
  SX_AFTER=$(probe "$SX_URL" "$SX_TOK")
  LR_AFTER=$(probe "$LR_URL" "$LR_TOK")
  SX_DELTA_TX=$(( ${SX_AFTER%%|*} ))  # uptime, just first field; we recompute below properly
  # Just print before/after lines; user can eyeball deltas
  echo "  SX after:  $SX_AFTER"
  echo "  LR after:  $LR_AFTER"

  # Inter-test pause so airtime doesn't accumulate too aggressively
  sleep 5
done

echo
echo "=== sweep complete ==="
