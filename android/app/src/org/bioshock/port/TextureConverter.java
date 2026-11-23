package org.bioshock.port;

import java.io.File;
import java.util.ArrayList;
import org.bioshock.port.launcher.TextureBackend;

/** The launcher's texture card (port.json "textures"): every DXT1/3/5 texture of the game's packages and bulk
 *  files is encoded once into ASTC 4x4 or ETC2 in <cache>/Content/port-textures.tvt (libtexconv.so,
 *  android/texconv/bioshock_tex.cpp); the cache root is the game folder unless the launcher names another one.
 *  A stored ZIP played in place is read through the launcher's index and the archive's descriptor; its game folder
 *  is the archive's sidecar folder, which then holds the pack.  The game reads the pack on GPUs that cannot sample
 *  the BC formats (android/render10/r10_pack.c); the original files stay as they are, and "Undo" removes only the
 *  pack. */
public final class TextureConverter implements TextureBackend {
    private static Boolean loaded;

    private static synchronized boolean load() {
        if (loaded == null) {
            try {
                System.loadLibrary("texconv");
                loaded = true;
            } catch (Throwable e) {
                android.util.Log.w("BioShock", "texture converter: " + e);
                loaded = false;
            }
        }
        return loaded;
    }

    private static native long[] nativeScan(String game, String cache, String index, int archiveFd);
    private static native int nativeConvert(String game, String cache, int format, int threads, String index, int archiveFd);
    private static native int nativeUndo(String game, String cache);
    private static native void nativeCancel();
    private static native long[] nativeProgress();
    private static native String nativeMessage();

    private static String path(File file) {
        return file == null ? null : file.getAbsolutePath();
    }

    @Override public Scan scan(File game) {
        return scan(game, game, null, -1);
    }

    @Override public Scan scan(File game, File index, int archiveFd) {
        return scan(game, game, index, archiveFd);
    }

    @Override public Scan scan(File game, File cache, File index, int archiveFd) {
        Scan out = new Scan();
        if (!load()) { out.failure = "The texture converter library is missing"; return out; }
        long[] values = nativeScan(path(game), path(cache), path(index), archiveFd);
        if (values == null || values.length != 6) { out.failure = nativeMessage(); return out; }
        out.textures = values[0];
        out.bytes = values[1];
        out.converted = (int) values[2];
        out.convertedTextures = values[3];
        out.convertedBytes = values[4];
        out.formats = new ArrayList<>();
        if ((values[5] & (1L << 3)) != 0) { out.formats.add("DXT1"); out.formats.add("DXT1_SRGB"); }
        if ((values[5] & (1L << 7)) != 0) { out.formats.add("DXT3"); out.formats.add("DXT3_SRGB"); }
        if ((values[5] & ((1L << 8) | (1L << 12))) != 0) out.formats.add("DXT5");
        if ((values[5] & (1L << 8)) != 0) out.formats.add("DXT5_SRGB");
        return out;
    }

    @Override public int convert(File game, int format, int threads) {
        return convert(game, game, format, threads, null, -1);
    }

    @Override public int convert(File game, int format, int threads, File index, int archiveFd) {
        return convert(game, game, format, threads, index, archiveFd);
    }

    @Override public int convert(File game, File cache, int format, int threads, File index, int archiveFd) {
        return load() ? nativeConvert(path(game), path(cache), format, threads, path(index), archiveFd) : -1;
    }

    @Override public int undo(File game) {
        return undo(game, game);
    }

    @Override public int undo(File game, File cache) {
        return load() ? nativeUndo(path(game), path(cache)) : -1;
    }

    @Override public void cancel() {
        if (load()) nativeCancel();
    }

    @Override public long[] progress() {
        return load() ? nativeProgress() : new long[4];
    }

    @Override public String message() {
        return load() ? nativeMessage() : "The texture converter library is missing";
    }
}
