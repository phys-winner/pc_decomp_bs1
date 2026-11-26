"""CrashReport's text: the reason and the clipboard excerpt it takes from bioshock.log, the session facts and the
library's build ID, run on a desktop JVM against small Android stubs (the real file, not a copy)."""
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    "android/content/ClipData.java": r'''
package android.content;
public class ClipData {
    public static class Item { private final CharSequence text; Item(CharSequence text){this.text=text;} public CharSequence getText(){return text;} }
    private final Item item;
    private ClipData(CharSequence text){item=new Item(text);}
    public static ClipData newPlainText(CharSequence label, CharSequence text){return new ClipData(text);}
    public int getItemCount(){return 1;}
    public Item getItemAt(int index){return item;}
}
''',
    "android/content/ClipboardManager.java": r'''
package android.content;
public class ClipboardManager {
    private ClipData clip;
    public void setPrimaryClip(ClipData clip){this.clip=clip;}
    public ClipData getPrimaryClip(){return clip;}
}
''',
    "android/content/Context.java": r'''
package android.content;
import java.io.File;
public class Context {
    private final File files; private final ClipboardManager clipboard=new ClipboardManager();
    public Context(File files){this.files=files;}
    public File getFilesDir(){return files;}
    @SuppressWarnings("unchecked") public <T> T getSystemService(Class<T> type){return (T)clipboard;}
}
''',
    "android/os/Environment.java": r'''
package android.os;
public class Environment { public static java.io.File getExternalStorageDirectory(){return new java.io.File(System.getProperty("crash.external"));} }
''',
    "android/util/Log.java": r'''
package android.util;
public class Log {
    public static int i(String tag,String text){return 0;}
    public static int w(String tag,String text){return 0;}
    public static int w(String tag,String text,Throwable error){return 0;}
}
''',
    "org/bioshock/port/CrashReportCheck.java": r'''
package org.bioshock.port;
import android.content.Context;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
public class CrashReportCheck {
    static void check(boolean condition, String what){ if(!condition) throw new AssertionError(what); }
    public static void main(String[] args) throws Exception {
        File root=new File(args[0]);
        String signal="[session] BioShock 0.2-dev (3) started on Fixture\n[perf-flags] BIOSHOCK_QA_CRASH=segv\n"
            +"[r10] VULKAN_DRIVER system\n[r10] Vulkan: Adreno (TM) 610, Vulkan 1.1, driver 1\n[noise] a\n"
            +"[bink-thread] calling lifted entry id=436\n[menu] x\n[menu] x\n[menu] x\n[void] host signal 11\n"
            +"[void] fault signal=SIGSEGV code=2 addr=0x2000000010 tid=7\n[void] fault thread=Bink machine=held guest func=0x10c58ad0 esp=0x3cfae0\n"
            +"[void] fault modules pc=libbioshock.so+0x5654b8 lr=libbioshock.so+0x565484\n";
        check("Crash in native game code: signal=SIGSEGV code=2 addr=0x2000000010 tid=7".equals(CrashReport.logReason(signal,false)),
            "the fault line names the signal: "+CrashReport.logReason(signal,false));
        check(CrashReport.logReason(signal,true).startsWith("Сбой в нативном коде игры: signal=SIGSEGV"), "Russian reason");
        check("Crash in native game code: signal 11".equals(CrashReport.logReason("[a] b\n[void] host signal 11\n",false)),
            "an old log with the signal line only");
        check("Game error: guest trap at 10000000".equals(CrashReport.logReason("[void] FATAL: guest trap at 10000000\n[void] dump\n",false)),
            "void_fatal");
        check(CrashReport.logReason("[java] uncaught exception on thread \"main\": java.lang.IllegalStateException\n\tat x\n",false)
            .startsWith("Unhandled Java exception: uncaught exception on thread \"main\""), "SessionLog's Java handler");
        check(CrashReport.logReason("[r10] frame 1\n[bioshock] process exit 0\n",false)==null, "a log without a fault has no reason");

        String excerpt=CrashReport.logExcerpt(signal,30,48);
        check(excerpt.contains("--- bioshock.log: before the fault ---\n"), "context header");
        check(excerpt.contains("[menu] x (×3)\n") && !excerpt.contains("[menu] x\n[menu] x"), "repeats collapse: "+excerpt);
        check(excerpt.indexOf("[bink-thread] calling lifted entry")<excerpt.indexOf("--- bioshock.log: fault ---"), "order");
        check(excerpt.contains("--- bioshock.log: fault ---\n[void] host signal 11\n[void] fault signal=SIGSEGV"), "fault block");
        check(excerpt.contains("guest func=0x10c58ad0"), "thread line");
        String limited=CrashReport.logExcerpt(signal,2,2);
        check(limited.contains("[menu] x (×3)\n--- bioshock.log: fault ---") && limited.contains("… 2 more lines in the saved report"), "limits: "+limited);
        check(CrashReport.logExcerpt("[a] 1\n[b] 2\n",30,48).startsWith("--- bioshock.log: last lines ---\n[a] 1\n[b] 2\n"), "no fault");

        String facts=CrashReport.sessionFacts(signal);
        check(facts.equals("[session] BioShock 0.2-dev (3) started on Fixture\n[perf-flags] BIOSHOCK_QA_CRASH=segv\n"
            +"[r10] VULKAN_DRIVER system\n[r10] Vulkan: Adreno (TM) 610, Vulkan 1.1, driver 1\n"), "facts: "+facts);

        check("0123456789abcdef0011223344556677deadbeef".equals(CrashReport.buildId(new File(args[1]))), "ELF build id: "+CrashReport.buildId(new File(args[1])));
        check(CrashReport.buildId(new File(root,"missing.so")).isEmpty(), "missing library");

        File log=new File(root,"profile/bioshock.log"); log.getParentFile().mkdirs(); Files.write(log.toPath(), signal.getBytes(StandardCharsets.UTF_8));
        Context context=new Context(new File(root,"files"));
        long before=System.currentTimeMillis()-2000;
        CrashReport.rememberSessionLog(context, log);
        check(log.equals(CrashReport.sessionLog(context, before)), "session log pointer");
        check(CrashReport.sessionLog(context, System.currentTimeMillis()+60000)==null, "an older pointer is not this crash's");
        check(CrashReport.plannedStop(context, before)==null, "no planned stop yet");
        CrashReport.plannedStop(context, "exit 0");
        check("exit 0".equals(CrashReport.plannedStop(context, before)), "planned stop");
        check(CrashReport.head(log, 40).isEmpty(), "head keeps whole lines only");
        check(CrashReport.head(log, 60).equals("[session] BioShock 0.2-dev (3) started on Fixture\n"), "head: "+CrashReport.head(log, 60));
        check(CrashReport.head(log, 1<<20).equals(signal), "head of a small file");

        check(CrashReport.copy(context, "report text"), "copy reads the clip back");
        check("report text".equals(new String(Files.readAllBytes(new File(root,"files/crash-last-clipboard.txt").toPath()), StandardCharsets.UTF_8)), "QA copy of the clipboard");
        check(new File(System.getProperty("crash.external"),"BioShock/crashes").equals(CrashReport.reportDirectory(new File(root,"files"))), "reports go to /sdcard/BioShock/crashes");
        check("Game exited with code 70".equals(CrashReport.exitReason(1,70,"exit",false)), "exit code");
        check("Игра завершилась с кодом 70".equals(CrashReport.exitReason(1,70,null,true)), "exit code ru");
        System.out.println("PASS crash report text");
    }
}
''',
}


