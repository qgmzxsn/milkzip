package app.localarchiver

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import java.io.File

/** Instrumentation-only provider. It exposes a disposable directory, never user data. */
class TestDocumentsProvider : DocumentsProvider() {
    private val base get() = File(context!!.cacheDir, "test-documents").apply { mkdirs() }
    override fun onCreate() = true
    private fun file(id: String): File {
        val f = if (id == "root") base else File(base, id.removePrefix("root/"))
        require(f.canonicalPath == base.canonicalPath || f.canonicalPath.startsWith(base.canonicalPath + "/"))
        return f
    }
    private fun id(f: File) = if (f == base) "root" else "root/" + f.relativeTo(base).path
    private val columns = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS)
    private fun add(cursor: MatrixCursor, f: File) {
        val values = mapOf(Document.COLUMN_DOCUMENT_ID to id(f), Document.COLUMN_DISPLAY_NAME to f.name,
            Document.COLUMN_MIME_TYPE to if (f.isDirectory) Document.MIME_TYPE_DIR else "application/octet-stream",
            Document.COLUMN_SIZE to f.length(), Document.COLUMN_LAST_MODIFIED to f.lastModified(),
            Document.COLUMN_FLAGS to (Document.FLAG_SUPPORTS_WRITE or Document.FLAG_SUPPORTS_DELETE or if (f.isDirectory) Document.FLAG_DIR_SUPPORTS_CREATE else 0))
        cursor.addRow(cursor.columnNames.map { values[it] }.toTypedArray())
    }
    override fun queryRoots(projection: Array<out String>?): Cursor = MatrixCursor(projection ?: arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_FLAGS)).apply {
        val values = mapOf(Root.COLUMN_ROOT_ID to "root", Root.COLUMN_DOCUMENT_ID to "root", Root.COLUMN_TITLE to "MilkZip test documents", Root.COLUMN_FLAGS to Root.FLAG_SUPPORTS_CREATE)
        addRow(columnNames.map { values[it] }.toTypedArray())
    }
    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor = MatrixCursor(projection ?: columns).apply { add(this, file(documentId)) }
    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor = MatrixCursor(projection ?: columns).apply { file(parentDocumentId).listFiles()?.forEach { add(this, it) } }
    override fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor = ParcelFileDescriptor.open(file(documentId), ParcelFileDescriptor.parseMode(mode))
    override fun createDocument(parentDocumentId: String, mimeType: String, displayName: String): String {
        require(displayName.isNotBlank() && displayName !in listOf(".", "..") && !displayName.contains('/') && !displayName.contains('\\'))
        val f = File(file(parentDocumentId), displayName)
        check(if (mimeType == Document.MIME_TYPE_DIR) f.mkdir() else f.createNewFile())
        return id(f)
    }
    override fun deleteDocument(documentId: String) { check(file(documentId) != base && file(documentId).deleteRecursively()) }
    override fun isChildDocument(parentDocumentId: String, documentId: String) = file(documentId).canonicalPath.startsWith(file(parentDocumentId).canonicalPath + "/")
}
