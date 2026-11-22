# MIDI correction and responsiveness — Stage A

Review/start HEAD f3c5b89e6e77a6666233a695f27760b672054840; no newer commits or unrelated changes at start. Date 2026-11-22. Stage A is committed independently before Collections implementation.

`compileMidi` joins unchanged continuation fragments within the target track/origin/payload and requires both absolute and source-offset adjacency. Other tracks and independent overlapping clips cannot interrupt a chain. Earliest unsplit order is restored after joining. Editing notes/controllers, quantizing or separating ranges prevents false joins. No serialized MIDI/project/recipe format changes.

Regression first failed with interleaved fragments (`verification/collections/stage-a/split-reproduction.log`). New tests compare every packet field, controller/sustain history, native sampler PCM and independent owned hosted-instrument oracle PCM for two/three fragments at44.1/48/96k. Actual view Cmd-E/session Undo/Redo, project reopen, edits and existing historical recipe tests are retained. The long-release edited split test investigated a possible release collision and passed; it is not represented as a reproduced defect.

## Scheduling policy and evidence

Only active MIDI monitoring uses a callback-aware producer target: callback frames + max(12 ms, 2×observed producer maximum+2 ms), capped at the previous1536 frames, with256 minimum. Ordinary Library/audio buffering remains1536. The playback service receives its MIDI pointer before worker construction. Callback code only publishes frame-count/clock atomics; telemetry sorting, event acquisition/journaling and graph preparation remain outside it. Invalid clock/rate defers acquisition, existing overflow/stale-generation/panic rules remain.

`MidiInputService::status().schedulingLatency` exposes a bounded8192-event acquisition→applied distribution with event/window count, p50/p95/p99/max. Capture journals retain original acquisition/host timestamps and applied frames. Delay is scheduling lookahead, not physical MIDI/controller, DAC, acoustic or plugin PDC latency.

Fresh controlled production-worker comparison: actual native virtual CoreMIDI source, production AudioPlaybackService, steady-clock synthetic callback thread,200 packets each case,128/512/1024 frames,44.1/48/96k, idle/four busy CPU threads. Independent journal quantiles agree with telemetry; no packets lost, reordered or stale-generation adopted. Sampler PDC=0; preparation, producer maximum and callback jitter reported separately in JSON. This is not a physical device callback timing benchmark. A6ms pilot incurred one underrun at96k/128 idle; retained separately. Raising the safety floor to12ms produced zero underruns in all36 comparison cases. Larger/high-rate buffers preserve the old cap and offer little reduction.

