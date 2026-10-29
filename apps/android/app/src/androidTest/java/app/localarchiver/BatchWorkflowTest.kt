package app.localarchiver

import androidx.test.core.app.ActivityScenario
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class BatchWorkflowTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val context = inst.targetContext
    private val storage = Storage(context)
    private lateinit var root: File
    private lateinit var originalBook: List<String>
    @Before fun setup() {
        inst.runOnMainSync { Tasks.init(context) }
        originalBook = PasswordBook(context).read(); PasswordBook(context).write(emptyList())
        root = File(context.cacheDir, "batch-tests-${System.nanoTime()}").apply { mkdirs() }
    }
    @After fun cleanup() {
        if (Tasks.state != null) { Tasks.cancel(); waitFor { Tasks.state == null } }
        PasswordBook(context).write(originalBook); root.deleteRecursively()
    }
    private fun waitFor(condition: () -> Boolean) {
        val end = System.currentTimeMillis() + 45000
        while (System.currentTimeMillis() < end) { if (condition()) return; Thread.sleep(50) }
        error("Batch timed out: ${Tasks.state}")
    }
    private fun fixture(name: String, dest: String = name) = File(root, dest).also { target ->
        inst.context.assets.open("fixtures/$name").use { input -> target.outputStream().use { input.copyTo(it) } }
    }
    private fun submit(scenario: ActivityScenario<MainActivity>, job: Job) = scenario.onActivity { assertTrue(Tasks.submit(job)) }
    @Test fun twoVolume7zRecursesIntoEncryptedZipscWithOnlyOnePassword() {
        val first = fixture("zipsc-split.7z.001"); val second = fixture("zipsc-split.7z.002")
        var prompts = 0
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            submit(scenario, Job("extract", listOf(storage.fromFile(first)), listOf(storage.fromFile(first), storage.fromFile(second)), parent = root, name = "nested-result"))
            val end = System.currentTimeMillis() + 45000
            val answered = mutableSetOf<String>()
            while (Tasks.state != null && System.currentTimeMillis() < end) {
                Tasks.prompt?.takeIf { answered.add(it.id) }?.let { prompt ->
                    prompts++; scenario.onActivity { Tasks.reply(prompt.id, PromptReply("batch-secret".toCharArray())) }
                }
                Thread.sleep(50)
            }
            waitFor { Tasks.state == null }
            assertEquals(Tasks.records.first().detail, 0, Tasks.records.first().code)
            assertEquals(1, prompts)
            assertEquals(128L * 1024, File(root, "nested-result/payload.bin").length())
            val bytes = File(root, "nested-result/payload.bin").readBytes()
            assertEquals("451b4fb8c03c26790b4964e8b9b86c0e724f9f4d6d73ea630d0853fc710a011e", java.security.MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) })
            assertTrue(first.exists()); assertTrue(second.exists())
        }
    }
    @Test fun batchReusesUnsavedPasswordContinuesBadSiblingAndDeletesOnlySuccessfulInputs() {
        val one = fixture("nested-zip-zipsc.zip", "one.zip")
        val bad = File(root, "bad.zip").apply { writeText("broken zip") }
        val two = fixture("nested-7z-zipsc.zip", "two.zip")
        val nodes = listOf(one, bad, two).map(storage::fromFile)
        var prompts = 0
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            submit(scenario, Job("extractBatch", nodes, nodes, settings = ExtractSettings(deleteSource = true), parent = root))
            val answered = mutableSetOf<String>(); val end = System.currentTimeMillis() + 45000
            while (Tasks.state != null && System.currentTimeMillis() < end) {
                Tasks.prompt?.takeIf { answered.add(it.id) }?.let { prompt ->
                    prompts++; scenario.onActivity { Tasks.reply(prompt.id, PromptReply("batch-secret".toCharArray())) }
                }
                Thread.sleep(50)
            }
            waitFor { Tasks.state == null }
            val result = Tasks.records.first()
            assertEquals(result.detail, 13, result.code); assertEquals(1, prompts)
            assertFalse(one.exists()); assertFalse(two.exists()); assertTrue(bad.exists())
            assertEquals(3, result.outputs.size)
            assertEquals(2, result.outputs.count { File(it, "payload.bin").length() == 128L * 1024 })
            assertTrue(PasswordBook(context).read().isEmpty())
        }
    }
    @Test fun seededBatchPasswordWinsOverFullBookAndIsWiped() {
        PasswordBook(context).write((0 until 64).map { "wrong-$it" })
        val one = fixture("nested-zip-zipsc.zip", "one.zip"); val two = fixture("nested-7z-zipsc.zip", "two.zip")
        val nodes = listOf(one, two).map(storage::fromFile); val password = "batch-secret".toCharArray()
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            submit(scenario, Job("extractBatch", nodes, nodes, parent = root, initialPassword = password))
            waitFor { Tasks.state == null || Tasks.prompt != null }
            assertNull("A seeded matching password should not prompt", Tasks.prompt)
            waitFor { Tasks.state == null }
            assertEquals(Tasks.records.first().detail, 0, Tasks.records.first().code)
            assertTrue(password.all { it == '\u0000' }); assertTrue(one.exists()); assertTrue(two.exists())
        }
    }
    @Test fun failedExportAndCancelledExtractionKeepOriginals() {
        val one = fixture("inner.zip")
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            submit(scenario, Job("extract", listOf(storage.fromFile(one)), settings = ExtractSettings(deleteSource = true), parent = root,
                name = "export-failed", exportTree = "content://app.localarchiver.test.documents/tree/does-not-exist"))
            waitFor { Tasks.state == null }; assertTrue(one.exists()); assertNotEquals(0, Tasks.records.first().code)
            val secret = fixture("secret.7z")
            submit(scenario, Job("extract", listOf(storage.fromFile(secret)), settings = ExtractSettings(deleteSource = true), parent = root, name = "cancelled"))
            waitFor { Tasks.prompt != null }; Tasks.cancel(); waitFor { Tasks.state == null }
            assertEquals(9, Tasks.records.first().code); assertTrue(secret.exists())
        }
    }
    @Test fun grantedFolderReceivesCompleteOutputBeforeSourceIsDeleted() {
        DocumentFixture.grant()
        val tree = android.net.Uri.parse("content://app.localarchiver.test.documents/tree/root")
        val parent = storage.treeRoot(tree)
        val folder = android.provider.DocumentsContract.createDocument(context.contentResolver, android.net.Uri.parse(parent.location), android.provider.DocumentsContract.Document.MIME_TYPE_DIR, "batch-export-${System.nanoTime()}")!!
        try {
            val source = android.provider.DocumentsContract.createDocument(context.contentResolver, folder, "application/zip", "source.zip")!!
            inst.context.assets.open("fixtures/nested-zip-zipsc.zip").use { input -> context.contentResolver.openOutputStream(source)!!.use { input.copyTo(it) } }
            val node = storage.fromUri(source, tree.toString()).copy(parent = folder.toString())
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                submit(scenario, Job("extractBatch", listOf(node), settings = ExtractSettings(deleteSource = true), sameDirectory = true, initialPassword = "batch-secret".toCharArray()))
                waitFor { Tasks.state == null }
                val record = Tasks.records.first(); assertEquals(record.detail, 0, record.code)
                val children = storage.children(storage.fromUri(folder, tree.toString()))
                assertFalse(children.any { it.name == "source.zip" })
                val exported = storage.children(children.single { it.directory })
                assertEquals(128L * 1024, exported.single { it.name == "payload.bin" }.size)
                record.outputs.forEach { File(it).deleteRecursively() }
            }
        } finally { android.provider.DocumentsContract.deleteDocument(context.contentResolver, folder) }
    }
    @Test fun ordinaryFilesAreSkippedWithoutCreatingEmptyResults() {
        val good = fixture("inner.zip"); val text = File(root, "readme.txt").apply { writeText("ordinary text") }
        val nodes = listOf(text, good).map(storage::fromFile)
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            submit(scenario, Job("extractBatch", nodes, nodes, parent = root))
            waitFor { Tasks.state == null }
            assertEquals(Tasks.records.first().detail, 0, Tasks.records.first().code)
            assertEquals(1, Tasks.records.first().outputs.size); assertTrue(Tasks.records.first().detail.contains("跳过 1"))
            assertTrue(text.exists()); assertFalse(File(root, "readme.txt_解压").exists())
        }
    }
    @Test fun splitPartsAreExtractedAndDeletedAsOneBatchInput() {
        val first = fixture("zipsc-split.7z.001"); val second = fixture("zipsc-split.7z.002")
        val nodes = listOf(second, first).map(storage::fromFile)
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            submit(scenario, Job("extractBatch", nodes, nodes, settings = ExtractSettings(deleteSource = true), parent = root, initialPassword = "batch-secret".toCharArray()))
            waitFor { Tasks.state == null }
            assertEquals(Tasks.records.first().detail, 0, Tasks.records.first().code)
            assertEquals(1, Tasks.records.first().outputs.size); assertFalse(first.exists()); assertFalse(second.exists())
        }
    }
}
