package app.localarchiver

import android.app.Application
import android.system.Os

class MilkZipApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        // The shared core uses mkdtemp for compound TAR streams. Android has no /tmp.
        Os.setenv("TMPDIR", cacheDir.absolutePath, true)
    }
}
