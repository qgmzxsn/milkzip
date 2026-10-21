package app.localarchiver

import java.io.File
import java.util.Locale

/** A document grant or a real filesystem path; display names are never used as paths. */
data class Node(val location: String, val name: String, val directory: Boolean, val size: Long = 0, val modified: Long = 0, val tree: String? = null) {
    val isDocument: Boolean get() = location.startsWith("content:")
    val file: File get() = File(location)
}

data class ArchivePage(val source: Node, val staged: File, val prefix: String, val rows: List<Node>, val count: Long, val size: Long, val packed: Long, val truncated: Boolean)
data class ExtractSettings(val recursive: Boolean = true, val ask: Boolean = false, val keep: Boolean = true, val depth: Int = 32, val maxBytes: Long = 64L shl 30)

object Rules {
    const val PASSWORD_LIMIT = 64
    fun validPassword(value: String) = value.isNotEmpty() && !value.contains('\u0000') && value.toByteArray(Charsets.UTF_8).size <= 1024
    fun safeName(value: String): String {
        require(value.isNotBlank() && value != "." && value != ".." && !value.contains('/') && !value.contains('\\') && !value.any { it.code < 32 }) { "文件名无效" }
        require(value.toByteArray(Charsets.UTF_8).size <= 240) { "文件名过长" }
        return value
    }
    fun resultName(source: String, suffix: String = "_解压"): String {
        val budget = 240 - suffix.toByteArray(Charsets.UTF_8).size
        val result = StringBuilder(); var used = 0; var offset = 0
        while (offset < source.length) {
            val point = source.codePointAt(offset)
            val piece = String(Character.toChars(point))
            val bytes = piece.toByteArray(Charsets.UTF_8).size
            if (used + bytes > budget) break
            result.append(piece); used += bytes; offset += Character.charCount(point)
        }
        return result.toString() + suffix
    }
    fun volumeGroup(name: String): String? {
        val lower = name.lowercase(Locale.ROOT)
        Regex("^(.*)\\.part[0-9]+\\.rar$").matchEntire(lower)?.let { return "rar:${it.groupValues[1]}" }
        Regex("^(.*)\\.r[0-9]{2,}$").matchEntire(lower)?.let { return "rar:${it.groupValues[1]}" }
        if (lower.endsWith(".rar")) return "rar:${lower.removeSuffix(".rar")}" 
        Regex("^(.*)\\.z[0-9]{2,}$").matchEntire(lower)?.let { return "zip:${it.groupValues[1]}" }
        if (lower.endsWith(".zip")) return "zip:${lower.removeSuffix(".zip")}" 
        Regex("^(.*)\\.[0-9]{3,}$").matchEntire(lower)?.let { return "split:${it.groupValues[1]}" }
        return null
    }
    fun resultLabel(code: Int) = when (code) {
        0 -> "已完成"; 1 -> "参数或密码格式无效"; 2 -> "文件读写失败"; 3 -> "无法识别或压缩包损坏"
        4 -> "解压引擎错误"; 5 -> "需要密码"; 6 -> "密码不正确"; 7 -> "已拦截不安全路径"
        8 -> "暂不支持此格式"; 9 -> "已取消"; 10 -> "缺少分卷"; 11 -> "需要选择内层压缩包"
        12 -> "达到解压上限"; 13 -> "部分完成"; else -> "任务失败"
    }
    fun size(bytes: Long): String {
        if (bytes < 0) return "—"
        if (bytes < 1024) return "$bytes B"
        var n = bytes.toDouble(); var unit = 0
        val units = arrayOf("KB", "MB", "GB", "TB")
        do { n /= 1024; unit++ } while (n >= 1024 && unit < units.size)
        return String.format(Locale.getDefault(), if (n >= 100) "%.0f %s" else "%.1f %s", n, units[unit - 1])
    }
    fun sorted(nodes: List<Node>, sort: String): List<Node> = nodes.sortedWith(
        compareBy<Node> { !it.directory }.thenComparator { a, b ->
            when (sort) {
                "size" -> b.size.compareTo(a.size)
                "date" -> b.modified.compareTo(a.modified)
                else -> a.name.lowercase(Locale.ROOT).compareTo(b.name.lowercase(Locale.ROOT))
            }.takeIf { it != 0 } ?: a.name.compareTo(b.name)
        })
}
