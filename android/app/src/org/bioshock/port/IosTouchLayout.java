package org.bioshock.port;

/**
 * iPhone (568x320) and iPad (1024x768) controls from the iOS HUD.
 * Positions use a fitted canvas. Related controls scale together so their art
 * and hit regions stay aligned. No Android types, so a host test can place and hit them.
 */
final class IosTouchLayout {
    static final int PHONE = 0, PAD = 1;
    static final int DRAW = 0, MOVE = 1, HOLD = 3, PULSE = 4;
    /** GameControls pulses TouchInput.PAUSE; this is not a held bit. */
    static final int PAUSE = -1;
    static final int FIRE = 1, ALT = 2, DUCK = 8, NEXT = 64, MAP = 512, RESCUE = 2048;
    static final int RELOAD = 1 << 23, HACK = 1 << 25, TAUNT = 1 << 26;
    static final int GATE_HACK = 1 << 5, GATE_RESCUE = 1 << 6, GATE_HARVEST = 1 << 7;
    static final int CAPTION_NONE = 0, CAPTION_RESCUE = 1, CAPTION_HARVEST = 2;

    static final class Spec {
        final int role, action, gate, caption;
        final float x, y, w, h;
        final String image, pressedImage;

        Spec(int role, int action, int gate, int caption, float x, float y, float w, float h,
                String image, String pressedImage) {
            this.role = role; this.action = action; this.gate = gate; this.caption = caption;
            this.x = x; this.y = y; this.w = w; this.h = h;
            this.image = image; this.pressedImage = pressedImage;
        }

        String sprite(boolean pressed) {
            return pressed && pressedImage != null ? pressedImage : image;
        }
    }

    private static Spec c(int role, int action, int gate, int caption, float x, float y, float w, float h,
            String image, String pressedImage) {
        return new Spec(role, action, gate, caption, x, y, w, h, image, pressedImage);
    }

    private static final float PHONE_W = 568, PHONE_H = 320, PAD_W = 1024, PAD_H = 768;
    /* Weapon-pad children are already in canvas coordinates (parent origin added). */
    private static final Spec[] PHONE_SPECS = {
        c(MOVE, 0, 0, 0, 10, 216, 96, 96, "stick", null),
        c(HOLD, DUCK, 0, 0, 1, 132, 39, 39, "crouch", null),
        c(PULSE, MAP, 0, 0, 9, 0, 33, 33, "map", "map_down"),
        c(PULSE, PAUSE, 0, 0, 4, 30, 29, 29, "pause", "pause_down"),
        c(DRAW, 0, 0, 0, 425, 131, 98, 97, "pad", null),
        c(PULSE, NEXT, 0, 0, 431, 137, 35, 36, "next", "next_down"),
        c(PULSE, RELOAD, 0, 0, 479, 92, 35, 35, "reload", "reload_down"),
        c(HOLD, ALT, 0, 0, 477, 142, 35, 35, null, null),
        c(HOLD, FIRE, 0, 0, 437, 181, 35, 35, null, null),
        c(DRAW, 0, 0, 0, 527, 53, 31, 31, "diary", null),
        c(PULSE, RESCUE, GATE_RESCUE, CAPTION_RESCUE, 173, 224, 88, 17, "gather", "gather_down"),
        c(PULSE, TAUNT, GATE_HARVEST, CAPTION_HARVEST, 305, 224, 88, 17, "gather", "gather_down"),
        c(PULSE, HACK, GATE_HACK, 0, 382, 219, 33, 33, "hack", null)
    };
    private static final Spec[] TABLET_SPECS = {
        c(MOVE, 0, 0, 0, 52, 585, 160, 160, "stick", null),
        c(HOLD, DUCK, 0, 0, 0, 386, 64, 64, "crouch", null),
        c(PULSE, MAP, 0, 0, 22, 12, 44, 44, "map", "map_down"),
        c(PULSE, PAUSE, 0, 0, 11, 49, 38, 38, "pause", "pause_down"),
        c(DRAW, 0, 0, 0, 809, 368, 186, 184, "pad", null),
        c(PULSE, NEXT, 0, 0, 818, 376, 66, 68, "next", "next_down"),
        c(PULSE, RELOAD, 0, 0, 917, 296, 70, 70, "reload", "reload_down"),
        c(HOLD, ALT, 0, 0, 908, 388, 66, 66, null, null),
        c(HOLD, FIRE, 0, 0, 832, 464, 66, 66, null, null),
        c(DRAW, 0, 0, 0, 945, 206, 64, 64, "diary", null),
        c(PULSE, RESCUE, GATE_RESCUE, CAPTION_RESCUE, 336, 466, 146, 28, "gather", "gather_down"),
        c(PULSE, TAUNT, GATE_HARVEST, CAPTION_HARVEST, 551, 464, 146, 28, "gather", "gather_down"),
        c(PULSE, HACK, GATE_HACK, 0, 734, 552, 55, 53, "hack", null)
    };

    private static Spec[] table(int pad) { return pad == PAD ? TABLET_SPECS : PHONE_SPECS; }
    static int count(int pad) { return table(pad).length; }
    static Spec spec(int pad, int index) { return table(pad)[index]; }

