package org.bioshock.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.hardware.display.DisplayManager;
import android.provider.Settings;
import android.view.Display;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Locale;
import java.util.TreeMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.bioshock.port.launcher.FpsOverlay;
import org.bioshock.port.launcher.Launch;

public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    static native void nativeSurface(Surface surface);
    static native int nativeStart(android.content.res.AssetManager assets, String userdata, String obbDirectory, String renderer, boolean fixedSeed, int renderWidth, int renderHeight);
    static native int nativeStatus();
    /** "start\n<reason>" or "lost\n<reason>" once the selected renderer failed (r10_failure.c), else null. */
    static native String nativeRendererFailure();
    static native long nativeFrameTimes(int[] intervalsUs);
    static native String nativeGpuStrings();
    static native void nativePause(boolean paused);
    static native void nativeKey(int vk, boolean down);
    static native boolean nativePointer(float x, float y, boolean relative);
    static native boolean nativeUiBegin(long context);
    static native int nativeUiControl(long context);
    static native boolean nativeUiScroll(int steps, long context);
    static native void nativeButton(boolean down);
    static native void nativePower(int plugged, int present, int level, int scale, int status);
    static native boolean nativeTouch(float moveX, float moveY, float lookX, float lookY, int heldMask, long context);
    static native boolean nativeTouchGyro(float moveX, float moveY, float lookX, float lookY, int heldMask, long context);
    static native boolean nativeTouchStick(float x, float y, long context);
    static native boolean nativeTouchWeapon(int phase, int slot, long context);
    static native long nativeTouchDetails();
    static native long nativeTouchIosChoices();
    static native void nativeTouchReset();
    static native void nativeTouchOptions(boolean enabled, int promptMode);
    static native void nativeTouchAim(boolean on);
    static native long nativeTouchContext();
    static native int nativeTouchVehicle();
    static native void nativeSettings(boolean controls);
    private static native boolean nativeCheat(int command);
    static native boolean nativeText(String text, long context);
    static native boolean nativeDelete(int count, long context);
    static native int nativeConfigure(float fps, int queryWaitUs, int precacheWaitUs, boolean benchmark);
    static native int nativeFrameRate(float fps);
    static native int nativePortOverlay(String path, boolean mobile);
    /** The physical gamepad's XINPUT_GAMEPAD (sticks up positive, triggers 0-255): the game's controller 0. */
    static native boolean nativeGamepad(boolean present, boolean playStation, int buttons, int leftTrigger, int rightTrigger,
                                        int leftX, int leftY, int rightX, int rightY);
    /** The game's rumble for controller 0: left motor speed in the high 16 bits, right in the low, 0-65535. */
    static native int nativeRumble();

    private static volatile GameActivity currentActivity;
    private static String preparedOverlay;

    private final Handler handler = new Handler();
    private final ExecutorService settingsIO = Executors.newSingleThreadExecutor();
    private TextView status;
    private GameControls controls;
    private GamepadInput gamepad;
    /** A gamepad that goes away mid-press sends no further edges; one that arrives is controller 0 at once. */
    private final android.hardware.input.InputManager.InputDeviceListener gamepads =
        new android.hardware.input.InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) { if (gamepad != null) gamepad.attach(id); inputDevicesChanged(); }
            @Override public void onInputDeviceChanged(int id) { if (gamepad != null) { gamepad.release(GamepadInput.attached()); gamepad.attach(id); } inputDevicesChanged(); }
            @Override public void onInputDeviceRemoved(int id) { if (gamepad != null) gamepad.release(GamepadInput.attached()); inputDevicesChanged(); }
        };
    private SurfaceView gameSurface;
    private DisplayManager displayManager;
    private PortPreferences portPreferences;
    private boolean settingsRefreshPending;
    private boolean surfaceReady;
    private boolean started;
    private String renderer;
    private int powerTicks;
    private int displayWidth;
    private int displayHeight;
    private String userdataProfile = "userdata";
    private Launch launch;
    private FrameLayout root;
    private FpsOverlay fpsOverlay;
    private AlertDialog inputDialog;
    private AlertDialog pauseDialog;
    private Runnable afterPauseDialog;
    private boolean activityResumed,externalInputAttached,inputPrompted;
    private int pauseShortcutKey;

    static String userdataName(String requested) {
        if (requested == null || requested.isEmpty()) return "userdata";
        if (requested.length() > 64 || ".".equals(requested) || "..".equals(requested) ||
            !requested.matches("[A-Za-z0-9][A-Za-z0-9._-]{0,63}")) return null;
        return requested;
    }

    private File gameFolder() {
        File folder = launch != null ? launch.folder("game") : null;
        return folder != null ? folder : new File("/sdcard/BioShock/game");
    }

    private File userdataFolder;

    private void refuse(String message) {
        status.setText(message);
        status.setVisibility(View.VISIBLE);
        if (launch != null) launch.failed(message);
    }

    PortPreferences portSettings() {
        return portPreferences;
    }

    void savePortSettings(Runnable complete) {
        settingsIO.execute(() -> {
            try {
                portPreferences.save();
                handler.post(() -> {
                    if (complete != null && !isFinishing()) complete.run();
                });
            } catch (IOException error) {
                handler.post(() -> Toast.makeText(this, "Не удалось сохранить настройки: " + error.getMessage(), Toast.LENGTH_LONG).show());
            }
        });
    }

    void openPortSettings() {
        openPortSettings(false);
    }

    private void openPortSettings(boolean touchOnly) {
        controls.release();
        if ((nativeTouchContext() & 3) == 2) pauseGame();
        nativeSettings(touchOnly);
    }

    private static void pauseGame() {
        nativeKey(27, true);
        nativeKey(27, false);
    }

    private boolean canOpenPauseChoices() {
        return started && portPreferences.getBoolean("cheats", false) && inputDialog == null &&
            pauseDialog == null && (nativeTouchContext() & 3) == 2;
    }

    private boolean openPauseChoices() {
        if (!canOpenPauseChoices()) return false;
        controls.release();
        showPauseDialog(new AlertDialog.Builder(this)
            .setTitle(text("Pause", "Пауза"))
            .setItems(new String[] {text("Control settings", "Настройки управления"),
                                    text("Cheats", "Меню читов"),
                                    text("Game menu", "Меню игры")}, (dialog, choice) -> {
                afterPauseDialog = () -> {
                    if (!activityResumed) return;
                    if (choice == 0) openPortSettings(true);
                    else if (choice == 1) showCheats();
                    else pauseGame();
                };
            }));
        return true;
    }

    private void showPauseDialog(AlertDialog.Builder builder) {
        nativePause(true);
        pauseDialog = builder.create();
        pauseDialog.setOnDismissListener(dialog -> {
            pauseDialog = null;
            Runnable next = afterPauseDialog;
            afterPauseDialog = null;
            if (started && activityResumed && surfaceReady && inputDialog == null) nativePause(false);
            immersive();
            if (next != null && activityResumed) handler.post(next);
        });
        pauseDialog.show();
    }

    private void showCheats() {
        if (!canOpenPauseChoices()) return;
        // Indexed by the native command table in core.c.
        String[] commands = {
            text("Toggle invulnerability", "Переключить неуязвимость"),
            text("Restore health", "Восстановить здоровье"),
            text("Restore EVE", "Восстановить Еву"),
            text("Add money", "Добавить деньги"),
            text("Give weapons and ammo", "Выдать оружие и патроны"),
            text("Give plasmids", "Выдать плазмиды"),
            text("Walk through walls", "Проходить сквозь стены"),
            text("Restore normal movement", "Вернуть обычное перемещение"),
        };
        showPauseDialog(new AlertDialog.Builder(this)
            .setTitle(text("Cheats", "Меню читов"))
            .setItems(commands, (dialog, choice) -> {
                if (!nativeCheat(choice))
                    Toast.makeText(this, text("Command not queued", "Команда не принята"), Toast.LENGTH_SHORT).show();
            })
            .setNegativeButton(text("Back", "Назад"), (dialog, which) -> afterPauseDialog = this::openPauseChoices));
    }

    private void copyOverlayAsset(String name, File target) throws IOException {
        File folder = target.getParentFile();
        if (!folder.isDirectory() && !folder.mkdirs()) throw new IOException("Cannot create native overlay directory");
        File temporary = new File(folder, target.getName() + ".tmp");
        try (InputStream input = getAssets().open(name);
             FileOutputStream output = new FileOutputStream(temporary)) {
            byte[] buffer = new byte[16384];
            for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
            output.getFD().sync();
        }
        Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    private String preparePortOverlay() throws IOException {
        if (preparedOverlay != null) return preparedOverlay;
        File root = new File(getFilesDir(), "native-overlay-v1");
        copyOverlayAsset("native-settings-pause.swf", new File(root, "Content/FlashMovies/pausePC.swf"));
        copyOverlayAsset("native-combolock.swf", new File(root, "Content/FlashMovies/ComboLockPC.swf"));
        copyOverlayAsset("native-plasminow.swf", new File(root, "Content/FlashMovies/PlasmiNowPC.swf"));
        copyOverlayAsset("native-loading.swf", new File(root, "Content/FlashMovies/ProgressBar.swf"));
        // Aspect-aware HUD layout: uniform scale, groups at the screen edges, interface scale.
        copyOverlayAsset("native-hud.swf", new File(root, "Content/FlashMovies/HUDPC.swf"));
        copyOverlayAsset("native-hud-radial.swf", new File(root, "Content/FlashMovies/HUDRadial.swf"));
        copyOverlayAsset("native-picker.swf", new File(root, "Content/FlashMovies/PCWeaponSelection.swf"));
        copyOverlayAsset("native-map.swf", new File(root, "Content/FlashMovies/mapsPC.swf"));
        /* The Direct3D 9 mobile shader overlay does not apply to the Direct3D 10 renderer: the game's own
           shaders.spk is used (its compiled shaders are the build-time library of the native renderer). */
        Files.deleteIfExists(new File(root, "Builds/Release/shaders.spk").toPath());
        preparedOverlay = root.getAbsolutePath();
        return preparedOverlay;
    }

    static String portSetting(String key, String value) {
        GameActivity activity = currentActivity;
        if (activity == null || activity.portPreferences == null || activity.settingsIO.isShutdown() ||
            key == null || key.length() > 64 || (value != null && value.length() > 128)) return null;
        try {
            return activity.portSettingValue(key.toLowerCase(Locale.ROOT), value);
        } catch (IllegalArgumentException | java.util.concurrent.RejectedExecutionException error) {
            return null;
        }
    }

    private String portSettingValue(String key, String value) {
        PortPreferences preferences = portPreferences;
        String result;
        float minimum = 0, maximum = 0, fallback = 0;
        switch (key) {
        case "touchacceleration":
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                preferences.put(key, "1".equals(value));
            }
            result = preferences.getBoolean(key, true) ? "1" : "0";
            break;
        case "cheats":
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                boolean enabled = "1".equals(value);
                preferences.put(key, enabled);
                if (enabled && !preferences.getBoolean("cheatsHintSeen", false)) {
                    preferences.put("cheatsHintSeen", true);
                    handler.post(() -> Toast.makeText(this,
                        text("Press Pause and select Cheats.", "Нажмите «Пауза» и выберите «Меню читов»."),
                        Toast.LENGTH_LONG).show());
                }
            }
            result = preferences.getBoolean(key, false) ? "1" : "0";
            break;
        case "fpschoices": {
            if (value != null) return null;
            StringBuilder choices = new StringBuilder("0");
            for (float rate : supportedFpsRates()) choices.append(',').append(Float.toString(rate));
            return choices.toString();
        }
        case "renderer":
            result = LauncherHooks.rendererMode(value != null ? value : preferences.getString(key, renderer != null ? renderer : "gles"));
            if (result == null) {
                if (value != null) return null;
                result = "gles";
            }
            if (value != null) preferences.put(key, result);
            break;
        case "fpslimit":
            if (value != null) {
                float selected = Float.parseFloat(value);
                boolean valid = selected == 0;
                for (float rate : supportedFpsRates()) valid |= Float.floatToIntBits(rate) == Float.floatToIntBits(selected);
                if (!Float.isFinite(selected) || !valid) return null;
                preferences.put(key, selected == 0 ? 0f : selected);
            }
            result = Float.toString(selectedFps());
            break;
        case "touchenabled":
        case "touchprompts":
        case "touchvitals":
        case "haptic":
            // The health gauge is a control unless the player turns it off. Vibration starts on.
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                preferences.put(key, "1".equals(value));
            }
            result = preferences.getBoolean(key, true) ? "1" : "0";
            break;
        case "touchuse":
        case "touchmore": {
            String displayKey = "touchDisplay" + key.substring("touch".length());
            int mode = preferences.getInt(displayKey, -1);
            boolean shown = TouchLayout.validDisplay(mode) ? (mode & TouchLayout.ON_SCREEN) != 0 :
                preferences.getBoolean(key, "touchuse".equals(key));
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                shown = "1".equals(value);
                preferences.put(key, shown);
                if (TouchLayout.validDisplay(mode))
                    preferences.put(displayKey, (mode & ~TouchLayout.ON_SCREEN) | (shown ? TouchLayout.ON_SCREEN : 0));
            }
            result = shown ? "1" : "0";
            break;
        }
        case "mobileshaders":
        case "touchlefthanded":
        case "touchinverty":
        case "touchaimassist":
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                preferences.put(key, "1".equals(value));
            }
            result = preferences.getBoolean(key, false) ? "1" : "0";
            break;
        case "hdr":
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                preferences.put(key, "1".equals(value));
            }
            result = preferences.getBoolean(key, true) ? "1" : "0";
            break;
        case "touchgyro":
            if (value != null) {
                int mode = Integer.parseInt(value);
                if (mode < 0 || mode > 4) return null;
                preferences.put(key, mode);
            }
            result = Integer.toString(Math.max(0, Math.min(4, preferences.getInt(key, 0))));
            break;
        case "renderresolutions":
            return value == null ? renderResolutions(displayWidth, displayHeight) : null;
        case "renderscale": {
            int scale = value != null ? Integer.parseInt(value) : preferences.getInt(key, 100);
            if (scale < 50 || scale > 100 || scale % 10 != 0) { if (value != null) return null; scale = 100; }
            if (displayWidth > 0 && displayHeight > 0) {
                int divisor = renderDivisor(displayWidth, displayHeight);
                int units = renderUnits(divisor, scale);
                while (scale < 100 && renderUnits(divisor, scale + 10) == units) scale += 10;
            }
            if (value != null) preferences.put(key, scale);
            result = Integer.toString(scale);
            break;
        }
        case "upscaler": {
            int mode = value != null ? Integer.parseInt(value) : preferences.getInt(key, 0);
            if (value == null && mode == 2) mode = 1;
            if (mode < 0 || mode > 1) { if (value != null) return null; mode = 0; }
            if (value != null) preferences.put(key, mode);
            result = Integer.toString(mode);
            break;
        }
        case "playerfov":
        case "weaponfov": {
            boolean weapon = "weaponfov".equals(key);
            int fallbackFov = weapon ? 60 : 75;
            int fov = value != null ? Integer.parseInt(value) : preferences.getInt(key, fallbackFov);
            if (fov < (weapon ? 40 : 55) || fov > (weapon ? 100 : 120)) {
                if (value != null) return null;
                fov = fallbackFov;
            }
            if (value != null) preferences.put(key, fov);
            result = Integer.toString(fov);
            break;
        }
        /* Game tweaks and fixes (android/host/bioshock_tweaks.c). The fixes are on by default; the
           tweaks keep the original game until the player chooses them. */
        case "fixscrounger":
        case "cutscenefps":
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                preferences.put(key, "1".equals(value));
            }
            result = preferences.getBoolean(key, true) ? "1" : "0";
            break;
        case "mouseraw":
            if (value != null) {
                if (!"0".equals(value) && !"1".equals(value)) return null;
                preferences.put(key, "1".equals(value));
            }
            result = preferences.getBoolean(key, false) ? "1" : "0";
            break;
        case "physicsrate": {
            int rate = value != null ? Integer.parseInt(value) : preferences.getInt(key, 30);
            if (rate != 30 && rate != 60) { if (value != null) return null; rate = 30; }
            if (value != null) preferences.put(key, rate);
            result = Integer.toString(rate);
            break;
        }
        case "anisotropy": {
            int level = value != null ? Integer.parseInt(value) : preferences.getInt(key, 0);
            if (level != 0 && level != 4 && level != 8 && level != 16) { if (value != null) return null; level = 0; }
            if (value != null) preferences.put(key, level);
            result = Integer.toString(level);
            break;
        }
        case "touchscheme": {
            boolean enabled = preferences.getBoolean("touchEnabled", true);
            int scheme = value != null ? Integer.parseInt(value) : enabled ? preferences.getInt(key, 1) : 0;
            if (scheme < 0 || scheme > 3) {
                if (value != null) return null;
                scheme = enabled ? 1 : 0;
            }
            if (value == null && enabled && scheme == 0) scheme = 1;
            if (value != null) {
                if (scheme != 0) preferences.put(key, scheme);
                preferences.put("touchEnabled", scheme != 0);
            }
            result = Integer.toString(scheme);
            break;
        }
        case "touchsensitivity": minimum = .04f; maximum = 3; fallback = .22f; result = null; break;
        case "iosuiscale": minimum = .5f; maximum = 2; fallback = 1; result = null; break;
        case "hapticstrength": minimum = 0; maximum = 1; fallback = 1; result = null; break;
        case "touchscale": minimum = .65f; maximum = 1.6f; fallback = 1; result = null; break;
        case "touchopacity": minimum = .2f; maximum = 1; fallback = .65f; result = null; break;
        case "touchgyrosensitivity": minimum = .1f; maximum = 5; fallback = 1; result = null; break;
        case "touchaltsensitivity": minimum = .1f; maximum = 3; fallback = 1; result = null; break;
        case "uiscale":
            minimum = 1; maximum = 1.5f; result = null;
            fallback = TouchLayout.defaultUiScale(getResources().getConfiguration().smallestScreenWidthDp);
            break;
        case "hudclear": {
            // Read-only: where the HUD's top-left group starts so Pause keeps its corner.
            if (value != null) return null;
            GameControls view = controls;
            return Float.toString(view != null ? view.hudControlClearStage() : 0f);
        }
        case "resetlayout":
            if (!"1".equals(value)) return null;
            for (String name : TouchLayout.NAMES) {
                preferences.put("touchX" + name, "");
                preferences.put("touchY" + name, "");
                preferences.put("touchSize" + name, "");
                preferences.put("touchDisplay" + name, "");
            }
            result = "1";
            break;
        case "editlayout":
            if (!"1".equals(value)) return null;
            return handler.post(() -> {
                if (currentActivity == this && controls != null) controls.requestLayoutEditor();
            }) ? "1" : null;
        default: return null;
        }
        if (result == null) {
            if (value != null) {
                float number = Float.parseFloat(value);
                if (!Float.isFinite(number) || number < minimum || number > maximum) return null;
                preferences.put(key, number);
            }
            result = Float.toString(Math.max(minimum, Math.min(maximum, preferences.getFloat(key, fallback))));
        }
        if (value != null) {
            savePortSettings(null);
            handler.post(() -> {
                if (currentActivity != this) return;
                if ("fpslimit".equals(key)) applyFrameRate();
                if (controls != null) controls.requestSettingsReload();
            });
        }
        return result;
    }

    private Display gameDisplay() {
        return Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
    }

    private static int renderDivisor(int width, int height) {
        while (height != 0) {
            int remainder = width % height;
            width = height;
            height = remainder;
        }
        return width;
    }

    // Keep whole multiples of the display's reduced aspect ratio. The scene uses the same rule.
    private static int renderUnits(int divisor, int percent) {
        return (divisor * percent + 50) / 100;
    }

    static String renderResolutions(int width, int height) {
        if (width <= 0 || height <= 0) return "";
        int divisor = renderDivisor(width, height);
        StringBuilder result = new StringBuilder();
        for (int scale = 50; scale <= 100; scale += 10) {
            int units = renderUnits(divisor, scale);
            if (scale < 100 && renderUnits(divisor, scale + 10) == units) continue;
            if (result.length() > 0) result.append(',');
            int tenths = (1000 * units + divisor / 2) / divisor;
            result.append(scale).append('=').append(width / divisor * units)
                .append(" x ").append(height / divisor * units).append(" (").append(tenths / 10);
            if (tenths % 10 != 0) result.append('.').append(tenths % 10);
            result.append("%)");
        }
        return result.toString();
    }

    private static Display.Mode physicalMode(Display display, Display.Mode[] modes) {
        Display.Mode active = display.getMode();
        // Some firmware reports the scaled size for the active mode and physical sizes in the mode list.
        for (Display.Mode mode : modes)
            if (mode.getModeId() == active.getModeId()) return mode;
        return active;
    }

    private static boolean compatibleMode(Display.Mode mode, Display.Mode current) {
        return mode.getPhysicalWidth() == current.getPhysicalWidth() &&
            mode.getPhysicalHeight() == current.getPhysicalHeight() &&
            Float.isFinite(mode.getRefreshRate()) && mode.getRefreshRate() >= 1f;
    }

    private float[] supportedFpsRates() {
        Display display = gameDisplay();
        if (display == null) return new float[0];
        Display.Mode[] modes = display.getSupportedModes();
        Display.Mode current = physicalMode(display, modes);
        ArrayList<Float> refreshRates = new ArrayList<>();
        for (Display.Mode mode : modes)
            if (compatibleMode(mode, current)) refreshRates.add(mode.getRefreshRate());
        refreshRates.sort(Collections.reverseOrder());
        // Key by the menu label (nearest half FPS), keeping the exact rate for pacing.
        // The lowest refresh with an even cadence wins. An exact-rate mode supersedes a doubled one.
        TreeMap<Integer, Float> caps = new TreeMap<>();
        for (float rate : refreshRates)
            if (rate >= 2f) caps.put(Math.round(rate), rate * .5f);
        for (float rate : refreshRates) caps.put(Math.round(rate * 2f), rate);
        float[] rates = new float[caps.size()];
        int i = 0;
        for (float cap : caps.values()) rates[i++] = cap;
        return rates;
    }

    private float selectedFps() {
        return resolveFps(supportedFpsRates(), portPreferences.getFloat("fpsLimit", Float.NaN));
    }

    /** The cap a player gets without a choice of their own (wiki note 13, D8). */
    static final float DEFAULT_FPS = 60f;
    /** Panels report fractional rates (60.000004, 59.94006): a saved rate matches within this many Hz. */
    static final float RATE_TOLERANCE = 0.5f;

    /** The FPS cap for a saved value on the current panel modes: 0 follows the display; no saved value means
     *  60; a saved rate matches a reported mode within RATE_TOLERANCE (another firmware or a profile copied from
     *  another phone reports the same rate with other fractional digits); a rate the panel does not have gives
     *  the highest mode below it, else the lowest mode - never silently the maximum, which raised the cap of a
     *  player who chose 60 on a 120 Hz panel. Rates are sorted ascending (supportedFpsRates). */
    static float resolveFps(float[] rates, float saved) {
        if (rates == null || rates.length == 0) return 0;
        if (saved == 0) return 0;
        float wanted = Float.isFinite(saved) && saved > 0 ? saved : DEFAULT_FPS;
        float nearest = rates[0], below = 0;
        for (float rate : rates) {
            if (Math.abs(rate - wanted) < Math.abs(nearest - wanted)) nearest = rate;
            if (rate <= wanted + RATE_TOLERANCE) below = rate;
        }
        if (Math.abs(nearest - wanted) <= RATE_TOLERANCE) return nearest;
        return below > 0 ? below : rates[0];
    }

    private float fpsLimit() {
        Display display = gameDisplay();
        if (display == null || supportedFpsRates().length == 0) return 0;
        float active = display.getRefreshRate();
        if (!Float.isFinite(active) || active <= 0) return 0;
        float selected = selectedFps();
        return selected == 0 ? active : Math.min(selected, active);
    }

    private static Display.Mode displayModeForFps(Display display, float cap) {
        Display.Mode[] modes = display.getSupportedModes();
        Display.Mode current = physicalMode(display, modes);
        Display.Mode chosen = null;
        for (Display.Mode mode : modes) {
            float rate = mode.getRefreshRate();
            if (!compatibleMode(mode, current) || (cap != 0 && rate != cap && rate * .5f != cap)) continue;
            if (chosen == null) { chosen = mode; continue; }
            float previous = chosen.getRefreshRate();
            if ((cap == 0 ? rate > previous : rate < previous) ||
                (rate == previous && mode.getModeId() == current.getModeId())) chosen = mode;
        }
        return chosen != null ? chosen : current;
    }

    private void applyFrameRate() {
        Display display = gameDisplay();
        float target = fpsLimit();
        if (display == null || target < 1) return;
        Display.Mode mode = displayModeForFps(display, selectedFps());
        float requested = mode.getRefreshRate();
        WindowManager.LayoutParams options = getWindow().getAttributes();
        options.preferredDisplayModeId = mode.getModeId();
        options.preferredRefreshRate = requested;
        getWindow().setAttributes(options);
        // The surface votes for the requested panel mode, not the mode still active
        // before the switch: a 60 Hz vote kept the 120 Hz panel at 60, so a frame that
        // missed a refresh waited 16.7 ms instead of 8.3 ms. The limiter follows the panel.
        if (Build.VERSION.SDK_INT >= 30 && surfaceReady && gameSurface != null)
            gameSurface.getHolder().getSurface().setFrameRate(requested, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        nativeFrameRate(target);
        savePortSettings(null);
    }

    private void refreshPortSettings() {
        if (settingsRefreshPending) return;
        settingsRefreshPending = true;
        settingsIO.execute(() -> {
            try {
                boolean changed = portPreferences.reloadIfChanged();
                int revision = portPreferences.getInt("layoutRevision", 0);
                boolean edit = revision != portPreferences.getInt("lastLayoutRevision", 0);
                if (edit) {
                    portPreferences.put("lastLayoutRevision", revision);
                    portPreferences.save();
                }
                handler.post(() -> {
                    settingsRefreshPending = false;
                    if (!isFinishing() && controls != null) {
                        if (changed) { applyFrameRate(); controls.requestSettingsReload(); }
                        if (edit) controls.requestLayoutEditor();
                    }
                });
            } catch (IOException error) {
                handler.post(() -> {
                    settingsRefreshPending = false;
                    status.setText("Не удалось прочитать настройки управления: " + error.getMessage());
                });
            }
        });
    }

    private final Runnable progress = new Runnable() {
        @Override public void run() {
            if (!started) return;
            /* The selected renderer did not start or lost its GPU device (android/render10/r10_failure.c, the
               launcher's renderer failure policy): the game stops, the launcher offers the next renderer. */
            String failure = nativeRendererFailure();
            if (failure != null) {
                int split = failure.indexOf('\n');
                String reason = split >= 0 ? failure.substring(split + 1) : failure;
                android.util.Log.e("BioShock", "renderer failure: " + failure.replace('\n', ' '));
                launch.rendererFailed(reason, failure.startsWith("lost"));
                return;
            }
            refreshPortSettings();
            if (powerTicks++ % 60 == 0) updatePowerSnapshot();
            int result = nativeStatus();
            if (result == Integer.MAX_VALUE) {
                status.setVisibility(View.GONE);
                if (launch != null) launch.firstFrame(root);
                offerTouchVisibility();
            } else if (result != Integer.MIN_VALUE) {
                String message = "Игра остановилась. Код " + result + ".";
                if (crashMonitoring()) message += " Журнал сохранён в " + userdataProfile + "/bioshock.log.";
                status.setText(message);
                status.setVisibility(View.VISIBLE);
                if (launch != null) launch.exited(result);
                return;
            }
            handler.postDelayed(this, 500);
        }
    };

    private String text(String english, String russian) {
        return "RUS".equals(portPreferences.getString("language", "RUS")) ? russian : english;
    }

    private void inputDevicesChanged() {
        boolean external = GamepadInput.externalInputAttached();
        if (external != externalInputAttached) {
            externalInputAttached = external;
            inputPrompted = false;
            if (inputDialog != null) inputDialog.dismiss();
        }
        offerTouchVisibility();
    }

    private void offerTouchVisibility() {
        if (!started || portPreferences == null) return;
        boolean external = externalInputAttached;
        if (!shouldOfferTouch(external, portPreferences.getBoolean("touchEnabled", true))) {
            inputPrompted = false;
            return;
        }
        if (inputPrompted || !activityResumed || !hasWindowFocus() || inputDialog != null || pauseDialog != null ||
            controls == null || !controls.canOfferInputPrompt() || nativeStatus() != Integer.MAX_VALUE) return;
        inputPrompted = true;
        controls.release();
        nativePause(true);
        inputDialog = new AlertDialog.Builder(this)
            .setTitle(text("On-screen controls", "Наэкранное управление"))
            .setMessage(external ? text("A controller or keyboard and mouse are connected. Turn off on-screen controls?",
                "Подключён геймпад или клавиатура с мышью. Отключить наэкранное управление?") :
                text("No controller or keyboard and mouse detected. Turn on on-screen controls?",
                "Геймпад и комплект из клавиатуры с мышью не обнаружены. Включить наэкранное управление?"))
            .setNegativeButton(text("Keep current settings", "Оставить как есть"), null)
            .setPositiveButton(external ? text("Turn off", "Отключить") : text("Turn on", "Включить"), (dialog, which) -> {
                portPreferences.put("touchEnabled", !external);
                savePortSettings(null);
                controls.reloadSettings();
            }).create();
        inputDialog.setOnDismissListener(dialog -> {
            inputDialog = null;
            if (started && activityResumed && surfaceReady && pauseDialog == null) nativePause(false);
            immersive();
        });
        inputDialog.show();
    }

    static boolean shouldOfferTouch(boolean external, boolean touchEnabled) {
        return external == touchEnabled;
    }

    private void updatePowerSnapshot() {
        Intent battery = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery == null) {
            nativePower(-1, -1, -1, -1, 1);
            return;
        }
        nativePower(battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1),
            battery.hasExtra(BatteryManager.EXTRA_PRESENT) && battery.getBooleanExtra(BatteryManager.EXTRA_PRESENT, false) ? 1 : -1,
            battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1),
            battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1),
            battery.getIntExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN));
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        if (Build.VERSION.SDK_INT >= 30) {
            android.view.WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(android.view.WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) immersive();
        else if (controls != null) controls.release();
        if (gamepad != null) gamepad.focus(focused);
        if (focused) offerTouchVisibility();
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        launch = Launch.attach(this);
        System.loadLibrary("bioshock");
        setVolumeControlStream(android.media.AudioManager.STREAM_MUSIC);
        try {
            File external = getExternalFilesDir(null);
            if (external == null) throw new IOException("App storage is unavailable");
            LauncherHooks.ensureLayout(this, gameFolder());
            userdataFolder = LauncherHooks.profile(this, gameFolder()).getParentFile();
            if (!getSharedPreferences("bioshock_paths", MODE_PRIVATE).edit()
                    .putString("gameFolder", userdataFolder.getAbsolutePath()).commit())
                throw new IOException("Cannot remember the profile location");
            userdataProfile = userdataName(getIntent().getStringExtra("userdata"));
            if (userdataProfile == null) throw new IOException("Invalid QA profile name");
            PortCrypto.init(external);
            portPreferences = new PortPreferences(new File(userdataFolder, userdataProfile));
            portPreferences.put("language", "rus".equals(Launch.option(this, "game_language")) ? "RUS" : "INT");
        } catch (IOException error) {
            if (launch != null) launch.failed(error.getMessage());
            new AlertDialog.Builder(this).setTitle("Хранилище BioShock").setMessage(error.getMessage())
                .setPositiveButton("Закрыть", (dialog, which) -> finish()).setCancelable(false).show();
            return;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        WindowManager.LayoutParams windowOptions = getWindow().getAttributes();
        if (Build.VERSION.SDK_INT >= 28)
            windowOptions.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(windowOptions);
        immersive();
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::back);
        root = new FrameLayout(this);
        root.setBackgroundColor(android.graphics.Color.BLACK);
        if (Build.VERSION.SDK_INT >= 29) root.setForceDarkAllowed(false);
        gameSurface = new SurfaceView(this);
        gameSurface.getHolder().addCallback(this);
        root.addView(gameSurface);
        controls = new GameControls(this);
        controls.setSettingsAction(this::openPortSettings);
        controls.setPauseAction(this::openPauseChoices);
        root.addView(controls);
        gamepad = new GamepadInput(handler);
        status = new TextView(this);
        status.setTextColor(0xFFFFFFFF);
        status.setBackgroundColor(0xC0000000);
        status.setTextSize(18);
        status.setPadding(24, 16, 24, 16);
        status.setGravity(Gravity.CENTER);
        root.addView(status, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        status.setOnClickListener(view -> {
            if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            }
        });
        setContentView(root);
        currentActivity = this;
        /* The native Direct3D 10 renderer: gles, vulkan, vulkan-turnip or vulkan-panvk (a DXVK mode of an
           older build becomes the native Vulkan mode on the same driver). */
        renderer = LauncherHooks.rendererMode(getIntent().getStringExtra("renderer"));
        if (renderer == null) renderer = LauncherHooks.rendererMode(portPreferences.getString("renderer", "gles"));
        if (renderer == null) renderer = "gles";
        launch.recordRenderer(renderer);
        displayManager = (DisplayManager)getSystemService(DISPLAY_SERVICE);
        if (displayManager != null) displayManager.registerDisplayListener(displayListener, handler);
        applyFrameRate();
        startIfReady();
    }

    private boolean hasEmbeddedAsset(String name) {
        try {
            getAssets().openFd(name).close();
            return true;
        } catch (IOException error) {
            return false;
        }
    }

    private static boolean crashMonitoring() {
        return !"0".equals(android.system.Os.getenv("PORT_OPTION_CRASH_MONITORING"));
    }

    /** A stored ZIP the launcher plays in place (Launch sets PORT_MOUNT_GAME, its index, and PORT_MOUNT_FD_GAME, the
     *  archive's descriptor). The game folder is then the archive's sidecar, which holds only the archive's few
     *  compressed files: Bioshock.exe stays in the archive, and the native file layer (files.c, pe_images.c) reads it
     *  there and checks its size and CRC. */
    private static boolean archiveMounted() {
        String index = android.system.Os.getenv("PORT_MOUNT_GAME");
        String descriptor = android.system.Os.getenv("PORT_MOUNT_FD_GAME");
        return index != null && !index.isEmpty() && new File(index).isFile() && descriptor != null && !descriptor.isEmpty();
    }

    private void startIfReady() {
        if (started || !surfaceReady || renderer == null || displayWidth <= 0 || displayHeight <= 0 || displayWidth < displayHeight) return;
        if (fpsLimit() < 1) { refuse("Ожидание поддерживаемой частоты дисплея…"); return; }
        File obbDirectory = getObbDir();
        boolean packedData = obbDirectory != null &&
            new File(obbDirectory, "main.1.org.bioshock.port.obb").isFile() &&
            new File(obbDirectory, "patch.1.org.bioshock.port.obb").isFile();
        if (!packedData) packedData = hasEmbeddedAsset("main.1.org.bioshock.port.obb") &&
            hasEmbeddedAsset("patch.1.org.bioshock.port.obb");
        File game = gameFolder();
        if (!packedData && !new File(game, "Builds/Release/Bioshock.exe").isFile() && !archiveMounted()) {
            refuse("Не найдены данные игры: выберите папку или ZIP BioShock в лаунчере.");
            return;
        }
        File userdata = new File(userdataFolder, userdataProfile);
        if (!userdata.isDirectory() && !userdata.mkdirs()) {
            refuse("Не удалось создать userdata в папке игры.");
            return;
        }
        String obbPath = obbDirectory != null ? obbDirectory.getAbsolutePath() : "";
        if (launch != null) launch.starting();
        int error;
        try {
            String language = GameLanguage.text(Launch.option(this, "game_language"));
            String voice = GameLanguage.voice(language, Launch.option(this, "voice_language"));
            java.util.List<String> missing = GameLanguage.missing(game, language, voice);
            if (!missing.isEmpty()) throw new IOException("Не найдены файлы языка: " + missing.get(0));
            NativeProfile.initialize(userdata, displayWidth, displayHeight,
                () -> getAssets().open("native-profile-default.ini"), language);
            NativeProfile.seedMobileShaders(userdata, false, null);
            SessionLog.begin(this, userdata, crashMonitoring());
            error = nativePortOverlay(preparePortOverlay(), false);
        } catch (IOException failure) {
            refuse("Не удалось подготовить настройки игры: " + failure.getMessage());
            return;
        }
        if (error == 0) error = nativeConfigure(fpsLimit(), 250, 50, false);
        if (error == 0) error = nativeStart(getAssets(), userdata.getAbsolutePath(), obbPath, renderer, false, displayWidth, displayHeight);
        if (error != 0) {
            refuse("Не удалось запустить игровой поток: " + error);
            return;
        }
        started = true;
        fpsOverlay = FpsOverlay.create(this, Launch.option(this, "fps_counter"),
            GameActivity::nativeFrameTimes, () -> renderer, this::fpsLimit, GameActivity::nativeGpuStrings);
        if (fpsOverlay != null) {
            root.addView(fpsOverlay, 1, new FrameLayout.LayoutParams(-1, -1));
            fpsOverlay.start();
        }
        status.setClickable(false);
        status.setText("Загрузка BioShock — " + renderer + "…");
        handler.post(progress);
    }

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int displayId) { }
        @Override public void onDisplayRemoved(int displayId) { }
        @Override public void onDisplayChanged(int displayId) {
            Display display = gameDisplay();
            if (display != null && displayId == display.getDisplayId()) handler.post(GameActivity.this::applyFrameRate);
        }
    };

    @Override public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
        applyFrameRate();
        nativeSurface(holder.getSurface());
        startIfReady();
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        displayWidth = width;
        displayHeight = height;
        applyFrameRate();
        nativeSurface(holder.getSurface());
        startIfReady();
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        controls.release();
        if (gamepad != null) gamepad.release(GamepadInput.attached());
        nativeSurface(null);
    }

    @Override protected void onResume() {
        super.onResume();
        activityResumed = true;
        immersive();
        if (portPreferences != null) {
            ((android.hardware.input.InputManager)getSystemService(INPUT_SERVICE)).registerInputDeviceListener(gamepads, handler);
            if (gamepad != null) for (int id : android.view.InputDevice.getDeviceIds()) gamepad.attach(id);
            applyFrameRate();
            updatePowerSnapshot();
            nativePause(inputDialog != null || pauseDialog != null);
            startIfReady();
            inputDevicesChanged();
        }
        if (fpsOverlay != null) fpsOverlay.start();
    }

    @Override protected void onPause() {
        activityResumed = false;
        pauseShortcutKey = 0;
        if (inputDialog != null) inputDialog.dismiss();
        if (pauseDialog != null) pauseDialog.dismiss();
        if (portPreferences != null)
            ((android.hardware.input.InputManager)getSystemService(INPUT_SERVICE)).unregisterInputDeviceListener(gamepads);
        if (controls != null) controls.release();
        if (gamepad != null) gamepad.release(GamepadInput.attached());
        if (fpsOverlay != null) fpsOverlay.stop();
        nativePause(true);
        super.onPause();
    }

    @Override protected void onDestroy() {
        if (currentActivity == this) currentActivity = null;
        if (displayManager != null) displayManager.unregisterDisplayListener(displayListener);
        handler.removeCallbacks(progress);
        if (fpsOverlay != null) fpsOverlay.stop();
        settingsIO.shutdown();
        super.onDestroy();
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == 1 && results.length > 0 && results[0] == 0) startIfReady();
    }

    @Override public void onBackPressed() { back(); }

    private void back() {
        if (!started) {
            if (launch != null) {
                Launch.openLauncher(this, launch.distribution());
                finish();
            } else {
                super.onBackPressed();
            }
            return;
        }
        if (controls != null && controls.handleBack()) return;
        if (!openPauseChoices()) pauseGame();
    }

    private long backDownTime = -1;
    private boolean backConsumed;

    @Override public boolean dispatchGenericMotionEvent(android.view.MotionEvent event) {
        if (started && gamepad != null && gamepad.motion(event)) return true;
        return super.dispatchGenericMotionEvent(event);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int key = event.getKeyCode();
        if (key == KeyEvent.KEYCODE_ESCAPE || key == KeyEvent.KEYCODE_BUTTON_START) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && (pauseShortcutKey == key ||
                (canOpenPauseChoices() && controls.canOfferInputPrompt()))) {
                pauseShortcutKey = key;
                return true;
            }
            if (event.getAction() == KeyEvent.ACTION_UP && pauseShortcutKey == key) {
                pauseShortcutKey = 0;
                if (!event.isCanceled() && !openPauseChoices()) pauseGame();
                return true;
            }
        }
        if (started && gamepad != null && gamepad.key(event)) return true;
        if (key == KeyEvent.KEYCODE_ENTER) android.util.Log.i("BioShock", "[input] dispatch Enter action=" + event.getAction());
        int vk = 0;
        if (key == KeyEvent.KEYCODE_BACK) {
            if (!started) {
                if (event.getAction() == KeyEvent.ACTION_UP) back();
                return true;
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                if (backDownTime != event.getDownTime()) {
                    backDownTime = event.getDownTime();
                    backConsumed = controls != null && controls.handleBack();
                    if (!backConsumed && canOpenPauseChoices()) {
                        pauseShortcutKey = key;
                        backConsumed = true;
                    }
                }
                if (!backConsumed) nativeKey(27, true);
                return true;
            }
            if (event.getAction() == KeyEvent.ACTION_UP) {
                if (backDownTime == event.getDownTime()) {
                    if (pauseShortcutKey == key) {
                        pauseShortcutKey = 0;
                        if (!event.isCanceled() && !openPauseChoices()) pauseGame();
                    }
                    if (!backConsumed) nativeKey(27, false);
                    backDownTime = -1;
                    backConsumed = false;
                }
                return true;
            }
        }
        if (key >= KeyEvent.KEYCODE_A && key <= KeyEvent.KEYCODE_Z) vk = 'A' + key - KeyEvent.KEYCODE_A;
        else if (key >= KeyEvent.KEYCODE_0 && key <= KeyEvent.KEYCODE_9) vk = '0' + key - KeyEvent.KEYCODE_0;
        else if (key >= KeyEvent.KEYCODE_F1 && key <= KeyEvent.KEYCODE_F12) vk = 0x70 + key - KeyEvent.KEYCODE_F1;
        else if (key == KeyEvent.KEYCODE_SPACE) vk = 32;
        else if (key == KeyEvent.KEYCODE_ENTER) vk = 13;
        else if (key == KeyEvent.KEYCODE_GRAVE) vk = 0xC0;
        else if (key == KeyEvent.KEYCODE_TAB) vk = 9;
        else if (key == KeyEvent.KEYCODE_MINUS) vk = 0xBD;
        else if (key == KeyEvent.KEYCODE_PERIOD) vk = 0xBE;
        else if (key == KeyEvent.KEYCODE_DEL) vk = 8;
        else if (key == KeyEvent.KEYCODE_ESCAPE) vk = 27;
        else if (key == KeyEvent.KEYCODE_SHIFT_LEFT) vk = 0xA0;
        else if (key == KeyEvent.KEYCODE_SHIFT_RIGHT) vk = 0xA1;
        else if (key == KeyEvent.KEYCODE_CTRL_LEFT) vk = 0xA2;
        else if (key == KeyEvent.KEYCODE_CTRL_RIGHT) vk = 0xA3;
        else if (key == KeyEvent.KEYCODE_DPAD_LEFT) vk = 0x25;
        else if (key == KeyEvent.KEYCODE_DPAD_UP) vk = 0x26;
        else if (key == KeyEvent.KEYCODE_DPAD_RIGHT) vk = 0x27;
        else if (key == KeyEvent.KEYCODE_DPAD_DOWN) vk = 0x28;
        if (vk != 0 && (event.getAction() == KeyEvent.ACTION_DOWN || event.getAction() == KeyEvent.ACTION_UP)) {
            nativeKey(vk, event.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
}
