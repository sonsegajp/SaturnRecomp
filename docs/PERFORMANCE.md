# Runtime performance

SaturnRecomp's shared runtime is profiled with real disc images and repeatable
controller sequences. Performance changes must preserve guest clocks, rendering
and sound behavior. A faster boot alone does not establish faster gameplay.

## September 2026 measurements

The local test machine has an Intel Core i5-10300H and an NVIDIA GTX 1660 Ti
Max-Q. Runs use the Vulkan renderer, native internal resolution, interpolation
disabled, an uncapped hidden window, and fresh, isolated settings and console
state. SDL's dummy audio device suppresses physical output; the 68000, SCSP mixer
and sound DSP still run normally.

The initial seven-title hotspot survey recorded these costs in selected
300-field intervals:

| Title and observed workload | Emulation, ms/field | Native composition, ms/field |
| --- | ---: | ---: |
| Sonic R, race | 13.37 | 0.41 |
| Fighting Vipers, active fight | 21.26 | 0.45 |
| NiGHTS, on-foot stage | 21.08 | 0.34 |
| Burning Rangers, training room | 20.77 | 0.33 |
| Daytona USA, attract sequence | 21.85 | 0.34 |
| Gunbird, opening movie | 14.89 | 0.31 |
| Sonic 3D Blast, opening movie | 18.37 | 0.28 |

These diagnostic measurements identify CPU emulation as the main cost at native
quality. They are an exploratory survey, not a controlled speedup comparison.
Other development work overlapped some runs. They do not measure the cost of
optional higher internal resolution, supersampling or interpolated pictures.

The CPU call-region counters include work performed by callees: for example,
the master/slave regions include sound synchronization caused by guest bus
accesses. The separate sound/CD region measures scheduled sound and disc work.
These counters cannot be interpreted as isolated instruction-level samples.

## Complete-workload comparisons

The final candidate combines PGO and the timer change below. These separate
runs disable profiling counters, frame capture and audio capture, alternate
build order, and time the whole process: boot, loading, menus and the scripted
scene. Both builds must finish at the same PC and master-cycle count, and the
candidate must explicitly confirm the requested number of fields.

| Workload | Pairs | Baseline seconds | Optimized seconds | Median time reduction |
| --- | ---: | ---: | ---: | ---: |
| Sonic R, race | 1 | 83.78 | 80.68 | 3.7% |
| Fighting Vipers, active fight | 2 | 47.42 | 40.94 | 13.7% |
| NiGHTS, on-foot stage | 2 | 136.05 | 132.77 | 2.4% |
| Burning Rangers, training room | 1 | 227.01 | 210.81 | 7.1% |
| Daytona USA, attract sequence | 1 | 139.50 | 94.37 | 32.4% |
| Gunbird, opening movie | 1 | 46.20 | 36.94 | 20.1% |
| Sonic 3D Blast, opening movie | 2 | 59.69 | 54.49 | 8.7% |

Fighting Vipers improved in both run orders: wall time fell 13.1–14.3% and
process CPU time fell 12.0–12.7%. NiGHTS was mixed: one pair improved 5.0%, while
the reversed-order pair took 0.35% longer. Its small median reduction does not
establish a stable wall-time improvement. Single pairs are observations with
limited repeatability evidence, and these totals are not steady gameplay FPS.

Sonic 3D Blast was also mixed. Its first pair was 3.7% slower; reversing the
order made the candidate 20.2% faster. Both samples remain in the report. This
does not establish a stable improvement or regression for that opening scene.

[Individual paired samples](performance/runtime-2026-09.csv) retain wall and
process CPU times, executable hashes and terminal guest state. A blank confirmed
field count identifies the older baseline executable; the corresponding
candidate verifies the budget and reaches the same PC and cycle count.

## Timer advancement

The shared SH-2 timer now advances a span in one operation when no compare match
or overflow can occur. Event crossings retain the original tick-by-tick path,
including compare-A clearing before compare B observes the counter. Single-tick
spans keep their short path. This change also applies to ordinary builds.

The regression compares complete CPU state against the original implementation
over 200,040 enabled/disabled cases, including prescalers, equal compares,
overflow, large spans and accumulator wrap. Six captured game CPU states also
matched after every one of 65,536 timer calls per state.

Nine interleaved timer microbenchmarks measured a 41–60% reduction in combined
master/slave timer cost for the captured Fighting Vipers, NiGHTS and Sonic R
states. The single-tick master cases cost 1.3–1.7 more CPU cycles per call; their
multi-tick slave cases save about 33. These figures concern the timer function,
which is only a small part of a field, and are not whole-game speedups.

## Profile-guided builds

The optional profile-guided optimization (PGO) build trains the SH-2 interpreter,
68000, SCSP mixer and sound DSP on actual game execution. The compiler uses those
branch frequencies to optimize host code. Guest instruction timing, rendering
quality and sound processing remain the same. The ordinary build works without
training data.

For this comparison, one instrumented binary ran Sonic R for 6,600 fields,
Fighting Vipers for 3,600, NiGHTS for 6,000, and Burning Rangers for 10,500. All
four runs exited normally. Every core's profile merged four runs and matched
the original compiler stamps, checksums and source hashes. The final optimized
executable contains no compiler profiling instrumentation.

See the [build instructions](../README.md#shared-runtime-optimization-builds)
and [benchmark workflow](TESTING.md) to collect profiles and compare builds on
your hardware. Keep the same object and profile directories between generation
and use; regenerate training data after editing profiled sources.

An inactive-slot LFO calculation experiment was also evaluated and rejected.
Tightly interleaved comparisons found it 1.2–2.9% slower in median CPU cycles
across three captured audio workloads despite identical PCM and machine state.
That experiment did not change the production mixer.

## Output and interaction checks

The final core objects pass the SH-2, 68000, cache, timer and sound-command
regressions, including 20,000 randomized SH-2 programs. The standard suite
reports 33 passed groups, one missing-media skip and no failures. The benchmark
tool itself has 13 checks covering ordering, workload completion and invalid
measurements.

The final executable also passes all 25 native F1 integration checks: resizing,
graphics/motion/audio changes, keyboard behavior and persistent settings,
including a temporary Windows file lock. The emulated game remains paused at
field three throughout those checks. This supplements the earlier renderer
and closed-panel overhead checks described in [TESTING.md](TESTING.md).

Completed output checks on the final executable include Sonic R and Fighting
Vipers at 120 Hz interpolation: their native checkpoints match the baseline
pixel-for-pixel, and the common internal-mix PCM prefixes are byte-identical
for 110.317 and 60.172 seconds respectively. NiGHTS' native checkpoint also
matches, and its inserted pictures were inspected. Its fresh baseline audio
and inserted-picture comparison was not completed. These are specific output
checks, not a claim that every scene or every game's audio has been validated.

## Reading the results

Native fields per second measure emulation throughput. A Saturn game may update
its geometry every two or three fields, so that number is not the number of
distinct animation frames. Interpolation adds presentation pictures without
changing the game's simulation rate.

Full-run wall time includes boot, loading and the supplied input sequence.
Selected gameplay intervals and full-run timing answer different questions.
Counter-free paired runs alternate build order and report every repeat alongside
the median. A result from a single machine or selected scene does not establish
full-speed performance throughout a game.

Game images, BIOS files, raw machine snapshots and captured audio stay in local
ignored output directories. Published results describe the workload and checks
without distributing game data. Audio-state and PCM comparisons establish the
paths they test; dummy-device benchmarks do not verify physical speaker output.
