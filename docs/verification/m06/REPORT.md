# Perform source triggers

In Perform, assign a pooled layer to a corner. Open its card menu while stopped and enable live sources. Play or record, then use Trigger for a one-shot, Switch source for an equal-power transition, or T on the focused card. MIDI Learn accepts a note or CC from the chosen device. Note velocity sets amplitude. A held key fires once.

Each corner has four independent voices. A full corner first steals its oldest one-shot or releasing voice, then the oldest sustained voice. The prepared bank allows 64 entries and 128 MiB across the graph. Each alternate starts with fresh source insert history and up to two seconds of baked source tail. Frozen source input stops at the take boundary. Downstream track, group and master processing continues through its configured tail.

The helper stamps accepted events on its producer clock. A checkpoint includes only the device-consumed prefix. The saved recipe records the entry, pad, slot, roles, source fingerprint, overlap and event order. An unprepared entry or changed file stamp rejects the request. Content hashes are checked during graph preparation. Arming a pool or learning a controller alone leaves previous frozen identities intact.

The Release source-pool suite tested every stereo sample at 44.1, 48 and 96 kHz, a trigger at frame 1200, a 100 ms switch at frame 2000, replay at different block sizes, seeking into the overlap, a helper checkpoint, altered sources, deterministic steals and the frozen boundary. A private CoreMIDI endpoint tested note velocity, held keys, release/retrigger, unplug and saved mapping. Six focused suites passed in 16.87 seconds. M09 retains physical controllers, listening, large multichannel stress and receiver persistence.
