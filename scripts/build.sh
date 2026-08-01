#!/usr/bin/env bash
# Configures and builds signalforge into ./build.
#
#   ./scripts/build.sh                 configure (if needed) and build
#   ./scripts/build.sh --clean         wipe ./build first, then configure and build
#   ./scripts/build.sh --test          also run ctest after a successful build
#   ./scripts/build.sh --release       configure with CMAKE_BUILD_TYPE=Release
#                                       (the default is whatever CMakeLists.txt
#                                       defaults to, currently Release itself)
#
# Anything else is passed straight through to the *configure* step, so
#   ./scripts/build.sh -DSIGNALFORGE_TEST_MODEL="$HOME/models/some capture.nam"
# works the same as calling cmake directly (see README.md).
#
# Re-running without --clean is incremental: cmake only reconfigures when
# CMakeLists.txt or a cache variable actually changed, and the build only
# recompiles what changed since last time.

set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${SF_BUILD_DIR:-$here/build}"

do_clean=0
do_test=0
cmake_args=()

for arg in "$@"; do
	case "$arg" in
	--clean)
		do_clean=1
		;;
	--test)
		do_test=1
		;;
	--release)
		cmake_args+=(-DCMAKE_BUILD_TYPE=Release)
		;;
	--debug)
		cmake_args+=(-DCMAKE_BUILD_TYPE=Debug)
		;;
	*)
		cmake_args+=("$arg")
		;;
	esac
done

if [[ $do_clean -eq 1 && -d "$build_dir" ]]; then
	echo "Removing $build_dir" >&2
	rm -rf "$build_dir"
fi

jobs="${SF_BUILD_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

cmake -B "$build_dir" -S "$here" "${cmake_args[@]}"
cmake --build "$build_dir" -j "$jobs"

echo "Built: $build_dir/src/signalforge"

if [[ $do_test -eq 1 ]]; then
	ctest --test-dir "$build_dir" --output-on-failure
fi
