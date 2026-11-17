package app.localarchiver;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;

/** Runs under the provider owner's UID and grants access to the actual target app. */
public class TestGrantActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        grantUriPermission("app.localarchiver", Uri.parse("content://app.localarchiver.test.documents/"),
            Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        finish();
    }
}
