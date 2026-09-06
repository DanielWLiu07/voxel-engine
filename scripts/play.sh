#!/usr/bin/env bash
#
# Launches the engine and brings it to the front.
#
# Exists because a binary started from a background shell on macOS does
# not become the active application, whatever the window asks for - so the
# window appears, renders, and never receives a keypress. That looks
# exactly like a dead engine, and it cost several rounds of debugging the
# wrong layer to find. Launching from a normal foreground shell does not
# need this; a script, an editor task or an agent does.
#
#   scripts/play.sh              4D world (the default)
#   scripts/play.sh --3d         the 3D world
#   scripts/play.sh --radius 12  any engine flag passes through
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
if [ ! -x build/voxel_engine ]; then
  echo "build/voxel_engine not found - run: cmake --build build -j" >&2
  exit 1
fi
./build/voxel_engine "$@" &
ENGINE_PID=$!
# Wait for the engine to be RENDERING, not merely running.
#
# The process exists within milliseconds and then spends seconds building
# the initial chunk grid. Activating during that window appeared to work
# and then lost focus again as startup finished, which is worse than not
# trying - it looks fixed on some runs and not others. Steady CPU is the
# signal that the render loop is going.
raise_it() {
  osascript -e 'tell application "System Events" to set frontmost of first process whose name is "voxel_engine" to true' 2>/dev/null
}
for _ in $(seq 1 200); do
  cpu=$(ps -o %cpu= -p "$ENGINE_PID" 2>/dev/null | tr -d ' ' | cut -d. -f1)
  [ -n "${cpu:-}" ] || break            # process gone
  [ "${cpu:-0}" -gt 5 ] && break
  sleep 0.1
done
sleep 1
raise_it || true
sleep 1
raise_it || true

# Whether that actually worked is not something this script can rely on.
#
# Launched from a normal foreground terminal the engine takes focus by
# itself and none of the above is needed. Launched from a fully
# backgrounded process chain - a script run with &, an editor task, an
# agent - macOS declines to activate it, and the same osascript that works
# when typed by hand does nothing from in here. Measured both ways.
#
# So say so rather than pretend. A window without keyboard focus renders
# perfectly and ignores every key, which is indistinguishable from a
# broken engine and is exactly the confusion this whole script exists to
# prevent.
if ! osascript -e 'tell application "System Events" to get name of first application process whose frontmost is true' 2>/dev/null | grep -q voxel_engine; then
  echo "" >&2
  echo "NOTE: the window is up but does not have keyboard focus." >&2
  echo "      Click it once, or run ./build/voxel_engine directly from" >&2
  echo "      your own terminal, where it focuses itself." >&2
  echo "      Without focus the engine renders and ignores every key." >&2
fi
wait $ENGINE_PID
