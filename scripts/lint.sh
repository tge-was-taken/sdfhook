#!/usr/bin/env bash
# Whole-program checks the compiler cannot make. The one that matters is unusedFunction: code
# written for a purpose and never wired up is invisible to -Wall, since it only warns about unused
# statics. applyGsRegisterWrites and ModelParser::getGsState sat unused while the GS state they
# exist to track stayed hardcoded, and that was the largest single error in the render.
#
# clang-tidy cannot answer this: it runs per translation unit, so a public member always looks like
# it might be called from another one. cppcheck sees the whole program at once.
set -uo pipefail
cd "$(dirname "$0")/.."
exec cppcheck --enable=unusedFunction --inline-suppr --quiet --language=c++ --std=c++20 \
	--suppress=missingInclude --suppress=missingIncludeSystem \
	-I src -I vendor/pcsx2-hook/pcsx2-hook/include src "$@"
