// examples/android/app/src/main/java/io/opencode/example/MainActivity.java
// Single-activity harness: creates an engine over the frozen C ABI, runs a
// task on a background thread, and logs every event phase to a TextView.
package io.opencode.example;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class MainActivity extends Activity {
    private TextView log;
    private long engine = 0;

    static {
        System.loadLibrary("opencodepp");
        System.loadLibrary("opencode_host");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        log = new TextView(this);
        log.setText("opencodepp: creating engine...\n");
        setContentView(log);
        runTask();
    }

    // Event sink called on the engine thread.
    public void onEngineEvent(int kind, String text) {
        runOnUiThread(() -> append("  [" + kindName(kind) + "] " + text + "\n"));
    }

    private void runTask() {
        new Thread(() -> {
            engine = engineCreate("http://127.0.0.1:8123");
            if (engine == 0) {
                append("engineCreate failed\n");
                return;
            }
            append("engine created (abi " + abiVersion() + ")\n");
            engineRun(engine, "Say hello in one sentence.");
            append("run finished\n");
            engineDestroy(engine);
        }).start();
    }

    private void append(String s) {
        runOnUiThread(() -> log.append(s));
    }

    private static String kindName(int k) {
        switch (k) {
            case 1: return "log";
            case 2: return "preparing";
            case 3: return "connecting";
            case 4: return "streaming";
            case 5: return "tool";
            case 6: return "verifying";
            case 7: return "applying";
            case 8: return "done";
            case 9: return "failed";
            case 10: return "cancelled";
            default: return "?";
        }
    }

    private static native int abiVersion();
    private static native long engineCreate(String baseUrl);
    private static native void engineRun(long handle, String prompt);
    private static native void engineDestroy(long handle);
}
