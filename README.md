# how-can-this-be: shared-memory transactions and the duplex audio callback

The sources behind two project docs, laid out so each doc's code sits beside it.

| Path | What | Doc |
|---|---|---|
| `docs/userspace-ipc-transactions.md` | SeqCell, ShadowCell, TxRing, TxRegion; RTM and libitm across processes | — |
| `docs/duplex-audio-block.md` | processBlock as producer and consumer of audio and MIDI against a peer | — |
| `utx/utx.hpp` | the four structures (self-contained header) | first |
| `utx/test.cc` | cross-process kill tests and timings (`all`, `seq`, `shadow`, `ring`, `region`, `bench`) | first |
| `utx/probes/` | seccomp purity proof, TSan run, layout dump, RTM, libitm, C++23 lifetime probes | first |
| `utx/ts/` | ShadowCell from TypeScript over a SharedArrayBuffer | first §8 |
| `duplex/duplex.hpp` | SampleRing, MidiLane, HostEnd, PeerEnd (needs `utx/utx.hpp`) | second |
| `duplex/duplex_test.cc` | host/peer processes, faults, all configs (`all`, `unit`, `latency`, `order`, `faults`, `lockstep`, `fifo`, `pure`) | second |
| `duplex/juce/glue.cc` | processBlock glue against JUCE's AudioBuffer/MidiBuffer, allocation counting | second §6 |
| `duplex/ts/` | JS Atomics facts and wrap-safe 32-bit positions | second §8a |
| `results/` | the output of the runs the second doc quotes | second |

## Build

Linux x86-64 (the tests use `fork`, `shm_open`, seccomp; WSL 2 should do), GCC 13.3 or Clang 18:

```
./build.sh            # build everything and run it (about 4 minutes; the duplex suite is most of it)
./build.sh duplex     # build only
CXX=clang++ ./build.sh utx
JUCE=~/src/JUCE ./build.sh glue   # needs a JUCE checkout (tested at 9.0.6) and zlib
```

`rtm` needs a CPU with TSX enabled; elsewhere it reports RTM=0 and stops.

TypeScript: `tsc --strict --noUncheckedIndexedAccess --target es2022 --module commonjs --outDir <out> <files>`, then run the emitted `run`/`check` with Node 22. No compiled `.js` is included.

Not built or run on Windows/MSVC: the headers are standard C++20 plus `_mm_pause`, but the tests are POSIX.
