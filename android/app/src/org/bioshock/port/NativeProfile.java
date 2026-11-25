package org.bioshock.port;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Arrays;

/** Prepares native graphics and protects saved settings across Android process restarts. */
final class NativeProfile {
    interface Template { InputStream open() throws IOException; }

    private static File child(File parent, String name) throws IOException {
        File[] children = parent.listFiles();
        if (children == null) throw new IOException("Cannot read profile directory: " + parent);
        File found = null;
        for (File candidate : children) {
            if (!candidate.getName().equalsIgnoreCase(name)) continue;
            if (found != null) throw new IOException("Ambiguous profile path: " + parent + "/" + name);
            found = candidate;
        }
        return found != null ? found : new File(parent, name);
    }

    static boolean initialize(File profile, int width, int height, Template template) throws IOException {
        return initialize(profile, width, height, template, null);
    }

    static boolean initialize(File profile, int width, int height, Template template, String language) throws IOException {
        if (language != null && !language.matches("int|deu|fra|esp|ita|rus")) throw new IOException("Invalid game language");
        if (width < 8 || height < 8 || width > 8192 || height > 8192 || width < height)
            throw new IOException("Invalid fullscreen dimensions: " + width + "x" + height);
        Path profileRoot = profile.toPath().toRealPath();
        File folder = profile;
        for (String name : new String[]{"AppData", "Roaming", "Bioshock"}) {
            folder = child(folder, name);
            requireWithinProfile(profileRoot, folder);
            if (!folder.isDirectory() && !folder.mkdir())
                throw new IOException("Cannot create game configuration directory: " + folder);
        }
        File running = child(folder, "Running.ini");
        markerExists(running, true);
        markerExists(child(folder, "DoNotDelete.ini"), false);
        File target = child(folder, "Bioshock.ini");
        requireWithinProfile(profileRoot, target);
        if (target.isFile()) {
            resizeFullscreen(target, width, height, language);
            protectAndroidConfiguration(profileRoot, folder, running);
            return false;
        }
        if (target.exists()) throw new IOException("Game configuration is not a file: " + target);
        byte[] original;
        try (InputStream input = template.open(); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
            original = output.toByteArray();
        }
        byte[] configured = fullscreen(original, width, height, true, language);
        Path temporary = Files.createTempFile(folder.toPath(), "bioshock-profile-", ".tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(temporary.toFile())) {
                output.write(configured);
                output.getFD().sync();
            }
            // No REPLACE_EXISTING: an existing original profile always wins.
            try { Files.move(temporary, target.toPath()); }
            catch (FileAlreadyExistsException existing) {
                if (!target.isFile()) throw existing;
                protectAndroidConfiguration(profileRoot, folder, running);
                return false;
            }
            try {
                protectAndroidConfiguration(profileRoot, folder, running);
            } catch (IOException failure) {
                // Only our newly committed INI belongs to this transaction.
                // Leave no unprotected seed that a retry would mistake for an existing profile.
                try { Files.delete(target.toPath()); }
                catch (IOException cleanup) { failure.addSuppressed(cleanup); }
                throw failure;
            }
            return true;
        } finally {
            Files.deleteIfExists(temporary);
        }
    }

    private static void requireWithinProfile(Path profileRoot, File file) throws IOException {
        Path path = file.toPath();
        path = Files.exists(path, LinkOption.NOFOLLOW_LINKS) ? path.toRealPath()
            : file.getParentFile().toPath().toRealPath().resolve(file.getName());
        if (!path.startsWith(profileRoot))
            throw new IOException("Game configuration escapes the selected profile: " + file);
    }

    private static void resizeFullscreen(File target, int width, int height, String language) throws IOException {
        byte[] original = Files.readAllBytes(target.toPath()), configured;
        try { configured = fullscreen(original, width, height, false, language); }
        catch (IOException unknownFormat) { if (language != null) throw unknownFormat; return; }
        if (Arrays.equals(original, configured)) return;
        Path temporary = Files.createTempFile(target.getParentFile().toPath(), "bioshock-display-", ".tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(temporary.toFile())) {
                output.write(configured);
                output.getFD().sync();
            }
            if (!Arrays.equals(original, Files.readAllBytes(target.toPath())))
                throw new IOException("Game configuration changed while updating fullscreen dimensions");
            try { Files.move(temporary, target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING); }
            catch (AtomicMoveNotSupportedException unavailable) {
                Files.move(temporary, target.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            Files.deleteIfExists(temporary);
        }
    }

    private static boolean markerExists(File marker, boolean empty) throws IOException {
        BasicFileAttributes attributes;
        try { attributes = Files.readAttributes(marker.toPath(), BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS); }
        catch (NoSuchFileException absent) { return false; }
        if (!attributes.isRegularFile() || (empty && attributes.size() != 0))
            throw new IOException("Unrecognized game runtime marker; preserved: " + marker);
        return true;
    }

    private static void protectAndroidConfiguration(Path profileRoot, File folder, File running) throws IOException {
        requireWithinProfile(profileRoot, folder);
        markerExists(running, true);
        keepConfiguration(folder);
        // Android process death leaves this empty marker; original recovery would wipe both player INIs.
        // Preserve settings after real crashes too; a configuration reset must be requested explicitly.
        if (markerExists(running, true)) Files.delete(running.toPath());
    }

    private static void keepConfiguration(File folder) throws IOException {
        // The original bootstrap deletes Bioshock.ini and User.ini when both
        // Running.ini and this native marker are absent. FirstRun remains untouched.
        File marker = child(folder, "DoNotDelete.ini");
        if (markerExists(marker, false)) return;
        boolean created = false;
        try {
            Files.createFile(marker.toPath());
            created = true;
            try (FileOutputStream output = new FileOutputStream(marker, true)) {
                output.getFD().sync();
            }
        } catch (FileAlreadyExistsException existing) {
            if (!markerExists(marker, false)) throw existing;
        } catch (IOException failure) {
            if (created) {
                try { Files.delete(marker.toPath()); }
                catch (IOException cleanup) { failure.addSuppressed(cleanup); }
            }
            throw failure;
        }
    }

    static boolean seedMobileShaders(File profile, boolean enabled, Template template) throws IOException {
        if (!enabled) return false;
        File folder = child(profile, "ShaderCache-mobile-v3");
        if (!folder.isDirectory() && !folder.mkdir())
            throw new IOException("Cannot create mobile shader cache directory: " + folder);
        File target = child(folder, "ShaderCache.pcs");
        if (target.isFile()) return false;
        if (target.exists()) throw new IOException("Mobile shader cache is not a file: " + target);
        Path temporary = Files.createTempFile(folder.toPath(), "bioshock-shader-seed-", ".tmp");
        try {
            try (InputStream input = template.open(); FileOutputStream output = new FileOutputStream(temporary.toFile())) {
                byte[] buffer = new byte[16384];
                for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
                output.getFD().sync();
            }
            // Existing engine progress always wins, including a file created during copying.
            try { Files.move(temporary, target.toPath()); }
            catch (FileAlreadyExistsException existing) {
                if (!target.isFile()) throw existing;
                return false;
            }
            return true;
        } finally {
            Files.deleteIfExists(temporary);
        }
    }

    private static byte[] fullscreen(byte[] original, int width, int height) throws IOException {
        return fullscreen(original, width, height, true);
    }

    private static byte[] fullscreen(byte[] original, int width, int height, boolean seed) throws IOException {
        return fullscreen(original, width, height, seed, null);
    }

    private static byte[] fullscreen(byte[] original, int width, int height, boolean seed, String language) throws IOException {
        Charset encoding = StandardCharsets.UTF_8;
        int bom = 0;
        if (original.length >= 2 && original[0] == (byte)0xff && original[1] == (byte)0xfe) {
            encoding = StandardCharsets.UTF_16LE; bom = 2;
        } else if (original.length >= 2 && original[0] == (byte)0xfe && original[1] == (byte)0xff) {
            encoding = StandardCharsets.UTF_16BE; bom = 2;
        } else if (original.length >= 3 && original[0] == (byte)0xef && original[1] == (byte)0xbb && original[2] == (byte)0xbf) {
            bom = 3;
        }
        String text;
        try {
            text = encoding.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(original, bom, original.length - bom)).toString();
        } catch (CharacterCodingException invalid) {
            throw new IOException("Invalid game configuration template encoding", invalid);
        }
        if (!seed) {
            boolean client = false;
            for (String line : text.split("[\\r\\n]+")) {
                String clean = line.trim();
                if (clean.startsWith("[") && clean.endsWith("]"))
                    client = clean.equalsIgnoreCase("[WinDrv.WindowsClient]");
                else if (client) {
                    int equals = clean.indexOf('=');
                    if (equals >= 0 && clean.substring(0, equals).trim().equalsIgnoreCase("FullscreenViewportY")) {
                        String value = clean.substring(equals + 1).split(";", 2)[0].trim();
                        int selectedHeight;
                        try { selectedHeight = Integer.parseInt(value); }
                        catch (NumberFormatException invalid) { throw new IOException("Invalid fullscreen height", invalid); }
                        long selectedWidth = ((long)selectedHeight * width + height / 2) / height;
                        if (selectedHeight < 8 || selectedHeight > 8192 || selectedWidth < 8 || selectedWidth > 8192)
                            throw new IOException("Fullscreen resolution exceeds display limits");
                        width = (int)selectedWidth;
                        height = selectedHeight;
                        break;
                    }
                }
            }
        }
        // The legacy controller flag can select the Xbox menu before Android's live input bridge takes over.
        String[] keys = {"FullscreenViewportX", "FullscreenViewportY", "StartupFullscreen", "UseController"};
        String[] values = {Integer.toString(width), seed ? Integer.toString(height) : null, seed ? "True" : null, "False"};
        String[] lowKeys = {"HighDetailShaders", "Shadows", "RealTimeReflection", "UseRippleSystem",
            "UseHighDetailSoftParticles", "UseSpecCubeMap", "UseDistortion", "UseHighDetailPostProcEffects",
            "TextureDetail", "ForceGlobalLighting", "HorizontalFOVLock"};
        // Original PCOptionsData.LowQuality uses RenderConfig texture enum 2 (Medium), actor detail 0 and global lighting.
        // HorizontalFOVLock (patch 1.1, default on) keeps the 4:3 horizontal angle, so a 20:9 phone loses 40 % of
        // the vertical view; off keeps the 4:3 vertical angle and widens the horizontal one (Hor+, wiki note 13 D4).
        // Only a new profile is seeded: a player's saved choice in Graphics Options always wins.
        String[] lowValues = {"False", "False", "False", "False", "False", "False", "False", "False", "Medium", "True", "False"};
        String[] actorKeys = {"HighDetailActors", "SuperHighDetailActors"};
        boolean[] lowSeen = new boolean[lowKeys.length];
        int renderEnd = -1;
        int[] counts = new int[keys.length];
        int sections = 0;
        boolean selected = false;
        boolean render = false;
        boolean renderDevice = false;
        boolean engine = false;
        int languages = 0;
        StringBuilder changed = new StringBuilder(text.length());
        for (int start = 0; start < text.length();) {
            int end = start;
            while (end < text.length() && text.charAt(end) != '\r' && text.charAt(end) != '\n') ++end;
            int next = end;
            if (next < text.length() && text.charAt(next) == '\r') ++next;
            if (next < text.length() && text.charAt(next) == '\n') ++next;
            String line = text.substring(start, end), clean = line.trim();
            if (clean.startsWith("[") && clean.endsWith("]")) {
                if (render) renderEnd = changed.length();
                selected = clean.equalsIgnoreCase("[WinDrv.WindowsClient]");
                render = seed && clean.equalsIgnoreCase("[Engine.RenderConfig]");
                renderDevice = seed && clean.equalsIgnoreCase("[D3DDrv.D3DRenderDevice]");
                engine = language != null && clean.equalsIgnoreCase("[Engine.Engine]");
                if (selected) ++sections;
            } else if (selected || render || renderDevice || engine) {
                int equals = line.indexOf('=');
                if (equals >= 0) {
                    String key = line.substring(0, equals).trim();
                    String[] targets = engine ? new String[]{"Language"} : selected ? keys : render ? lowKeys : actorKeys;
                    for (int index = 0; index < targets.length; ++index) {
                        if (!key.equalsIgnoreCase(targets[index])) continue;
                        if (engine) ++languages;
                        else if (selected) ++counts[index];
                        else if (render) lowSeen[index] = true;
                        if (selected && values[index] == null) continue;
                        int begin = equals + 1;
                        while (begin < line.length() && Character.isWhitespace(line.charAt(begin))) ++begin;
                        int finish = line.indexOf(';', begin);
                        if (finish < 0) finish = line.length();
                        while (finish > begin && Character.isWhitespace(line.charAt(finish - 1))) --finish;
                        line = line.substring(0, begin) + (engine ? language : selected ? values[index] : render ? lowValues[index] : "False") + line.substring(finish);
                    }
                }
            }
            changed.append(line).append(text, end, next);
            start = next;
        }
        if (sections != 1 || counts[0] != 1 || counts[1] != 1 || counts[2] != 1)
            throw new IOException("Game configuration template lacks unique fullscreen settings");
        if (language != null && languages != 1) throw new IOException("Game configuration lacks a unique language setting");
        if (render) renderEnd = changed.length();
        if (renderEnd >= 0) {
            String newline = text.contains("\r\n") ? "\r\n" : "\n";
            StringBuilder missing = new StringBuilder();
            if (renderEnd > 0 && changed.charAt(renderEnd - 1) != '\n' && changed.charAt(renderEnd - 1) != '\r')
                missing.append(newline);
            for (int index = 0; index < lowKeys.length; ++index)
                if (!lowSeen[index]) missing.append(lowKeys[index]).append('=').append(lowValues[index]).append(newline);
            changed.insert(renderEnd, missing);
        }
        byte[] body = changed.toString().getBytes(encoding);
        byte[] result = new byte[bom + body.length];
        System.arraycopy(original, 0, result, 0, bom);
        System.arraycopy(body, 0, result, bom, body.length);
        return result;
    }
}
