# Native square panner checks

The generated QA design used one Stereo source and a 5.1 Rear Master. The panner opened through Track Options. Native actions activated the conversion preview, dragged the object, nudged it with Shift-Right, added a double-click point, changed its exact time to 1.500 s, moved a point, and exercised Undo/Redo. The window required explicit field focus and floating order for reliable pointer input. Both were added and checked again.

During native playback a pointer pass added one applied pair and a release/return around the two existing paths. The saved r13 fixture contains the same five X/Y frames (0, 72000, 463359, 463360 and 468160). The final pair returns from the release frame in 4800 frames at 48 kHz. An earlier immediate stop reported no applied motion. That attempt left both Read curves at two points. A separate sustained CoreAudio test applies dozens of points and checks no graph, transport or helper restart. The full output test checks speaker roles and PCM separately.

The final candidate reopened r13 with its paths. The minimum panner capture shows a 350×400 window. The initial minimum capture exposed overlap between the LFE hint and the numeric row. The final capture verifies the revised footer. Edit minimum is 1040×680 including its native title bar. Browser and Inspector remain open. AX shows the existing single expanded Volume-first lane on the Audio Track and the collapsed Master lane. Native clipboard, deletion and Undo commands changed four isolated revisions.

Screenshots and state snapshots contain generated audio and QA data. These checks establish the native layout, interaction and producer retention. Human listening and measured physical speaker positions remain open.
