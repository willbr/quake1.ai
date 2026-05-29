#!/usr/bin/env bash
set -u
LOG=$(mktemp -t ai-test.XXXXXX.log)

zig build run -- +map ai_t01_nav +bot 1 > "$LOG" 2>&1 &
PID=$!

TIMEOUT_S=720
DEADLINE=$(( $(date +%s) + TIMEOUT_S ))

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  if grep -q "AI-TEST DONE" "$LOG"; then break; fi
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  sleep 1
done

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

echo "=== AI test markers seen ==="
grep -aE "AI-TEST " "$LOG" || echo "(none)"

EXPECTED="t01_nav t02_combat t03_stimulus t04_smoke t05_light t06_wander t07_lift t08_bridge t09_train DONE"
RC=0
for tag in $EXPECTED; do
  if ! grep -aq "AI-TEST $tag" "$LOG"; then
    echo "MISS: AI-TEST $tag"
    RC=1
  fi
done

if [ $RC -eq 0 ]; then
  echo "ALL SCENARIOS PASSED"
else
  echo "Log: $LOG"
fi
exit $RC
