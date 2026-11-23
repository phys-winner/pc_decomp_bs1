"""Prepare the port's pcrecomp overlay without modifying the submodule."""
from functools import cache
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
# Keep the patched package available for lazy imports until Python exits.
_WORK = TemporaryDirectory(prefix="bioshock-pcrecomp-")


@cache
def patched_tools():
    checkout = ROOT / "reference/pcrecomp"
    if not (checkout / "tools/lift/lift32.py").is_file():
        raise FileNotFoundError("Initialize the pcrecomp submodule before lifting.")
    work = Path(_WORK.name)
    tools = shutil.copytree(checkout / "tools", work / "tools",
                            ignore=shutil.ignore_patterns("__pycache__"))
    subprocess.run(["git", "apply", "--ignore-space-change", str(ROOT / "tools/pcrecomp.patch")],
                   cwd=work, check=True)
    return tools
