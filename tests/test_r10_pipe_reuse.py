"""Exercise the renderer's retained pipeline ownership and its warm seed extension."""
from pathlib import Path
import struct
import unittest

from tests.test_perf_20261125 import capture, recipe, run_harness, warmup

ROOT = Path(__file__).resolve().parents[1]


class RetainedPipelineTest(unittest.TestCase):
    def test_seed_preserves_fallback(self):
        base = warmup.build([(capture([(0, recipe(1))]), 0)], [], [])
        hot = capture([(1, recipe(7)), (1, recipe(7)), (1, recipe(8))])
        extended = warmup.with_retained(base, [hot])
        self.assertEqual(struct.unpack_from("<4s5I", extended), (b"BSWP", 2, 1, 1, 1088, 2))
        self.assertEqual(extended[24:len(base)], base[24:])
        self.assertEqual(extended[len(base):], recipe(7) + recipe(8))
        with self.assertRaises(ValueError):
            warmup.with_retained(base, [capture([(0, recipe(i)) for i in range(257)])])

    def test_retirement_publication_and_drain(self):
        source = (ROOT / "android/render10/r10_vulkan.c").read_text(encoding="utf-8")
        table = source[source.index("#define RETAINED_MAX "):source.index("static VkBlendFactor vk_blend")]
        program_structs = source[source.index("typedef struct vk_pipeline {"):source.index("static void vk_shader_destroy(")]
        fixture = Path(__file__).with_suffix(".c").read_text(encoding="utf-8")
        run_harness(self, "retained_pipelines", fixture.replace("@PROGRAM@", program_structs).replace("@TABLE@", table))


if __name__ == "__main__":
    unittest.main()
