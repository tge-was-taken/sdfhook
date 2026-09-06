#!/usr/bin/env bash
# Scores every 3D scene against the emulated frame buffer, at 512x448 so the two are comparable.
# Pass scene names to run a subset. This is the number every render change has to move.
set -uo pipefail
cd "$(dirname "$0")/.."

: "${SDF_BIOS:=$HOME/projects/p3fes-pc-poc/assets/bios.bin}"
: "${SDF_ISO:=/mnt/e/Projects/PS2/Games/Shin Megami Tensei - Nocturne (USA).iso}"
: "${SDF_FRAMES:=240}"
# A savestate that cannot resume spins forever, and a sweep that waits on one never reaches the
# scenes after it. 240 frames takes well under a minute.
: "${SDF_TIMEOUT:=180}"
: "${SDF_SHOT_FRAME:=230}"
: "${OUT:=dumps/refdiff}"
export SDF_BIOS SDF_FRAMES SDF_SHOT_FRAME SDF_SYMBOLS=assets SDF_SCALE=1
export GALLIUM_DRIVER=${GALLIUM_DRIVER:-d3d12}
export MESA_LOADER_DRIVER_OVERRIDE=${MESA_LOADER_DRIVER_OVERRIDE:-d3d12}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/usr/lib/wsl/lib}

REFDIFF=${REFDIFF:-$HOME/projects/pcsx2-hook/pcsx2-hook/scripts/refdiff.py}
mkdir -p "$OUT"

scenes=("$@")
if [ ${#scenes[@]} -eq 0 ]; then
	for state in states/*.p2s; do scenes+=("$(basename "$state" .p2s)"); done
fi

for scene in "${scenes[@]}"; do
	SDF_SHOT="$OUT/$scene-emu.png" \
	SDF_NATIVE_SHOT="$OUT/$scene-native.png" \
	SDF_TRACE="$OUT/$scene.jsonl" \
	PCSX2HOOK_STATE="states/$scene.p2s" \
		timeout "$SDF_TIMEOUT" ./build/bin/sdfhook "$SDF_ISO" > "$OUT/$scene.log" 2>&1
	printf '%-34s ' "$scene"
	python3 "$REFDIFF" "$OUT/$scene-emu.png" "$OUT/$scene-native.png" 2>&1 | grep "mean abs" || echo "no 3D"
done
