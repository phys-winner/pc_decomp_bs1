"""Restore the original Russian BioShock sound banks from VCDIFF and the matching base game."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def apply(game, package, xdelta):
    game = game.resolve(strict=True)
    directory = (game / "Content/Sounds_Windows").resolve(strict=True)
    if not directory.is_relative_to(game):
        raise ValueError("Sounds_Windows must be inside the selected game directory")
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix=".bioshock-voice-", dir=directory) as temp:
        pending = []
        for bank in manifest:
            name = bank["name"]
            if not re.fullmatch(r"streams_(?:[0-7]|99|aivo)_audio", name):
                raise ValueError(f"Unexpected sound bank: {name}")
            base = directory / (name + ".fsb")
            target = directory / (name + ".rus_fsb")
            if target.exists():
                if sha(target) != bank["sha256"]:
                    raise ValueError(f"Different Russian bank already exists: {target}")
                continue
            if sha(base) != bank["base_sha256"]:
                raise ValueError(f"Base sound bank does not match: {base}")
            check = Path(temp) / target.name
            delta = package / (name + ".vcdiff")
            subprocess.run([xdelta, "-d", "-D", "-R", "-s", base, delta, check], check=True)
            if check.stat().st_size != bank["bytes"] or sha(check) != bank["sha256"]:
                raise ValueError(f"Decoded bank does not match: {target}")
            pending.append((check, target))
        for checked, target in pending:
            checked.rename(target)
        print(f"Verified {len(manifest)} banks, restored {len(pending)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path, help="Directory containing BioShock's Content folder")
    parser.add_argument("--xdelta", default="xdelta3", help="xdelta3 command or full executable path")
    args = parser.parse_args()
    apply(args.game, Path(__file__).resolve().parent, args.xdelta)
