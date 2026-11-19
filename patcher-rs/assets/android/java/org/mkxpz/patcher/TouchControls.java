// Made by the MKXP-Z Patcher: on-screen controls for touchscreens.
//
// RPG Maker games only know keys, so a D-pad (arrow keys, also diagonal) on
// the left and three buttons on the right press them for the game: OK
// (Enter: confirm, talk), Back (Escape: cancel, menu) and Dash (Shift).
// The small button at the top right opens the app's menu (settings, reset,
// quit). Touches elsewhere go on to the game (mkxp-z sees them as the mouse).
// The controls hide while a keyboard or controller is used and come back on
// the next touch.
package org.mkxpz.patcher;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.SparseArray;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

public class TouchControls extends View {
    /** A press shorter than this is held this long: the game reads keys once per frame. */
    private static final long MIN_PRESS_MS = 70;

    private static final int UP = KeyEvent.KEYCODE_DPAD_UP, DOWN = KeyEvent.KEYCODE_DPAD_DOWN,
            LEFT = KeyEvent.KEYCODE_DPAD_LEFT, RIGHT = KeyEvent.KEYCODE_DPAD_RIGHT;
    /** not a key: the menu button */
    private static final int MENU = -1;

    private static class Button {
        final String label;
        final int key;
        final RectF area = new RectF();
        Button(String label, int key) {
            this.label = label;
            this.key = key;
        }
    }

    private final Button[] buttons = {
        new Button("OK", KeyEvent.KEYCODE_ENTER),
        new Button("Back", KeyEvent.KEYCODE_ESCAPE),
        new Button("Dash", KeyEvent.KEYCODE_SHIFT_LEFT),
        new Button("", MENU),
    };
    private final Runnable onMenu;
    private final RectF pad = new RectF();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pressedFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint menuLine = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Handler handler = new Handler(Looper.getMainLooper());

    /** key -> how many fingers hold it (and when it went down) */
    private final SparseArray<int[]> held = new SparseArray<>();
    private final SparseArray<Long> downAt = new SparseArray<>();
    /** pointer id -> keys that finger presses (the D-pad: up to two) */
    private final SparseArray<int[]> fingers = new SparseArray<>();
    private boolean hidden, cancelling;

    public TouchControls(Context context, Runnable onMenu) {
        super(context);
        this.onMenu = onMenu;
        fill.setColor(Color.argb(70, 255, 255, 255));
        pressedFill.setColor(Color.argb(140, 255, 255, 255));
        line.setColor(Color.argb(150, 0, 0, 0));
        line.setStyle(Paint.Style.STROKE);
        text.setColor(Color.argb(220, 255, 255, 255));
        text.setTextAlign(Paint.Align.CENTER);
        text.setShadowLayer(3, 0, 0, Color.BLACK);
        menuLine.setColor(Color.argb(220, 255, 255, 255));
        menuLine.setStrokeCap(Paint.Cap.ROUND);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        float unit = Math.min(w, h) / 9f;  // about 1/9 of the short side
        float margin = unit * 0.4f;
        float size = unit * 3.2f;
        pad.set(margin, h - margin - size, margin + size, h - margin);
        float r = unit * 0.75f;  // button radius
        float cx = w - margin - r * 1.2f, cy = h - margin - r * 1.2f;
        place(buttons[0], cx - r * 0.2f, cy - r * 0.2f, r * 1.15f);            // OK: biggest, bottom right
        place(buttons[1], cx - r * 2.6f, cy + r * 0.15f, r * 0.9f);            // Back: left of it
        place(buttons[2], cx - r * 0.1f, cy - r * 2.6f, r * 0.9f);             // Dash: above it
        place(buttons[3], w - margin - r * 0.55f, margin + r * 0.55f, r * 0.55f); // menu: top right, small
        line.setStrokeWidth(Math.max(2, unit / 30));
        text.setTextSize(unit * 0.45f);
        menuLine.setStrokeWidth(Math.max(3, unit / 14));
    }

    private static void place(Button b, float cx, float cy, float r) {
        b.area.set(cx - r, cy - r, cx + r, cy + r);
    }

    @Override
    protected void onDraw(Canvas c) {
        if (hidden) {
            return;
        }
        // D-pad: a cross
        float w = pad.width() / 3;
        RectF h = new RectF(pad.left, pad.top + w, pad.right, pad.bottom - w);
        RectF v = new RectF(pad.left + w, pad.top, pad.right - w, pad.bottom);
        c.drawRoundRect(h, w * 0.3f, w * 0.3f, fill);
        c.drawRoundRect(v, w * 0.3f, w * 0.3f, fill);
        if (isHeld(LEFT)) c.drawRect(pad.left, pad.top + w, pad.left + w, pad.bottom - w, pressedFill);
        if (isHeld(RIGHT)) c.drawRect(pad.right - w, pad.top + w, pad.right, pad.bottom - w, pressedFill);
        if (isHeld(UP)) c.drawRect(pad.left + w, pad.top, pad.right - w, pad.top + w, pressedFill);
        if (isHeld(DOWN)) c.drawRect(pad.left + w, pad.bottom - w, pad.right - w, pad.bottom, pressedFill);
        c.drawRoundRect(h, w * 0.3f, w * 0.3f, line);
        c.drawRoundRect(v, w * 0.3f, w * 0.3f, line);
        for (Button b : buttons) {
            c.drawOval(b.area, isHeld(b.key) ? pressedFill : fill);
            c.drawOval(b.area, line);
            if (b.key == MENU) {
                // three lines
                float lw = b.area.width() * 0.22f, gap = b.area.height() * 0.15f;
                for (int i = -1; i <= 1; i++) {
                    float y = b.area.centerY() + i * gap;
                    c.drawLine(b.area.centerX() - lw, y, b.area.centerX() + lw, y, menuLine);
                }
                continue;
            }
            c.drawText(b.label, b.area.centerX(), b.area.centerY() - (text.descent() + text.ascent()) / 2, text);
        }
    }

