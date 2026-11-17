package app.localarchiver

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.test.platform.app.InstrumentationRegistry
import java.io.FileInputStream

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
