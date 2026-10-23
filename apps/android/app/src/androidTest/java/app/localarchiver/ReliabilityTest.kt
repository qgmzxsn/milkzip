package app.localarchiver

import android.net.Uri
import android.os.Bundle
import android.os.Process
import android.provider.DocumentsContract
import androidx.test.core.app.ActivityScenario
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.action.ViewActions.*
import androidx.test.espresso.matcher.ViewMatchers.*
import androidx.test.espresso.matcher.RootMatchers.isDialog
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CancellationException

@RunWith(AndroidJUnit4::class)
class ReliabilityTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val context = inst.targetContext
    private val storage = Storage(context)
    private open class Listener : NativeCallbacks {
        override fun onProgress(done: Long, total: Long) = false
        override fun onPassword(path: String, rejected: Boolean): CharArray? = null
        override fun onPasswordAccepted(password: CharArray) { }
        override fun onChoose(paths: Array<String>) = -3
    }
    private fun fixture(name: String, root: File) = File(root, name).also { out ->
        inst.context.assets.open("fixtures/$name").use { input -> out.outputStream().use { input.copyTo(it) } }
    }
    private fun invoke(root: File, source: File, dest: File? = null, listener: NativeCallbacks = Listener()): JSONObject {
        val args = Bundle().apply {
            putString("kind", if (dest == null) "test" else "extract"); putString("path", source.path)
            putString("dest", dest?.path); putInt("depth", 32); putLong("bytes", 64L shl 20); putBoolean("keep", true)
        }
        return JSONObject(NativeWorker.run(context, args, emptyArray(), File(root, "response.json"), listener))
    }
    @Test fun deleteFoldersWithoutFollowingLinksAndCancelBeforeDeletion() {
        val root = storage.jobDirectory()
        try {
            val outside = File(root, "keep.txt").apply { writeText("keep") }
            val folder = File(root, "remove").apply { mkdir() }
            File(folder, "nested").mkdir(); File(folder, "nested/file.txt").writeText("remove")
            Files.createSymbolicLink(File(folder, "link").toPath(), outside.toPath())
            storage.delete(storage.fromFile(folder), { false }) { }
            assertFalse(folder.exists()); assertEquals("keep", outside.readText())
            try { storage.delete(storage.fromFile(outside), { true }) { }; fail("must cancel") }
            catch (_: CancellationException) { assertTrue(outside.exists()) }
            try { storage.checkDeletable(storage.fromFile(storage.results)); fail("protect root") }
            catch (_: IllegalArgumentException) { }
        } finally { root.deleteRecursively() }
    }
    @Test fun deleteAuthorizedDocumentFoldersAndProtectTreeRoot() {
        DocumentFixture.grant()
        val tree = Uri.parse("content://app.localarchiver.test.documents/tree/root")
        val root = storage.treeRoot(tree)
        val folder = DocumentsContract.createDocument(context.contentResolver, Uri.parse(root.location), DocumentsContract.Document.MIME_TYPE_DIR, "delete-${System.nanoTime()}")!!
        val nested = DocumentsContract.createDocument(context.contentResolver, folder, DocumentsContract.Document.MIME_TYPE_DIR, "nested")!!
        DocumentsContract.createDocument(context.contentResolver, nested, "text/plain", "file.txt")!!
        storage.delete(storage.fromUri(folder, tree.toString()), { false }) { }
        assertFalse(storage.children(root).any { it.location == folder.toString() })
        try { storage.delete(root, { false }) { }; fail("protect tree root") } catch (_: IllegalArgumentException) { }
    }
    @Test fun nativeProcessDeathIsReportedAndNextTaskWorks() {
        val root = storage.jobDirectory()
        try {
            val file = fixture("bcj2.7z", root)
            var killed = false
            val listener = object : Listener() {
                override fun onProgress(done: Long, total: Long): Boolean {
                    if (!killed) {
                        val pid = NativeWorker.pid
                        assertTrue(pid > 0 && pid != Process.myPid())
                        killed = true; Process.killProcess(pid)
                    }
                    return false
                }
            }
            try { invoke(root, file, File(root, "interrupted"), listener); fail("must report engine death") }
            catch (error: NativeWorkerException) { assertTrue(error.message.orEmpty().contains("解压引擎")) }
            assertTrue(killed)
            val result = invoke(root, file, File(root, "recovered"))
            assertEquals(result.toString(), 0, result.getInt("code"))
            assertEquals(3000000L, File(root, "recovered/machine.bin").length())
        } finally { root.deleteRecursively() }
    }
    @Test fun promptedPasswordCrossesProcessAndReplyIsWiped() {
        val root = storage.jobDirectory()
        val reply = "安卓 密码".toCharArray()
        try {
            val source = fixture("secret.7z", root)
            val listener = object : Listener() {
                override fun onPassword(path: String, rejected: Boolean) = reply
            }
            val result = invoke(root, source, File(root, "secret"), listener)
            assertEquals(result.toString(), 0, result.getInt("code"))
            assertTrue(reply.all { it == '\u0000' })
        } finally { reply.fill('\u0000'); root.deleteRecursively() }
    }
    @Test fun thirtyTwoLayer7zAndExecutableContainers() {
        val root = storage.jobDirectory()
        try {
            for (name in listOf("deep-32.7z", "seven-sfx.exe", "nsis.exe", "cab-sfx.exe")) {
                val file = fixture(name, root)
                val result = invoke(root, file, File(root, "out-$name"))
                assertEquals("$name: $result", 0, result.getInt("code"))
                val final = File(root, "out-$name/${when (name) { "nsis.exe" -> "nsis-payload.txt"; "cab-sfx.exe" -> "cab-payload.txt"; else -> "deep-final.txt" }}")
                assertTrue(final.path, final.isFile)
            }
        } finally { root.deleteRecursively() }
    }
    private fun awaitDeleteConfirmation() {
        val end = System.currentTimeMillis() + 10000
        while (System.currentTimeMillis() < end) {
            inst.waitForIdleSync()
            if (inst.uiAutomation.rootInActiveWindow?.findAccessibilityNodeInfosByText("删除 1 项？")?.isNotEmpty() == true) return
            Thread.sleep(50)
        }
        fail("Delete confirmation did not become the active window")
    }
    @Test fun longPressDeleteCanBeCancelledAndConfirmed() {
        val file = File(storage.results, "删除测试.txt").apply { writeText("delete fixture") }
        context.getSharedPreferences("mobile-settings", android.content.Context.MODE_PRIVATE).edit().putBoolean("notification-asked", true).commit()
        ActivityScenario.launch(MainActivity::class.java).use {
            onView(withText("结果")).perform(click())
            val until = System.currentTimeMillis() + 10000
            while (System.currentTimeMillis() < until && inst.uiAutomation.rootInActiveWindow?.findAccessibilityNodeInfosByText(file.name)?.isEmpty() != false) Thread.sleep(100)
            onView(withText(file.name)).perform(longClick())
            onView(withText("删除")).inRoot(isDialog()).perform(click())
            awaitDeleteConfirmation()
            onView(withId(android.R.id.button2)).inRoot(isDialog()).perform(click()); assertTrue(file.exists())
            onView(withText(file.name)).perform(longClick()); onView(withText("删除")).inRoot(isDialog()).perform(click())
            awaitDeleteConfirmation()
            onView(withId(android.R.id.button1)).inRoot(isDialog()).perform(click())
            val end = System.currentTimeMillis() + 10000
            while (System.currentTimeMillis() < end && (file.exists() || Tasks.state != null)) Thread.sleep(100)
            assertFalse(file.exists()); assertEquals(0, Tasks.records.first().code)
        }
        file.delete()
    }
}
