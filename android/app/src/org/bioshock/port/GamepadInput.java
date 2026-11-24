package org.bioshock.port;

import android.os.Build;
import android.os.CombinedVibration;
import android.os.Handler;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * A physical gamepad as the game's original XInput controller 0 (input.c pad_set). Android key and
 * joystick events become one XINPUT_GAMEPAD state; the original User.ini XENON_* bindings act in
 * gameplay. The PC Pause movie also receives native menu commands. Consuming the events keeps
 * Android's fallbacks away from the game: Generic.kcm turns an unused B into BACK, X into DEL, Y into
 * SPACE and A/Start into DPAD_CENTER, and ViewRootImpl turns an unused stick or hat into DPAD keys.
 */
final class GamepadInput {
    private static final String TAG = "BioShock";
    private static final int PAD_UP = 0x0001, PAD_DOWN = 0x0002, PAD_LEFT = 0x0004, PAD_RIGHT = 0x0008;
    /** The original force feedback sets motor speeds every tick; a one-shot outlasts the poll. */
    private static final long RUMBLE_POLL_MS = 40, RUMBLE_MS = 1000, RUMBLE_RENEW_MS = 800;
    private final Handler handler;
    private int keys, hat, device = -1, logged = -1, rumbleMotors, rumbleLogged;
    private float leftX, leftY, rightX, rightY, leftTrigger, rightTrigger, keyLeftTrigger, keyRightTrigger;
    private boolean present, playStation, focused = true, rumblePolling;
    private long rumbleSince;
    private final Runnable rumblePoll = new Runnable() {
        @Override public void run() {
            rumble(present && focused ? GameActivity.nativeRumble() : 0);
            if (present) handler.postDelayed(this, RUMBLE_POLL_MS);
            else rumblePolling = false;
        }
    };

    GamepadInput(Handler handler) { this.handler = handler; }

