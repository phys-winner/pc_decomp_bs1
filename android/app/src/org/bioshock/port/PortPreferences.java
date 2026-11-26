package org.bioshock.port;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Locale;
import java.util.Properties;

final class PortPreferences {
    static final int STICK_DEAD_ZONE_DEFAULT = 12;
    static final int STICK_HIDDEN = 0, STICK_TOUCHING = 1, STICK_ALWAYS = 2;

    static boolean validStickDeadZone(int percent) {
        switch (percent) {
            case 0: case 5: case 8: case 12: case 16: case 20: case 25: case 30: case 40: return true;
            default: return false;
        }
    }

    private final File file;
    private final Properties values = new Properties();
    private long lastModified;
    private long lastLength;
    private boolean dirty;

    PortPreferences(File root) throws IOException {
        if (root == null) throw new IOException("App storage is unavailable");
        file = new File(root, "bioshock-port.properties");
        if (file.isFile()) load();
    }

    private static String key(String name) {
        return name.toLowerCase(Locale.ROOT);
    }

    private void load() throws IOException {
        Properties loaded = new Properties();
        try (FileInputStream input = new FileInputStream(file)) {
            loaded.load(input);
        }
        values.clear();
        for (String name : loaded.stringPropertyNames()) values.setProperty(key(name), loaded.getProperty(name));
        lastModified = file.lastModified();
        lastLength = file.length();
        dirty = false;
    }

    synchronized String getString(String name, String fallback) {
        return values.getProperty(key(name), fallback);
    }

    synchronized int getInt(String name, int fallback) {
        try {
            return Integer.parseInt(values.getProperty(key(name)));
        } catch (NumberFormatException | NullPointerException error) {
            return fallback;
        }
    }

    synchronized float getFloat(String name, float fallback) {
        try {
            float value = Float.parseFloat(values.getProperty(key(name)));
            return Float.isFinite(value) ? value : fallback;
        } catch (NumberFormatException | NullPointerException error) {
            return fallback;
        }
    }

    synchronized boolean getBoolean(String name, boolean fallback) {
        String value = values.getProperty(key(name));
        if (value == null) return fallback;
        if ("true".equalsIgnoreCase(value) || "1".equals(value)) return true;
        if ("false".equalsIgnoreCase(value) || "0".equals(value)) return false;
        return fallback;
    }

    synchronized void put(String name, Object value) {
        String text = String.valueOf(value);
        String normalized = key(name);
        if (text.equals(values.getProperty(normalized))) return;
        values.setProperty(normalized, text);
        dirty = true;
    }

    synchronized boolean reloadIfChanged() throws IOException {
        if (dirty || !file.isFile() || (file.lastModified() == lastModified && file.length() == lastLength)) return false;
        Properties before = new Properties();
        before.putAll(values);
        load();
        return !before.equals(values);
    }

    synchronized void save() throws IOException {
        if (!dirty) return;
        File parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create settings directory");
        File temporary = new File(parent, file.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            values.store(output, "BioShock Android port");
            output.getFD().sync();
        }
        Properties check = new Properties();
        try (FileInputStream input = new FileInputStream(temporary)) {
            check.load(input);
        }
        if (!check.equals(values)) {
            temporary.delete();
            throw new IOException("Settings verification failed");
        }
        try {
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException unsupported) {
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
        lastModified = file.lastModified();
        lastLength = file.length();
        dirty = false;
    }
}
