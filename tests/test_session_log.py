"""Run the production session-log rotation and check native integration."""
import os
from pathlib import Path
import subprocess
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/session-log"

SOURCES = {
    "android/app/ApplicationExitInfo.java": r'''
        package android.app;
        import java.io.InputStream;
        public class ApplicationExitInfo {
            public static final int REASON_EXIT_SELF=1, REASON_SIGNALED=2, REASON_LOW_MEMORY=3,
                REASON_CRASH=4, REASON_CRASH_NATIVE=5, REASON_ANR=6, REASON_INITIALIZATION_FAILURE=7,
                REASON_PERMISSION_CHANGE=8, REASON_EXCESSIVE_RESOURCE_USAGE=9, REASON_USER_REQUESTED=10,
                REASON_USER_STOPPED=11, REASON_DEPENDENCY_DIED=12, REASON_OTHER=13;
            private final String process, description;
            private final int reason, status;
            private final long timestamp;
            public ApplicationExitInfo(String process, int reason, int status, long timestamp, String description) {
                this.process=process; this.reason=reason; this.status=status; this.timestamp=timestamp; this.description=description;
            }
            public String getProcessName() { return process; }
            public int getReason() { return reason; }
            public int getStatus() { return status; }
            public long getTimestamp() { return timestamp; }
            public String getDescription() { return description; }
            public long getPss() { return 1024; }
            public long getRss() { return 2048; }
            public InputStream getTraceInputStream() { return null; }
        }
    ''',
    "android/app/ActivityManager.java": r'''
        package android.app;
        import java.util.ArrayList;
        import java.util.List;
        public class ActivityManager {
            public static final List<ApplicationExitInfo> exits = new ArrayList<>();
            public List<ApplicationExitInfo> getHistoricalProcessExitReasons(String packageName, int pid, int max) { return exits; }
        }
    ''',
    "android/app/Application.java": r'''
        package android.app;
        public class Application {
            public static String getProcessName() { return "org.bioshock.port"; }
        }
    ''',
    "android/content/SharedPreferences.java": r'''
        package android.content;
        public interface SharedPreferences {
            long getLong(String key, long fallback);
            Editor edit();
            interface Editor {
                Editor putLong(String key, long value);
                void apply();
            }
        }
    ''',
    "android/content/Context.java": r'''
        package android.content;
        import android.app.ActivityManager;
        import android.content.pm.PackageManager;
        import java.util.HashMap;
        import java.util.Map;
        public class Context {
            public static final String ACTIVITY_SERVICE="activity";
            public static final int MODE_PRIVATE=0;
            private static class Preferences implements SharedPreferences, SharedPreferences.Editor {
                private final Map<String,Long> values = new HashMap<>();
                public long getLong(String key, long fallback) { return values.getOrDefault(key, fallback); }
                public SharedPreferences.Editor edit() { return this; }
                public SharedPreferences.Editor putLong(String key, long value) { values.put(key, value); return this; }
                public void apply() { }
            }
            private final SharedPreferences preferences = new Preferences();
            private final ActivityManager manager = new ActivityManager();
            private final PackageManager packages = new PackageManager();
            public Object getSystemService(String name) { return manager; }
            public SharedPreferences getSharedPreferences(String name, int mode) { return preferences; }
            public PackageManager getPackageManager() { return packages; }
            public String getPackageName() { return "org.bioshock.port"; }
        }
    ''',
    "android/content/pm/PackageInfo.java": r'''
        package android.content.pm;
        public class PackageInfo {
            public String versionName = "0.1.0-arm64";
            public int versionCode = 1;
            public long getLongVersionCode() { return versionCode; }
        }
    ''',
    "android/content/pm/PackageManager.java": r'''
        package android.content.pm;
        public class PackageManager {
            public PackageInfo getPackageInfo(String packageName, int flags) { return new PackageInfo(); }
        }
    ''',
    "android/os/Build.java": r'''
        package android.os;
        public class Build {
            public static final String MANUFACTURER="QA", MODEL="RMX3851", DEVICE="qa", HARDWARE="qcom";
            public static final String SOC_MANUFACTURER="Qualcomm", SOC_MODEL="SM8635";
            public static final String[] SUPPORTED_ABIS={"arm64-v8a"};
            public static class VERSION { public static final int SDK_INT=30; public static final String RELEASE="16"; }
        }
    ''',
    "android/util/Log.java": r'''
        package android.util;
        public class Log {
            public static int w(String tag, String text) { return 0; }
            public static String getStackTraceString(Throwable error) { return error.toString(); }
        }
    ''',
    "org/bioshock/port/SessionLogCheck.java": r'''
        package org.bioshock.port;
        import android.app.ActivityManager;
        import android.app.ApplicationExitInfo;
        import android.content.Context;
        import java.io.File;
        import java.nio.charset.StandardCharsets;
        import java.nio.file.Files;
        public class SessionLogCheck {
            private static void write(File file, String text) throws Exception {
                Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
            }
            private static String read(File file) throws Exception {
                return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            }
            public static void main(String[] args) throws Exception {
                File dir = new File(args[0]);
                if (!dir.isDirectory() && !dir.mkdirs()) throw new AssertionError("mkdir");
                File log = new File(dir, "bioshock.log"), one = new File(dir, "bioshock.1.log"), two = new File(dir, "bioshock.2.log");
                write(log, "current-session"); write(one, "previous-session"); write(two, "older-session");
                ActivityManager.exits.clear();
                ActivityManager.exits.add(new ApplicationExitInfo("org.bioshock.port",
                    ApplicationExitInfo.REASON_CRASH_NATIVE, 11, 123456789L, "fixture"));
                Thread.setDefaultUncaughtExceptionHandler((thread, error) -> { });
                SessionLog.begin(new Context(), dir);
                assert read(two).contains("previous-session") : read(two);
                assert !read(two).contains("older-session");
                assert read(one).contains("current-session") : read(one);
                assert read(one).contains("native crash") && read(one).contains("signal 11") : read(one);
                assert read(log).contains("[session] BioShock 0.1.0-arm64 (1)") : read(log);
                assert !read(log).contains("current-session");
                Thread.getDefaultUncaughtExceptionHandler().uncaughtException(Thread.currentThread(), new RuntimeException("fixture-java"));
                assert read(log).contains("[java] uncaught exception") && read(log).contains("fixture-java");
                String before = read(log);
                SessionLog.begin(new Context(), dir);
                assert read(log).equals(before) : "begin must run once per process";
                System.out.println("PASS production SessionLog: two previous generations, ApplicationExitInfo, Java hook, single begin");
            }
        }
    ''',
}


