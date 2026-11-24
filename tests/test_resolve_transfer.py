"""Focused tests for Fairlight active-track timing without an open Resolve project."""

import hashlib
import importlib.util
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path
from unittest.mock import Mock, patch


spec = importlib.util.spec_from_file_location(
    "resolve_transfer", Path(__file__).resolve().parents[1] / "resources/resolve/transfer.py"
)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class FairlightDurationTests(unittest.TestCase):
    def test_active_track_uses_timeline_samples(self):
        for source_rate, timeline_rate in ((44100, 48000), (48000, 48000), (96000, 48000), (48000, 96000)):
            with self.subTest(source_rate=source_rate, timeline_rate=timeline_rate):
                timeline = Mock()
                values = {"timelineSampleRate": str(timeline_rate), "timelineFrameRate": "24",
                          "timelineDropFrameTimecode": "0"}
                timeline.GetSetting.side_effect = values.__getitem__
                timeline.GetCurrentTimecode.return_value = "00:00:01:00"
                timeline.GetStartTimecode.return_value = "00:00:00:00"
                timeline.GetStartFrame.return_value = 0
                timeline.GetTrackCount.return_value = 1
                timeline.GetIsTrackLocked.return_value = False
                item = Mock()
                item.GetUniqueId.return_value = "placement"
                timeline.GetItemListInTrack.side_effect = [[], [], [item]]
                resolve = Mock()
                resolve.GetCurrentPage.return_value = "fairlight"
                project = Mock()
                response = {}
                identity = {"project": "test-project", "timeline": "test-timeline"}
                with tempfile.TemporaryDirectory() as folder:
                    source = Path(folder) / "owned-fixture.wav"
                    source.write_bytes(b"stub; the receiver's media verification is mocked")
                    request = {"path": str(source), "layout": {"id": "mono-v1", "roles": ["C"]},
                               "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                               "rate": source_rate, "samples": source_rate, "target": identity}
                    with patch.object(bridge, "same_target", return_value=(resolve, project, timeline, identity)), \
                         patch.object(bridge, "verify_placement") as verify:
                        bridge.place(request, response)
                        self.assertEqual(verify.call_args.args[4:6], (24, Fraction(24)))
                    project.InsertAudioToCurrentTrackAtPlayhead.assert_called_once_with(
                        str(source.resolve()), 0, timeline_rate
                    )
                self.assertEqual(response["timelineSampleRate"], timeline_rate)
                self.assertEqual(response["insertDurationSamples"], timeline_rate)

    def test_fractional_conversion_covers_last_sample(self):
        timeline = Mock()
        timeline.GetSetting.return_value = "48000"
        self.assertEqual(bridge.fairlight_duration(timeline, 1001, 44100), (48000, 1090))

    def test_missing_timeline_rate_is_refused(self):
        timeline = Mock()
        for value in ("", None, "0", "unknown"):
            with self.subTest(value=value):
                timeline.GetSetting.return_value = value
                with self.assertRaisesRegex(RuntimeError, "timeline sample rate"):
                    bridge.fairlight_duration(timeline, 44100, 44100)


if __name__ == "__main__":
    unittest.main()
