#!/usr/bin/env bash
# mcp-feature-smoke.sh — feature smoke test for the SpecialAgent MCP server.
#
# Exercises the behaviours hardened in the stability/screenshot/docs/durability
# pass against a LIVE editor with the plugin loaded (server on 127.0.0.1:8767).
# Requires: curl, jq. Run AFTER building the plugin and opening a level.
#
#   ./mcp-feature-smoke.sh
#   HOST=http://127.0.0.1:8767 ./mcp-feature-smoke.sh
#
# Exit code 0 = all checks passed, non-zero = at least one failed.
set -uo pipefail

HOST="${HOST:-http://127.0.0.1:8767}"
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass() { echo "ok   $1"; }
fail() { echo "FAIL $1: ${2:-}"; FAIL=1; }
note() { echo "--   $1"; }

command -v jq  >/dev/null || { echo "jq is required"; exit 2; }
command -v curl >/dev/null || { echo "curl is required"; exit 2; }

# --- session handshake ------------------------------------------------------
# 1. /health
R=$(curl -sS "$HOST/health" 2>/dev/null) || { fail health "server not reachable on $HOST — is the editor running with the plugin?"; exit 1; }
[ "$(echo "$R" | jq -r '.status // empty')" = "healthy" ] && pass health || fail health "status not healthy: $R"

# 2. initialize → capture Mcp-Session-Id
curl -sS -D "$TMP/h" -o "$TMP/init" -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' \
  "$HOST/mcp" >/dev/null
SID=$(tr -d '\r' < "$TMP/h" | awk -F': ' 'BEGIN{IGNORECASE=1} /^Mcp-Session-Id/ {print $2; exit}')
[ -n "$SID" ] && pass initialize.session || fail initialize.session "no Mcp-Session-Id header"
HDR=(-H 'Content-Type: application/json')
[ -n "$SID" ] && HDR+=(-H "Mcp-Session-Id: $SID")

# rpc <id> <method> <paramsJson> -> raw response on stdout
rpc() {
  curl -sS -X POST "${HDR[@]}" \
    -d "{\"jsonrpc\":\"2.0\",\"id\":$1,\"method\":\"$2\",\"params\":$3}" "$HOST/mcp"
}
# call <id> <tool> <argsJson> -> raw tools/call response on stdout
call() { rpc "$1" "tools/call" "{\"name\":\"$2\",\"arguments\":$3}"; }
# inner <response> -> the service result object decoded from content[].text
inner() { echo "$1" | jq -c '(.result.content[]? | select(.type=="text") | .text) // "{}" | fromjson' 2>/dev/null | head -1; }

# 3. tools/list non-empty
N=$(rpc 2 "tools/list" "{}" | jq '.result.tools | length')
[ "${N:-0}" -gt 0 ] 2>/dev/null && pass "tools.list.count=$N" || fail tools.list "got '$N'"

# --- screenshots: correct JPEG mimeType, not mislabelled PNG ----------------
R=$(call 3 "screenshot/capture" '{"width":640,"height":360,"quality":85}')
ERR=$(echo "$R" | jq -r '.result.isError // false')
MIME=$(echo "$R" | jq -r '.result.content[]? | select(.type=="image") | .mimeType' | head -1)
HASIMG=$(echo "$R" | jq -r '[.result.content[]? | select(.type=="image")] | length')
if [ "$ERR" = "false" ] && [ "${HASIMG:-0}" -ge 1 ]; then pass screenshot.capture.ok; else fail screenshot.capture "isError=$ERR image_blocks=$HASIMG"; fi
[ "$MIME" = "image/jpeg" ] && pass "screenshot.mimetype=$MIME (jpeg, not mislabelled png)" \
  || fail screenshot.mimetype "expected image/jpeg at quality 85, got '$MIME'"

# quality 100 should be PNG
MIME100=$(call 4 "screenshot/capture" '{"width":320,"height":180,"quality":100}' | jq -r '.result.content[]? | select(.type=="image") | .mimeType' | head -1)
[ "$MIME100" = "image/png" ] && pass "screenshot.mimetype@100=png" || fail screenshot.mimetype@100 "expected image/png, got '$MIME100'"

# --- read-only call succeeds ------------------------------------------------
R=$(call 5 "world/list_actors" '{}')
[ "$(echo "$R" | jq -r '.result.isError // false')" = "false" ] && pass world.list_actors || fail world.list_actors "$(echo "$R" | jq -c '.result.content')"

# --- failures surface through isError (not a fake success) ------------------
R=$(call 6 "world/get_actor" '{"actor_name":"__definitely_missing_actor__"}')
[ "$(echo "$R" | jq -r '.result.isError // false')" = "true" ] && pass error.surfaces_via_isError \
  || fail error.surfaces_via_isError "missing actor should report isError=true; got $(echo "$R" | jq -c '.result')"

# --- mutation round-trip: spawn a cube, read its label, move it, delete it ---
R=$(call 10 "world/spawn_actor" '{"actor_class":"/Engine/BasicShapes/Cube.Cube","location":[0,0,300]}')
IN=$(inner "$R")
LABEL=$(echo "$IN" | jq -r '.actor.actor_label // .actor.name // empty')
if [ -n "$LABEL" ]; then
  pass "world.spawn_actor (actor_label='$LABEL')"
  # move it (undoable transaction path)
  RM=$(call 11 "world/set_actor_location" "{\"actor_name\":\"$LABEL\",\"location\":[200,0,300]}")
  [ "$(echo "$RM" | jq -r '.result.isError // false')" = "false" ] && pass world.set_actor_location || fail world.set_actor_location "$(echo "$RM" | jq -c '.result.content')"
  # clean up
  RD=$(call 12 "world/delete_actor" "{\"actor_name\":\"$LABEL\"}")
  [ "$(echo "$RD" | jq -r '.result.isError // false')" = "false" ] && pass world.delete_actor || fail world.delete_actor "$(echo "$RD" | jq -c '.result.content')"
else
  fail world.spawn_actor "no actor_label in result: $(echo "$R" | jq -c '.result.content')"
  note "spawn may fail if /Engine/BasicShapes/Cube.Cube is unavailable; adjust actor_class for your project"
fi

echo
[ "$FAIL" = "0" ] && echo "ALL FEATURE CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL
