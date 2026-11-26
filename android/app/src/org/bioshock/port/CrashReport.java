package org.bioshock.port;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Environment;
import android.util.Log;
import java.io.ByteArrayInputStream;
import java.io.DataInput;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.Locale;

/** Crash reports shared by the game process and the crash watcher (GameWatchActivity); after Frostpunk's
 *  android/app/CrashReport.java. The game names its session log here (SessionLog.begin) and records a stop it
 *  made on purpose (a clean exit, a renderer failure the launcher handles); the watcher reads both. */
final class CrashReport {
    static final String EXTRA_WATCHED = "crash.watched", EXTRA_LANGUAGE = "crash.language", EXTRA_EXIT_CODE = "crash.exitCode";
    private static final String SESSION_LOG = "crash-session-log.txt", PLANNED_STOP = "crash-planned-stop.txt";

    private CrashReport() { }

    /** /sdcard/BioShock, where the game data already lives: a player finds the reports with any file manager. */
    private static File sharedDirectory() {
        return new File(Environment.getExternalStorageDirectory(), "BioShock");
    }

    static void prepareDirectory(File directory) throws IOException {
        if (!directory.isDirectory()) directory.mkdirs();
        if (!Files.isDirectory(directory.toPath(), LinkOption.NOFOLLOW_LINKS) || !directory.canWrite())
            throw new IOException("Cannot write to " + directory);
    }

    static File reportDirectory(File privateFiles) {
        File directory = new File(sharedDirectory(), "crashes");
        try {
            prepareDirectory(directory);
            return directory;
        } catch (IOException unavailable) {
            return new File(privateFiles, "crashes");
        }
    }

    /** Game process: the session's bioshock.log, for the watcher (both processes share the private files). */
    static void rememberSessionLog(Context context, File log) {
        try { write(new File(context.getFilesDir(), SESSION_LOG), log.getAbsolutePath()); }
        catch (IOException error) { Log.w("BioShock", "crash session log: " + error); }
    }

    /** The log the game named after the watcher started it, or null (an older session's log is not this crash's). */
    static File sessionLog(Context context, long since) {
        File pointer = new File(context.getFilesDir(), SESSION_LOG);
        if (pointer.lastModified() < since) return null;
        try {
            String path = tail(pointer, 4096).trim();
            return path.isEmpty() ? null : new File(path);
        } catch (IOException error) {
            return null;
        }
    }

    /** Game process, right before it ends itself on purpose: the watcher then shows no report. */
    static void plannedStop(Context context, String what) {
        try { write(new File(context.getFilesDir(), PLANNED_STOP), what); }
        catch (IOException error) { Log.w("BioShock", "crash planned stop: " + error); }
    }

    static String plannedStop(Context context, long since) {
        File file = new File(context.getFilesDir(), PLANNED_STOP);
        if (file.lastModified() < since) return null;
        try { return tail(file, 256).trim(); } catch (IOException error) { return null; }
    }