class SessionLogTest(unittest.TestCase):
    def test_java_rotation_and_native_contract(self):
        src = OUT / "src"
        classes = OUT / "classes"
        work = OUT / "work"
        classes.mkdir(parents=True, exist_ok=True)
        work.mkdir(parents=True, exist_ok=True)
        java_sources = [ROOT / "android/app/src/org/bioshock/port/SessionLog.java"]
        for name, text in SOURCES.items():
            path = src / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")
            java_sources.append(path)
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        commands = ([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(classes), *map(str, java_sources)],
                    [str(java / "java.exe"), "-ea", "-cp", str(classes), "org.bioshock.port.SessionLogCheck", str(work)])
        for command in commands:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        core = (ROOT / "android/host/core.c").read_text(encoding="utf-8")
        opening = core[core.index("void void_log_open("):core.index("static void log_va(")]
        self.assertIn('#ifdef UT3_ANDROID_APP', opening)
        self.assertIn('fopen(path, "a")', opening)
        self.assertIn('fopen(path, "w")', opening)
        self.assertIn("sigaction(signo, &previous_actions[signo], NULL);", core)
        self.assertIn("sigaction(signals[i], &sa, &previous_actions[signals[i]]);", core)
        activity = (ROOT / "android/app/src/org/bioshock/port/GameActivity.java").read_text(encoding="utf-8")
        begin = activity.index("SessionLog.begin(this, userdata);")
        self.assertLess(activity.index("launch.starting();"), begin)
        self.assertLess(begin, activity.index("nativeConfigure(", begin))
        self.assertLess(begin, activity.index("nativeStart(", begin))


if __name__ == "__main__":
    unittest.main()
