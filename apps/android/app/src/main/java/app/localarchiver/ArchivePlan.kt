package app.localarchiver

import java.io.File
import java.net.URI
import java.net.URLDecoder

/** Keep independent archives separate, while treating all volume parts as one input. */
object ArchivePlan {
    private fun documentId(location: String): String? = runCatching {
        val raw = URI(location).rawPath ?: return@runCatching null
        if (!raw.contains("/document/")) null else URLDecoder.decode(raw.substringAfterLast("/document/").replace("+", "%2B"), "UTF-8")
    }.getOrNull()
    fun directoryKey(node: Node): String {
        if (!node.isDocument) return File(node.location).absoluteFile.parent.orEmpty()
        val authority = runCatching { URI(node.location).authority }.getOrDefault("")
        val parent = node.parent?.let { documentId(it) } ?: documentId(node.location)?.substringBeforeLast('/', "")
        return "$authority:${parent ?: node.location.substringBeforeLast('/')}"
    }
    private fun family(node: Node): String? {
        val type = Rules.volumeGroup(node.name)?.substringBefore(':') ?: return null
        val stem = when (type) {
            "rar" -> node.name.replace(Regex("(?i)\\.part[0-9]+\\.rar$"), "").let { if (it == node.name) it.substringBeforeLast('.') else it }
            "zip" -> node.name.substringBeforeLast('.')
            else -> node.name.substringBeforeLast('.').let { value ->
                val ext = value.substringAfterLast('.', "")
                if (ext.lowercase(java.util.Locale.ROOT) in listOf("7z", "zip", "rar")) value.substringBeforeLast('.') + "." + ext.lowercase(java.util.Locale.ROOT) else value
            }
        }
        // Android paths can contain A.zip and a.zip as independent inputs. Never delete both as one group.
        return "$type:$stem"
    }
    fun companions(source: Node, siblings: List<Node>): List<Node> {
        val group = family(source) ?: return listOf(source)
        return (siblings.filter { !it.directory && directoryKey(it) == directoryKey(source) && family(it) == group } + source)
            .distinctBy { it.location }
    }
    private fun order(node: Node): Long {
        val name = node.name.lowercase(java.util.Locale.ROOT)
        if (name.endsWith(".rar") && !name.contains(Regex("\\.part[0-9]+\\.rar$"))) return 0
        if (name.endsWith(".zip")) return 0
        Regex("\\.part([0-9]+)\\.rar$").find(name)?.let { return it.groupValues[1].toLongOrNull() ?: Long.MAX_VALUE }
        Regex("\\.([rz]?)([0-9]+)$").find(name)?.let { return (it.groupValues[2].toLongOrNull() ?: Long.MAX_VALUE) }
        return 0
    }
    fun roots(selected: List<Node>, siblings: List<Node>): List<Node> {
        val groups = linkedMapOf<String, Node>()
        for (node in selected.filter { !it.directory }) {
            val key = family(node)?.let { "${directoryKey(node)}:$it" } ?: node.location
            groups.putIfAbsent(key, companions(node, siblings).minWith(compareBy<Node> { order(it) }.thenBy { it.name }))
        }
        return groups.values.toList()
    }
    fun deletionRoots(nodes: List<Node>): List<Node> = nodes.distinctBy { it.location }.filter { node ->
        nodes.none { parent -> parent.directory && parent.location != node.location &&
            if (!node.isDocument && !parent.isDocument) node.file.toPath().toAbsolutePath().normalize().startsWith(parent.file.toPath().toAbsolutePath().normalize())
            else node.isDocument && parent.isDocument && URI(node.location).authority == URI(parent.location).authority &&
                documentId(parent.location)?.let { documentId(node.location)?.startsWith("$it/") } == true
        }
    }
}