    static String tail(File file, int limit) throws IOException {
        if (file == null || limit <= 0 || !Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return "";
        try (FileChannel input = FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            long size = input.size();
            if (size <= 0) return "";
            long start = Math.max(0, size - limit);
            input.position(start);
            ByteBuffer data = ByteBuffer.allocate((int) (size - start));
            while (data.hasRemaining() && input.read(data) > 0) { }
            String text = new String(data.array(), 0, data.position(), StandardCharsets.UTF_8);
            int newline = start > 0 ? text.indexOf('\n') : -1;
            return newline >= 0 ? text.substring(newline + 1) : text;
        }
    }

    /** The start of a file up to its last whole line within limit bytes. */
    static String head(File file, int limit) throws IOException {
        if (file == null || limit <= 0 || !Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return "";
        try (FileChannel input = FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            ByteBuffer data = ByteBuffer.allocate((int) Math.min(limit, input.size()));
            while (data.hasRemaining() && input.read(data) > 0) { }
            String text = new String(data.array(), 0, data.position(), StandardCharsets.UTF_8);
            if (data.position() >= input.size()) return text;
            int newline = text.lastIndexOf('\n');
            return newline >= 0 ? text.substring(0, newline + 1) : "";
        }
    }

    private static final java.util.regex.Pattern SESSION_FACT = java.util.regex.Pattern.compile(
        "^\\[(?:session\\]|perf-flags\\]|r10\\] (?:VULKAN_DRIVER|GLES|OpenGL|RENDERER_ACTIVE|Vulkan: .*\\bdriver\\b))");

    /** What the session ran on, from the start of bioshock.log: SessionLog's header and how the previous
     *  session ended, the GPU, driver and renderer (r10), QA switches. */
    static String sessionFacts(String head) {
        StringBuilder text = new StringBuilder();
        int count = 0;
        for (String line : head.split("\\R")) {
            if (count == 12) break;
            if (!SESSION_FACT.matcher(line).find()) continue;
            text.append(shorten(line, 240)).append('\n');
            ++count;
        }
        return text.toString();
    }

    private static boolean faultStart(String line) {
        return line.startsWith("[void] host signal") || line.startsWith("[void] FATAL") || line.startsWith("[java] uncaught");
    }

    /** Why bioshock.log says the game stopped: the native fault report of core.c (on_signal, void_fatal) or the
     *  Java handler of SessionLog; null when the log ends without one. */
    static String logReason(String log, boolean russian) {
        String[] lines = log.split("\\R");
        String signal = null;
        for (int i = lines.length - 1; i >= 0; i--) {
            String line = lines[i];
            if (line.startsWith("[void] FATAL: "))
                return (russian ? "Ошибка игры: " : "Game error: ") + shorten(line.substring(14), 400);
            if (line.startsWith("[java] uncaught exception"))
                return (russian ? "Необработанная ошибка Java: " : "Unhandled Java exception: ") + shorten(line.substring(7), 400);
            if (line.startsWith("[void] fault signal=") && signal == null) signal = line.substring(13);
            if (line.startsWith("[void] host signal")) {
                if (signal == null) signal = "signal " + line.substring(18).trim();
                return (russian ? "Сбой в нативном коде игры: " : "Crash in native game code: ") + shorten(signal, 200);
            }
        }
        return null;
    }

    /** The clipboard part of bioshock.log: the lines before the fault, which show what the game was doing, and
     *  the fault report itself (signal, registers, frames as library+offset, guest memory). */
    static String logExcerpt(String log, int before, int fault) {
        String[] lines = log.split("\\R");
        int start = -1;
        for (int i = lines.length - 1; i >= 0 && start < 0; i--) if (faultStart(lines[i])) start = i;
        int end = start >= 0 ? start : lines.length;
        StringBuilder text = new StringBuilder();
        text.append(start >= 0 ? "--- bioshock.log: before the fault ---\n" : "--- bioshock.log: last lines ---\n");
        // Repeats (a per-frame trace) count once with their number, so the lines show more of what happened.
        java.util.ArrayList<String> context = new java.util.ArrayList<>();
        for (int i = end - 1, repeats = 1; i >= 0 && context.size() < before; i--) {
            if (i > 0 && lines[i - 1].equals(lines[i])) { ++repeats; continue; }
            context.add(shorten(lines[i], 240) + (repeats > 1 ? " (×" + repeats + ")" : ""));
            repeats = 1;
        }
        for (int i = context.size() - 1; i >= 0; i--) text.append(context.get(i)).append('\n');
        if (start >= 0) {
            text.append("--- bioshock.log: fault ---\n");
            for (int i = start; i < lines.length && i < start + fault; i++) text.append(shorten(lines[i], 240)).append('\n');
            if (lines.length - start > fault) text.append("… ").append(lines.length - start - fault).append(" more lines in the saved report\n");
        }
        return text.toString();
    }

    /** The GNU build ID of a shared library (its PT_NOTE), so that a stack is symbolized with the matching
     *  unstripped library. */
    static String buildId(File library) {
        try (RandomAccessFile input = new RandomAccessFile(library, "r")) {
            byte[] header = new byte[64];
            input.readFully(header);
            if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' || header[4] != 2 || header[5] != 1) return "";
            ByteBuffer elf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
            long table = elf.getLong(32);
            int entry = elf.getShort(54) & 0xffff, count = elf.getShort(56) & 0xffff;
            byte[] program = new byte[56];
            for (int i = 0; i < count && i < 64 && entry >= 56; i++) {
                input.seek(table + (long) i * entry);
                input.readFully(program);
                ByteBuffer segment = ByteBuffer.wrap(program).order(ByteOrder.LITTLE_ENDIAN);
                if (segment.getInt(0) != 4) continue;
                long offset = segment.getLong(8), size = segment.getLong(32);
                if (size < 12 || size > 4096) continue;
                byte[] notes = new byte[(int) size];
                input.seek(offset);
                input.readFully(notes);
                ByteBuffer note = ByteBuffer.wrap(notes).order(ByteOrder.LITTLE_ENDIAN);
                for (int at = 0; at + 12 <= size; ) {
                    int nameSize = note.getInt(at), descSize = note.getInt(at + 4), type = note.getInt(at + 8);
                    int name = at + 12;
                    if (nameSize < 0 || descSize < 0 || nameSize > size) break;
                    int desc = name + ((nameSize + 3) & ~3);
                    if (desc + (long) descSize > size) break;
                    if (type == 3 && nameSize == 4 && notes[name] == 'G' && notes[name + 1] == 'N' && notes[name + 2] == 'U') {
                        StringBuilder id = new StringBuilder();
                        for (int k = 0; k < descSize; k++) id.append(String.format(Locale.ROOT, "%02x", notes[desc + k] & 0xff));
                        return id.toString();
                    }
                    at = desc + ((descSize + 3) & ~3);
                }
            }
        } catch (IOException | RuntimeException error) {
            Log.w("BioShock", "build id of " + library + ": " + error);
        }
        return "";
    }

    private static long varint(DataInput input) throws IOException {
        long value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            int part = input.readUnsignedByte();
            if (shift == 63 && part > 1) break;
            value |= (long) (part & 127) << shift;
            if (part < 128) return value;
        }
        throw new IOException("Invalid tombstone varint");
    }

    static String nativeAbortMessage(byte[] prefix) throws IOException {
        // Android debuggerd's Tombstone protobuf stores abort_message in field 14.
        DataInputStream input = new DataInputStream(new ByteArrayInputStream(prefix));
        while (input.available() != 0) {
            long tag = varint(input);
            long length;
            switch ((int) (tag & 7)) {
                case 0: varint(input); continue;
                case 1: length = 8; break;
                case 2: length = varint(input); break;
                case 5: length = 4; break;
                default: return "";
            }
            if (length < 0 || length > input.available()) return "";
            if (tag == ((14 << 3) | 2)) {
                byte[] message = new byte[(int) Math.min(length, 4096)];
                input.readFully(message);
                return new String(message, StandardCharsets.UTF_8).trim();
            }
            input.skipBytes((int) length);
        }
        return "";
    }

    static String nativeStackTrace(File tombstone) throws IOException {
        if (!Files.isRegularFile(tombstone.toPath(), LinkOption.NOFOLLOW_LINKS)) return "";
        try (RandomAccessFile input = new RandomAccessFile(tombstone, "r")) {
            // debuggerd/proto/tombstone.proto: tid (6), threads (16), and their map entries (1, 2).
            long end = input.length(), tid = 0;
            ProtoFields fields = new ProtoFields(input, 0, end);
            for (int tag; (tag = fields.next()) != 0; ) {
                if (tag == (6 << 3)) { tid = fields.number; break; }
            }
            if (tid == 0) return "";
            fields = new ProtoFields(input, 0, end);
            for (int tag; (tag = fields.next()) != 0; ) {
                if (tag != ((16 << 3) | 2)) continue;
                ProtoFields entry = fields.message(), thread = null;
                long key = 0;
                for (int member; (member = entry.next()) != 0; ) {
                    if (member == (1 << 3)) key = entry.number;
                    if (member == ((2 << 3) | 2)) thread = entry.message();
                }
                if (key == tid && thread != null) return nativeThreadTrace(thread, tid);
            }
        }
        return "";
    }

    private static String nativeThreadTrace(ProtoFields thread, long tid) throws IOException {
        String name = "";
        StringBuilder frames = new StringBuilder();
        int count = 0;
        for (int tag; (tag = thread.next()) != 0; ) {
            if (tag == ((2 << 3) | 2)) name = thread.text(80);
            if (tag != ((4 << 3) | 2)) continue;
            if (count < 16) appendNativeFrame(frames, thread.message(), count);
            ++count;
        }
        if (count == 0) return "";
        if (count > 16) frames.append("\n… ").append(count - 16).append(" more frames (see tombstone)");
        return "tid=" + tid + (name.isEmpty() ? "" : " (" + name + ")") + frames;
    }

    private static void appendNativeFrame(StringBuilder text, ProtoFields frame, int index) throws IOException {
        long pc = 0, functionOffset = 0, fileOffset = 0;
        String file = "", function = "", buildId = "";
        for (int tag; (tag = frame.next()) != 0; ) {
            switch (tag) {
                case 1 << 3: pc = frame.number; break;
                case (4 << 3) | 2: function = frame.text(256); break;
                case 5 << 3: functionOffset = frame.number; break;
                case (6 << 3) | 2: file = frame.text(256); break;
                case 7 << 3: fileOffset = frame.number; break;
                case (8 << 3) | 2: buildId = frame.text(64); break;
                default: break;
            }
        }
        // The library's name only: the APK's install path says nothing, libbioshock.so's BuildId is in the header.
        String name = file.isEmpty() ? "<unknown>" : file.substring(file.lastIndexOf('/') + 1);
        text.append(String.format(Locale.ROOT, "\n#%02d pc %016x  %s", index, pc, name));
        if (fileOffset != 0) text.append(" (offset 0x").append(Long.toHexString(fileOffset)).append(')');
        if (!function.isEmpty()) text.append(" (").append(function).append('+').append(functionOffset).append(')');
        if (!buildId.isEmpty() && !name.equals("libbioshock.so")) text.append(" (BuildId: ").append(buildId).append(')');
    }

    private static final class ProtoFields {
        private final RandomAccessFile input;
        private final long end;
        private long position;
        long number;

        ProtoFields(RandomAccessFile input, long start, long end) {
            this.input = input;
            position = start;
            this.end = end;
        }

        int next() throws IOException {
            if (position == end) return 0;
            input.seek(position);
            long tag = varint(input), length;
            switch ((int) tag & 7) {
                case 0: number = varint(input); length = 0; break;
                case 1: length = 8; break;
                case 2: length = varint(input); break;
                case 5: length = 4; break;
                default: throw new IOException("Unsupported tombstone field");
            }
            long start = input.getFilePointer();
            if (tag < 8 || tag > 0xffffffffL || start > end || length < 0 || length > end - start)
                throw new IOException("Invalid tombstone field");
            position = start + length;
            return (int) tag;
        }

        ProtoFields message() throws IOException {
            return new ProtoFields(input, input.getFilePointer(), position);
        }

        String text(int limit) throws IOException {
            long length = position - input.getFilePointer();
            byte[] bytes = new byte[(int) Math.min(limit, length)];
            input.readFully(bytes);
            String value = new String(bytes, StandardCharsets.UTF_8).replace('\n', ' ').replace('\r', ' ');
            return length > limit ? value + "…" : value;
        }
    }

    static void write(File file, String text) throws IOException {
        write(file, new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)));
    }

    private static void write(File file, InputStream contents) throws IOException {
        File parent = file.getParentFile();
        prepareDirectory(parent);
        File temporary = File.createTempFile(".bioshock-", ".tmp", parent);
        try {
            try (FileOutputStream out = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[16384];
                int count;
                while ((count = contents.read(buffer)) != -1) out.write(buffer, 0, count);
                out.getFD().sync();
            }
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally {
            Files.deleteIfExists(temporary.toPath());
        }
    }

    static File saveTombstone(File report, File privateReport, InputStream trace) throws IOException {
        File local = new File(privateReport.getPath() + ".pb");
        write(local, trace);
        if (report.equals(privateReport)) return local;

        File shared = new File(report.getPath() + ".pb");
        try (InputStream contents = Files.newInputStream(local.toPath())) {
            write(shared, contents);
        } catch (IOException unavailable) {
            return local;
        }
        if (!local.delete()) Log.w("BioShock", "Cannot remove private copy of exported tombstone");
        return shared;
    }

    static String shorten(String text, int limit) {
        return text.length() <= limit ? text : text.substring(0, limit) + "…";
    }

    /** Copies and reads the clip back: true only when the clipboard holds exactly this report. */
    static boolean copy(Context context, String text) {
        try {
            ClipboardManager clipboard = context.getSystemService(ClipboardManager.class);
            if (clipboard == null) return false;
            clipboard.setPrimaryClip(ClipData.newPlainText("BioShock crash report", text));
            ClipData copied = clipboard.getPrimaryClip();
            CharSequence contents = copied != null && copied.getItemCount() == 1 ? copied.getItemAt(0).getText() : null;
            boolean same = contents != null && text.contentEquals(contents);
            Log.i("BioShock", "[crash-report] copied " + text.length() + " chars, read back " + (same ? "equal" : "different"));
            // QA: what the button put on the clipboard (run-as ... cat files/crash-last-clipboard.txt).
            if (same) try { write(new File(context.getFilesDir(), "crash-last-clipboard.txt"), text); }
            catch (IOException error) { Log.w("BioShock", "crash clipboard copy: " + error); }
            return same;
        } catch (RuntimeException error) {
            Log.w("BioShock", "Cannot copy crash report", error);
            return false;
        }
    }

    static String exitReason(int reason, int status, String description, boolean russian) {
        if (description != null && !description.trim().isEmpty() && reason != 1) return description.trim();
        switch (reason) {
        case 1:
            return (russian ? "Игра завершилась с кодом " : "Game exited with code ") + status;
        case 2:
            return (russian ? "Процесс игры завершён сигналом " : "Game process terminated by signal ")
                + (status == 11 ? "SIGSEGV" : status == 6 ? "SIGABRT" : status == 7 ? "SIGBUS" : status == 9 ? "SIGKILL" : Integer.toString(status));
        case 3:
            return russian ? "Android завершил игру из-за нехватки памяти" : "Android stopped the game because memory was low";
        case 4:
            return russian ? "Необработанная ошибка Java" : "Unhandled Java exception";
        case 5:
            return russian ? "Сбой в нативном коде игры" : "Crash in native game code";
        case 6:
            return russian ? "Игра перестала отвечать (ANR)" : "The game stopped responding (ANR)";
        default:
            return russian ? "Процесс игры неожиданно завершился" : "Game process stopped unexpectedly";
        }
    }
}