| Rate | Buffer | Load | Policy | p50 / p95 / p99 / max ms | Underruns |
|---|---|---|---|---|---|
| 44100 | 128 | idle | fixed1536 | 34.83 / 37.14 / 37.73 / 37.73 | 0 |
| 44100 | 128 | idle | buffer-aware | 15.10 / 17.10 / 17.82 / 17.96 | 0 |
| 44100 | 128 | four CPU workers | fixed1536 | 35.44 / 37.21 / 37.71 / 37.73 | 0 |
| 44100 | 128 | four CPU workers | buffer-aware | 15.26 / 17.28 / 17.82 / 17.82 | 0 |
| 44100 | 512 | idle | fixed1536 | 38.84 / 44.24 / 45.78 / 45.94 | 0 |
| 44100 | 512 | idle | buffer-aware | 28.39 / 33.83 / 34.85 / 35.12 | 0 |
| 44100 | 512 | four CPU workers | fixed1536 | 39.52 / 45.03 / 45.92 / 46.10 | 0 |
| 44100 | 512 | four CPU workers | buffer-aware | 28.25 / 34.22 / 35.03 / 35.26 | 0 |
| 44100 | 1024 | idle | fixed1536 | 44.74 / 55.51 / 57.78 / 57.87 | 0 |
| 44100 | 1024 | idle | buffer-aware | 44.94 / 55.33 / 57.46 / 57.53 | 0 |
| 44100 | 1024 | four CPU workers | fixed1536 | 44.31 / 54.97 / 58.03 / 58.05 | 0 |
| 44100 | 1024 | four CPU workers | buffer-aware | 44.72 / 55.28 / 57.51 / 57.80 | 0 |
| 48000 | 128 | idle | fixed1536 | 32.10 / 34.08 / 34.67 / 34.67 | 0 |
| 48000 | 128 | idle | buffer-aware | 14.73 / 16.58 / 17.19 / 17.60 | 0 |
| 48000 | 128 | four CPU workers | fixed1536 | 32.10 / 34.19 / 34.67 / 34.69 | 0 |
| 48000 | 128 | four CPU workers | buffer-aware | 14.69 / 16.79 / 17.21 / 17.21 | 0 |
| 48000 | 512 | idle | fixed1536 | 36.23 / 41.25 / 42.21 / 42.50 | 0 |
| 48000 | 512 | idle | buffer-aware | 26.19 / 31.21 / 31.96 / 32.27 | 0 |
| 48000 | 512 | four CPU workers | fixed1536 | 35.77 / 40.92 / 41.92 / 42.67 | 0 |
| 48000 | 512 | four CPU workers | buffer-aware | 26.62 / 31.85 / 33.00 / 33.23 | 0 |
| 48000 | 1024 | idle | fixed1536 | 41.08 / 50.44 / 52.33 / 53.23 | 0 |
| 48000 | 1024 | idle | buffer-aware | 41.35 / 51.04 / 52.56 / 52.60 | 0 |
| 48000 | 1024 | four CPU workers | fixed1536 | 41.58 / 51.06 / 52.71 / 53.12 | 0 |
| 48000 | 1024 | four CPU workers | buffer-aware | 41.46 / 51.10 / 52.79 / 52.90 | 0 |
| 96000 | 128 | idle | fixed1536 | 15.60 / 16.89 / 17.17 / 17.18 | 0 |
| 96000 | 128 | idle | buffer-aware | 12.53 / 14.42 / 14.66 / 14.67 | 0 |
| 96000 | 128 | four CPU workers | fixed1536 | 15.66 / 17.11 / 17.33 / 17.33 | 0 |
| 96000 | 128 | four CPU workers | buffer-aware | 13.33 / 14.33 / 14.65 / 14.67 | 0 |
| 96000 | 512 | idle | fixed1536 | 17.49 / 20.27 / 21.05 / 21.31 | 0 |
| 96000 | 512 | idle | buffer-aware | 17.53 / 20.41 / 20.85 / 21.03 | 0 |
| 96000 | 512 | four CPU workers | fixed1536 | 17.20 / 20.25 / 20.92 / 21.02 | 0 |
| 96000 | 512 | four CPU workers | buffer-aware | 17.31 / 20.28 / 21.07 / 21.09 | 0 |
| 96000 | 1024 | idle | fixed1536 | 19.77 / 24.58 / 25.61 / 26.24 | 0 |
| 96000 | 1024 | idle | buffer-aware | 20.12 / 25.27 / 26.04 / 26.07 | 0 |
| 96000 | 1024 | four CPU workers | fixed1536 | 19.83 / 24.65 / 26.40 / 26.41 | 0 |
| 96000 | 1024 | four CPU workers | buffer-aware | 20.02 / 24.81 / 26.67 / 26.67 | 0 |

Full data: `verification/collections/stage-a/latency-12ms.json`; rejected pilot: `latency-6ms-pilot.json`. Each points to durable per-case journals under build/midi-responsiveness. Fresh native UI capture in a separate catalog used MacBook Pro Speakers and the owned CoreMIDI source:186/186 events retained, p50=39.125ms, p95=42.895833ms, p99/max=44.666667ms at48k project/source rate. The actual device buffer/rate was not newly measured. This is unpaired with the previous126-event capture and is not evidence of a physical latency improvement. Native Record/Stop/adoption/Save completed; see `native-latency.json`, `native-output-ax.txt` and `native-output.jpg`.

Targeted final relevant gate8/8 passed23.68s (MIDI, microphone prefix, automation, take integrity, recorded takes, Designer view/session and instruments). The preceding qualification run explicitly enabled AU:3/3 passed72.92s. Complete unchanged-build qualification follows below. Physical controller/loopback, listening, physical surround, broad vendor instruments and actual Resolve receiver drag remain unqualified. Original in-place r4478 workspace protected and kept closed throughout this stage; isolated QA catalogs only.

Complete final Stage A Release gate: **38/38 passed in385.30s**, with SOUNDPLUS_VENDOR_QUALIFICATION, SOUNDPLUS_LIVE_DEVICE_QUALIFICATION=1 and SOUNDPLUS_QUALIFY_AU=1. Before/after identities agree for298 source/resource files and8 binaries (`verification/collections/stage-a/release`). Log `full-gate.log`; fresh gate timing matrix retained separately in `full-gate-latency.json`. No source or binary edits occurred during the gate.
