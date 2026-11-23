"""Keep the original loading screen; animate its bar until engine progress exists."""
import json
from pathlib import Path
import subprocess
import tempfile
import zlib

from patch_hud_layout import digest, tools

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "work/localization/text/Content/FlashMovies/ProgressBar.swf"
SOURCE_SHA256 = "129977bcd06fa0d602a31f2a00a20934db870c39c72a06f702eabce455b95381"
ORIGINAL = """function FillProgressBar(ProgressPercentage)
{
   _root.ProgressBarFiller._width = _root.ProgressBarFillerWidth * ProgressPercentage;
}
"""


def build(output):
    if digest(SOURCE) != SOURCE_SHA256:
        raise ValueError("Loading indicator requires the pinned original ProgressBar.swf")
    script = ROOT / "android/flash/loading_indicator.as"
    java, ffdec = tools()
    identity = {"source_sha256": SOURCE_SHA256, "script_sha256": digest(script),
                "generator_sha256": digest(Path(__file__)), "ffdec_sha256": digest(ffdec)}
    receipt_path = output.with_suffix(".json")
    if output.is_file() and receipt_path.is_file():
        old = json.loads(receipt_path.read_text())
        if all(old.get(k) == v for k, v in identity.items()) and old.get("sha256") == digest(output):
            return old
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="loading-indicator-", dir=output.parent) as temporary:
        stage = Path(temporary)

        def run(*args):
            result = subprocess.run([str(java), "-Xmx512m", "-jar", str(ffdec), "-onerror", "abort", *map(str, args)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

        run("-export", "script", stage / "export", SOURCE)
        path = Path("frame_1/DoAction.as")
        original = (stage / "export/scripts" / path).read_text(encoding="utf-8")
        if original.count(ORIGINAL) != 1 or "onEnterFrame" in original:
            raise ValueError("Original loading callbacks changed")
        patched = stage / "patch" / path
        patched.parent.mkdir(parents=True)
        patched.write_text(original.replace(ORIGINAL, "") + "\n" + script.read_text(encoding="utf-8"), encoding="utf-8")
        run("-importScript", SOURCE, stage / "patched.swf", stage / "patch")
        data = (stage / "patched.swf").read_bytes()
        plain = zlib.decompress(data[8:]) if data[:3] == b"CWS" else data[8:]
        if b"PortLoadingFrame" not in plain or b"NotifyLevelLoadingScreenStopped" not in plain:
            raise ValueError("Loading indicator import failed")
        output.write_bytes(data)
    receipt = {**identity, "sha256": digest(output), "bytes": len(data),
               "progress": "Original engine fraction; moving original bar segment while fraction is zero or unavailable"}
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


if __name__ == "__main__":
    print(json.dumps(build(ROOT / "build/native-loading/native-loading.swf"), indent=2))