def elf_with_build_id(path, build_id):
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
    header = (b"\x7fELF\x02\x01\x01" + b"\0" * 9
              + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0))
    program = struct.pack("<IIQQQQQQ", 4, 4, 120, 0, 0, len(note), len(note), 4)
    Path(path).write_bytes(header + program + note)


class CrashReportTextTest(unittest.TestCase):
    def test_reason_excerpt_facts_and_build_id(self):
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        with tempfile.TemporaryDirectory(prefix="bioshock-crash-report-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            sources = []
            for name, text in STUBS.items():
                path = folder / "src" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
                sources.append(str(path))
            sources.append(str(ROOT / "android/app/src/org/bioshock/port/CrashReport.java"))
            classes = folder / "classes"
            library = folder / "libfixture.so"
            elf_with_build_id(library, bytes.fromhex("0123456789abcdef0011223344556677deadbeef"))
            for command in ([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-nowarn", "-d", str(classes), *sources],
                            [str(java / "java.exe"), "-ea", "-Dfile.encoding=UTF-8", "-Dcrash.external=" + str(folder / "external"),
                             "-cp", str(classes), "org.bioshock.port.CrashReportCheck", str(folder / "run"), str(library)]):
                result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS crash report text", result.stdout)


if __name__ == "__main__":
    unittest.main()
