package app.localarchiver

import android.net.Uri
import android.os.Build
import android.provider.DocumentsContract
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class DocumentStorageTest {
    @Test fun stageSplitDocumentsAndExportWithoutOverwrite() {
        val inst = InstrumentationRegistry.getInstrumentation()
        DocumentFixture.grant()
        val context = inst.targetContext; val storage = Storage(context)
        val tree = Uri.parse("content://app.localarchiver.test.documents/tree/root")
        val root = storage.treeRoot(tree)
        val folder = DocumentsContract.createDocument(context.contentResolver, Uri.parse(root.location), DocumentsContract.Document.MIME_TYPE_DIR, "roundtrip-${System.nanoTime()}")!!
        val scratch = storage.jobDirectory()
        try {
            val source = DocumentsContract.createDocument(context.contentResolver, folder, "application/octet-stream", "archive.7z.001")!!
            val second = DocumentsContract.createDocument(context.contentResolver, folder, "application/octet-stream", "archive.7z.002")!!
            context.contentResolver.openOutputStream(source)!!.use { it.write("first".toByteArray()) }
            context.contentResolver.openOutputStream(second)!!.use { it.write("second".toByteArray()) }
            val parent = storage.fromUri(folder, tree.toString()); val siblings = storage.children(parent)
            assertEquals(2, siblings.size)
            val staged = storage.stage(siblings.first { it.name.endsWith("001") }, siblings, scratch, { false }) { _, _, _ -> }
            assertEquals("first", staged.readText()); assertEquals("second", File(staged.parentFile, "archive.7z.002").readText())
            val result = File(scratch, "解压结果").apply { mkdirs() }; File(result, "中文.txt").writeText("导出内容")
            val targetTree = DocumentsContract.buildTreeDocumentUri("app.localarchiver.test.documents", DocumentsContract.getDocumentId(folder))
            val firstExport = storage.export(result, targetTree, { false }) { _, _, _ -> }
            storage.export(result, targetTree, { false }) { _, _, _ -> }
            val exported = storage.children(storage.fromUri(firstExport, targetTree.toString()))
            assertEquals("导出内容", storage.input(exported.single()).bufferedReader().use { it.readText() })
            assertTrue(storage.children(parent).any { it.name == "解压结果 (1)" })
        } finally {
            DocumentsContract.deleteDocument(context.contentResolver, folder)
            scratch.deleteRecursively()
        }
    }
}
