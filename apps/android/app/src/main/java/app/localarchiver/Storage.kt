package app.localarchiver

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import java.io.File
import java.io.InputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CancellationException

class Storage(private val context: Context) {
    val results: File get() = File(context.getExternalFilesDir(null) ?: context.filesDir, "Results").apply { mkdirs() }
    val shared: File get() = Environment.getExternalStorageDirectory()
    val allFiles: Boolean get() = if (Build.VERSION.SDK_INT >= 30) Environment.isExternalStorageManager() else
        context.checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED
    fun fromFile(file: File) = Node(file.absolutePath, file.name.ifEmpty { "内部存储" }, file.isDirectory, file.length(), file.lastModified(), parent = file.absoluteFile.parent)
    fun fromUri(uri: Uri, tree: String? = null): Node {
        if (uri.scheme == "file") return fromFile(File(requireNotNull(uri.path)))
        val columns = arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_LAST_MODIFIED)
        fun metadata(projection: Array<String>): Node? = context.contentResolver.query(uri, projection, null, null, null)?.use { c ->
            if (!c.moveToFirst()) return@use null
            val name = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            val size = c.getColumnIndex(OpenableColumns.SIZE)
            val mime = c.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
            val modified = c.getColumnIndex(DocumentsContract.Document.COLUMN_LAST_MODIFIED)
            if (name < 0) return@use null
            Node(uri.toString(), c.getString(name) ?: "archive",
                mime >= 0 && c.getString(mime) == DocumentsContract.Document.MIME_TYPE_DIR,
                if (size < 0 || c.isNull(size)) -1 else c.getLong(size),
                if (modified < 0 || c.isNull(modified)) 0 else c.getLong(modified), tree)
        }
        try { metadata(columns)?.let { return it } }
        catch (_: IllegalArgumentException) { /* Shared-file providers may reject extra columns. */ }
        catch (_: UnsupportedOperationException) { }
        metadata(arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE))?.let { return it }
        throw IOException("无法读取文件信息，请重新授权")
    }
    fun treeRoot(uri: Uri) = fromUri(DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri)), uri.toString())
    fun children(node: Node): List<Node> {
        require(node.directory)
        if (!node.isDocument) return node.file.listFiles()?.filter { !java.nio.file.Files.isSymbolicLink(it.toPath()) }?.map(::fromFile)
            ?: throw IOException("无法读取目录，请授予文件访问权限")
        val tree = Uri.parse(requireNotNull(node.tree) { "请通过“授权文件夹”打开这个目录" })
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(Uri.parse(node.location)))
        val columns = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_LAST_MODIFIED)
        return buildList {
            context.contentResolver.query(uri, columns, null, null, null)?.use { c ->
                while (c.moveToNext()) add(Node(DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0)).toString(), c.getString(1) ?: "file",
                    c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR, if (c.isNull(3)) -1 else c.getLong(3), if (c.isNull(4)) 0 else c.getLong(4), node.tree, node.location))
            } ?: throw IOException("无法读取授权目录")
        }
    }
    fun input(node: Node): InputStream = if (node.isDocument) context.contentResolver.openInputStream(Uri.parse(node.location))
        ?: throw IOException("无法读取 ${node.name}") else node.file.inputStream()
    fun related(source: Node, siblings: List<Node>): List<Node> = ArchivePlan.companions(source, siblings)
    fun available(file: File) = file.usableSpace
    fun checkSpace(file: File, size: Long) {
        if (size > 0 && size + (16L shl 20) > file.usableSpace) throw IOException("存储空间不足，需要约 ${Rules.size(size)}")
    }
    fun copy(input: InputStream, output: java.io.OutputStream, cancelled: () -> Boolean, progress: (Long) -> Unit = {}): Long {
        val buffer = ByteArray(256 * 1024); var total = 0L
        while (true) {
            if (cancelled()) throw CancellationException()
            val n = input.read(buffer); if (n < 0) break
            output.write(buffer, 0, n); total += n; progress(total)
        }
        return total
    }
    fun stage(source: Node, siblings: List<Node>, jobDirectory: File, cancelled: () -> Boolean, progress: (String, Long, Long) -> Unit): File {
        if (!source.isDocument) return source.file
        val folder = File(jobDirectory, "input").apply { mkdirs() }
        val volumes = related(source, siblings)
        require(volumes.map { Rules.safeName(it.name) }.distinct().size == volumes.size) { "同名文件来自不同目录，请只选择一组分卷" }
        checkSpace(folder, volumes.sumOf { it.size.coerceAtLeast(0) })
        for (volume in volumes) {
            val dest = File(folder, Rules.safeName(volume.name))
            input(volume).use { stream -> dest.outputStream().use { copy(stream, it, cancelled) { n -> progress("读取 ${volume.name}", n, volume.size) } } }
        }
        return File(folder, Rules.safeName(source.name))
    }
    fun uniqueDirectory(parent: File, preferred: String): File {
        Rules.safeName(preferred)
        parent.mkdirs()
        for (i in 0..9999) {
            val candidate = File(parent, if (i == 0) preferred else "$preferred ($i)")
            if (candidate.mkdir()) return candidate
            if (!candidate.exists()) throw IOException("无法创建输出目录")
        }
        throw IOException("同名结果过多")
    }
    fun export(directory: File, target: Uri, cancelled: () -> Boolean, progress: (String, Long, Long) -> Unit): Uri {
        return exportToFolder(directory, treeRoot(target), cancelled, progress)
    }
    fun exportToFolder(directory: File, parent: Node, cancelled: () -> Boolean, progress: (String, Long, Long) -> Unit): Uri {
        val existing = children(parent).map { it.name }.toSet()
        var name = directory.name; var index = 1
        while (name in existing) { name = "${directory.name} (${index++})" }
        val root = DocumentsContract.createDocument(context.contentResolver, Uri.parse(parent.location), DocumentsContract.Document.MIME_TYPE_DIR, name)
            ?: throw IOException("不能在选定目录创建结果")
        // A newly created provider directory is retained if copying is cancelled.
        fun transfer(from: File, to: Uri) {
            from.listFiles()?.sortedBy { it.name }?.forEach { file ->
                if (cancelled()) throw CancellationException()
                if (java.nio.file.Files.isSymbolicLink(file.toPath())) throw IOException("结果中出现符号链接，已停止导出")
                val child = DocumentsContract.createDocument(context.contentResolver, to,
                    if (file.isDirectory) DocumentsContract.Document.MIME_TYPE_DIR else "application/octet-stream", file.name)
                    ?: throw IOException("无法创建 ${file.name}")
                if (file.isDirectory) transfer(file, child) else {
                    try {
                        file.inputStream().use { input -> context.contentResolver.openOutputStream(child, "w")?.use { output ->
                            copy(input, output, cancelled) { n -> progress("保存 ${file.name}", n, file.length()) }
                        } ?: throw IOException("无法写入 ${file.name}") }
                    } catch (error: Throwable) {
                        runCatching { DocumentsContract.deleteDocument(context.contentResolver, child) }
                        throw error
                    }
                }
            } ?: throw IOException("无法读取结果目录")
        }
        transfer(directory, root)
        return root
    }
    fun jobDirectory(): File = File(context.cacheDir, "jobs/${UUID.randomUUID()}").apply { mkdirs() }
    fun checkDeletable(node: Node) {
        if (node.isDocument) {
            val uri = Uri.parse(node.location)
            // A granted tree already identifies a DocumentsProvider. Package visibility can make
            // isDocumentUri() return false for a provider even when this URI grant works.
            val id = try { DocumentsContract.getDocumentId(uri) }
            catch (_: IllegalArgumentException) { throw IllegalArgumentException("此来源不支持删除，请授权所在文件夹后重试") }
            node.tree?.let { tree ->
                require(uri.authority == Uri.parse(tree).authority && id != DocumentsContract.getTreeDocumentId(Uri.parse(tree))) { "不能删除已授权文件夹的根目录" }
            }
        } else {
            val input = node.file.toPath().toAbsolutePath().normalize()
            val target = input.parent?.toFile()?.canonicalFile?.toPath()?.resolve(input.fileName) ?: input
            require(target.parent != null) { "不能删除存储根目录" }
            val protected = listOfNotNull(shared, context.dataDir, context.getExternalFilesDir(null), results)
            require(protected.none { it.canonicalFile.toPath().startsWith(target) }) { "不能删除存储或应用的根目录" }
        }
    }
    fun delete(node: Node, cancelled: () -> Boolean, progress: (String) -> Unit) {
        checkDeletable(node)
        fun check(name: String) {
            if (cancelled()) throw CancellationException()
            progress(name)
        }
        check(node.name)
        if (node.isDocument) {
            if (!DocumentsContract.deleteDocument(context.contentResolver, Uri.parse(node.location))) throw IOException("文件来源未能删除 ${node.name}")
            return
        }
        // walkFileTree does not follow links and does not consume the Java stack for deep folders.
        val input = node.file.toPath().toAbsolutePath().normalize()
        val path = input.parent!!.toFile().canonicalFile.toPath().resolve(input.fileName)
        java.nio.file.Files.walkFileTree(path, object : java.nio.file.SimpleFileVisitor<java.nio.file.Path>() {
            override fun preVisitDirectory(dir: java.nio.file.Path, attrs: java.nio.file.attribute.BasicFileAttributes): java.nio.file.FileVisitResult {
                check(dir.fileName.toString()); return java.nio.file.FileVisitResult.CONTINUE
            }
            override fun visitFile(file: java.nio.file.Path, attrs: java.nio.file.attribute.BasicFileAttributes): java.nio.file.FileVisitResult {
                check(file.fileName.toString()); java.nio.file.Files.delete(file); return java.nio.file.FileVisitResult.CONTINUE
            }
            override fun postVisitDirectory(dir: java.nio.file.Path, error: IOException?): java.nio.file.FileVisitResult {
                if (error != null) throw error
                check(dir.fileName.toString()); java.nio.file.Files.delete(dir); return java.nio.file.FileVisitResult.CONTINUE
            }
        })
    }
    fun shareCopy(node: Node): File {
        val folder = File(context.cacheDir, "share").apply { mkdirs() }
        val file = File(folder, "${UUID.randomUUID()}-${Rules.safeName(node.name)}")
        checkSpace(folder, node.size)
        input(node).use { source -> file.outputStream().use { source.copyTo(it) } }
        return file
    }
}
