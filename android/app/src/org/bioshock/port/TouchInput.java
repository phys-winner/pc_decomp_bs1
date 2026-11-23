package org.bioshock.port;

import android.os.Handler;
import android.os.Looper;

/** Pointer ownership and sampling, independent of Android and the render rate. */
final class TouchInput {
    static final int FIRE = 1, ALT = 2, JUMP = 4, DUCK = 8, USE = 16;
    static final int PREVIOUS = 32, NEXT = 64, PAUSE = 128, SCORES = 256, MAP = 512;
    static final int BOARD = 1024, HORN = 2048, BEST = 1 << 21;
    static final int WEAPON1 = 1 << 13, HERO = 1 << 23, FEIGN = 1 << 24, COMMAND = 1 << 12;
    /** BioShock HintButtonAlias (H) and LaunchPCWeaponSelection (Shift). */
    static final int TAUNT = 1 << 26, PICKER = 1 << 27;
    /** BioShock's native key map; legacy bit names above keep the JNI mask stable. */
    static final int RELOAD = HERO, HEAL = FEIGN, HACK = 1 << 25, RESCUE = HORN,
        HELP = COMMAND, LOG = 1 << 20, WALK = 1 << 22, AMMO_UP = BOARD, AMMO_DOWN = 1 << 19;
    /* F8/F9, Q/L/M, F1..F6, B/T/Z/Ctrl, R/F/V, H, Shift; two reserved bits each side. */

    /** What a contact does: drive the stick, turn the camera, or only hold its action. */
    static final int MOVEMENT = 1, LOOK = 2, BUTTON = 3, PASSIVE = 4;
    static final int LOOK_BUTTONS = FIRE | ALT | JUMP | DUCK;
    /**
     * Look acceleration: a drag slower than ACCEL_SLOW dp/ms turns linearly; the
     * gain then rises evenly to ACCEL_MAX at ACCEL_FAST, so a quick swipe across
     * half the screen turns around while slow aiming keeps its precision.
     */
    static final float ACCEL_SLOW = .5f, ACCEL_FAST = 2.5f, ACCEL_MAX = 2.5f;
    /** Speed smoothing time constant; touch samples arrive every 4-8 ms. */
    static final float ACCEL_SMOOTH_MS = 30;
    interface Sink {
        void submit(float moveX, float moveY, float lookX, float lookY, int held);
        /** Neutral auxiliary stick for BioShock; weapon selection uses the original UI. */
        boolean stick(float x, float y);
        void reset();
    }
    private final Sink sink;
    private final Handler releaseHandler = new Handler(Looper.getMainLooper());
    private final int[] ids = new int[10], actions = new int[10], roles = new int[10];
    private final float[] x = new float[10], y = new float[10], speed = new float[10], untimed = new float[10];
    private final long[] times = new long[10];
    private final boolean[] looks = new boolean[10];
    private int movement = -1;
    private int latched, pendingPulses, generation;
    private int crouchPointer = -1;
    private long crouchDownTime, lastCrouchTap = -1;
    private float crouchX, crouchY;
    private float anchorX, anchorY, moveX, moveY, stickX, stickY;
    private float radius = 60, gain = 2.2f, altGain = 1, density = 1;
    private boolean invertY, acceleration = true;

    TouchInput(Sink sink) { this.sink = sink; clear(); }

    void configure(float radiusPixels, float countsPerPixel, float alternativeGain, boolean invert) {
        radius = Math.max(1, finite(radiusPixels, 60));
        gain = Math.max(0.001f, finite(countsPerPixel, 2.2f));
        altGain = Math.max(0.1f, Math.min(3, finite(alternativeGain, 1)));
        invertY = invert;
    }
    void density(float value) { density = Math.max(.5f, Math.min(8, finite(value, 1))); }
    void acceleration(boolean enabled) { acceleration = enabled; }

    private static float finite(float value, float fallback) {
        return Float.isNaN(value) || Float.isInfinite(value) ? fallback : value;
    }
    /** Camera gain at a smoothed finger speed in dp/ms (1 when slow). */
    static float accelerationGain(float dpPerMs) {
        float t = (dpPerMs - ACCEL_SLOW) / (ACCEL_FAST - ACCEL_SLOW);
        return 1 + (ACCEL_MAX - 1) * (Float.isFinite(t) ? Math.max(0, Math.min(1, t)) : 0);
    }

