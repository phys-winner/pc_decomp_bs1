package org.bioshock.port;

import android.content.Context;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.List;
import org.bioshock.port.launcher.Launch;
import org.bioshock.port.launcher.PortHooks;

public final class LauncherHooks implements PortHooks {
    @Override public String optionProblem(Context context, String key, String value) {
        boolean textOption = "game_language".equals(key);
        if (!textOption && !"voice_language".equals(key)) return null;
        File folder = Launch.folder(context, "bioshock", "game");
        String text = GameLanguage.text(Launch.option(context, "game_language"));
        List<String> missing = textOption ? GameLanguage.missing(folder, value, "int")
            : GameLanguage.missing(folder, "int", GameLanguage.voice(text, value));
        if (missing.isEmpty()) return null;
        return "ru".equals(Launch.language(context))
            ? "Не найдены файлы языка" : "Language files not found";
    }

    @Override public List<String> missing(Context context, String distribution, String part, File folder) {
        String text = GameLanguage.text(Launch.option(context, "game_language"));
        return GameLanguage.missing(folder, text, GameLanguage.voice(text, Launch.option(context, "voice_language")));
    }
    private static File savedFolder(Context context) {
        String folder = context.getSharedPreferences("bioshock_paths", Context.MODE_PRIVATE)
            .getString("gameFolder", "/sdcard/BioShock/game");
        File selected = new File(folder);
        if (selected.equals(new File(context.getFilesDir(), "launcher/mount-bioshock-game")))
            return new File("/sdcard/BioShock/game");
        return selected;
    }

    private static File profile(Context context) {
        return new File(savedFolder(context), "userdata");
    }

    static File profile(Context context, File game) {
        if (game == null) return profile(context);
        File sidecar = new File(context.getFilesDir(), "launcher/mount-bioshock-game");
        if (!sidecar.equals(game)) return new File(game, "userdata");
        try {
            String descriptor = android.system.Os.getenv("PORT_MOUNT_FD_GAME");
            if (descriptor != null && !descriptor.isEmpty()) {
                File archive = new File(android.system.Os.readlink("/proc/self/fd/" + Integer.parseInt(descriptor)));
                if (archive.isFile() && archive.getParentFile() != null)
                    return new File(archive.getParentFile(), "userdata");
            }
        } catch (android.system.ErrnoException | RuntimeException error) {
            android.util.Log.w("BioShock", "archive profile path: " + error);
        }
        return profile(context);
    }

    /** Resolve current renderer IDs and migrate saved IDs to the corresponding Vulkan driver. */
    static String rendererMode(String mode) {
        if (mode == null) return null;
        switch (mode) {
        case "gles": case "vulkan": case "vulkan-turnip": case "vulkan-panvk": return mode;
        case "dxvk": return "vulkan";
        case "dxvk-turnip": return "vulkan-turnip";
        default: return null;
        }
    }

    @Override public String renderer(Context context) {
        try {
            return rendererMode(new PortPreferences(profile(context, Launch.folder(context, "bioshock", "game"))).getString("renderer", null));
        } catch (IOException error) {
            return null;
        }
    }

    @Override public void setRenderer(Context context, String mode) {
        try {
            PortPreferences preferences = new PortPreferences(profile(context, Launch.folder(context, "bioshock", "game")));
            preferences.put("renderer", mode);
            preferences.save();
        } catch (IOException error) {
            android.util.Log.w("BioShock", "launcher renderer setting: " + error);
        }
    }

    @Override public String rendererWarning(Context context, String mode, String gpu, String driver) {
        boolean gles = "gles".equals(mode) && ((gpu.equals("Mali-G51") && driver.contains("r18p0")) ||
            (gpu.equals("Mali-T830") && driver.contains("r28p0")) ||
            (gpu.startsWith("Mali-G76") && driver.contains("r26p0")));
        boolean panvk = "vulkan-panvk".equals(mode) && "begonia".equals(android.os.Build.DEVICE) &&
            gpu.contains("Mali-G76") && driver.contains("Mesa");
        if (!gles && !panvk) return null;
        return "ru".equals(Launch.language(context))
            ? "Этот драйвер может показывать чёрный кадр. Выберите системный Vulkan."
            : "This driver can show a black frame. Select system Vulkan.";
    }

    private static void copyIfAbsent(File source, File target) throws IOException {
        if (source.getName().equals("bioshock.log") || source.getName().startsWith("bioshock.log.")) return;
        if (!source.exists()) return;
        if (source.isDirectory()) {
            if (target.exists() && !target.isDirectory()) return;
            if (!target.isDirectory() && !target.mkdirs()) throw new IOException("cannot create " + target);
            File[] children = source.listFiles();
            if (children == null) throw new IOException("cannot list " + source);
            for (File child : children) copyIfAbsent(child, new File(target, child.getName()));
            return;
        }
        if (target.exists()) return;
        File parent = target.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        try (InputStream input = new FileInputStream(source); OutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[65536];
            for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
            ((FileOutputStream) output).getFD().sync();
        }
    }

    private static void ensureLayout(Context context) {
        ensureLayout(context, null);
    }

    static void ensureLayout(Context context, File selected) {
        File previous = profile(context);
        File profile = selected != null ? profile(context, selected) : previous;
        File game = profile.getParentFile();
        for (File directory : new File[]{game, profile}) {
            if (!directory.isDirectory() && !directory.mkdirs())
                android.util.Log.w("BioShock", "cannot create " + directory);
        }
        try {
            File external = context.getExternalFilesDir(null);
            File previousGame = previous.getParentFile();
            copyIfAbsent(previous, profile);
            copyIfAbsent(new File(external, "userdata"), profile);
            copyIfAbsent(new File(external, "bioshock-port.properties"), new File(profile, "bioshock-port.properties"));
            for (String name : new String[]{"userdata-requirements-qa", "userdata-requirements-qa-empty-20261104"}) {
                File target = new File(game, name);
                copyIfAbsent(new File(previousGame, name), target);
                copyIfAbsent(new File(external, name), target);
            }
            android.util.Log.i("BioShock", "profile: " + profile.getAbsolutePath());
        } catch (IOException error) {
            android.util.Log.w("BioShock", "userdata migration: " + error);
        }
    }

    @Override public void migrate(Context context) {
        ensureLayout(context);
        Map<String, String> choices = new HashMap<>();
        choices.put("part.bioshock.game", savedFolder(context).getAbsolutePath());
        choices.put("intro.bioshock", "1");
        try {
            // Migration imports the legacy profile before launcher choices are adopted.
            String mode = rendererMode(new PortPreferences(profile(context)).getString("renderer", null));
            if (mode != null) choices.put("renderer.bioshock", mode);
        } catch (IOException | RuntimeException error) {
            android.util.Log.w("BioShock", "launcher migration: " + error);
        }
        Launch.adopt(context, choices);
    }
}
