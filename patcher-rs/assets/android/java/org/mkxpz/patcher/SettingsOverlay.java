// Made by the MKXP-Z Patcher: shows mkxp-z's settings menu (F1) on Android.
//
// Android has only the game's window, so the engine draws the menu into a
// picture and hands it to GameActivity, which shows it here over the game.
// Touches go back to the menu as the mouse: a tap clicks, a held finger
// right-clicks (clears a key slot), a moving finger drags (sliders, the
// shader list). Back closes the menu like Escape.
package org.mkxpz.patcher;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;

public class SettingsOverlay extends View {
    /** what the engine's menu gets: GameActivity.nativeSettingsInput(kind, x, y, button) */
    interface Input {
        void send(int kind, int x, int y, int button);
    }

    static final int MOTION = 0, DOWN = 1, UP = 2, LEAVE = 3, ESCAPE = 4;
    private static final int LEFT = 1, RIGHT = 3;

    private final Input input;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final RectF dst = new RectF();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final int slop;
    private Bitmap picture;

    /** a finger that hasn't moved yet: tap or hold isn't decided */
    private boolean pending, leftDown;
    private float downX, downY;
    private int downMx, downMy;
    private final Runnable hold = this::held;

    /** a finger held still: a right click */
    private void held() {
        if (pending) {
            pending = false;
            input.send(DOWN, downMx, downMy, RIGHT);
            input.send(UP, downMx, downMy, RIGHT);
        }
    }

    public SettingsOverlay(Context context, Input input) {
        super(context);
        this.input = input;
        slop = ViewConfiguration.get(context).getScaledTouchSlop();
        setBackgroundColor(Color.argb(150, 0, 0, 0));
        setClickable(true);
    }

    /** A new picture of the menu. */
    void show(Bitmap b) {
        picture = b;
        setVisibility(VISIBLE);
        layoutPicture();
        invalidate();
    }

    void hide() {
        handler.removeCallbacks(hold);
        pending = leftDown = false;
        picture = null;
        setVisibility(GONE);
    }

    boolean isOpen() {
        return getVisibility() == VISIBLE && picture != null;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layoutPicture();
    }

    private void layoutPicture() {
        if (picture == null || getWidth() == 0) {
            return;
        }
        float s = Math.min(getWidth() / (float) picture.getWidth(), getHeight() / (float) picture.getHeight());
        float w = picture.getWidth() * s, h = picture.getHeight() * s;
        dst.set((getWidth() - w) / 2, (getHeight() - h) / 2, (getWidth() + w) / 2, (getHeight() + h) / 2);
    }

    @Override
    protected void onDraw(Canvas c) {
        if (picture != null) {
            c.drawBitmap(picture, null, dst, paint);
        }
    }

    private int mx(float x) {
        return Math.round((x - dst.left) * picture.getWidth() / dst.width());
    }

    private int my(float y) {
        return Math.round((y - dst.top) * picture.getHeight() / dst.height());
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (picture == null || e.getActionIndex() != 0 && e.getActionMasked() != MotionEvent.ACTION_MOVE) {
            return true; // one finger only
        }
        float x = e.getX(0), y = e.getY(0);
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = x;
                downY = y;
                downMx = mx(x);
                downMy = my(y);
                pending = true;
                leftDown = false;
                input.send(MOTION, downMx, downMy, 0);
                handler.postDelayed(hold, ViewConfiguration.getLongPressTimeout());
                break;
            case MotionEvent.ACTION_MOVE:
                if (pending && Math.hypot(x - downX, y - downY) > slop) {
                    pending = false;
                    handler.removeCallbacks(hold);
                    input.send(DOWN, downMx, downMy, LEFT);
                    leftDown = true;
                }
                if (leftDown) {
                    input.send(MOTION, mx(x), my(y), 1);
                }
                break;
            case MotionEvent.ACTION_UP:
                handler.removeCallbacks(hold);
                if (pending) {
                    input.send(DOWN, downMx, downMy, LEFT);
                    input.send(UP, downMx, downMy, LEFT);
                } else if (leftDown) {
                    input.send(UP, mx(x), my(y), LEFT);
                }
                pending = leftDown = false;
                input.send(LEAVE, 0, 0, 0);
                break;
            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(hold);
                if (leftDown) {
                    input.send(UP, mx(x), my(y), LEFT);
                }
                pending = leftDown = false;
                input.send(LEAVE, 0, 0, 0);
                break;
        }
        return true;
    }
}
