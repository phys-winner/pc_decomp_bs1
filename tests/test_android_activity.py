"""Check BioShock Activity window, Back and QA-profile contracts."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests import test_touch_controls as touch_checks

ROOT = Path(__file__).resolve().parents[1]
ACTIVITY = ROOT / "android/app/src/org/bioshock/port/GameActivity.java"


class AndroidActivityTest(unittest.TestCase):
    def compile_and_run(self, name, source):
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        with tempfile.TemporaryDirectory(prefix="bioshock-activity-") as temporary:
            path = Path(temporary) / (name + ".java")
            path.write_text(source, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, name], check=True)

    def test_window_and_predictive_back_registration(self):
        source = ACTIVITY.read_text(encoding="utf-8")
        for required in (
            "controller.hide(android.view.WindowInsets.Type.systemBars())",
            "android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE",
            "getWindow().setDecorFitsSystemWindows(false)",
            "root.setBackgroundColor(android.graphics.Color.BLACK)",
            "root.setForceDarkAllowed(false)",
            "setVolumeControlStream(android.media.AudioManager.STREAM_MUSIC)",
            "getOnBackInvokedDispatcher().registerOnBackInvokedCallback",
            "new File(userdataFolder, userdataProfile)",
        ):
            self.assertIn(required, source)
        resume = touch_checks.TouchControlsTest._method(source, "@Override protected void onResume()")
        self.assertIn("immersive();", resume)

    def test_qa_profile_validation(self):
        method = touch_checks.TouchControlsTest._method(ACTIVITY.read_text(encoding="utf-8"), "static String userdataName(")
        program = "public class ProfileCheck {\n" + method + r'''
    public static void main(String[] args) {
        assert userdataName(null).equals("userdata");
        assert userdataName("").equals("userdata");
        assert userdataName("userdata-requirements-qa").equals("userdata-requirements-qa");
        assert userdataName("QA_2026.11-04").equals("QA_2026.11-04");
        for (String invalid : new String[]{".", "..", "../userdata", "qa/profile", "qa\\profile", " ", "player 1", "русский"}) {
            assert userdataName(invalid) == null : invalid;
        }
        assert userdataName("a123456789012345678901234567890123456789012345678901234567890123") != null;
        assert userdataName("a1234567890123456789012345678901234567890123456789012345678901234") == null;
        System.out.println("PASS default userdata remains unchanged; QA profile is isolated and traversal is rejected");
    }
}
'''
        self.compile_and_run("ProfileCheck", program)

    def test_back_before_start_overlay_and_escape(self):
        method = touch_checks.TouchControlsTest._method(ACTIVITY.read_text(encoding="utf-8"), "private void back()")
        program = r'''
class Launch {
    String distribution() { return "bioshock"; }
    static void openLauncher(Object activity, String distribution) { }
}
class ActivityBase {
    int finishes;
    void finish() { ++finishes; }
    public void onBackPressed() { ++finishes; }
}
class Controls {
    int closes;
    boolean overlay;
    boolean handleBack() {
        if (!overlay) return false;
        overlay = false;
        ++closes;
        return true;
    }
}
public class BackCheck extends ActivityBase {
    Launch launch;
    boolean started;
    final Controls controls = new Controls();
    final java.util.ArrayList<Integer> keys = new java.util.ArrayList<>();
    boolean pauseChoices;
    int pauseOffers;
    boolean openPauseChoices() { ++pauseOffers; return pauseChoices; }
    void pauseGame() { nativeKey(27, true); nativeKey(27, false); }
    void nativeKey(int vk, boolean down) { keys.add(down ? vk : -vk); }
''' + method + r'''
    public static void main(String[] args) {
        BackCheck check = new BackCheck();
        check.back();
        assert check.finishes == 1 && check.keys.isEmpty();
        check.started = true;
        check.controls.overlay = true;
        check.back();
        assert check.controls.closes == 1 && check.keys.isEmpty() && check.pauseOffers == 0;
        check.pauseChoices = true;
        check.back();
        assert check.pauseOffers == 1 && check.keys.isEmpty();
        check.pauseChoices = false;
        check.back();
        assert check.keys.equals(java.util.Arrays.asList(27, -27));
        System.out.println("PASS Back finishes pre-start, closes port layers first, offers pause choices, then falls back to one Escape pair");
    }
}
'''
        self.compile_and_run("BackCheck", program)

    def test_fps_cap_default_fractional_rates_and_fallback(self):
        """D8: no saved choice gives 60, fractional panel rates match a saved rate, an unavailable rate never
        raises the cap to the maximum; 0 follows the display. The old code returned the top mode (120)."""
        source = ACTIVITY.read_text(encoding="utf-8")
        method = touch_checks.TouchControlsTest._method(source, "static float resolveFps(")
        constants = "\n".join(line for line in source.splitlines()
                              if line.strip().startswith(("static final float DEFAULT_FPS", "static final float RATE_TOLERANCE")))
        self.assertEqual(constants.count("static final"), 2)
        program = "public class FpsCheck {\n" + constants + "\n" + method + r'''
    static void check(float[] rates, float saved, float expected) {
        float actual = resolveFps(rates, saved);
        assert Float.floatToIntBits(actual) == Float.floatToIntBits(expected) : saved + " -> " + actual + ", expected " + expected;
    }
    public static void main(String[] args) {
        float[] realme = {60.000004f, 90f, 120.00001f};      // RMX3371/RMX3851 report these
        float[] ntsc = {59.94006f, 90f, 119.88012f};
        check(realme, Float.NaN, 60.000004f);                  // no choice yet: 60, not the 120 Hz top mode
        check(ntsc, Float.NaN, 59.94006f);
        check(realme, 0f, 0f);                                 // follow the display
        check(realme, 60f, 60.000004f);                        // a saved 60 survives fractional reporting
        check(ntsc, 60.000004f, 59.94006f);                    // a profile copied from another phone
        check(realme, 120.00001f, 120.00001f);
        check(realme, 90f, 90f);
        check(realme, 144f, 120.00001f);                       // unavailable: the highest mode below it
        check(realme, 75f, 60.000004f);                        // never silently the maximum
        check(new float[]{40f}, Float.NaN, 40f);               // a single-mode panel (BOOX GALILEO3)
        check(new float[]{40f}, 60f, 40f);
        check(new float[]{90f, 120f}, Float.NaN, 90f);         // no mode at or below 60: the lowest
        check(new float[]{90f, 120f}, -5f, 90f);
        check(new float[]{90f, 120f}, Float.POSITIVE_INFINITY, 90f);
        check(new float[0], 60f, 0f);
        check(null, 60f, 0f);
        System.out.println("PASS FPS cap: default 60, fractional-rate tolerance, unavailable rates fall back below, 0 follows the display");
    }
}
'''
        self.compile_and_run("FpsCheck", program)
        # The menu getter and the window request both go through the resolver.
        selected = touch_checks.TouchControlsTest._method(source, "private float selectedFps()")
        self.assertIn("resolveFps(supportedFpsRates(), portPreferences.getFloat(\"fpsLimit\", Float.NaN))", selected)


if __name__ == "__main__":
    unittest.main()
