// Made by the MKXP-Z Patcher: the first screen of a game's APK.
//
// RPG Maker games read their files from a real folder and write saves and
// settings into it, so the game in the APK (assets/game) is copied into the
// app's own storage on the first start, and again after an update (another
// assets/game.stamp). Files the game wrote itself (saves) stay. Then the
// game starts (GameActivity).
package org.mkxpz.patcher;

import android.app.Activity;
import android.content.Intent;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public class ExtractActivity extends Activity {
    /** The game folder in the app's storage. */
    static File gameDir(Activity a) {
        return new File(a.getFilesDir(), "game");
    }

    private ProgressBar bar;
    private TextView text;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String want = readAsset("game.stamp");
        File stamp = new File(getFilesDir(), "game.stamp");
        if (want != null && want.equals(readFile(stamp))) {
            startGame();
            return;
        }
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(Color.BLACK);
        box.setPadding(64, 64, 64, 64);
        text = new TextView(this);
        text.setTextColor(Color.WHITE);
        text.setGravity(Gravity.CENTER);
        text.setText("Preparing the game for the first start …");
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        box.addView(text);
        box.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                       LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(box);
        new Thread(() -> {
            try {
                extract();
                writeFile(stamp, want == null ? "" : want);
                runOnUiThread(this::startGame);
            } catch (IOException e) {
                runOnUiThread(() -> text.setText("Couldn't prepare the game: " + e.getMessage()
                        + "\n\nIs there enough free storage?"));
            }
        }, "extract").start();
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }

    /** Copies every file listed in assets/game.files (one path per line). */
    private void extract() throws IOException {
        List<String> files = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("game.files"),
                                                                       StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) {
                if (!line.isEmpty()) {
                    files.add(line);
                }
            }
        }
        File root = gameDir(this);
        AssetManager am = getAssets();
        byte[] buf = new byte[1 << 16];
        int n = files.size();
        for (int i = 0; i < n; i++) {
            String name = files.get(i);
            File out = new File(root, name);
            File parent = out.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                throw new IOException("can't create " + parent);
            }
            File tmp = new File(out.getPath() + ".part");
            try (InputStream in = am.open("game/" + name); OutputStream o = new FileOutputStream(tmp)) {
                for (int k; (k = in.read(buf)) > 0; ) {
                    o.write(buf, 0, k);
                }
            }
            if (!tmp.renameTo(out)) {
                throw new IOException("can't write " + out);
            }
            final int done = i + 1;
            if (done % 20 == 0 || done == n) {
                runOnUiThread(() -> {
                    bar.setMax(n);
                    bar.setProgress(done);
                });
            }
        }
    }

    private String readAsset(String name) {
        try (InputStream in = getAssets().open(name)) {
            return readAll(in);
        } catch (IOException e) {
            return null;
        }
    }

    private static String readFile(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            return readAll(in);
        } catch (IOException e) {
            return null;
        }
    }

    private static String readAll(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder();
        BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
        for (int c; (c = r.read()) >= 0; ) {
            sb.append((char) c);
        }
        return sb.toString();
    }

    private static void writeFile(File f, String s) throws IOException {
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(s.getBytes(StandardCharsets.UTF_8));
        }
    }
}
