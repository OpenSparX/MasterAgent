#!/system/bin/sh
# sparx_agent — on-device agent daemon for Android.
#
# Launched by `sparx deploy --start`. Listens on a unix abstract socket
# (sparx_agent) that `sparx shell --device` connects to.
#
# This script is pushed to /data/local/tmp/sparx/ and executed via
# `adb shell sh /data/local/tmp/sparx/sparx_agent.sh`.
#
# It is intentionally minimal — the heavy lifting is done by the native
# master_agent binary (or the mock REPL for initial testing). The script's
# job is to:
#   1. Set up the library path so libGenie.so / libQnnHtp.so are findable.
#   2. Launch the agent binary with the deployed config.
#   3. Restart on crash (with backoff) until explicitly stopped.

SPARX_DIR="/data/local/tmp/sparx"
CONFIG="$SPARX_DIR/agent.yaml"
LOG="$SPARX_DIR/agent.log"
PID_FILE="$SPARX_DIR/agent.pid"

# Export library paths for QNN/Genie
export LD_LIBRARY_PATH="/vendor/lib64:$SPARX_DIR/lib:$LD_LIBRARY_PATH"
export ADSP_LIBRARY_PATH="/vendor/lib/rfsa/adsp:/vendor/dsp/cdsp"

# Write our PID so `sparx deploy --stop` can find us
echo $$ > "$PID_FILE"

log() {
    echo "$(date '+%H:%M:%S') $1" >> "$LOG"
    echo "  [agent] $1"
}

cleanup() {
    log "shutting down (signal received)"
    rm -f "$PID_FILE"
    exit 0
}
trap cleanup INT TERM

if [ ! -f "$CONFIG" ]; then
    log "ERROR: $CONFIG not found. Run 'sparx deploy' first."
    exit 1
fi

log "starting agent daemon"
log "config: $CONFIG"
log "socket: localabstract:sparx_agent"

# Restart loop with exponential backoff (capped at 30s)
BACKOFF=1
while true; do
    # Check for the native binary first; fall back to shell REPL
    if [ -x "$SPARX_DIR/bin/master_agent" ]; then
        log "launching native agent"
        "$SPARX_DIR/bin/master_agent" \
            --config "$CONFIG" \
            --socket "localabstract:sparx_agent" \
            >> "$LOG" 2>&1
        EXIT_CODE=$?
    else
        log "native binary not found, running mock loop"
        log "listening on localabstract:sparx_agent (mock)"

        # Mock: just stay alive. Real implementation uses the abstract socket.
        # This lets `sparx shell --device` detect the agent is "running" and
        # gives a clear upgrade path: push the binary later, daemon restarts.
        while true; do
            sleep 60
        done
        EXIT_CODE=0
    fi

    if [ $EXIT_CODE -eq 0 ]; then
        log "agent exited cleanly"
        break
    fi

    log "agent crashed (exit=$EXIT_CODE), restarting in ${BACKOFF}s"
    sleep $BACKOFF
    BACKOFF=$((BACKOFF * 2))
    if [ $BACKOFF -gt 30 ]; then
        BACKOFF=30
    fi
done

rm -f "$PID_FILE"
