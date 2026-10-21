package app.localarchiver

import android.graphics.Bitmap
import android.os.Build
import android.app.UiModeManager
import android.content.Context
import androidx.test.core.app.ActivityScenario
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.action.ViewActions.*
import androidx.test.espresso.matcher.ViewMatchers.*
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.hamcrest.Matchers.equalTo
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class UiWorkflowTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val context = inst.targetContext
    private fun waitFor(condition: () -> Boolean) {
        val end = System.currentTimeMillis() + 30000
        while (System.currentTimeMillis() < end) { inst.waitForIdleSync(); if (condition()) return; Thread.sleep(100) }
        error("Timed out waiting for Android workflow: ${Tasks.state}")
    }
    private fun screenshot(name: String) {
        inst.waitForIdleSync()
        val folder = File(context.getExternalFilesDir(null), "screenshots").apply { mkdirs() }
        val bitmap = requireNotNull(inst.uiAutomation.takeScreenshot()) { "Emulator screenshot unavailable" }
        File(folder, "$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
        // Gradle uninstalls the tested app afterwards; retain screenshots in shared Downloads.
        android.os.ParcelFileDescriptor.AutoCloseInputStream(inst.uiAutomation.executeShellCommand("mkdir -p /sdcard/Download/MilkZipScreenshots && cp '${File(folder, "$name.png").path}' /sdcard/Download/MilkZipScreenshots/$name.png")).use { it.readBytes() }
    }
    @org.junit.After fun finishPendingTask() {
        if (Tasks.state != null) { Tasks.cancel(); waitFor { Tasks.state == null } }
    }
    @Test fun cachedDocumentsSurviveFolderBrowsingAndAreReplacedSafely() {
        DocumentFixture.grant()
        val storage = Storage(context)
        val tree = android.net.Uri.parse("content://app.localarchiver.test.documents/tree/root")
        val root = storage.treeRoot(tree)
        val folder = android.provider.DocumentsContract.createDocument(context.contentResolver, android.net.Uri.parse(root.location), android.provider.DocumentsContract.Document.MIME_TYPE_DIR, "cache-test-${System.nanoTime()}")!!
        fun document(name: String): Node {
            val uri = android.provider.DocumentsContract.createDocument(context.contentResolver, folder, "application/zip", name)!!
            inst.context.assets.open("fixtures/inner.zip").use { input -> context.contentResolver.openOutputStream(uri)!!.use { input.copyTo(it) } }
            return storage.fromUri(uri, tree.toString())
        }
        try {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                val first = document("first.zip")
                scenario.onActivity { assertTrue(Tasks.submit(Job("list", listOf(first)))) }
                waitFor { Tasks.state == null && Tasks.page?.source?.location == first.location }
                val cached = Tasks.page!!.staged
                assertTrue(cached.exists())
                val version = Tasks.pageVersion
                scenario.onActivity { assertTrue(Tasks.submit(Job("list", listOf(first), prefix = "中文目录/", staged = cached))) }
                waitFor { Tasks.state == null && Tasks.pageVersion > version }
                assertTrue(cached.exists()); assertEquals(cached, Tasks.page!!.staged)
                val next = document("second.zip")
                scenario.onActivity { assertTrue(Tasks.submit(Job("list", listOf(next)))) }
                waitFor { Tasks.state == null && Tasks.page?.source?.location == next.location }
                assertFalse(cached.exists()); assertTrue(Tasks.page!!.staged.exists())
            }
        } finally {
            android.provider.DocumentsContract.deleteDocument(context.contentResolver, folder)
        }
    }
    @Test fun browseExtractRotatePasswordPromptAndShowReports() {
        val storage = Storage(context)
        val archive = File(storage.results, "嵌套压缩包.txt")
        inst.context.assets.open("fixtures/renamed.txt").use { input -> archive.outputStream().use { input.copyTo(it) } }
        File(storage.results, "照片与文档").mkdirs()
        File(storage.results, "使用说明.txt").writeText("MilkZip Android")
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            onView(withText("结果")).perform(click())
            screenshot("01-files")
            scenario.onActivity { assertTrue(Tasks.submit(Job("list", listOf(storage.fromFile(archive))))) }
            waitFor { Tasks.state == null && Tasks.page?.source?.location == archive.path }
            screenshot("02-archive")
            onView(withText("解压")).perform(click()); screenshot("03-extract-options")
            onView(withText("取消")).perform(click())
            val encrypted = File(storage.results, "加密测试.7z")
            inst.context.assets.open("fixtures/secret.7z").use { input -> encrypted.outputStream().use { input.copyTo(it) } }
            scenario.onActivity { assertTrue(Tasks.submit(Job("extract", listOf(storage.fromFile(encrypted))))) }
            waitFor { Tasks.prompt != null }
            scenario.recreate()
            waitFor { inst.uiAutomation.rootInActiveWindow?.findAccessibilityNodeInfosByText("需要解压密码")?.isNotEmpty() == true }
            onView(withHint("密码（保留首尾空格）")).inRoot(androidx.test.espresso.matcher.RootMatchers.isDialog()).perform(replaceText("安卓 密码"), closeSoftKeyboard())
            onView(withText("继续")).inRoot(androidx.test.espresso.matcher.RootMatchers.isDialog()).perform(click())
            waitFor { Tasks.state == null }
            assertEquals(Tasks.records.first().detail, 0, Tasks.records.first().code)
            onView(withTagValue(equalTo("nav1"))).perform(click())
            screenshot("04-tasks")
            onView(withTagValue(equalTo("nav2"))).perform(click())
            onView(withText("添加")).perform(click())
            onView(withHint("密码（保留首尾空格）")).perform(replaceText(" 测试密码 "), closeSoftKeyboard())
            onView(withText("确定")).perform(click())
            onView(withText("保存 *")).perform(click())
            assertTrue(PasswordBook(context).read().contains(" 测试密码 "))
            // Password pages intentionally block screenshots. Verify controls and encrypted storage instead.
            onView(withTagValue(equalTo("nav0"))).perform(click())
            onView(withText("结果")).perform(click())
            if (Build.VERSION.SDK_INT >= 31) {
                scenario.onActivity { (it.getSystemService(Context.UI_MODE_SERVICE) as UiModeManager).setApplicationNightMode(UiModeManager.MODE_NIGHT_YES) }
                inst.waitForIdleSync(); screenshot("05-files-dark")
                scenario.onActivity { (it.getSystemService(Context.UI_MODE_SERVICE) as UiModeManager).setApplicationNightMode(UiModeManager.MODE_NIGHT_AUTO) }
            }
        }
    }
}