    /** The keys a point presses: a button, or one or two D-pad directions. */
    private int[] keysAt(float x, float y) {
        for (Button b : buttons) {
            float dx = x - b.area.centerX(), dy = y - b.area.centerY(), r = b.area.width() / 2 * 1.15f;
            if (dx * dx + dy * dy <= r * r) {
                return new int[] {b.key};
            }
        }
        // the D-pad, a little larger than drawn; 8 directions by angle
        float cx = pad.centerX(), cy = pad.centerY(), r = pad.width() / 2 * 1.3f;
        float dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy > r * r) {
            return null;
        }
        if (dx * dx + dy * dy < (pad.width() / 10) * (pad.width() / 10)) {
            return new int[0]; // the middle: nothing
        }
        double a = Math.toDegrees(Math.atan2(-dy, dx)); // 0 = right, 90 = up
        if (a < 0) a += 360;
        int sector = (int) Math.round(a / 45) % 8;
        switch (sector) {
            case 0: return new int[] {RIGHT};
            case 1: return new int[] {RIGHT, UP};
            case 2: return new int[] {UP};
            case 3: return new int[] {LEFT, UP};
            case 4: return new int[] {LEFT};
            case 5: return new int[] {LEFT, DOWN};
            case 6: return new int[] {DOWN};
            default: return new int[] {RIGHT, DOWN};
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        if (hidden && (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN)) {
            hidden = false;
            invalidate();
        }
        boolean used = false;
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int[] keys = keysAt(e.getX(index), e.getY(index));
                if (keys != null) {
                    fingers.put(e.getPointerId(index), keys);
                    for (int k : keys) press(k);
                    used = true;
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    int[] old = fingers.get(id);
                    if (old == null) {
                        continue;
                    }
                    used = true;
                    // a finger on the D-pad slides between directions; one on a button stays on it
                    if (old.length == 1 && !isDirection(old[0])) {
                        continue;
                    }
                    int[] now = keysAt(e.getX(i), e.getY(i));
                    if (now == null || (now.length == 1 && !isDirection(now[0]))) {
                        now = new int[0];
                    }
                    if (!sameKeys(old, now)) {
                        for (int k : now) if (!contains(old, k)) press(k);
                        for (int k : old) if (!contains(now, k)) release(k);
                        fingers.put(id, now);
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (action == MotionEvent.ACTION_CANCEL) {
                    cancelling = true; // (no menu)
                    for (int i = 0; i < fingers.size(); i++) for (int k : fingers.valueAt(i)) release(k);
                    cancelling = false;
                    fingers.clear();
                    used = true;
                    break;
                }
                int id = e.getPointerId(index);
                int[] keys = fingers.get(id);
                if (keys != null) {
                    for (int k : keys) release(k);
                    fingers.remove(id);
                    used = true;
                }
                break;
            }
        }
        if (used) {
            invalidate();
        }
        return used || fingers.size() > 0 && action != MotionEvent.ACTION_DOWN;
    }

    private static boolean isDirection(int k) {
        return k == UP || k == DOWN || k == LEFT || k == RIGHT;
    }

    private static boolean contains(int[] a, int k) {
        for (int x : a) if (x == k) return true;
        return false;
    }

    private static boolean sameKeys(int[] a, int[] b) {
        if (a.length != b.length) return false;
        for (int x : a) if (!contains(b, x)) return false;
        return true;
    }

    private boolean isHeld(int key) {
        int[] n = held.get(key);
        return n != null && n[0] > 0;
    }

    private void press(int key) {
        int[] n = held.get(key);
        if (n == null) {
            n = new int[1];
            held.put(key, n);
        }
        if (n[0]++ == 0) {
            downAt.put(key, SystemClock.uptimeMillis());
            if (key == MENU) {
                return;
            }
            SDLActivity.onNativeKeyDown(key);
        }
    }

    private void release(int key) {
        final int[] n = held.get(key);
        if (n == null || n[0] == 0) {
            return;
        }
        if (--n[0] == 0) {
            if (key == MENU) {
                if (!cancelling) {
                    handler.post(onMenu); // when the finger is up
                }
                return;
            }
            long held = SystemClock.uptimeMillis() - downAt.get(key, 0L);
            if (held >= MIN_PRESS_MS) {
                SDLActivity.onNativeKeyUp(key);
            } else {
                handler.postDelayed(() -> {
                    if (n[0] == 0) {
                        SDLActivity.onNativeKeyUp(key);
                    }
                }, MIN_PRESS_MS - held);
            }
        }
    }

    /** A keyboard or controller is in use: hide (the next touch shows the controls again). */
    public void hideForKeys() {
        if (!hidden) {
            hidden = true;
            invalidate();
        }
    }
}
