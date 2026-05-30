#!/usr/bin/env bash
# Run the autonomous AI bot through the chained ai_t01..t09 test maps and check
# that every "AI-TEST <tag>" marker (printed as each map's worldspawn message)
# reaches the log. Set HEADLESS=1 to run with no window/audio/GUI (server+bot
# only) — markers then arrive on stderr, which we still capture via 2>&1.
#   ./scripts/run_ai_tests.sh             # windowed
#   HEADLESS=1 ./scripts/run_ai_tests.sh  # headless
set -u
LOG=$(mktemp -t ai-test.XXXXXX.log)

MODE=${HEADLESS:+headless}; MODE=${MODE:-windowed}
echo "mode: $MODE  (log: $LOG)"
zig build run -- ${HEADLESS:+--headless} +map ai_t01_nav +bot 1 > "$LOG" 2>&1 &
PID=$!

TIMEOUT_S=720
DEADLINE=$(( $(date +%s) + TIMEOUT_S ))

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  if grep -aq "AI-TEST DONE" "$LOG"; then break; fi
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
