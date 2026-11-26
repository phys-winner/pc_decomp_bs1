"""Run R10's actual Vulkan query code against controllable submission fences and query availability."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def production_query_code(folder):
    source = (ROOT / "android/render10/r10_vulkan.c").read_text(encoding="utf-8")
    constants = [re.search(r"^#define " + name + r" \d+$", source, re.M)[0]
                 for name in ("FRAMES", "QUERY_POOL_SIZE")]
    options = re.search(r"enum panvk_option \{[^}]+\};.*?static int panvk_opt\([^)]*\) \{[^}]+\}", source, re.S)
    constants.append(options[0])
    (folder / "query_limits.h").write_text("\n".join(constants), encoding="utf-8")

    # Preserve calls to actual create/begin/end/poll/harvest logic. The fixture supplies
    # Vulkan, frame submission and its completion fence at the API boundary.
    query = source[source.index("typedef struct vk_query {"):source.index("static int serial_done(")]
    poll = source[source.index("static int vk_query_poll("):source.index("static void vk_flush(")]
    next_frame = source.index("static void next_frame(void)")
    fence = source.index("wait_frame(f);", next_frame)
    harvest = source.index("harvest(frame_index);", fence)
    start = source.index("    if (occlusion_pools[frame_index]) {", harvest)
    end = source.index("\n}", start)
    reset = "static void reset_fenced_pool(void) {\n" + source[start:end] + "\n}\n"
    (folder / "r10_query_code.h").write_text(query + poll + reset, encoding="utf-8")


class VulkanQueriesTest(unittest.TestCase):
    def test_query_generations(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            production_query_code(folder)
            source = Path(__file__).with_suffix(".c")
            exe = folder / ("query.exe" if os.name == "nt" else "query")
            if os.name == "nt":
                from tests.host_environment import CLANG, environment
                env, linker = environment("x64")
                obj = folder / "query.obj"
                subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/I" + str(folder),
                                "/clang:--target=x86_64-pc-windows-msvc", "/c", str(source),
                                "/Fo" + str(obj)], check=True, env=env)
                subprocess.run([str(linker), "/nologo", str(obj), "/OUT:" + str(exe)],
                               check=True, env=env)
            else:
                cc = os.environ.get("CC") or shutil.which("cc")
                self.assertIsNotNone(cc, "A host C compiler is required")
                subprocess.run([cc, "-std=c11", "-O2", "-I", str(folder), str(source),
                                "-o", str(exe)], check=True)
            for mode in ("legacy", "prefix", "host", "host-prefix"):
                with self.subTest(mode=mode):
                    subprocess.run([str(exe), mode], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
