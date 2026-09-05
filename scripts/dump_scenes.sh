#!/usr/bin/env bash
# Runs every curated state and leaves one dump directory per scene. Pass scene names to run a subset.
set -euo pipefail
cd "$(dirname "$0")/.."

: "${SDF_BIOS:=$HOME/projects/p3fes-pc-poc/assets/bios.bin}"
: "${SDF_ISO:=/mnt/e/Projects/PS2/Games/Shin Megami Tensei - Nocturne (USA).iso}"
: "${SDF_FRAMES:=120}"
: "${SDF_SHOT_FRAME:=110}"
export SDF_BIOS SDF_FRAMES SDF_SHOT_FRAME
export SDF_SYMBOLS=assets

# The GPU path; drop these two for llvmpipe.
export GALLIUM_DRIVER=${GALLIUM_DRIVER:-d3d12}
export MESA_LOADER_DRIVER_OVERRIDE=${MESA_LOADER_DRIVER_OVERRIDE:-d3d12}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/usr/lib/wsl/lib}

scenes=("$@")
if [ ${#scenes[@]} -eq 0 ]; then
	for state in states/*.p2s; do
		scenes+=("$(basename "$state" .p2s)")
	done
fi

for scene in "${scenes[@]}"; do
	out="dumps/$scene"
	mkdir -p "$out"
	# The state is kept beside its dump: a run nobody can reproduce is worth little later.
	SDF_WINDOW=1 \
	SDF_TRACE="$out/trace.jsonl" \
	SDF_SHOT="$out/shot.ppm" \
	SDF_NATIVE_SHOT="$out/native.ppm" \
	SDF_DUMP_TEXTURES="$out" \
	PCSX2HOOK_GS_TRACE="$out/gs.jsonl" \
	PCSX2HOOK_STATE="states/$scene.p2s" \
		./build/bin/sdfhook "$SDF_ISO" > "$out/run.log" 2>&1 || true
	printf '%-34s %s\n' "$scene" "$(grep -hoE 'sprites from the draw API.*' "$out/run.log" | head -1)"
done
