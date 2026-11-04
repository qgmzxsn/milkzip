package app.localarchiver

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Diagnostics stay private until the user explicitly shares the report. */
internal object CrashDiagnostics {
    data class Report(val summary: String, val file: File?)
    fun capture(context: Context, pid: Int): Report {
        if (Build.VERSION.SDK_INT < 30 || pid <= 0) return Report("", null)
        return runCatching {
            val manager = context.getSystemService(ActivityManager::class.java)
            val exit = manager.getHistoricalProcessExitReasons(context.packageName, pid, 1).firstOrNull()
            val reason = exit?.let { when (it.reason) {
                android.app.ApplicationExitInfo.REASON_LOW_MEMORY -> "内存不足"
                android.app.ApplicationExitInfo.REASON_CRASH_NATIVE -> "原生解码器崩溃"
                android.app.ApplicationExitInfo.REASON_CRASH -> "运行异常"
                android.app.ApplicationExitInfo.REASON_SIGNALED -> "进程被终止"
                else -> "原因 ${it.reason}"
            } } ?: "系统暂未提供退出记录"
            val memory = ActivityManager.MemoryInfo().also(manager::getMemoryInfo)
            val metadata = JSONObject().put("pid", pid).put("reason", exit?.reason).put("status", exit?.status)
                .put("description", exit?.description).put("model", Build.MODEL).put("manufacturer", Build.MANUFACTURER)
                .put("sdk", Build.VERSION.SDK_INT).put("abis", Build.SUPPORTED_ABIS.joinToString())
                .put("availableMemory", memory.availMem).put("totalMemory", memory.totalMem)
            val folder = File(context.filesDir, "diagnostics").apply { mkdirs() }
            val file = File(folder, "milkzip-${System.currentTimeMillis()}-${UUID.randomUUID()}.zip")
            try {
                ZipOutputStream(file.outputStream()).use { zip ->
                    exit?.traceInputStream?.use { input ->
                        zip.putNextEntry(ZipEntry("native-trace.pb"))
                        val buffer = ByteArray(16384); var bytes = 0
                        while (bytes < 8 * 1024 * 1024) {
                            val n = input.read(buffer, 0, minOf(buffer.size, 8 * 1024 * 1024 - bytes)); if (n < 0) break
                            zip.write(buffer, 0, n); bytes += n
                        }
                        metadata.put("traceBytes", bytes); metadata.put("traceTruncated", input.read() >= 0)
                        zip.closeEntry()
                    }
                    zip.putNextEntry(ZipEntry("exit.json")); zip.write(metadata.toString(2).toByteArray()); zip.closeEntry()
                }
                folder.listFiles()?.filter { it != file }?.sortedByDescending { it.lastModified() }?.drop(4)?.forEach { it.delete() }
                Report("\n系统退出原因：$reason${exit?.description?.let { "（$it）" }.orEmpty()}\n可在报告中分享诊断文件。", file)
            } catch (_: Exception) { file.delete(); Report("\n系统退出原因：$reason", null) }
        }.getOrDefault(Report("", null))
    }
}
