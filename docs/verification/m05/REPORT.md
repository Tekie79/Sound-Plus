# Automation development checks

The native lane retains independent visible and armed targets. Mixer faders and editable stock controls use the same session-owned scalar pass. Read, original Touch, paired Square Touch and passive hidden curves retain their existing contracts. Only one scalar pass owns the producer at a time. MIDI controller input consumes CC messages independently of instrument note capture.

An armed Write pass is prepared before graph priming and FIFO fill. Its half-open bounds use project frames. Stop retains only the device-consumed prefix. Repeat punch range starts a fresh compensated graph and Undo transaction on each completed pass. Preparation between passes can introduce a monitoring gap. Replace removes prior points inside the interval. Merge keeps prior noncolliding points and gives the new pass ownership at duplicate frames. Preview/Cancel never commits provisional data.

Relative Trim stores separate dB or octave intervals. Clip follow and advanced automation are opt-in. Existing recipes preserve their historical encoding. Qualified hosted targets keep their indexed, fingerprinted bindings and existing scheduler tolerance.

The Release evidence includes [scalar capture](scalar.log), [relative trim](trim.log), [the private CoreMIDI endpoint and hosted AU/VST3](hosted-midi.log), [pad/curve/session integration](modes.log), and [transport boundaries and hosted regression](punch.log). A transient AU preset-recall failure during another regression run exposed an ordering gap between the native program notification and the next Read block. The helper now observes program identity on the native editor thread before reapplying Read.

Physical controller feedback, human listening, and operator workflows remain for M09.

The AU preset regression passed [three consecutive runs](au-preset.log) after the ordering fix. The final focused Release build passed [10/10](regression.log), including VST3 and AU, transport, both pad paths, the mixer and virtual CoreMIDI. This is development verification. Full operator workflows remain open.
