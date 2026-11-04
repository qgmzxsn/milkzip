package app.localarchiver

import android.content.Context
import androidx.test.core.app.ActivityScenario
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.Espresso.onData
import androidx.test.espresso.action.ViewActions.*
import androidx.test.espresso.matcher.ViewMatchers.*
import androidx.test.espresso.matcher.RootMatchers.isDialog
import androidx.test.espresso.matcher.RootMatchers.isPlatformPopup
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.hamcrest.Matchers.equalTo
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class DisplaySelectionTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val context = inst.targetContext
    private val prefs get() = context.getSharedPreferences("mobile-settings", Context.MODE_PRIVATE)
    private fun waitFor(condition: () -> Boolean) {
        val end = System.currentTimeMillis() + 30000
        while (System.currentTimeMillis() < end) { inst.waitForIdleSync(); if (condition()) return; Thread.sleep(50) }
        error("UI timed out")
    }
    private fun hasText(value: String) = inst.uiAutomation.rootInActiveWindow?.findAccessibilityNodeInfosByText(value)?.isNotEmpty() == true
    private fun screenshot(name: String) {
        fun shell(command: String) = inst.uiAutomation.executeShellCommand(command).use { android.os.ParcelFileDescriptor.AutoCloseInputStream(it).readBytes() }
        shell("mkdir -p /sdcard/Download/MilkZipScreenshots")
        shell("screencap -p /sdcard/Download/MilkZipScreenshots/$name.png")
    }
    @Test fun listGridExpandableFoldersAndPalettesSurviveRecreation() {
        val root = File(Storage(context).results, "0000-外观测试").apply { mkdirs() }
        val child = File(root, "子目录").apply { mkdir() }; File(child, "展开后的文件.txt").writeText("tree")
        File(root, "普通文件.txt").writeText("file")
        prefs.edit().putString("view-mode", "list").putString("palette", "green").commit()
        try {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                onView(withText("结果")).perform(click()); waitFor { hasText(root.name) }
                onView(withText(root.name)).perform(click()); waitFor { hasText("普通文件.txt") }
                onView(withText("视图")).perform(click())
                onView(withContentDescription("文件展示方式")).inRoot(isDialog()).perform(click())
                waitFor { hasText("图标网格") }; onData(equalTo("图标网格")).inRoot(isPlatformPopup()).perform(click())
                onView(withContentDescription("界面配色")).inRoot(isDialog()).perform(click())
                waitFor { hasText("海洋蓝") }; onData(equalTo("海洋蓝")).inRoot(isPlatformPopup()).perform(click())
                var previous: MainActivity? = null; scenario.onActivity { previous = it }
                onView(withText("应用")).inRoot(isDialog()).perform(click())
                waitFor { var changed = false; runCatching { scenario.onActivity { changed = it !== previous && it.window.decorView.hasWindowFocus() } }; changed }
                waitFor { prefs.getString("view-mode", "") == "grid" && hasText("普通文件.txt") }
                screenshot("06-grid-blue")
                scenario.recreate(); waitFor { hasText("普通文件.txt") }; assertEquals("grid", prefs.getString("view-mode", ""))
                onView(withText("视图")).perform(click())
                onView(withContentDescription("文件展示方式")).inRoot(isDialog()).perform(click())
                waitFor { hasText("原位展开文件夹") }; onData(equalTo("原位展开文件夹")).inRoot(isPlatformPopup()).perform(click())
                onView(withContentDescription("界面配色")).inRoot(isDialog()).perform(click())
                waitFor { hasText("简约黑白") }; onData(equalTo("简约黑白")).inRoot(isPlatformPopup()).perform(click())
                scenario.onActivity { previous = it }
                onView(withText("应用")).inRoot(isDialog()).perform(click())
                waitFor { var changed = false; runCatching { scenario.onActivity { changed = it !== previous && it.window.decorView.hasWindowFocus() } }; changed }
                waitFor { hasText("▸ 子目录") }
                onView(withText("▸ 子目录")).perform(click()); waitFor { hasText("展开后的文件.txt") }
                screenshot("07-expanded-mono")
                scenario.recreate(); waitFor { hasText("展开后的文件.txt") }
                onView(withText("多选")).perform(click()); onView(withText("全选")).perform(click())
                screenshot("08-select-all")
                onView(withText("删除")).perform(click()); onView(withText("取消")).inRoot(isDialog()).perform(click())
                assertTrue(child.exists()); assertTrue(File(child, "展开后的文件.txt").exists())
            }
        } finally { prefs.edit().putString("view-mode", "list").putString("palette", "green").commit(); root.deleteRecursively() }
    }
    @Test fun visibleSelectAllAndBatchButtonLaunchOneOperation() {
        val storage = Storage(context); val root = File(storage.results, "0000-批量入口").apply { mkdirs() }
        for (name in listOf("a.zip", "b.zip")) inst.context.assets.open("fixtures/inner.zip").use { input -> File(root, name).outputStream().use { input.copyTo(it) } }
        prefs.edit().putString("view-mode", "list").commit()
        try {
            ActivityScenario.launch(MainActivity::class.java).use {
                onView(withText("结果")).perform(click()); waitFor { hasText(root.name) }; onView(withText(root.name)).perform(click()); waitFor { hasText("a.zip") }
                onView(withText("多选")).perform(click()); onView(withText("全选")).perform(click()); onView(withText("批量解压")).perform(click())
                waitFor { hasText("批量解压 2 组") }
                onView(withText("开始批量解压")).inRoot(isDialog()).perform(click())
                waitFor { Tasks.state == null && Tasks.records.firstOrNull()?.title == "批量解压 2 组" }
                assertEquals(Tasks.records.first().detail, 0, Tasks.records.first().code)
                assertEquals(2, Tasks.records.first().outputs.size)
                assertTrue(File(root, "a.zip").exists()); assertTrue(File(root, "b.zip").exists())
            }
        } finally { if (Tasks.state != null) { Tasks.cancel(); waitFor { Tasks.state == null } }; root.deleteRecursively() }
    }
}
