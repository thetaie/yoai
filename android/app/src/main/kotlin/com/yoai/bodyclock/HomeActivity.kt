package com.yoai.bodyclock

import com.yoai.bodyclock.day.HomeScreen
import com.yoai.bodyclock.ui.YoaiTheme
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge

/**
 * The app.
 *
 * Separate from [MainActivity], which is the measurement bench and stays one --
 * android/README.md is about that bench, and none of what it says (no chat, no
 * settings, no session storage, models pushed by hand) is a claim this screen
 * has to keep true. Both are launchable on a dev device, under different labels;
 * the bench's entry is the thing to drop from a release manifest, not the
 * activity.
 *
 * The palette family is fixed to the default here. The picker that changes it
 * is design round 1g, which is a settings screen and not yet built; when it
 * lands it reads a stored preference and passes it in, which is why
 * [YoaiTheme] takes a family rather than looking one up.
 */
class HomeActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            YoaiTheme {
                HomeScreen()
            }
        }
    }
}