    /** A gamepad, or a joystick with X and Y axes: a fingerprint sensor's "joystick" has none. */
    static boolean isGamepad(InputDevice device) {
        if (device == null || device.isVirtual()) return false;
        int sources = device.getSources();
        if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) return true;
        return (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK &&
            device.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null &&
            device.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) != null;
    }

    static boolean attached() {
        for (int id : InputDevice.getDeviceIds()) if (isGamepad(InputDevice.getDevice(id))) return true;
        return false;
    }

    static boolean externalInputAttached() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) continue;
            String name = device.getName();
            name = name == null ? "" : name.toLowerCase(java.util.Locale.ROOT);
            if (name.contains("fingerprint") || name.startsWith("uinput-fpc") || name.startsWith("uinput-goodix")) continue;
            if (isGamepad(device) || device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC) return true;
        }
        return false;
    }

    private static boolean fromGamepad(InputEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK || isGamepad(event.getDevice());
    }

    /** The XINPUT_GAMEPAD wButtons bit of a gamepad key; L2 and R2 are the triggers. */
    private static int button(int key) {
        switch (key) {
        case KeyEvent.KEYCODE_DPAD_UP: return PAD_UP;
        case KeyEvent.KEYCODE_DPAD_DOWN: return PAD_DOWN;
        case KeyEvent.KEYCODE_DPAD_LEFT: return PAD_LEFT;
        case KeyEvent.KEYCODE_DPAD_RIGHT: return PAD_RIGHT;
        case KeyEvent.KEYCODE_BUTTON_START: return 0x0010;
        case KeyEvent.KEYCODE_BUTTON_SELECT: return 0x0020;
        case KeyEvent.KEYCODE_BUTTON_THUMBL: return 0x0040;
        case KeyEvent.KEYCODE_BUTTON_THUMBR: return 0x0080;
        case KeyEvent.KEYCODE_BUTTON_L1: return 0x0100;
        case KeyEvent.KEYCODE_BUTTON_R1: return 0x0200;
        case KeyEvent.KEYCODE_BUTTON_A: return 0x1000;
        case KeyEvent.KEYCODE_BUTTON_B: return 0x2000;
        case KeyEvent.KEYCODE_BUTTON_X: return 0x4000;
        case KeyEvent.KEYCODE_BUTTON_Y: return 0x8000;
        default: return 0;
        }
    }

    /** An axis as Android reports it, -1..1: the game's bindings apply their own dead zones. */
    private static float raw(MotionEvent event, int axis) {
        float value = event.getAxisValue(axis);
        return Float.isFinite(value) ? Math.max(-1, Math.min(1, value)) : 0;
    }

    private static int stick(float value) { return Math.round(Math.max(-1, Math.min(1, value)) * 32767); }

    private void note(InputEvent event) {
        device = event.getDeviceId();
        if (logged == device) return;
        logged = device;
        InputDevice info = event.getDevice();
        playStation = info != null && info.getVendorId() == 0x054c;
        Log.i(TAG, "[pad] input from device " + device + (info == null ? "" : " \"" + info.getName() +
            "\" vid=" + Integer.toHexString(info.getVendorId()) + " pid=" + Integer.toHexString(info.getProductId()) +
            " sources=0x" + Integer.toHexString(info.getSources()) + " controller=" + info.getControllerNumber()));
    }

    private void submit() {
        int left = Math.round(255 * Math.max(leftTrigger, keyLeftTrigger));
        int right = Math.round(255 * Math.max(rightTrigger, keyRightTrigger));
        // Android Y grows downwards; XInput's thumb Y is up positive.
        GameActivity.nativeGamepad(present, playStation, keys | hat, left, right,
            stick(leftX), stick(-leftY), stick(rightX), stick(-rightY));
        if (present && !rumblePolling) { rumblePolling = true; handler.postDelayed(rumblePoll, RUMBLE_POLL_MS); }
    }

    /** A gamepad button: true when used. BACK stays the activity's Escape; MODE, volume and media stay Android's. */
    boolean key(KeyEvent event) {
        int code = event.getKeyCode(), action = event.getAction();
        if (!fromGamepad(event) || code == KeyEvent.KEYCODE_BACK ||
            (action != KeyEvent.ACTION_DOWN && action != KeyEvent.ACTION_UP)) return false;
        int bit = button(code);
        boolean trigger = code == KeyEvent.KEYCODE_BUTTON_L2 || code == KeyEvent.KEYCODE_BUTTON_R2;
        if (bit == 0 && !trigger) return false;
        note(event);
        if (event.getRepeatCount() > 0) return true;
        boolean down = action == KeyEvent.ACTION_DOWN && (event.getFlags() & KeyEvent.FLAG_CANCELED) == 0;
        if (code == KeyEvent.KEYCODE_BUTTON_L2) keyLeftTrigger = down ? 1 : 0;
        else if (code == KeyEvent.KEYCODE_BUTTON_R2) keyRightTrigger = down ? 1 : 0;
        else keys = down ? keys | bit : keys & ~bit;
        present = true;
        submit();
        return true;
    }

    /** Sticks, analog triggers and a hat D-pad: true when used, so no DPAD key synthesis follows. */
    boolean motion(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK ||
            event.getActionMasked() != MotionEvent.ACTION_MOVE || !isGamepad(event.getDevice())) return false;
        note(event);
        leftX = raw(event, MotionEvent.AXIS_X); leftY = raw(event, MotionEvent.AXIS_Y);
        boolean standard = event.getDevice().getMotionRange(MotionEvent.AXIS_Z, InputDevice.SOURCE_JOYSTICK) != null &&
            event.getDevice().getMotionRange(MotionEvent.AXIS_RZ, InputDevice.SOURCE_JOYSTICK) != null;
        rightX = raw(event, standard ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX);
        rightY = raw(event, standard ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY);
        leftTrigger = Math.max(0, Math.max(raw(event, MotionEvent.AXIS_LTRIGGER), raw(event, MotionEvent.AXIS_BRAKE)));
        rightTrigger = Math.max(0, Math.max(raw(event, MotionEvent.AXIS_RTRIGGER), raw(event, MotionEvent.AXIS_GAS)));
        float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X), hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hat = (hatX < -.5f ? PAD_LEFT : hatX > .5f ? PAD_RIGHT : 0) | (hatY < -.5f ? PAD_UP : hatY > .5f ? PAD_DOWN : 0);
        present = true;
        submit();
        return true;
    }

    /** A gamepad attached before its first event: the game sees a connected, neutral controller 0. */
    void attach(int id) {
        InputDevice info = InputDevice.getDevice(id);
        if (present || !isGamepad(info)) return;
        Log.i(TAG, "[pad] attached device " + id + " \"" + info.getName() + "\"");
        device = id;
        playStation = info.getVendorId() == 0x054c;
        present = true;
        submit();
    }

    /** Focus, the surface or the device is gone: Android sends no further edges, so neutral. */
    void release(boolean stillAttached) {
        rumble(0);
        keys = hat = 0;
        leftX = leftY = rightX = rightY = leftTrigger = rightTrigger = keyLeftTrigger = keyRightTrigger = 0;
        if (present || stillAttached) {
            present = stillAttached;
            GameActivity.nativeGamepad(present, playStation, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    void focus(boolean value) {
        focused = value;
        if (!value) release(attached());
    }

    /** Left (strong, low-frequency) motor in the high 16 bits, right in the low 16, as XInput has them. */
    private void rumble(int motors) {
        long now = SystemClock.uptimeMillis();
        if (motors == rumbleMotors && (motors == 0 || now - rumbleSince < RUMBLE_RENEW_MS)) return;
        rumbleMotors = motors;
        rumbleSince = now;
        InputDevice info = device >= 0 ? InputDevice.getDevice(device) : null;
        int left = motors >>> 16, right = motors & 0xFFFF;
        if (motors != 0 && rumbleLogged < 3) {
            ++rumbleLogged;
            Log.i(TAG, "[pad] rumble " + left + "/" + right + " on " + (info == null ? "no device" : info.getName()));
        }
        if (info == null) return;
        try {
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager manager = info.getVibratorManager();
                int[] ids = manager.getVibratorIds();
                if (ids.length == 0) return;
                if (motors == 0) { manager.cancel(); return; }
                CombinedVibration.ParallelCombination all = CombinedVibration.startParallel();
                if (ids.length >= 2) {
                    if (left > 0) all.addVibrator(ids[0], shot(left));
                    if (right > 0) all.addVibrator(ids[1], shot(right));
                } else all.addVibrator(ids[0], shot(Math.max(left, right)));
                manager.vibrate(all.combine());
            } else {
                Vibrator pad = info.getVibrator();
                if (!pad.hasVibrator()) return;
                if (motors == 0) pad.cancel();
                else pad.vibrate(shot(Math.max(left, right)));
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "[pad] rumble: " + error);
        }
    }

    private static VibrationEffect shot(int speed) {
        return VibrationEffect.createOneShot(RUMBLE_MS, Math.max(1, Math.min(255, Math.round(speed * 255f / 65535f))));
    }
}
