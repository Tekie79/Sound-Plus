"""SoundPlus local Resolve Studio bridge. One request and one durable JSON response."""
import hashlib
import json
import os
import re
import sys
from fractions import Fraction
from pathlib import Path


RATE = {"23.976": (24, Fraction(24000, 1001)), "24": (24, Fraction(24)),
        "25": (25, Fraction(25)), "29.97": (30, Fraction(30000, 1001)),
        "30": (30, Fraction(30)), "50": (50, Fraction(50)),
        "59.94": (60, Fraction(60000, 1001)), "60": (60, Fraction(60))}
TRACKS = {"mono-v1": "mono", "stereo-v1": "stereo", "5.0-v1": "5.0",
          "5.1-side-v1": "5.1", "5.1-rear-v1": "5.1", "7.1-v1": "7.1"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def timecode_frame(code, nominal, drop):
    m = re.fullmatch(r"(\d{2}):(\d{2}):(\d{2})[:;](\d{2})", str(code))
    require(m is not None, "Resolve must display playhead timecode as HH:MM:SS:FF")
    hour, minute, second, frame = map(int, m.groups())
    require(hour < 24 and minute < 60 and second < 60 and frame < nominal, "Invalid Resolve timecode")
    count = nominal * (hour * 3600 + minute * 60 + second) + frame
    if drop:
        skipped = nominal // 15
        require(not (minute % 10 and second == 0 and frame < skipped), "Resolve returned a skipped DF label")
        count -= skipped * (hour * 60 + minute - (hour * 60 + minute) // 10)
    return count


def timing(timeline):
    rate_setting = str(timeline.GetSetting("timelineFrameRate"))
    fields = rate_setting.split()
    rate_key = fields[0] if fields else ""
    if "." in rate_key:
        rate_key = rate_key.rstrip("0").rstrip(".")
    require(rate_key in RATE, "Unsupported Resolve timeline frame rate: " + rate_setting)
    nominal, rate = RATE[rate_key]
    start, cursor = timeline.GetStartTimecode(), timeline.GetCurrentTimecode()
    drop_setting = str(timeline.GetSetting("timelineDropFrameTimecode")).lower()
    drop = (";" in str(start) or ";" in str(cursor) or re.search(r"\bDF\b", rate_setting.upper()) is not None
            or drop_setting in ("1", "true"))
    require(not drop or rate_key in ("29.97", "59.94"), "Unexpected Resolve drop-frame setting")
    relative = timecode_frame(cursor, nominal, drop) - timecode_frame(start, nominal, drop)
    require(relative >= 0, "Resolve playhead precedes the active timeline start")
    start_frame = timeline.GetStartFrame()
    require(isinstance(start_frame, (float, int)) and start_frame >= 0, "Resolve timeline start is unavailable")
    return int(start_frame) + relative, cursor, rate, drop


def connect():
    import DaVinciResolveScript
    resolve = DaVinciResolveScript.scriptapp("Resolve")
    require(resolve, "Resolve Studio is unreachable. Open it and allow Local external scripting.")
    version = resolve.GetVersionString()
    project = resolve.GetProjectManager().GetCurrentProject()
    require(project, "Resolve has no active project")
    timeline = project.GetCurrentTimeline()
    require(timeline, "Resolve has no active timeline")
    require(resolve.GetCurrentPage() in ("edit", "cut", "fairlight"), "Open the Resolve Edit, Cut or Fairlight page")
    identity = {"project": project.GetUniqueId(), "timeline": timeline.GetUniqueId(), "resolveVersion": version}
    require(identity["project"] and identity["timeline"], "Resolve did not report stable project and timeline IDs")
    return resolve, project, timeline, identity


def track_items(timeline, track):
    items = timeline.GetItemListInTrack("audio", track)
    require(isinstance(items, list), "Resolve did not report track contents")
    return items


def safe_track(timeline, index, subtype, first, end):
    require(timeline.GetTrackSubType("audio", index) == subtype, "Resolve track layout does not match the media")
    require(timeline.GetIsTrackLocked("audio", index) is False, "Resolve audio track is locked or unavailable")
    for clip in track_items(timeline, index):
        lo, hi = clip.GetStart(True), clip.GetEnd(True)
        require(isinstance(lo, (int, float)) and isinstance(hi, (int, float)) and hi >= lo,
                "Resolve did not report reliable occupied intervals")
        require(not (lo < end and hi > first), "Resolve target is occupied at the playhead. Choose another track.")


def get_bin(pool):
    root = pool.GetRootFolder()
    matches = [b for b in root.GetSubFolderList() if b.GetName() == "SoundPlus Transfers"]
    require(len(matches) <= 1, "Multiple SoundPlus Transfers bins exist in Resolve")
    folder = matches[0] if matches else pool.AddSubFolder(root, "SoundPlus Transfers")
    require(folder, "Resolve could not open the SoundPlus Transfers bin")
    return folder


def verified_media(item, path, channels, sample_rate):
    info = item.GetClipProperty()
    require(info and info.get("File Path") and Path(info["File Path"]).resolve() == path,
            "Resolve media path does not match the verified file")
    require(info.get("Online Status") == "Online", "Resolve reported offline transfer media")
    require(str(info.get("Audio Ch", "")) == str(channels), "Resolve reported different audio channels")
    rate = str(info.get("Sample Rate", "")).replace(",", "").rstrip(" Hz")
    require(float(rate or 0) == sample_rate, "Resolve reported a different audio sample rate")
    require(item.GetMediaId(), "Resolve did not report a media identity")
    return info


def verify_mapping(encoded, subtype, channels):
    data = json.loads(encoded)
    mapping = data.get("track_mapping", {}).get("1", {})
    require(data.get("embedded_audio_channels") == channels and not data.get("linked_audio") and
            len(data.get("track_mapping", {})) == 1 and mapping.get("type", "").lower() == subtype and
            mapping.get("mute") is False and mapping.get("channel_idx") == list(range(1, channels + 1)),
            "Resolve did not verify the native source channel mapping")
    return data


def layout_matches_file(path, layout):
    required_masks = {"5.1-side-v1": 0x60f, "5.1-rear-v1": 0x3f}
    if layout not in required_masks:
        return
    with path.open("rb") as wav:
        require(wav.read(4) == b"RIFF", "A named 5.1 transfer needs a verified PCM WAV mask")
        wav.read(4)
        require(wav.read(4) == b"WAVE", "A named 5.1 transfer needs a WAVE header")
        while header := wav.read(8):
            require(len(header) == 8, "Truncated WAV header")
            length = int.from_bytes(header[4:], "little")
            if header[:4] == b"fmt ":
                fmt = wav.read(min(length, 40))
                require(len(fmt) >= 24 and fmt[:2] == b"\xfe\xff" and
                        int.from_bytes(fmt[20:24], "little") == required_masks[layout],
                        "WAV speaker mask does not match the requested 5.1 roles")
                return
            wav.seek(length + (length & 1), 1)
    raise RuntimeError("WAV channel format is missing")


def same_target(expected):
    resolve, project, timeline, identity = connect()
    require(identity["project"] == expected.get("project") and identity["timeline"] == expected.get("timeline"),
            "The active Resolve project or timeline changed after Send")
    return resolve, project, timeline, identity


def place(request, response):
    _, project, timeline, identity = same_target(request["target"])
    response.update(identity)
    layout = request["layout"]["id"]
    roles = request["layout"]["roles"]
    subtype = TRACKS.get(layout)
    require(subtype, "Resolve transfer cannot safely map this layout. The prepared file is available for drag.")
    path = Path(request["path"]).resolve(strict=True)
    with path.open("rb") as file:
        require(hashlib.file_digest(file, "sha256").hexdigest() == request["sha256"],
                "Transfer file changed before Resolve import")
    layout_matches_file(path, layout)
    first, playhead, rate, drop = timing(timeline)
    length = (int(request["samples"]) * rate / int(request["rate"]))
    frame_span = max(1, -(-length.numerator // length.denominator))
    count = timeline.GetTrackCount("audio")
    index = int(request.get("track", 0))
    dedicated = "SoundPlus " + layout.removesuffix("-v1")
    if index:
        require(1 <= index <= count, "Chosen Resolve track does not exist")
    else:
        candidates = [i for i in range(1, count + 1) if timeline.GetTrackName("audio", i) == dedicated]
        require(len(candidates) <= 1, "Ambiguous dedicated Resolve audio track. Choose a track number.")
        index = candidates[0] if candidates else 0
    if index:
        safe_track(timeline, index, subtype, first, first + frame_span)

    pool = project.GetMediaPool()
    prior = pool.GetCurrentFolder()
    folder = get_bin(pool)
    matching = [i for i in folder.GetClipList()
                if i.GetClipProperty("File Path") and Path(i.GetClipProperty("File Path")).resolve() == path]
    require(len(matching) <= 1, "Several matching Resolve media items exist in the transfer bin")
    if matching:
        media = matching[0]
        response["reusedMedia"] = True
    else:
        require(pool.SetCurrentFolder(folder), "Cannot open the dedicated Resolve media bin")
        try:
            imported = pool.ImportMedia([str(path)])
        finally:
            if prior:
                pool.SetCurrentFolder(prior)
        require(imported and len(imported) == 1, "Resolve did not import the verified audio file")
        media = imported[0]
    response["media"] = media.GetMediaId()
    response["mediaPath"] = str(path)
    info = verified_media(media, path, len(roles), request["rate"])
    verify_mapping(media.GetAudioMapping(), subtype, len(roles))

    same_target(identity)
    require(timeline.GetCurrentTimecode() == playhead, "Resolve playhead moved during import. The media is retained.")
    if not index:
        require(timeline.GetTrackCount("audio") == count, "Resolve tracks changed during import")
        require(timeline.AddTrack("audio", subtype), "Resolve could not add a compatible audio track")
        index = count + 1
        require(timeline.GetTrackCount("audio") == index and timeline.SetTrackName("audio", index, dedicated),
                "Resolve did not confirm the dedicated audio track")
    safe_track(timeline, index, subtype, first, first + frame_span)
    previous = {i.GetUniqueId() for i in track_items(timeline, index)}
    response["placementAttempted"] = True
    placed = pool.AppendToTimeline([{"mediaPoolItem": media, "mediaType": 2, "trackIndex": index,
                                     "recordFrame": first}])
    require(placed and len(placed) == 1, "Resolve imported media but did not confirm placement")
    item = placed[0]
    uid = item.GetUniqueId()
    response["observedTimelineItem"] = uid
    target = item.GetTrackTypeAndIndex()
    verify_item = next((clip for clip in track_items(timeline, index) if clip.GetUniqueId() == uid), None)
    require(uid and uid not in previous and verify_item and tuple(target or ()) == ("audio", index),
            "Resolve did not verify the new audio-only timeline item")
    start, end = verify_item.GetStart(True), verify_item.GetEnd(True)
    require(start == first and end > start and abs((end - start) - float(length)) <= 1,
            "Resolve item position or duration did not match the requested transfer")
    linked = verify_item.GetMediaPoolItem()
    require(linked and linked.GetMediaId() == media.GetMediaId() and path.is_file(),
            "Resolve could not verify the placed item's online media identity")
    verified_media(linked, path, len(roles), request["rate"])
    mapping = verify_mapping(verify_item.GetSourceAudioChannelMapping(), subtype, len(roles))
    response.update({"ok": True, "item": uid, "recordFrame": first, "endFrame": end, "track": index,
                     "timecode": playhead, "timelineRate": str(rate), "dropFrame": drop,
                     "durationRounding": "Resolve natural duration, verified within one record frame",
                     "channels": len(roles), "sampleRate": request["rate"], "roles": roles,
                     "trackSubtype": subtype, "mediaDuration": info.get("Duration", ""),
                     "online": True, "audioMapping": mapping})


def main(source, destination):
    response = {"ok": False}
    try:
        request = json.loads(Path(source).read_text())
        if request["operation"] == "probe":
            _, _, timeline, identity = connect()
            timing(timeline)
            response.update(identity)
            response["ok"] = True
        elif request["operation"] == "place":
            place(request, response)
        else:
            raise RuntimeError("Unknown transfer request")
    except Exception as error:
        response["error"] = str(error)
    pending = destination + ".pending"
    with open(pending, "w") as file:
        json.dump(response, file)
        file.flush()
        os.fsync(file.fileno())
    os.replace(pending, destination)


if __name__ == "__main__":
    main(*sys.argv[1:])
