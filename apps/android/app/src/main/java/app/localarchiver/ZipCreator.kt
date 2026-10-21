package app.localarchiver

import java.io.File
import java.io.IOException
import java.util.concurrent.CancellationException
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Same STORE ZIP mode as the desktop; bounded memory, UTF-8 names and ZIP64. */
class ZipCreator(private val storage: Storage) {
    fun create(sources: List<Node>, output: File, cancelled: () -> Boolean, progress: (String, Long, Long) -> Unit) {
        require(sources.isNotEmpty()) { "请选择文件或文件夹" }
        require(sources.map { it.name }.distinct().size == sources.size) { "选中的顶层文件重名" }
        val entries = mutableListOf<Pair<Node, String>>()
        fun collect(node: Node, path: String, depth: Int) {
            if (cancelled()) throw CancellationException()
            require(depth <= 64 && entries.size < 100000) { "目录层数或文件数量过多" }
            Rules.safeName(node.name)
            entries.add(node to path)
            if (node.directory) storage.children(node).forEach { collect(it, "$path/${it.name}", depth + 1) }
        }
        sources.forEach { collect(it, it.name, 0) }
        storage.checkSpace(output.parentFile!!, entries.sumOf { if (it.first.directory) 0 else it.first.size.coerceAtLeast(0) } + entries.size * 256L)
        check(output.createNewFile()) { "输出文件已存在" }
        try {
            ZipOutputStream(output.outputStream().buffered(), Charsets.UTF_8).use { zip ->
                for ((node, path) in entries) {
                    if (cancelled()) throw CancellationException()
                    val entry = ZipEntry(if (node.directory) "$path/" else path).apply { method = ZipEntry.STORED; time = node.modified.takeIf { it > 0 } ?: System.currentTimeMillis() }
                    if (node.directory) { entry.size = 0; entry.crc = 0; zip.putNextEntry(entry); zip.closeEntry(); continue }
                    var size = 0L; val crc = CRC32(); val buffer = ByteArray(256 * 1024)
                    storage.input(node).use { input ->
                        while (true) {
                            if (cancelled()) throw CancellationException()
                            val n = input.read(buffer); if (n < 0) break
                            crc.update(buffer, 0, n); size += n; progress("校验 ${node.name}", size, node.size)
                        }
                    }
                    entry.size = size; entry.compressedSize = size; entry.crc = crc.value
                    zip.putNextEntry(entry)
                    var written = 0L; val secondCrc = CRC32()
                    storage.input(node).use { input ->
                        while (true) {
                            if (cancelled()) throw CancellationException()
                            val n = input.read(buffer); if (n < 0) break
                            zip.write(buffer, 0, n); secondCrc.update(buffer, 0, n); written += n
                            progress("打包 ${node.name}", written, size)
                        }
                    }
                    if (written != size || secondCrc.value != crc.value) throw IOException("${node.name} 在打包时发生变化，请重试")
                    zip.closeEntry()
                }
            }
        } catch (error: Throwable) { output.delete(); throw error }
    }
}
