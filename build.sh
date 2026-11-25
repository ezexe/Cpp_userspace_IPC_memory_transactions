#!/usr/bin/env bash
# Builds and runs everything behind docs/. Linux x86-64, GCC 13+ (Clang 18 also works: CXX=clang++).
# JUCE glue (optional): ./build.sh glue. JUCE defaults to the checkout on the Windows drive, /mnt/e/gritz/JUCE
# (E:\gritz\JUCE, where this project used to sit beside it); override with JUCE=/path/to/JUCE.
# Compile database for editors: ./build.sh compdb  -> .out/compile_commands.json
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-g++}
JUCE=${JUCE:-/mnt/e/gritz/JUCE}
W="-std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion"
D="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG=1 -DJUCE_STANDALONE_APPLICATION=1"
mkdir -p .out
case "${1:-all}" in
  utx|all)
    $CXX $W utx/test.cc -o .out/utx_test
    $CXX -std=c++20 -O2 utx/probes/pure.cc -o .out/pure
    $CXX -std=c++20 -O2 utx/probes/layout.cc -o .out/layout
    $CXX -std=c++20 -O1 -g -fsanitize=thread utx/probes/tsan.cc -o .out/tsan 2>/dev/null
    $CXX -O2 -mrtm utx/probes/rtm.cc -o .out/rtm
    $CXX -O2 -fgnu-tm utx/probes/tm.cc -o .out/tm
    ;;&
  duplex|all)
    $CXX $W duplex/duplex_test.cc -o .out/duplex_test
    ;;&
  run|all)
    .out/utx_test all; .out/pure; .out/layout
    # ASLR off for the TSan run: with it on, this gcc's TSan runtime can die at startup ("unexpected memory mapping").
    setarch "$(uname -m)" -R .out/tsan 2>&1 | tail -1
    .out/rtm
    for m in "" ml_wt gl_wt serial htm; do echo "ITM_DEFAULT_METHOD=$m"; ITM_DEFAULT_METHOD=$m .out/tm t; ITM_DEFAULT_METHOD=$m .out/tm; done
    .out/duplex_test all
    ;;
  glue)
    J="$JUCE/modules"
    [ -d "$J" ] || { echo "no JUCE checkout at $JUCE (is the E: drive mounted?); set JUCE=/path/to/JUCE" >&2; exit 1; }
    $CXX -std=c++20 -O2 $D -I"$J" -c "$J/juce_core/juce_core.cpp" -o .out/juce_core.o
    $CXX -std=c++20 -O2 $D -I"$J" -c "$J/juce_core/juce_core_CompilationTime.cpp" -o .out/juce_ct.o
    $CXX -std=c++20 -O2 $D -I"$J" -c "$J/juce_audio_basics/juce_audio_basics.cpp" -o .out/juce_audio_basics.o
    $CXX -std=c++20 -O2 $D -I"$J" -c duplex/juce/glue.cc -o .out/glue.o
    $CXX .out/glue.o .out/juce_audio_basics.o .out/juce_core.o .out/juce_ct.o -o .out/glue -lpthread -ldl -lz
    .out/glue
    ;;
  compdb)
    # One entry per translation unit, with the flags the targets above use. Headers take their flags from these.
    C=$(command -v "$CXX"); P=$PWD
    entry() { printf '  {"directory": "%s", "file": "%s/%s", "command": "%s %s %s"}' "$P" "$P" "$1" "$C" "$2" "$1"; }
    {
      echo "["
      entry utx/test.cc "$W"; echo ","
      for f in pure layout; do entry "utx/probes/$f.cc" "-std=c++20 -O2"; echo ","; done
      entry utx/probes/tsan.cc "-std=c++20 -O1 -g -fsanitize=thread"; echo ","
      entry utx/probes/rtm.cc "-O2 -mrtm"; echo ","
      entry utx/probes/tm.cc "-O2 -fgnu-tm"; echo ","
      entry duplex/duplex_test.cc "$W"
      if [ -d "$JUCE/modules" ]; then echo ","; entry duplex/juce/glue.cc "-std=c++20 -O2 $D -I$JUCE/modules"; fi
      echo; echo "]"
    } > .out/compile_commands.json
    echo "wrote .out/compile_commands.json$([ -d "$JUCE/modules" ] && echo " (with glue.cc against $JUCE)")"
    ;;
esac
