package dev.stockglass.auto.testmedia;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** Starts the player in the playing state and goes away. */
public final class Start extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        startForegroundService(new Intent(this, Player.class).setAction(Player.ACTION_PLAY));
        finish();
    }
}
