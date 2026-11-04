package app.localarchiver;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/** Emulator-only home avoids Quickstep first-boot ANRs stealing test window focus. */
public class TestHomeActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView label = new TextView(this);
        label.setText("MilkZip test home");
        label.setGravity(android.view.Gravity.CENTER);
        setContentView(label);
    }
}
