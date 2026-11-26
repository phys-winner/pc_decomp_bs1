package org.bioshock.port;

import java.util.Arrays;

/** Geometry and pointer ownership for the Classic inventory tab. */
final class InventoryBar {
    private static final int HEADER = 7, TAB = -2, EMPTY = -1;
    private static final long ANIMATION_MS = 150;
    private int[] items = new int[0];
    private final float[] scroll = new float[2];
    private float[] tab;
    private float left, right, size;
    private long animationStart;
    private float animationFrom;
    private int captured, pressId = -1, pressed = EMPTY, pressRow;
    private int pressedClass;
    private float pressX, pressY, pressScroll;
    private boolean dragging;
    boolean open;

    static final class Selection {
        final int phase, image, itemClass;
        Selection(int row, int image, int itemClass) {
            this.phase = 11 + row;
            this.image = image;
            this.itemClass = itemClass;
        }
    }

    boolean update(int[] snapshot) {
        if (snapshot == null || snapshot.length < HEADER || snapshot[0] < 0 || snapshot[1] < 0 ||
            snapshot[0] > 32 || snapshot[1] > 32 || snapshot.length != HEADER + 2 * (snapshot[0] + snapshot[1]))
            snapshot = new int[0];
        if (Arrays.equals(items, snapshot)) return false;
        items = snapshot;
        clampScroll(0);
        clampScroll(1);
        return true;
    }

    void layout(float[] tab, float left, float right, float size) {
        this.tab = tab;
        this.left = left;
        this.right = right;
        this.size = size;
        clampScroll(0);
        clampScroll(1);
    }

    int count(int row) { return items.length == 0 ? 0 : items[row]; }
    int currentImage(int row) { return items.length == 0 ? 0 : items[2 + row]; }
    int activeRow() { return items.length == 0 ? 0 : items[4]; }
    private int word(int row, int index) { return HEADER + 2 * (row == 0 ? index : count(0) + index); }
    int image(int row, int index) { return items[word(row, index)]; }
    boolean current(int row, int index) { return items[word(row, index) + 1] == items[5 + row]; }
    float[] tab() { return tab; }
    float cellSize() { return size; }
    float rowWidth(int row) { return Math.min(right - left, count(row) * size); }
    float rowLeft(int row) { return (left + right - rowWidth(row)) / 2; }
    float cellLeft(int row, int index) { return rowLeft(row) + index * size - scroll[row]; }
    private int rows() { return (count(0) > 0 ? 1 : 0) + (count(1) > 0 ? 1 : 0); }
    float rowTop(int row, long now) {
        return tab[1] - size * (rows() * progress(now) - (row == 1 && count(0) > 0 ? 1 : 0));
    }

    private void clampScroll(int row) {
        scroll[row] = Math.max(0, Math.min(count(row) * size - rowWidth(row), scroll[row]));
    }
    float progress(long now) {
        float alpha = Math.max(0, Math.min(1, (now - animationStart) / (float)ANIMATION_MS));
        return animationFrom + ((open ? 1 : 0) - animationFrom) * alpha;
    }
    boolean animating(long now) { return animationFrom != (open ? 1 : 0) && now - animationStart < ANIMATION_MS; }
    private void setOpen(boolean next, long now) {
        animationFrom = progress(now);
        animationStart = now;
        open = next;
        if (next && animationFrom == 0) Arrays.fill(scroll, 0);
    }
    void hide() {
        open = false;
        animationFrom = 0;
        pressed = EMPTY;
        pressId = -1;
    }
    void cancel() { hide(); captured = 0; }
    boolean owns(int id) { return (captured & (1 << id)) != 0; }

    private int hitRow(float x, float y, long now) {
        if (!open || y >= tab[1]) return EMPTY;
        for (int row = 0; row < 2; ++row) {
            float top = rowTop(row, now), l = rowLeft(row);
            if (count(row) > 0 && x >= l && x < l + rowWidth(row) && y >= top && y < top + size) return row;
        }
        return EMPTY;
    }
    private boolean hitTab(float x, float y) {
        return x >= tab[0] && y >= tab[1] && x < tab[2] && y < tab[3];
    }
    private int index(int row, float x) {
        return Math.min(count(row) - 1, (int)((x - rowLeft(row) + scroll[row]) / size));
    }

    boolean down(int id, float x, float y, long now) {
        if (tab == null) return false;
        int kind = hitTab(x, y) ? TAB : hitRow(x, y, now);
        if (kind == EMPTY) {
            if (open) setOpen(false, now);
            return false;
        }
        captured |= 1 << id;
        if (pressId < 0) {
            pressId = id;
            pressRow = kind;
            pressX = x;
            pressY = y;
            dragging = false;
            pressed = kind < 0 ? kind : index(kind, x);
            if (kind >= 0) {
                pressedClass = items[word(kind, pressed) + 1];
                pressScroll = scroll[kind];
            }
        }
        return true;
    }

    void move(int id, float x, float y, float slop) {
        if (id != pressId) return;
        dragging |= Math.hypot(x - pressX, y - pressY) > slop;
        if (dragging && pressRow >= 0) {
            scroll[pressRow] = pressScroll + pressX - x;
            clampScroll(pressRow);
        }
    }

    Selection up(int id, float x, float y, long now) {
        captured &= ~(1 << id);
        if (id != pressId) return null;
        pressId = -1;
        if (dragging || tab == null) return null;
        if (pressed == TAB && hitTab(x, y)) {
            setOpen(!open, now);
            return null;
        }
        int row = hitRow(x, y, now);
        if (row < 0 || row != pressRow) return null;
        int index = index(row, x), word = word(row, index);
        if (items[word + 1] != pressedClass) return null;
        setOpen(false, now);
        return new Selection(row, items[word], pressedClass);
    }
}
