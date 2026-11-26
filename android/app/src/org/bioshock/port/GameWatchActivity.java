package org.bioshock.port;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import org.bioshock.port.launcher.Launch;

/** Separate process (:watch) that outlives a crash of the game process and shows why the game stopped: the
 *  report goes to /sdcard/BioShock/crashes and a button copies it for the player to send (after Frostpunk's
 *  android/app/GameWatchActivity.java). LauncherHooks.prepare starts the game through it while the launcher's
 *  "Crash monitoring" option is on. */
public final class GameWatchActivity extends Activity {
    /** Only the game: the extras of this exported activity never choose what it starts. */
    private static final String GAME = "org.bioshock.port.GameActivity";
    private long started;
    private boolean returned, shown;
    private String renderer;
    private int exitCode;

    private File session() { return new File(getFilesDir(), "crash-watch-session.txt"); }
    private boolean russian() {
        String language = getIntent().getStringExtra(CrashReport.EXTRA_LANGUAGE);
        if (language == null) language = java.util.Locale.getDefault().getLanguage();
        return language.startsWith("ru");
    }
    private String say(String english, String russian) { return russian() ? russian : english; }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) {
            started = state.getLong("started");
            returned = state.getBoolean("returned");
            exitCode = state.getInt("exitCode");
            renderer = state.getString("renderer");
            return;
        }
        try {
            /* This process was gone while the game ran (Android stopped it in the background): the game's end is
               reported on the next start, from the session the previous watcher recorded. */
            String[] previous = CrashReport.tail(session(), 128).trim().split("\n", 2);
            if (!previous[0].isEmpty()) {
                started = Long.parseLong(previous[0]);
                renderer = previous.length > 1 && !previous[1].isEmpty() ? previous[1] : null;
                if (report()) return;
            }
        } catch (IOException | NumberFormatException error) {
            android.util.Log.e("BioShock", "crash monitor storage", error);
        }
        started = System.currentTimeMillis();
        renderer = getIntent().getStringExtra(Launch.EXTRA_RENDERER);
        try { CrashReport.write(session(), started + "\n" + (renderer == null ? "" : renderer)); }
        catch (IOException error) { android.util.Log.e("BioShock", "crash monitor storage", error); }
        Intent game = new Intent().setClassName(this, GAME);
        if (getIntent().getExtras() != null) game.putExtras(getIntent().getExtras());
        game.putExtra(CrashReport.EXTRA_WATCHED, true);
        startActivityForResult(game, 1);
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putLong("started", started);
        state.putBoolean("returned", returned);
        state.putInt("exitCode", exitCode);
        state.putString("renderer", renderer);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != 1) return;
        returned = true;
        exitCode = data != null ? data.getIntExtra(CrashReport.EXTRA_EXIT_CODE, 0) : 0;
        if (result == RESULT_OK && exitCode == 0) { close(); return; }
        // Android records the exit of the game process a moment after it dies.
        new Handler().postDelayed(() -> {
            if (!isFinishing() && !report()) close();
        }, 600);
    }

    @Override protected void onResume() {
        super.onResume();
        if (returned && !shown) new Handler().postDelayed(() -> {
            if (!isFinishing() && !shown && !report()) close();
        }, 650);
    }

    /** Android records exit reasons from API 30. Below it bioshock.log (core.c's fault report, SessionLog's Java
     *  handler) is the only source. */
    private ApplicationExitInfo exitInfo() {
        if (Build.VERSION.SDK_INT < 30) return null;
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (manager == null) return null;
        for (ApplicationExitInfo info : manager.getHistoricalProcessExitReasons(getPackageName(), 0, 16))
            if (getPackageName().equals(info.getProcessName()) && info.getTimestamp() >= started) return info;
        return null;
    }

    private boolean quiet(ApplicationExitInfo info, String logReason) {
        if (logReason != null || exitCode != 0) return false;
        if (CrashReport.plannedStop(this, started) != null) return true;
        if (info == null) return false;
        int reason = info.getReason();
        return reason == ApplicationExitInfo.REASON_USER_REQUESTED
            || reason == ApplicationExitInfo.REASON_USER_STOPPED
            || reason == ApplicationExitInfo.REASON_PACKAGE_UPDATED
            || reason == ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE
            || (reason == ApplicationExitInfo.REASON_EXIT_SELF && info.getStatus() == 0);
    }

    private boolean report() {
        if (shown) return true;
        try {
            boolean ru = russian();
            File log = CrashReport.sessionLog(this, started);
            String logText = "";
            if (log != null) {
                try { logText = CrashReport.tail(log, 65536); }
                catch (IOException error) { logText = "(" + log + ": " + error + ")"; }
            }
            String logReason = CrashReport.logReason(logText, ru);
            ApplicationExitInfo info = exitInfo();
            if (quiet(info, logReason)) return false;
            if (logReason == null && info == null && exitCode == 0) return false;
            String reason = logReason != null ? logReason
                : exitCode != 0 ? (ru ? "Игра завершилась с кодом " : "Game exited with code ") + exitCode
                : info != null ? CrashReport.exitReason(info.getReason(), info.getStatus(), info.getDescription(), ru)
                : say("Game process stopped unexpectedly", "Процесс игры неожиданно завершился");
            String exitDetails = info == null ? "" : "\n" + say("Android exit reason: ", "Причина выхода Android: ")
                + info.getReason() + ", status: " + info.getStatus()
                + (info.getDescription() != null && !info.getDescription().trim().isEmpty()
                    ? " (" + CrashReport.shorten(info.getDescription().trim(), 80) + ")" : "");
            StringBuilder text = new StringBuilder("BioShock crash report\nTime: ")
                .append(java.time.OffsetDateTime.now().withNano(0));
            try {
                android.content.pm.PackageInfo app = getPackageManager().getPackageInfo(getPackageName(), 0);
                text.append("\nVersion: ").append(app.versionName).append(" (")
                    .append(Build.VERSION.SDK_INT >= 28 ? app.getLongVersionCode() : app.versionCode).append(')');
            } catch (Exception ignored) { }
            String id = CrashReport.buildId(new File(getApplicationInfo().nativeLibraryDir, "libbioshock.so"));
            if (!id.isEmpty()) text.append("\nlibbioshock.so BuildId: ").append(id);
            String soc = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : Build.HARDWARE;
            text.append("\nDevice: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" (").append(Build.DEVICE).append("), ").append(soc)
                .append("\nAndroid: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")")
                .append("\nBuild fingerprint: ").append(Build.FINGERPRINT)
                .append("\nRenderer: ").append(String.valueOf(renderer));
            if (log != null) text.append("\nProfile: ").append(log.getParentFile() != null ? log.getParentFile().getName() : "?");
            String facts = "";
            if (log != null) try { facts = CrashReport.sessionFacts(CrashReport.head(log, 65536)).trim(); }
            catch (IOException error) { facts = log + ": " + error; }
            if (!facts.isEmpty()) text.append("\n\n--- Session ---\n").append(facts);
            text.append("\n\n");
            String visibleSummary = CrashReport.shorten(reason, 600) + exitDetails;
            StringBuilder clipboard = new StringBuilder(text.toString()).append(reason).append(exitDetails);
            text.append(reason).append(exitDetails);
            if (info != null && (info.getPss() > 0 || info.getRss() > 0)) {
                String memory = "\nLast PSS (KiB): " + info.getPss() + "\nLast RSS (KiB): " + info.getRss();
                text.append(memory);
                clipboard.append(memory);
            }
            String stackTrace = "";
            String reportName = "bioshock-crash-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.ROOT)
                .format(new java.util.Date()) + ".txt";
            File privateReport = new File(new File(getFilesDir(), "crashes"), reportName);
            File saved = new File(CrashReport.reportDirectory(getFilesDir()), reportName);
            if (info != null) try (InputStream trace = info.getTraceInputStream()) {
                if (trace != null) {
                    boolean nativeCrash = info.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                        && Build.VERSION.SDK_INT >= 31;
                    if (nativeCrash) {
                        File tombstone = CrashReport.saveTombstone(saved, privateReport, trace);
                        String location = "\nAndroid tombstone: " + tombstone.getAbsolutePath();
                        text.append(location);
                        clipboard.append(location);
                        stackTrace = CrashReport.nativeStackTrace(tombstone);
                        String abort;
                        try (InputStream prefix = Files.newInputStream(tombstone.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                            abort = CrashReport.nativeAbortMessage(readTracePrefix(prefix, 65536));
                        }
                        if (!abort.isEmpty()) {
                            text.append("\nAbort message: ").append(abort);
                            clipboard.append("\nAbort message: ").append(CrashReport.shorten(abort, 800));
                        }
                    } else {
                        String traceText = new String(readTracePrefix(trace, 65536), StandardCharsets.UTF_8);
                        text.append("\n\n--- Android trace ---\n").append(traceText);
                        stackTrace = CrashReport.shorten(traceText.trim(), 4000);
                    }
                }
            } catch (IOException error) {
                String traceError = "\nAndroid trace: " + error;
                text.append(traceError);
                clipboard.append(CrashReport.shorten(traceError, 320));
            }
            if (!stackTrace.isEmpty()) text.append("\n\n--- Stack trace ---\n").append(stackTrace);
            text.append("\n\n--- bioshock.log (").append(log != null ? log.getAbsolutePath() : "not written")
                .append(", last 64 KiB) ---\n").append(logText);
            String full = text.toString();
            try { CrashReport.write(saved, full); }
            catch (IOException unavailable) {
                if (saved.equals(privateReport)) throw unavailable;
                CrashReport.write(privateReport, full);
                saved = privateReport;
            }
            if (session().exists() && !session().delete()) android.util.Log.w("BioShock", "Cannot retire crash session");
            shown = true;
            android.util.Log.i("BioShock", "[crash-report] saved " + saved.getAbsolutePath() + ": " + reason.replace('\n', ' '));
            String savedLocation = say("Report saved: ", "Отчёт сохранён: ") + saved.getAbsolutePath();
            String visible = visibleSummary + "\n\n" + savedLocation + "\n\n"
                + say("Copy the report and send it to the port's author, or send the file.",
                      "Скопируйте отчёт и отправьте его автору порта или отправьте сам файл.");
            clipboard.append("\n\n").append(savedLocation);
            if (!stackTrace.isEmpty()) clipboard.append("\n\n--- Stack trace ---\n").append(stackTrace);
            if (!logText.isEmpty()) clipboard.append("\n\n").append(CrashReport.logExcerpt(logText, 30, 48));
            String copied = clipboard.toString();
            AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(say("The game crashed", "Игра завершилась с ошибкой"))
                .setMessage(visible)
                .setPositiveButton(say("Copy to clipboard", "Скопировать в буфер обмена"), null)
                .setNeutralButton(say("Launcher", "В лаунчер"), (d, which) -> close())
                .setNegativeButton(say("Exit", "Выйти"), (d, which) -> finishAndRemoveTask())
                .setCancelable(false).create();
            dialog.setOnShowListener(d -> {
                TextView message = dialog.findViewById(android.R.id.message);
                if (message != null) message.setTextIsSelectable(true);
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                    String result = CrashReport.copy(this, copied) ? say("Report copied", "Отчёт скопирован")
                        : say("Could not copy the report", "Не удалось скопировать отчёт");
                    Toast.makeText(this, result, Toast.LENGTH_SHORT).show();
                });
            });
            dialog.show();
            return true;
        } catch (Exception error) {
            android.util.Log.e("BioShock", "crash report", error);
            return false;
        }
    }

    private static byte[] readTracePrefix(InputStream trace, int limit) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        while (bytes.size() < limit) {
            int count = trace.read(buffer, 0, Math.min(buffer.length, limit - bytes.size()));
            if (count < 0) break;
            bytes.write(buffer, 0, count);
        }
        return bytes.toByteArray();
    }

    private void close() {
        if (session().exists() && !session().delete()) android.util.Log.w("BioShock", "Cannot retire crash session");
        Launch.openLauncher(this, getIntent().getStringExtra(Launch.EXTRA_DISTRIBUTION));
        finish();
    }
}
