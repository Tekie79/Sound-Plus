# Virtual Library organization contract

Collections share the authoritative Library SQLite catalog and one serialized background writer. The same LibraryView instance appears full-size and in Designer. Source audio, source/channel interpretation, waveform caches, embeddings, index generations and audio/project recipes remain independent of collection metadata.

## Identity and retention

Catalog v7 adds stable UUID collections and references. Full-source references and half-open original-sample region references are distinct. Region identity includes source UUID, revision and exact [first,last), plus cached source/catalog provenance. A region is usable only while revision, sample count and rate agree; changed sources are Needs Review. Missing source rows retain cached unresolved references. Never reassign a reference by filename. Offline current identities retain membership and become usable again when online. Saving metadata never creates audio files.

Names are trimmed, bounded and unique case-insensitively among siblings. At most1024 collections and16 nesting levels. Reparent checks both cycles and resulting descendant depth. Reorder is explicit among siblings. Collection deletion removes only its virtual subtree/memberships, with confirmation; Favorites, referenced source audio and Designer clips survive. Undo can restore deleted hierarchy. Bulk metadata changes accept at most4096 references per action; oversized deletion is rejected with an actionable error rather than blocking indefinitely. Metadata actions are atomic and have their own durable bounded Undo/Redo history. Recently Used records explicit audition/add use and is not an undoable source edit.

## Search and original regions

All Sounds preserves existing fast FTS browse and semantic ranking. Favorites, Recent and a collection (including descendants) intersect the source-root filter. Scope eligibility is applied before semantic ranking; region candidates use existing overlapping embedding windows clipped to eligible original intervals. This does not create new short-event embeddings or imply sample-precise semantic localization. Multiple bookmarks in one source remain distinct references/results. Browsing/metadata changes do not stop or silently switch playing/pinned audio.

Saved region auditions/adds/drags use the stored interval, independent of a broad embedding window. Stale/unresolved regions remain visible but cannot silently deliver old coordinates. The user must explicitly review current audio and create a fresh bookmark. Original full-file external drag semantics remain unchanged; cross-application persistence is a separate qualification.

## UI and boundaries

All Sounds, Favorites, Recently Used and My Collections share one tree/scope. Compact Designer browser exposes the same tree, root filter and metadata actions without changing Perform sizing or Inspector. Local-only operations remain available with paused indexing/offline drives. No decorative AI Generated scope is shown because this catalog has no implemented generated-source class.

Physical MIDI/loopback, human listening, physical surround, external receiver drag, pads B–D/overdubs, new effects and external AI generation are future work. This increment preserves the current single reusable automation lane per processable track.
