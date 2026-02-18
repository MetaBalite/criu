#!/bin/bash
# Test io_uring checkpoint/restore with CRIU
# Don't use set -e — we handle errors manually

CRIU="$(cd "$(dirname "$0")/.." && pwd)/criu/criu"
TEST="$(cd "$(dirname "$0")" && pwd)/io_uring_test"
CKPT_DIR=$(mktemp -d /tmp/criu-iouring-XXXXXX)

echo "=== io_uring checkpoint/restore test ==="
echo "CRIU: $CRIU"
echo "Test: $TEST"
echo "Checkpoint dir: $CKPT_DIR"
echo

# Start test program in background
$TEST &
PID=$!
echo "Started test program (PID=$PID)"

# Let it run a few ticks
sleep 3
echo
echo "--- Checkpointing PID $PID ---"

$CRIU dump -t $PID -D "$CKPT_DIR" --shell-job -v4 -o dump.log
DUMP_RC=$?

if [ $DUMP_RC -ne 0 ]; then
    echo "FAIL: dump failed (rc=$DUMP_RC)"
    echo "--- dump.log (last 50 lines) ---"
    tail -50 "$CKPT_DIR/dump.log" 2>/dev/null
    kill $PID 2>/dev/null
    exit 1
fi

echo "Dump succeeded!"
echo

# Check the process is gone
if kill -0 $PID 2>/dev/null; then
    echo "WARN: process still alive after dump?"
fi

echo "--- Restoring ---"
$CRIU restore -D "$CKPT_DIR" --shell-job -v4 -o restore.log -d
RESTORE_RC=$?

if [ $RESTORE_RC -ne 0 ]; then
    echo "FAIL: restore failed (rc=$RESTORE_RC)"
    echo "--- restore.log (last 50 lines) ---"
    tail -50 "$CKPT_DIR/restore.log" 2>/dev/null
    exit 1
fi

echo "Restore succeeded! Waiting for ticks..."
echo

# The restored process should continue ticking
sleep 3

# Check it's still alive
if kill -0 $PID 2>/dev/null; then
    echo
    echo "=== PASS: process alive and ticking after restore ==="
    kill $PID 2>/dev/null
    rm -rf "$CKPT_DIR"
else
    echo
    echo "=== FAIL: process died after restore ==="
    echo "--- restore.log (last 50 lines) ---"
    tail -50 "$CKPT_DIR/restore.log" 2>/dev/null
    echo
    echo "Checkpoint dir preserved: $CKPT_DIR"
    exit 1
fi
