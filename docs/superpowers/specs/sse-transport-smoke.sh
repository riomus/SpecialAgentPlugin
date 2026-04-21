#!/usr/bin/env bash
# sse-transport-smoke.sh — integration smoke for the SpecialAgent raw-TCP MCP transport.
# Requires jq. Intended to be run against a live editor on 127.0.0.1:8767.
#
# Phase-aware cases: `SESSION_ENFORCED` defaults to 0 (Phase 1 — session
# validation not yet live). Set SESSION_ENFORCED=1 once Phase 3a (session
# machinery) lands.

set -euo pipefail
HOST="${HOST:-http://127.0.0.1:8767}"
SESSION_ENFORCED="${SESSION_ENFORCED:-1}"
FAIL=0
pass() { echo "ok  $1"; }
fail() { echo "FAIL $1: $2"; FAIL=1; }

# 1. /health
R=$(curl -sS "$HOST/health") || { fail health "no response"; exit 1; }
[ "$(echo "$R" | jq -r .status)" = "healthy" ] && pass health || fail health "status not healthy: $R"

# 2. initialize → session id (returned via Mcp-Session-Id header)
R=$(curl -sS -D /tmp/init-headers -o /tmp/init-body -X POST \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' \
     "$HOST/mcp" -w '%{http_code}')
[ "$R" = "200" ] && pass initialize.status=200 || fail initialize.status "got $R"
SID=$(tr -d '\r' < /tmp/init-headers | awk -F': ' 'BEGIN{IGNORECASE=1} /^Mcp-Session-Id/ {print $2; exit}')
if [ "$SESSION_ENFORCED" = "1" ]; then
  [ -n "$SID" ] && pass initialize.session_header || fail initialize.session_header "no Mcp-Session-Id header"
else
  echo "skip initialize.session_header (Phase 1 — session machinery not yet wired)"
fi

# 3. tools/list carrying session (Phase 1: session header ignored, still passes)
HDR=()
[ -n "$SID" ] && HDR+=(-H "Mcp-Session-Id: $SID")
N=$(curl -sS -X POST -H 'Content-Type: application/json' "${HDR[@]}" \
    -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' "$HOST/mcp" | jq '.result.tools | length')
[ "$N" = "300" ] && pass tools.list.count=300 || fail tools.list.count "got $N"

# 4. tools/call level/get_current_path
STATUS=$(curl -sS -X POST -H 'Content-Type: application/json' "${HDR[@]}" \
    -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"level/get_current_path","arguments":{}}}' \
    "$HOST/mcp" | jq -r '.result.isError // false')
[ "$STATUS" = "false" ] && pass level.get_current_path || fail level.get_current_path "isError=$STATUS"

# 5. missing session id on non-initialize (Phase 3a enforced)
CODE=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":4,"method":"tools/list"}' "$HOST/mcp")
if [ "$SESSION_ENFORCED" = "1" ]; then
  [ "$CODE" = "400" ] && pass missing.session_id=400 || fail missing.session_id "got $CODE"
else
  echo "skip missing.session_id (Phase 1 — session enforcement not yet wired; got $CODE)"
fi

# 6. OPTIONS preflight
CODE=$(curl -sS -o /dev/null -w '%{http_code}' -X OPTIONS "$HOST/mcp")
[ "$CODE" = "204" ] && pass options.204 || fail options.204 "got $CODE"

# 7. /sse without session
CODE=$(curl -sS -o /dev/null -w '%{http_code}' "$HOST/sse")
if [ "$SESSION_ENFORCED" = "1" ]; then
  [ "$CODE" = "400" ] && pass sse.no_session=400 || fail sse.no_session "got $CODE"
else
  # Phase 1: GET /sse returns 501 Not Implemented (stub)
  [ "$CODE" = "501" ] && pass sse.stub=501 || fail sse.stub "got $CODE"
fi

# 8. oversized body → 413
CODE=$(dd if=/dev/zero bs=1048576 count=20 2>/dev/null | \
       curl -sS -o /dev/null -w '%{http_code}' --data-binary @- \
            -H 'Content-Type: application/octet-stream' \
            "$HOST/mcp")
[ "$CODE" = "413" ] && pass oversized.body=413 || fail oversized.body "got $CODE"

exit $FAIL
