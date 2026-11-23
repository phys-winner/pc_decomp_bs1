package org.bioshock.port;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Original PC languages; Russian data is an optional pack beside the original files. */
final class GameLanguage {
    static final String[] CODES = {"int", "deu", "fra", "esp", "ita", "rus"};
    static final String[] LEVELS = {"0-lighthouse", "1-welcome", "1-medical", "2-fisheries", "2-subbay",
        "3-arcadia", "3-market", "4-recreation", "5-hephaestus", "5-ryan", "6-resi", "6-slums",
        "7-bossfight", "7-gauntlet", "7-science"};

    static boolean valid(String language) {
        for (String code : CODES) if (code.equals(language)) return true;
        return false;
    }

    private static File child(File root, String path) {
        File at = root;
        for (String part : path.split("/")) {
            File direct = new File(at, part);
            if (direct.exists()) { at = direct; continue; }
            File[] children = at.listFiles();
            at = null;
            if (children != null) for (File file : children) if (part.equalsIgnoreCase(file.getName())) { at = file; break; }
            if (at == null) return null;
        }
        return at;
    }

    /** The non-empty files of a stored ZIP the launcher plays in place, lower case with '/': its index
     *  ("ZMI1", StoredArchive.java) lies beside the sidecar folder that the launcher names as the game folder
     *  (files/launcher/mount-bioshock-game and mount-bioshock-game.zmi). Empty for a plain folder. */
    private static Set<String> archived(File folder, Set<String> wanted) {
        Set<String> names = new HashSet<>();
        File index = folder == null ? null : new File(folder.getPath() + ".zmi");
        if (index == null || !index.isFile()) return names;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(index), 1 << 16))) {
            byte[] head = new byte[48];
            in.readFully(head);
            if (head[0] != 'Z' || head[1] != 'M' || head[2] != 'I' || head[3] != '1') return names;
            long count = (head[4] & 0xFFL) | (head[5] & 0xFFL) << 8 | (head[6] & 0xFFL) << 16 | (head[7] & 0xFFL) << 24;
            byte[] record = new byte[34];
            for (long i = 0; i < count; ++i) {
                in.readFully(record);   /* offset, size, CRC-32, flags, time, name length: little endian */
                long size = 0;
                for (int b = 7; b >= 0; --b) size = size << 8 | (record[8 + b] & 0xFF);
                byte[] name = new byte[(record[32] & 0xFF) | (record[33] & 0xFF) << 8];
                in.readFully(name);
                if (size > 0) {
                    String path = new String(name, StandardCharsets.UTF_8).replace('\\', '/').toLowerCase(Locale.ROOT);
                    if (wanted.contains(path)) names.add(path);
                }
            }
        } catch (IOException | RuntimeException error) {
            names.clear();
        }
        return names;
    }

    static String text(String option) {
        return option == null ? "int" : option;
    }

    static String voice(String text, String option) {
        return option == null || "same".equals(option) ? text : option;
    }

    private static boolean contains(File folder, Set<String> archive, String path) {
        if (archive.contains(path.toLowerCase(Locale.ROOT))) return true;
        File file = folder == null ? null : child(folder, path);
        return file != null && file.isFile() && file.canRead() && file.length() > 0;
    }

    static List<String> missing(File folder, String text, String voice) {
        ArrayList<String> missing = new ArrayList<>();
        if (!valid(text) || !valid(voice)) { missing.add("Invalid game language"); return missing; }
        ArrayList<String> required = new ArrayList<>();
        // English files belong to the base installation.
        if (!"int".equals(text)) required.add("Content/Localized" + text + ".lbf");
        if (!"int".equals(voice)) {
            for (String level : LEVELS) required.add("Content/Maps/" + level + "_" + voice + ".bsm");
            for (String bank : new String[]{"0", "1", "2", "3", "4", "5", "6", "7", "99", "aivo"})
                required.add("Content/Sounds_Windows/streams_" + bank + "_audio." + voice + "_fsb");
        }
        if (required.isEmpty()) return missing;
        Set<String> wanted = new HashSet<>();
        for (String path : required) {
            wanted.add(path.toLowerCase(Locale.ROOT));
            if (path.endsWith(".rus_fsb")) {
                String bank = path.substring(0, path.lastIndexOf('.')).toLowerCase(Locale.ROOT);
                wanted.add(bank + ".rus_fsd");
                wanted.add(bank + ".fsb");
            }
        }
        Set<String> archive = archived(folder, wanted);
        for (String path : required) {
            if (contains(folder, archive, path)) continue;
            if (path.endsWith(".rus_fsb")) {
                String bank = path.substring(0, path.lastIndexOf('.'));
                if (contains(folder, archive, bank + ".rus_fsd") && contains(folder, archive, bank + ".fsb")) continue;
            }
            missing.add(path);
        }
        return missing;
    }
}