    private int find(int id) {
        for (int i = 0; i < ids.length; ++i) if (ids[i] == id) return i;
        return -1;
    }

    /** Legacy form: a contact outside buttons drives the stick in its zone, else looks. */
    boolean down(int id, float px, float py, int action, boolean movementZone) {
        return down(id, px, py, action, action != 0 ? BUTTON : movementZone ? MOVEMENT : LOOK, -1);
    }

    /** time: MotionEvent time in ms for look acceleration, -1 when unknown. */
    boolean down(int id, float px, float py, int action, int role, long time) {
        if (id < 0 || find(id) >= 0 || !valid(px, py) || (action & 0xF0000000) != 0) return false;
        int slot = find(-1);
        if (slot < 0) return false;
        // The stick zone holds one stick; a second finger there turns the camera.
        if (role == MOVEMENT && movement >= 0) role = LOOK;
        ids[slot] = id; actions[slot] = action; roles[slot] = role; x[slot] = px; y[slot] = py;
        times[slot] = time; speed[slot] = untimed[slot] = 0;
        if (role == MOVEMENT) {
            movement = slot; anchorX = px; anchorY = py; moveX = moveY = 0;
        }
        if (action == DUCK && time >= 0) {
            // The game's first short C press already toggles crouch. Coalesce the second tap.
            if (lastCrouchTap >= 0 && time >= lastCrouchTap && time - lastCrouchTap <= 300) {
                actions[slot] = 0;
                lastCrouchTap = -1;
            } else if (crouchPointer < 0) {
                crouchPointer = id; crouchDownTime = time; crouchX = px; crouchY = py;
            } else {
                crouchPointer = -1;
                lastCrouchTap = -1;
            }
        }
        // Each aiming contact owns its delta, so a held trigger never blocks the other thumb.
        looks[slot] = role == LOOK || (role == BUTTON && Integer.bitCount(action) == 1 && (action & LOOK_BUTTONS) != 0);
        send(0, 0);
        return true;
    }

    void move(int id, float px, float py) { move(id, px, py, -1); }

    /** Call for every historical sample, then the current sample. */
    void move(int id, float px, float py, long time) {
        int slot = find(id);
        if (slot < 0 || !valid(px, py)) return;
        if (id == crouchPointer && Math.hypot(px - crouchX, py - crouchY) > 10 * density) {
            crouchPointer = -1;
            lastCrouchTap = -1;
        }
        float dx = 0, dy = 0;
        if (slot == movement) {
            float nx = (px - anchorX) / radius, ny = (anchorY - py) / radius;
            float length = (float)Math.hypot(nx, ny);
            float magnitude = Math.max(0, Math.min(1, (length - 0.12f) / 0.88f));
            moveX = length > 0 ? nx / length * magnitude : 0;
            moveY = length > 0 ? ny / length * magnitude : 0;
        }
        if (looks[slot]) {
            float multiplier = ((held() & ALT) != 0 ? altGain : 1) * lookGain(slot, px - x[slot], py - y[slot], time);
            dx = (px - x[slot]) * gain * multiplier;
            dy = (py - y[slot]) * gain * multiplier * (invertY ? -1 : 1);
        }
        x[slot] = px; y[slot] = py;
        send(dx, dy);
    }

    /** Acceleration from the smoothed finger speed; untimed samples stay linear. */
    private float lookGain(int slot, float dx, float dy, long time) {
        if (!acceleration || time < 0) return 1;
        if (times[slot] < 0 || time < times[slot]) { times[slot] = time; untimed[slot] = speed[slot] = 0; return 1; }
        // Samples sharing a millisecond are timed together with the next one.
        untimed[slot] += (float)Math.hypot(dx, dy) / density;
        long dt = time - times[slot];
        if (dt > 0) {
            speed[slot] += (untimed[slot] / dt - speed[slot]) * (float)(1 - Math.exp(-dt / ACCEL_SMOOTH_MS));
            times[slot] = time; untimed[slot] = 0;
        }
        return accelerationGain(speed[slot]);
    }

