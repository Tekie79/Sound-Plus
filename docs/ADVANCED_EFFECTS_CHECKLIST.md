# Advanced Delay and Reverb v1 — tasks in progress

Reviewed/start HEAD `c2c3f6b84fc2ec1d56e0aa03889bc756fe2ba941`; no later commits or working changes at initial inspection. Read the full attached review and existing native, automation, MIDI and take contracts. Preserve original media, finalized paths, latest user session and all historical DSP/serialization.

◉ A Contracts/foundation: three new definition-keyed effect identities, constant project tempo, structural/live controls, explicit IR reference/routing, bounded memory/tails/PDC, manifest16 and recipe7 only for new effects; focused schema/identity/compatibility tests; independent commit/push.

○ B Advanced Delay: four independent taps/free+synced subdivisions, stable role-based feedback, ping-pong, modulation/damping/ducking, preallocation; independent PCM/partition/stability/automation tests; commit/push.

○ C Algorithmic Room/Plate/Hall: independent early/late network, pre-delay/size/RT60/damping/diffusion/width/ducking and explicit LFE; independent response/decay/partition/history/take tests; commit/push.

○ D Convolution: local content-hashed referenced IRs, explicit mono/stereo/true-stereo/role maps, partitioned bounded FFT DSP and latency, missing/changed/relink workflow; independent direct/FFT/resampling/partition/take tests; commit/push.

○ E Existing one-lane automation, presets/locks/Undo, native UI at normal/minimum, realistic multi-owner/16-channel/MIDI/device stress, final unchanged Release vendor/AUv2/device gate and source/binary identities, latest-workspace audit/reopen, closure1–31/report/roadmap and commit/push.

Human listening, physical surround/controller/DAC loopback, Resolve receiver persistence, oversampling/true-peak, third-party writable automation, extra Perform pads and provider work are separate qualifications or future work. Never mark tool attempts as verified passes.
