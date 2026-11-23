"""The compact movies keep the complete original video and audio bitstreams."""

import struct
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compact_bink_locale import audio_packet, movie, remux, sixth_tracks, track_ids


def fixture(path, ids, tag, picture=b"video000"):
    frames = [b"".join(struct.pack("<I", 4) + bytes((track, i, tag, 0)) for track in ids) + picture for i in range(2)]
    start = 44 + len(ids) * 12 + 12
    end = start + sum(map(len, frames))
    path.write_bytes(struct.pack("<4s10I", b"BIKi", end - 8, 2, len(frames[0]), 2, 64, 64, 30, 1, 0, len(ids)) +
                     struct.pack(f"<{len(ids) * 3}I", *([32] * len(ids)), *([0x7000BB80] * len(ids)), *ids) +
                     struct.pack("<3I", start | 1, start + len(frames[0]), end) + b"".join(frames))


class BinkVoiceRemuxTest(unittest.TestCase):
    def test_track_order_packets_and_alias(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "Intro.bik"
            localized = root / "Intro_rus.bik"
            output = root / "compact"
            fixture(base, (0, 3, 1, 2, 4), 1)
            fixture(localized, (3, 0), 2)
            (root / "IntroAlternate_rus.bik").write_bytes(localized.read_bytes())
            fixture(root / "IntroAlternate.bik", (0, 3, 1, 2, 4), 1, picture=b"video111")
            merged, aliases = sixth_tracks(root, output)
            self.assertEqual(merged, {"Intro_rus.bik": "Intro.bik"})
            self.assertEqual(aliases, {"IntroAlternate_rus.bik": "Intro.bik"})
            self.assertEqual((output / "IntroAlternate_rus.bkr").read_bytes(), b"BKR1Intro.bik")
            identity, original_frames, original = movie(base)
            compact_identity, compact_frames, compact = movie(output / base.name)
            _, russian_frames, russian = movie(localized)
            self.assertEqual(identity, compact_identity)
            self.assertEqual(track_ids(compact), (*track_ids(original), 5))
            self.assertEqual(struct.unpack_from("<I", compact, 44 + 6 * 12)[0] & 1, 1)
            for (start, video, _), (new_start, _, _), (ru_start, _, _) in zip(original_frames, compact_frames, russian_frames):
                self.assertEqual(compact[new_start:new_start + video - start], original[start:video])
                self.assertEqual(audio_packet(compact, new_start, 5), audio_packet(russian, ru_start, 1))
            self.assertFalse(remux(root / "IntroAlternate.bik", localized, output / "wrong.bik"))
            self.assertFalse((output / "wrong.bik").exists())


if __name__ == "__main__":
    unittest.main()
