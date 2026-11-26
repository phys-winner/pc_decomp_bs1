"""Release update metadata stays consistent with the APK and its cached facts."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools import update_manifest as manifest


class UpdateManifestTest(unittest.TestCase):
    def test_generate_then_reuse_cached_facts(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            apk = folder / "game.apk"
            apk.write_bytes(b"release APK fixture")
            digest = hashlib.sha256(apk.read_bytes()).hexdigest()
            asset = {"id": 42, "name": "game.apk", "size": apk.stat().st_size,
                     "digest": "sha256:" + digest,
                     "browser_download_url": "https://github.com/example/game/releases/download/v2/game.apk"}
            release = {"tag_name": "v2", "name": "Version 2", "published_at": "2026-11-26T00:00:00Z",
                       "html_url": "https://github.com/example/game/releases/tag/v2", "body": "Release notes",
                       "assets": [asset]}
            facts = {"package": "org.example.game", "versionCode": 4, "versionName": "0.2.1-dev",
                     "minSdk": 26, "abis": ["arm64-v8a"], "signer": "a" * 64}
            update, state = folder / "update.json", folder / "versions.json"
            args = ["update_manifest.py", "--repo", "example/game", "--out", str(update),
                    "--state", str(state), "--apk", "v2=" + str(apk)]
            releases = [dict(release, draft=True), dict(release, prerelease=True), release]
            with patch.object(manifest, "releases", return_value=releases), \
                 patch.object(manifest, "build_tools", return_value=folder), \
                 patch.object(manifest, "apk_facts", return_value=facts) as read_apk, \
                 patch.object(manifest, "download", side_effect=AssertionError("unexpected download")), \
                 patch.object(sys, "argv", args), contextlib.redirect_stderr(io.StringIO()):
                manifest.main()
                original = update.read_bytes(), state.read_bytes()
                result = json.loads(original[0])
                self.assertEqual(result["package"], facts["package"])
                self.assertEqual(result["latest"]["versionCode"], 4)
                self.assertEqual(result["latest"]["apk"]["sha256"], digest)
                self.assertEqual(result["latest"]["apk"]["signer"], facts["signer"])
                self.assertEqual(len(result["history"]), 1)
                manifest.main()
                read_apk.assert_called_once()
                self.assertEqual(original, (update.read_bytes(), state.read_bytes()))
                self.assertIn("42:" + digest, json.loads(state.read_bytes()))

                # A changed release asset must not reuse stale facts or overwrite valid output.
                asset["digest"] = "sha256:" + "0" * 64
                with self.assertRaisesRegex(SystemExit, "SHA-256"):
                    manifest.main()
                self.assertEqual(original, (update.read_bytes(), state.read_bytes()))


if __name__ == "__main__":
    unittest.main()