    void relativeLook(float dx, float dy) {
        if (!valid(dx, dy)) return;
        float multiplier = (held() & ALT) != 0 ? altGain : 1;
        send(dx * multiplier, dy * multiplier * (invertY ? -1 : 1));
    }

    void up(int id, float px, float py) { up(id, px, py, -1); }

    void up(int id, float px, float py, long time) {
        int slot = find(id);
        if (slot < 0) return;
        move(id, px, py, time);
        if (id == crouchPointer) {
            crouchPointer = -1;
            lastCrouchTap = valid(px, py) && time >= crouchDownTime && time - crouchDownTime <= 250 ? time : -1;
        }
        boolean picker = (actions[slot] & PICKER) != 0;
        ids[slot] = -1; actions[slot] = roles[slot] = 0;
        if (movement == slot) { movement = -1; moveX = moveY = 0; }
        looks[slot] = false;
        if (picker && (held() & PICKER) == 0) stickX = stickY = 0;
        send(0, 0);
    }

    /** Change what an owned contact holds, e.g. the weapon button entering the picker. */
    boolean hold(int id, int action) {
        int slot = find(id);
        if (slot < 0 || (action & 0xF0000000) != 0) return false;
        if (actions[slot] == action) return true;
        boolean picker = (actions[slot] & PICKER) != 0;
        actions[slot] = action;
        if (picker && (held() & PICKER) == 0) stickX = stickY = 0;
        send(0, 0);
        return true;
    }

    /** Picker direction while some contact holds PICKER; neutral otherwise. */
    boolean stick(float sx, float sy) {
        if (!valid(sx, sy) || (held() & PICKER) == 0) sx = sy = 0;
        float length = (float)Math.hypot(sx, sy);
        if (length > 1) { sx /= length; sy /= length; }
        if (sx == stickX && sy == stickY) return true;
        stickX = sx; stickY = sy;
        return sink.stick(sx, sy);
    }

    void pulse(int action) {
        int base = held();
        if ((base & action) != 0) return;
        sink.submit(moveX, moveY, 0, 0, base | action);
        send(0, 0);
    }

    void pulseDelayed(int action, long delayMs) {
        int base = held();
        if ((base & action) != 0) return;
        pendingPulses |= action;
        sink.submit(moveX, moveY, 0, 0, held());
        final int pulseGeneration = generation;
        releaseHandler.postDelayed(new Runnable() {
            @Override public void run() {
                if (pulseGeneration != generation) return;
                pendingPulses &= ~action;
                send(0, 0);
            }
        }, delayMs);
    }

    void toggle(int action) { latched ^= action; send(0, 0); }
    boolean latched(int action) { return (latched & action) != 0; }

    int held() {
        int mask = latched | pendingPulses;
        for (int i = 0; i < ids.length; ++i) if (ids[i] >= 0) mask |= actions[i];
        return mask;
    }
    boolean owns(int id) { return find(id) >= 0; }
    float anchorX() { return anchorX; }
    float anchorY() { return anchorY; }
    float moveX() { return moveX; }
    float moveY() { return moveY; }
    float stickX() { return stickX; }
    float stickY() { return stickY; }
    boolean moving() { return movement >= 0; }
    boolean looking() {
        for (boolean value : looks) if (value) return true;
        return false;
    }
    boolean hasContacts() {
        for (int id : ids) if (id >= 0) return true;
        return false;
    }
    private void send(float dx, float dy) { sink.submit(moveX, moveY, dx, dy, held()); }
    private static boolean valid(float x, float y) {
        return !Float.isNaN(x) && !Float.isNaN(y) && !Float.isInfinite(x) && !Float.isInfinite(y);
    }
    private void clear() {
        for (int i = 0; i < ids.length; ++i) { ids[i] = -1; actions[i] = roles[i] = 0; looks[i] = false; }
        movement = -1; moveX = moveY = stickX = stickY = 0; latched = pendingPulses = 0;
        crouchPointer = -1; lastCrouchTap = -1;
    }
    void cancel() { clear(); ++generation; sink.reset(); }
}
