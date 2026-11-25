package org.bioshock.port;

/**
 * Original iOS controls; widescreen iPhone/iPad viewports are 568x320 / 1024x576.
 * Positions use a fitted canvas. Related controls scale together so their art
 * and hit regions stay aligned. No Android types, so a host test can place and hit them.
 */
final class IosTouchLayout {
    static final int PHONE = 0, PAD = 1;
    static final int DRAW = 0, MOVE = 1, HOLD = 3, PULSE = 4;
    /** GameControls pulses TouchInput.PAUSE; this is not a held bit. */
    static final int PAUSE = -1;
    static final int FIRE = 1, ALT = 2, JUMP = 4, DUCK = 8, USE = 16, NEXT = 64, MAP = 1 << 12, RESCUE = 2048;
    static final int AIM = 1 << 21;
    static final int LOG = 1 << 20;
    static final int RELOAD = 1 << 23, HACK = 1 << 25, TAUNT = 1 << 26;
    static final int GATE_USE = 1 << 4, GATE_HACK = 1 << 5, GATE_RESCUE = 1 << 6, GATE_HARVEST = 1 << 7;
    static final int GATE_REROLL = 1 << 8;
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

    private static final float PHONE_W = 568, PHONE_H = 320, PAD_W = 1024, PAD_H = 576;
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
        c(PULSE, LOG, 0, 0, 527, 53, 31, 31, "diary", "diary_down"),
        c(HOLD, JUMP, 0, 0, 527, 181, 35, 35, null, null),
        c(PULSE, AIM, 0, 0, 527, 236, 35, 35, null, null),
        c(PULSE, RESCUE, GATE_RESCUE, CAPTION_RESCUE, 173, 224, 88, 17, "gather", "gather_down"),
        c(PULSE, TAUNT, GATE_HARVEST, CAPTION_HARVEST, 305, 224, 88, 17, "gather", "gather_down"),
        c(PULSE, HACK, GATE_HACK, 0, 382, 219, 33, 33, "hack", null),
        c(PULSE, USE, GATE_USE | GATE_REROLL, 0, 366, 170, 58, 58, "use", "use_down")
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
        c(PULSE, LOG, 0, 0, 945, 206, 64, 64, "diary", "diary_down"),
        c(HOLD, JUMP, 0, 0, 945, 480, 64, 64, null, null),
        c(PULSE, AIM, 0, 0, 945, 585, 64, 64, null, null),
        c(PULSE, RESCUE, GATE_RESCUE, CAPTION_RESCUE, 336, 466, 146, 28, "gather", "gather_down"),
        c(PULSE, TAUNT, GATE_HARVEST, CAPTION_HARVEST, 551, 464, 146, 28, "gather", "gather_down"),
        c(PULSE, HACK, GATE_HACK, 0, 734, 552, 55, 53, "hack", null),
        c(PULSE, USE, GATE_USE | GATE_REROLL, 0, 707, 460, 97, 96, "use", "use_down")
    };

    // Loot_Button geometry inside the LootAction backing from each original HUD's XML.
    private static final float[] PHONE_USE_BUTTON = {12f / 58, 10f / 58, 50f / 58, 48f / 58};
    private static final float[] PAD_USE_BUTTON = {20f / 97, 16f / 96, 84f / 97, 80f / 96};

    static float useButtonFraction(int pad, int edge) {
        return (pad == PAD ? PAD_USE_BUTTON : PHONE_USE_BUTTON)[edge];
    }

    private static Spec[] table(int pad) { return pad == PAD ? TABLET_SPECS : PHONE_SPECS; }
    static int count(int pad) { return table(pad).length; }
    static Spec spec(int pad, int index) { return table(pad)[index]; }

    /** A 100% iOS layout retains comfortable combat buttons on dense phone screens, including iPad mode. */
    static float scaleForDensity(int pad, float width, float height, float requested, float minimumButton) {
        float fit = Math.min(width / (pad == PAD ? PAD_W : PHONE_W), height / (pad == PAD ? PAD_H : PHONE_H));
        return requested * Math.max(1, minimumButton / (fit * (pad == PAD ? 64 : 35)));
    }

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
        // The lower-left anchor is independent of weapon ownership, gauges and ammo visibility.
        placeGroup(specs, rects, 0, 1, left, top, right, bottom, canvasW, stageY, fit,
                Math.min(size, safeH * .48f / specs[0].h), false);
        float stickHeight = rects[3] - rects[1];
        rects[3] = bottom - safeH * .025f; rects[1] = rects[3] - stickHeight;
        placeGroup(specs, rects, 1, 2, left, top, right, bottom, canvasW, stageY, fit, size, false);
        float gap = 4 * fit;
        // Keep crouch at the left edge, directly above the fixed stick when space is tight.
        float crouchHeight = rects[7] - rects[5];
        rects[5] = Math.max(top, Math.min(rects[5], rects[1] - gap - crouchHeight));
        rects[7] = rects[5] + crouchHeight;
        placeGroup(specs, rects, 4, 10, left, top, right, bottom, canvasW, stageY, fit, size, true);
        // Map above Pause, beside the gauges, as in the original iOS screenshots.
        float utility = Math.min(Math.max(specs[8].w * size, specs[2].w * size),
                Math.min(safeW * .22f, (rects[5] - top - 2 * gap) * .5f));
        utility = Math.max(Math.min(fit, safeH * .02f), utility);
        for (int i = 2; i < 4; ++i) {
            rects[4*i] = left + specs[i].x * fit;
            rects[4*i+1] = top + (i - 2) * (utility + gap);
            rects[4*i+2] = rects[4*i] + utility;
            rects[4*i+3] = rects[4*i+1] + utility;
        }
        // Audio-log button sits to the left of the ammunition panel, clear of Reload at every scale.
        float diaryWidth = rects[38] - rects[36], diaryHeight = rects[39] - rects[37];
        rects[36] = Math.min(right - diaryWidth, Math.max(left + safeW * .4f, right - safeH * (260f * 1.5f / 720f) - gap - diaryWidth));
        rects[38] = rects[36] + diaryWidth;
        rects[37] = Math.min(bottom - diaryHeight, top + 8 * fit); rects[39] = rects[37] + diaryHeight;
        float centerLeft = Math.max(Math.max(rects[2], rects[6]), Math.max(rects[10], rects[14]));
        float centerRight = rects[16];
        Spec extra = specs[10];
        float extraSize = Math.min(size, Math.min((centerRight - centerLeft) / (2 * extra.w + 4),
                safeH / (3 * extra.h)));
        float extraBottom = bottom - Math.max(safeH * .14f, extra.h * extraSize * .8f);
        float extraTop = extraBottom - extra.h * extraSize;
        for (int i = 10; i < 12; ++i) {
            float x = centerRight - extra.w * extraSize - (i - 10) * (extra.w + 4) * extraSize;
            rects[i * 4] = x; rects[i * 4 + 1] = extraTop;
            rects[i * 4 + 2] = x + extra.w * extraSize; rects[i * 4 + 3] = extraBottom;
        }
        // Keep the actionable loot and hack buttons as large as the weapon controls, in separate cells.
        float button = Math.max(rects[34] - rects[32], extra.w * extraSize);
        gap = button * .16f;
        float useFraction = useButtonFraction(pad, 2) - useButtonFraction(pad, 0);
        float useWidth = Math.max(specs[15].w * size, button / useFraction);
        float useHeight = useWidth * specs[15].h / specs[15].w;
        float actionBandTop = top + 8 * fit + diaryHeight + gap;
        float contextFit = Math.min(1, Math.min((centerRight - centerLeft) / (useWidth + button + gap),
                (extraTop - actionBandTop) / (useHeight + button + 2 * gap)));
        useWidth *= contextFit; useHeight *= contextFit; button *= contextFit; gap *= contextFit;
        float actionTop = Math.max(actionBandTop, extraTop - gap - Math.max(useHeight, button));
        rects[60] = centerRight - button - gap - useWidth;
        rects[61] = actionTop; rects[62] = rects[60] + useWidth; rects[63] = actionTop + useHeight;
        rects[56] = centerRight - button; rects[57] = actionTop + (useHeight - button) * .5f;
        rects[58] = centerRight; rects[59] = rects[57] + button;
        float captionWidth = Math.min(button * 2.5f, (centerRight - centerLeft - gap) * .5f);
        float captionTop = Math.max(actionBandTop, actionTop - gap - button);
        for (int i = 12; i < 14; ++i) {
            float x = centerRight - (14 - i) * captionWidth - (13 - i) * gap;
            rects[4*i] = x; rects[4*i+1] = captionTop;
            rects[4*i+2] = x + captionWidth; rects[4*i+3] = captionTop + button;
        }
        return rects;
    }

    /** Three alternatives fan out to the left of the original Next button. */
    static float[] selector(float[] controls) {
        float cx=(controls[20]+controls[22])*.5f, cy=(controls[21]+controls[23])*.5f;
        float size=(controls[22]-controls[20])*1.25f;
        float[] rects=new float[12];
        for(int i=0;i<3;++i){
            float x=Math.max(0,cx-size*(i==1?2.05f:1.7f));
            float y=Math.max(0,cy+size*(i-1)*1.05f-size*.5f);
            rects[4*i]=x;rects[4*i+1]=y;rects[4*i+2]=x+size;rects[4*i+3]=y+size;
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
            if (specs[i].action == USE) {
                float w = r - l, h = b - t;
                r = l + w * useButtonFraction(pad, 2);
                b = t + h * useButtonFraction(pad, 3);
                l += w * useButtonFraction(pad, 0);
                t += h * useButtonFraction(pad, 1);
            }
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
