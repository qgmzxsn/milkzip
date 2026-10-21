package app.localarchiver

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.zip.ZipFile

@RunWith(AndroidJUnit4::class)
class ArchiveIntegrationTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context = instrumentation.targetContext
    private val storage = Storage(context)
    private lateinit var dir: File
    private val engine get() = context.applicationInfo.nativeLibraryDir + "/lib7z.so"
    private open class Listener : NativeCallbacks {
        var cancel = false
        val accepted = mutableListOf<String>()
        var prompts = 0
        override fun onProgress(done: Long, total: Long) = cancel
        override fun onPassword(path: String, rejected: Boolean): CharArray? { prompts++; return null }
        override fun onPasswordAccepted(password: CharArray) { accepted.add(String(password)) }
        override fun onChoose(paths: Array<String>) = -3
    }
    @Before fun setup() { dir = File(context.cacheDir, "android-tests-${System.nanoTime()}").apply { mkdirs() } }
    @After fun cleanup() { dir.deleteRecursively() }
    private fun fixture(name: String): File = File(dir, name).also { target -> instrumentation.context.assets.open("fixtures/" + if (name.endsWith(".gz")) "$name.fixture" else name).use { input -> target.outputStream().use { input.copyTo(it) } } }
    private fun extract(file: File, folder: String, listener: NativeCallbacks = Listener(), passwords: Array<CharArray> = emptyArray(), depth: Int = 32, limit: Long = 64L shl 20, ask: Boolean = false): JSONObject = JSONObject(
        ArchiverNative.nativeExtractNested(file.path, File(dir, folder).path, engine, passwords, depth, limit, true, ask, listener))
    @Test fun renamedUnicodeNestedAndListDirectory() {
        val file = fixture("renamed.txt")
        val json = JSONObject(ArchiverNative.nativeList(file.path, engine, emptyArray(), "", Listener()))
        assertEquals(json.toString(), 0, json.getInt("code")); assertEquals(2, json.getJSONArray("entries").length())
        assertEquals(0, extract(file, "out").getInt("code"))
        assertTrue(File(dir, "out").walkTopDown().any { it.name == "你好.txt" && it.readText().contains("MilkZip") })
        val inside = fixture("inner.zip")
        val page = JSONObject(ArchiverNative.nativeList(inside.path, engine, emptyArray(), "中文目录/", Listener()))
        assertEquals("中文目录/你好.txt", page.getJSONArray("entries").getJSONObject(0).getString("path"))
    }
    @Test fun encryptedHeaderPromptsAndBookTriesInOrderWithoutLoggingPasswords() {
        val file = fixture("secret.7z")
        val callback = object : Listener() {
            override fun onPassword(path: String, rejected: Boolean): CharArray {
                prompts++; return if (prompts == 1) "wrong".toCharArray() else "安卓 密码".toCharArray()
            }
        }
        val json = extract(file, "secret", callback)
        assertEquals(json.toString(), 0, json.getInt("code")); assertEquals(2, callback.prompts)
        assertEquals(listOf("安卓 密码"), callback.accepted)
        val book = arrayOf("bad".toCharArray(), "安卓 密码".toCharArray())
        val next = Listener()
        assertEquals(0, extract(file, "from-book", next, book).getInt("code")); assertEquals(0, next.prompts)
        assertTrue(book.all { chars -> chars.all { it == '\u0000' } })
        assertFalse(json.toString().contains("安卓 密码"))
    }
    @Test fun splitCompanionsAndMissingVolume() {
        val names = instrumentation.context.assets.list("fixtures")!!.filter { it.startsWith("split.7z.") }.sorted()
        assertTrue(names.size >= 2)
        names.forEach { fixture(it) }
        assertEquals(0, extract(File(dir, names.last()), "split").getInt("code"))
        File(dir, names.last()).delete()
        assertEquals(10, extract(File(dir, names.first()), "missing").getInt("code"))
    }
    @Test fun nativeLz4AndCompressedStreams() {
        for (name in listOf("linked.lz4", "legacy.lz4", "payload.gz", "payload.bz2", "payload.xz", "payload.tar.gz", "payload.lzma", "payload.zst", "payload.Z", "rar5.rar", "carrier.mp4")) {
            val file = fixture(name); val json = extract(file, "out-$name")
            assertEquals("$name: $json", 0, json.getInt("code"))
            assertTrue(File(dir, "out-$name").walkTopDown().any { it.isFile && it.length() > 0 })
        }
    }
    @Test fun partialBranchesAndUnsafePaths() {
        val partial = extract(fixture("partial.zip"), "partial")
        assertEquals(partial.toString(), 13, partial.getInt("code"))
        assertTrue(File(dir, "partial").walkTopDown().any { it.name == "你好.txt" })
        assertEquals(7, extract(fixture("bad-path.zip"), "unsafe").getInt("code"))
        assertFalse(File(dir, "escape.txt").exists())
    }
    @Test fun cancellationLimitsIntegrityAndLayerChoice() {
        val file = fixture("renamed.txt")
        assertEquals(9, extract(file, "cancel", Listener().apply { cancel = true }).getInt("code"))
        assertEquals(12, extract(file, "limit", limit = 1).getInt("code"))
        val stop = object : Listener() { override fun onChoose(paths: Array<String>) = -1 }
        assertEquals(0, extract(file, "one-layer", stop, ask = true).getInt("code"))
        assertFalse(File(dir, "one-layer").walkTopDown().any { it.name == "你好.txt" })
        assertEquals(0, JSONObject(ArchiverNative.nativeTest(file.path, engine, emptyArray(), Listener())).getInt("code"))
        assertEquals(9, JSONObject(ArchiverNative.nativeTest(file.path, engine, emptyArray(), Listener().apply { cancel = true })).getInt("code"))
    }
    @Test fun zipStoreUnicodeFoldersAndCancellation() {
        val source = File(dir, "文件夹").apply { mkdirs() }; File(source, "你好.txt").writeText("你好 Android")
        val output = File(dir, "created.zip")
        ZipCreator(storage).create(listOf(storage.fromFile(source)), output, { false }) { _, _, _ -> }
        ZipFile(output).use { zip ->
            assertEquals("你好 Android", zip.getInputStream(zip.getEntry("文件夹/你好.txt")).bufferedReader().readText())
            assertEquals(java.util.zip.ZipEntry.STORED, zip.getEntry("文件夹/你好.txt").method)
        }
        assertEquals(0, JSONObject(ArchiverNative.nativeTest(output.path, engine, emptyArray(), Listener())).getInt("code"))
        val cancelled = File(dir, "cancelled.zip")
        try { ZipCreator(storage).create(listOf(storage.fromFile(source)), cancelled, { true }) { _, _, _ -> }; fail("must cancel") }
        catch (_: java.util.concurrent.CancellationException) { assertFalse(cancelled.exists()) }
    }
    @Test fun applicationContainersRemainFilesDuringRecursion() {
        val result = extract(fixture("documents.zip"), "documents")
        assertEquals(result.toString(), 0, result.getInt("code"))
        assertTrue(File(dir, "documents/keep.docx").isFile)
        assertFalse(File(dir, "documents").walkTopDown().any { it.name == "document.xml" })
    }
    @Test fun sharedFileProviderWithOnlyOpenableColumnsCanBeStaged() {
        val shared = File(context.cacheDir, "share/分享测试.txt").apply { parentFile!!.mkdirs() }
        instrumentation.context.assets.open("fixtures/inner.zip").use { input -> shared.outputStream().use { input.copyTo(it) } }
        try {
            val uri = androidx.core.content.FileProvider.getUriForFile(context, "${context.packageName}.files", shared)
            val node = storage.fromUri(uri)
            assertEquals("分享测试.txt", node.name); assertEquals(shared.length(), node.size)
            val staged = storage.stage(node, listOf(node), dir, { false }) { _, _, _ -> }
            assertEquals(0, JSONObject(ArchiverNative.nativeList(staged.path, engine, emptyArray(), "", Listener())).getInt("code"))
        } finally { shared.delete() }
    }
    @Test fun passwordBookEncryptedPersistenceAndOrder() {
        val book = PasswordBook(context); val old = book.read()
        try {
            book.write(listOf(" 首尾空格 ", "第二条🔒"))
            assertEquals(listOf(" 首尾空格 ", "第二条🔒"), PasswordBook(context).read())
            val bytes = File(context.filesDir, "password-book.bin").readBytes()
            assertFalse(String(bytes).contains("首尾空格")); assertFalse(String(bytes).contains("第二条"))
            book.addVerified(" 首尾空格 "); assertEquals(2, book.read().size)
            book.write(book.read().reversed()); assertEquals("第二条🔒", book.read().first())
            val imported = book.import("\uFEFF新密码\r\n新密码\n 尾空格 \n")
            assertEquals(2, imported.second); assertEquals(" 尾空格 ", imported.first.last())
            assertEquals(2, book.read().size)
        } finally { book.write(old) }
    }
}
