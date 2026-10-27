package com.birabittoh.nocturnerecomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Starts the game again after NocturneRecompActivity.restartApp() has killed
 * its process. It lives in its own process (see the manifest) so it survives
 * that kill, and being a foreground activity it is allowed to launch the game
 * from the background, which a plain alarm or a service no longer is.
 */
public class RestartTrampolineActivity extends Activity {

    static final String EXTRA_NEXT = "next";
    static final String EXTRA_PID = "pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent next = getIntent().getParcelableExtra(EXTRA_NEXT);
        // Already gone in the normal case; this only makes sure of it before
        // the new instance maps the guest memory again.
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0 && pid != Process.myPid()) {
            Process.killProcess(pid);
        }
        if (next != null) {
            startActivity(next);
        }
        finish();
        Runtime.getRuntime().exit(0);
    }
}
