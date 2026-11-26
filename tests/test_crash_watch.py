"""Crash reports after Frostpunk's watcher: a separate process shows, saves and copies the report of a crashed game.

GameWatchActivity (:watch) starts GameActivity for a result and, when the game process dies, reads Android's
exit record, the tombstone and the session's bioshock.log (core.c's fault report). These checks keep the
pieces wired: the launcher hook, the session log pointer, the planned stops that must stay quiet, the QA
synthetic crash and the fault report's thread line.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "android/app/src/org/bioshock/port"


def read(path):
    return Path(path).read_text(encoding="utf-8", errors="replace")


class CrashWatchTest(unittest.TestCase):
    def test_manifest_runs_the_watcher_in_its_own_process(self):
        manifest = read(ROOT / "android/app/AndroidManifest.xml")
        entry = re.search(r'<activity android:name="\.GameWatchActivity"[^>]*/>', manifest, re.S)
        self.assertIsNotNone(entry)
        self.assertIn('android:process=":watch"', entry.group(0))
        game = re.search(r'<activity android:name="\.GameActivity"[^>]*/>', manifest, re.S).group(0)
        self.assertNotIn("android:process", game)

    def test_launcher_hook_routes_through_the_watcher_unless_monitoring_is_off(self):
        hooks = read(APP / "LauncherHooks.java")
        prepare = hooks[hooks.index("public void prepare("):]
        self.assertIn('"0".equals(Launch.option(context, "crash_monitoring"))', prepare)
        self.assertIn('"org.bioshock.port.GameWatchActivity"', prepare)
        self.assertIn('intent.putExtra("crash.language"', prepare)
        report = read(APP / "CrashReport.java")
        self.assertIn('EXTRA_LANGUAGE = "crash.language"', report)

    def test_watcher_starts_only_the_game(self):
        watch = read(APP / "GameWatchActivity.java")
        self.assertIn('GAME = "org.bioshock.port.GameActivity"', watch)
        self.assertIn("new Intent().setClassName(this, GAME)", watch)
        self.assertNotIn('getStringExtra("crash.game")', watch)
        self.assertIn("startActivityForResult(game, 1)", watch)
        self.assertIn('"Скопировать в буфер обмена"', watch)
        self.assertIn("CrashReport.copy(this, copied)", watch)

    def test_session_names_its_log_and_planned_stops_stay_quiet(self):
        game = read(APP / "GameActivity.java")
        begin = game.index("SessionLog.begin(this, userdata, crashMonitoring());")
        self.assertLess(begin, game.index("CrashReport.rememberSessionLog(this, new File(userdata, SessionLog.LOG))"))
        failure = game.index('CrashReport.plannedStop(GameActivity.this, "renderer failure")')
        self.assertLess(failure, game.index("launch.rendererFailed(reason"))
        self.assertIn('if (result == 0) CrashReport.plannedStop(GameActivity.this, "exit 0")', game)
        self.assertIn("CrashReport.EXTRA_EXIT_CODE, result", game)
        watch = read(APP / "GameWatchActivity.java")
        self.assertIn("CrashReport.plannedStop(this, started) != null", watch)

    def test_report_reads_the_native_fault_report(self):
        report = read(APP / "CrashReport.java")
        for prefix in ('"[void] host signal"', '"[void] FATAL: "', '"[void] fault signal="', '"[java] uncaught exception"'):
            self.assertIn(prefix, report)
        core = read(ROOT / "android/host/core.c")
        self.assertIn('char line[] = "[void] host signal 00\\n"', core)
        self.assertIn('"[void] fault signal="', core)
        self.assertIn('"[void] fault thread="', core)
        session = read(APP / "SessionLog.java")
        self.assertIn('"[java] uncaught exception on thread', session)

    def test_qa_synthetic_crash_is_a_whitelisted_perf_flag(self):
        main = read(ROOT / "android/host/android_main.c")
        block = main[main.index("allowed[] = {"):]
        self.assertIn('"BIOSHOCK_QA_CRASH"', block[:block.index("};")])
        presented = main[main.index("void void_android_frame_presented(void)"):]
        self.assertIn("qa_crash_poll();", presented[:presented.index("\n}\n")])
        self.assertIn('"%s/qa-crash-now"', main)


if __name__ == "__main__":
    unittest.main()
