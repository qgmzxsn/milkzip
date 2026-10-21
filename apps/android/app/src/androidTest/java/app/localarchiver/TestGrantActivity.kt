package app.localarchiver

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.test.platform.app.InstrumentationRegistry
import java.io.FileInputStream

/** Runs as the provider's test-only package, issuing a real URI grant to the app. */
class TestGrantActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        grantUriPermission("app.localarchiver", Uri.parse("content://app.localarchiver.test.documents/"),
            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
        finish()
    }
}

object DocumentFixture {
    fun grant() {
        val inst = InstrumentationRegistry.getInstrumentation()
        android.os.ParcelFileDescriptor.AutoCloseInputStream(inst.uiAutomation.executeShellCommand("am start -n app.localarchiver.test/app.localarchiver.TestGrantActivity")).use { it.readBytes() }
        val storage = Storage(inst.targetContext)
        val end = System.currentTimeMillis() + 10000
        while (true) {
            if (runCatching { storage.treeRoot(Uri.parse("content://app.localarchiver.test.documents/tree/root")) }.isSuccess) return
            check(System.currentTimeMillis() < end) { "Document provider URI grant was not received" }
            Thread.sleep(50)
        }
    }
}
