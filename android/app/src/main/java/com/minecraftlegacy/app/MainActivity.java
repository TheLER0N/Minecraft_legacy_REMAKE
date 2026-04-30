package com.minecraftlegacy.app;

import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;
import android.view.View;

import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {
    private static final String TAG = "MinecraftLegacy";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "MainActivity.onCreate");
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onCreate(savedInstanceState);
        hideSystemUi();
    }

    @Override
    protected void onResume() {
        Log.i(TAG, "MainActivity.onResume");
        super.onResume();
        hideSystemUi();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            hideSystemUi();
        }
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        Log.i(TAG, "MainActivity.forceLandscape width=" + w + " height=" + h + " hint=" + hint);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    @Override
    protected void onPause() {
        Log.i(TAG, "MainActivity.onPause");
        super.onPause();
    }

    @Override
    public void finish() {
        Log.i(TAG, "MainActivity.finish");
        super.finish();
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        );
    }
}