    /** l,t,r,b for every control. */
    static float[] place(int pad, float left, float top, float right, float bottom, float uiScale,
            float leftTop, float leftBottom) {
        Spec[] specs = table(pad);
        float canvasW = pad == PAD ? PAD_W : PHONE_W, canvasH = pad == PAD ? PAD_H : PHONE_H;
        float safeW = Math.max(1f, right - left), safeH = Math.max(1f, bottom - top);
        float fit = Math.min(safeW / canvasW, safeH / canvasH);
        float scale = uiScale < 0.5f ? 0.5f : uiScale > 2f ? 2f : uiScale;
        float size = fit * scale;
        float slackX = safeW - canvasW * fit, slackY = safeH - canvasH * fit;
        float stageY = top + slackY * .5f;
        float[] rects = new float[specs.length * 4];
        leftTop = Math.max(top, Math.min(top + safeH * .4f, leftTop));
        leftBottom = Math.min(bottom, Math.max(top + safeH * .6f, leftBottom));
        float splitRatio = (stageY - top + (specs[3].y + specs[3].h + specs[1].y) * .5f * fit) / safeH;
        float leftSplit = leftTop + (leftBottom - leftTop) * splitRatio;
        placeGroup(specs, rects, 0, 2, left, leftSplit, right, leftBottom, canvasW, stageY, fit, size, false);
        placeGroup(specs, rects, 2, 4, left, leftTop, right, leftSplit, canvasW, stageY, fit, size, false);
        placeGroup(specs, rects, 4, 10, left, top, right, bottom, canvasW, stageY, fit, size, true);
        float centerLeft = Math.max(rects[2], rects[6]);
        float centerRight = rects[16];
        float center = (centerLeft + centerRight) * .5f;
        for (int i = 10; i < specs.length; ++i) {
            Spec s = specs[i];
            float minX = s.caption == CAPTION_HARVEST ? center : centerLeft;
            float maxX = s.caption == CAPTION_RESCUE ? center : centerRight;
            float actionSize = Math.min(size, Math.min((maxX - minX) / s.w, safeH / s.h));
            float w = s.w * actionSize, h = s.h * actionSize;
            float x = left + slackX * .5f + (s.x + s.w * .5f) * fit - w * .5f;
            float y = stageY + (s.y + s.h * .5f) * fit - h * .5f;
            x = Math.max(minX, Math.min(maxX - w, x));
            y = Math.max(top, Math.min(bottom - h, y));
            rects[i * 4] = x; rects[i * 4 + 1] = y; rects[i * 4 + 2] = x + w; rects[i * 4 + 3] = y + h;
        }
        return rects;
    }

    private static void placeGroup(Spec[] specs, float[] rects, int first, int end,
            float left, float top, float right, float bottom, float canvasW,
            float stageY, float fit, float size, boolean onRight) {
        float minX = canvasW, minY = Float.POSITIVE_INFINITY, maxX = 0, maxY = 0;
        for (int i = first; i < end; ++i) {
            Spec spec = specs[i];
            minX = Math.min(minX, spec.x); minY = Math.min(minY, spec.y);
            maxX = Math.max(maxX, spec.x + spec.w); maxY = Math.max(maxY, spec.y + spec.h);
        }
        float width = maxX - minX, height = maxY - minY;
        size = Math.min(size, Math.min((right - left) * .48f / width, (bottom - top) / height));
        float x = onRight ? right - (canvasW - maxX) * fit - width * size : left + minX * fit;
        float y = stageY + (minY + maxY) * .5f * fit - height * size * .5f;
        x = Math.max(left, Math.min(right - width * size, x));
        y = Math.max(top, Math.min(bottom - height * size, y));
        for (int i = first; i < end; ++i) {
            Spec spec = specs[i];
            rects[i * 4] = x + (spec.x - minX) * size;
            rects[i * 4 + 1] = y + (spec.y - minY) * size;
            rects[i * 4 + 2] = x + (spec.x + spec.w - minX) * size;
            rects[i * 4 + 3] = y + (spec.y + spec.h - minY) * size;
        }
    }

    /**
     * Topmost smallest control under the point. Decorative art is not a target.
     * allow null treats every non-decorative control as shown.
     */
    static int hit(int pad, float[] rects, boolean[] allow, float x, float y, float slop) {
        Spec[] specs = table(pad);
        int best = -1;
        float bestDistance = Float.POSITIVE_INFINITY, bestArea = Float.POSITIVE_INFINITY;
        int n = Math.min(specs.length, rects.length / 4);
        for (int i = n - 1; i >= 0; --i) {
            if (specs[i].role == DRAW) continue;
            if (allow != null && (i >= allow.length || !allow[i])) continue;
            float l = rects[i * 4], t = rects[i * 4 + 1], r = rects[i * 4 + 2], b = rects[i * 4 + 3];
            float dx = Math.max(0f, Math.max(l - x, x - r)), dy = Math.max(0f, Math.max(t - y, y - b));
            float distance = dx * dx + dy * dy;
            if (distance > slop * slop) continue;
            float area = (r - l) * (b - t);
            if (distance < bestDistance || (distance == bestDistance && area < bestArea)) {
                best = i; bestDistance = distance; bestArea = area;
            }
        }
        return best;
    }
}
