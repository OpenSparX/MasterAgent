#!/usr/bin/env bash
# demo/record_demo.sh — Generate terminal recording for README social preview
# Requires: asciinema, agg (or svg-term-cli)
#
# Usage:
#   ./demo/record_demo.sh         # Records to demo/sparx-demo.cast
#   agg demo/sparx-demo.cast demo/sparx-demo.gif  # Convert to GIF
#
# The GIF can be uploaded as the GitHub social preview image,
# or embedded directly in the README.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CAST_FILE="$SCRIPT_DIR/sparx-demo.cast"

echo "🎬 Recording Sparx demo..."
echo "   Output: $CAST_FILE"
echo ""
echo "   This will record a scripted demo showing:"
echo "   1. sparx init"
echo "   2. sparx demo automotive"
echo "   3. sparx plan show"
echo ""

# Create a scripted input file for asciinema
SCRIPT_INPUT=$(mktemp)
cat > "$SCRIPT_INPUT" << 'SCRIPT'
# Sparx — AI agents that run 100% on-device
echo ""
echo "$ npm install -g @sparx/cli"
sleep 1
echo "✓ Installed sparx v2.1.14"
sleep 0.5
echo ""
echo "$ sparx demo automotive"
sleep 1.5
echo ""
echo "🚗 Automotive Voice Assistant"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo '  You: "Turn on AC, set to 22°C, interior mode"'
echo ""
sleep 1
echo "  ⚙️  Processing..."
sleep 0.3
echo "  ├─ Intent: climate_control ✓"
sleep 0.3
echo "  ├─ Skills: ac.power, ac.temperature, ac.circulation ✓"
sleep 0.3
echo "  ├─ MCP Services: vehicle.climate [87ms] ✓"
sleep 0.3
echo "  └─ Result: Climate control updated ✓"
echo ""
echo "  ⚡ Latency: 87ms (Qualcomm NPU accelerated)"
echo ""
sleep 2
echo "$ sparx plan show plans/turn-off-ac.yaml"
sleep 1
echo ""
echo "  Plan: turn-off-ac (priority=p1, deadline=3000ms)"
echo ""
echo "  ┌─────────────┐"
echo "  │  read_temp  │  vehicle.climate.getTemperature"
echo "  └──────┬──────┘"
echo "         │"
echo "         ▼"
echo "  ┌─────────────┐"
echo "  │   set_ac    │  vehicle.climate.setPower (power: off)"
echo "  └─────────────┘"
echo ""
echo "  ✓ valid — 2 nodes, 1 dependency"
sleep 2
SCRIPT

echo "📋 Demo script ready at: $SCRIPT_INPUT"
echo ""
echo "To record with asciinema:"
echo "  asciinema rec $CAST_FILE -c 'bash $SCRIPT_INPUT'"
echo ""
echo "To convert to GIF:"
echo "  agg $CAST_FILE $SCRIPT_DIR/sparx-demo.gif --theme monokai"
echo ""
echo "To convert to SVG (lighter, sharper):"
echo "  svg-term --in $CAST_FILE --out $SCRIPT_DIR/sparx-demo.svg --window"
echo ""
echo "Then set the GIF/SVG as GitHub social preview at:"
echo "  Settings → Social preview → Upload"
echo ""
echo "Or embed in README:"
echo '  <p align="center"><img src="demo/sparx-demo.gif" width="700" /></p>'

rm -f "$SCRIPT_INPUT"
