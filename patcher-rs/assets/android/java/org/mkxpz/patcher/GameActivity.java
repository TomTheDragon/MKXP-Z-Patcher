// Made by the MKXP-Z Patcher: runs the game with mkxp-z (SDL's activity).
//
// The engine is lib/<cpu>/libmain.so in the APK; SDL, mkxp-z and Ruby are
// all inside it. It finds the game through SRCDIR: the folder with the
// Android mkxp.json (mkxp-z/android in the game folder that ExtractActivity
// prepared), which leads it on to the game itself. On touchscreens the
// on-screen controls (TouchControls) lie over the game.
//
// The menu button of the touch controls and the Back key open a small menu:
// mkxp-z's settings (F1, shown by SettingsOverlay), resetting the game (F12)
// and quitting.
package org.mkxpz.patcher;

import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.nio.ByteBuffer;

public class GameActivity extends SDLActivity {
    private TouchControls controls;
    private SettingsOverlay settings;
    private final Handler handler = new Handler(Looper.getMainLooper());

    /** a touch on the settings menu (libmain.so, settingsmenu.cpp) */
    static native void nativeSettingsInput(int kind, int x, int y, int button);

    @Override
    protected void onCreate(Bundle state) {
        File conf = new File(ExtractActivity.gameDir(this), "mkxp-z/android");
        try {
            Os.setenv("SRCDIR", conf.getPath(), true);
        } catch (ErrnoException e) {
            // mkxp-z then looks next to itself and reports what's missing
        }
        super.onCreate(state);
        if (mLayout == null) {
            return; // SDL couldn't start (it shows why)
        }
        if (getPackageManager().hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN)) {
            controls = new TouchControls(this, this::showGameMenu);
            mLayout.addView(controls, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                 ViewGroup.LayoutParams.MATCH_PARENT));
        }
        settings = new SettingsOverlay(this, GameActivity::nativeSettingsInput);
        settings.setVisibility(SettingsOverlay.GONE);
        mLayout.addView(settings, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                             ViewGroup.LayoutParams.MATCH_PARENT));
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // Back (the navigation bar's, a keyboard's): our menu; SDL would take it
        // as a game key. A controller's Back button stays the controller's.
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK
                && (event.getSource() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) == 0) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) {
                onBackPressed();
            }
            return true;
        }
        // a real keyboard or controller: the on-screen controls aren't needed
        if (controls != null && event.getAction() == KeyEvent.ACTION_DOWN && event.getDevice() != null
                && !event.getDevice().isVirtual()
                && (event.getSource() & (InputDevice.SOURCE_KEYBOARD | InputDevice.SOURCE_GAMEPAD
                                         | InputDevice.SOURCE_JOYSTICK | InputDevice.SOURCE_DPAD)) != 0
                && event.getKeyCode() != KeyEvent.KEYCODE_BACK && event.getKeyCode() != KeyEvent.KEYCODE_VOLUME_UP
                && event.getKeyCode() != KeyEvent.KEYCODE_VOLUME_DOWN) {
            controls.hideForKeys();
        }
        return super.dispatchKeyEvent(event);
    }

    /** Back: closes the settings, else opens the game menu (instead of quitting right away). */
    @Override
    public void onBackPressed() {
        if (settings != null && settings.isOpen()) {
            nativeSettingsInput(SettingsOverlay.ESCAPE, 0, 0, 0);
        } else if (mLayout != null) {
            showGameMenu();
        } else {
            super.onBackPressed();
        }
    }

    private void showGameMenu() {
        if (isFinishing()) {
            return;
        }
        String[] items = {"Settings", "Reset game (back to the title screen)", "Quit game"};
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setItems(items, (d, which) -> {
                    switch (which) {
                        case 0: tapKey(KeyEvent.KEYCODE_F1); break;
                        case 1: tapKey(KeyEvent.KEYCODE_F12); break;
                        default: finish(); break;
                    }
                })
                .setNegativeButton("Back to the game", null)
                .show();
    }

    /** Presses a key for the engine (F1 settings, F12 reset) once the game runs again. */
    private void tapKey(int key) {
        handler.postDelayed(() -> {
            SDLActivity.onNativeKeyDown(key);
            handler.postDelayed(() -> SDLActivity.onNativeKeyUp(key), 150);
        }, 150);
    }

    /** The engine's settings menu has a new picture (called on SDL's thread). */
    public void mkxpzSettingsFrame(int width, int height, byte[] rgba) {
        Bitmap b = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        b.copyPixelsFromBuffer(ByteBuffer.wrap(rgba));
        runOnUiThread(() -> {
            if (settings != null) {
                settings.show(b);
            }
        });
    }

    /** The settings menu closed (called on SDL's thread). */
    public void mkxpzSettingsClose() {
        runOnUiThread(() -> {
            if (settings != null) {
                settings.hide();
            }
        });
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
