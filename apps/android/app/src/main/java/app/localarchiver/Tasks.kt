package app.localarchiver

import android.app.*
import android.content.Context
import android.content.Intent
import android.os.*
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean

internal data class Job(
    val kind: String, val sources: List<Node>, val siblings: List<Node> = sources,
    val prefix: String = "", val staged: File? = null, val settings: ExtractSettings = ExtractSettings(),
    val parent: File? = null, val name: String = "", val exportTree: String? = null
)
data class TaskRecord(val id: String, val title: String, val time: Long, val code: Int, val detail: String, val output: String?)
data class OpenFailure(val recordId: String, val source: Node)
data class TaskState(val title: String, val phase: String, val done: Long = 0, val total: Long = 0, val waiting: Boolean = false)
data class Prompt(val id: String = UUID.randomUUID().toString(), val path: String = "", val rejected: Boolean = false, val branches: List<String>? = null)
data class PromptReply(val password: CharArray? = null, val remember: Boolean = false, val choice: Int = -2)

/** Process-owned jobs survive Activity recreation; the foreground service owns execution. */
object Tasks {
    private lateinit var context: Context
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    val listeners = linkedSetOf<() -> Unit>()
    @Volatile var state: TaskState? = null; private set
    @Volatile var prompt: Prompt? = null; private set
    @Volatile var page: ArchivePage? = null; private set
    @Volatile var openFailure: OpenFailure? = null; private set
    @Volatile var pageVersion = 0; private set
    @Volatile var records: List<TaskRecord> = emptyList(); private set
    @Volatile private var active: Job? = null
    private val cancelled = AtomicBoolean(false)
    private var future: CompletableFuture<PromptReply>? = null
    private var running = false
    private var lastNotify = 0L
    private val sessionPasswords = mutableListOf<CharArray>()
    private var typedPassword: CharArray? = null
    private var rememberTyped = false
    private val warnings = mutableListOf<String>()
    fun init(app: Context) {
        if (::context.isInitialized) return
        context = app.applicationContext
        records = runCatching {
            val file = File(context.filesDir, "task-history.json")
            if (!file.exists()) emptyList() else JSONArray(file.readText()).let { a -> (0 until a.length()).map {
                a.getJSONObject(it).let { o -> TaskRecord(o.getString("id"), o.getString("title"), o.getLong("time"), o.getInt("code"), o.optString("detail"), o.optString("output").ifEmpty { null }) }
            } }
        }.getOrDefault(emptyList())
        // Cached inputs have no live owner after a process restart; results are outside cache.
        File(context.cacheDir, "jobs").listFiles()?.forEach { it.deleteRecursively() }
        File(context.cacheDir, "share").listFiles()?.filter { System.currentTimeMillis() - it.lastModified() > TimeUnit.DAYS.toMillis(1) }?.forEach { it.delete() }
    }
    private fun publish() { main.post { listeners.toList().forEach { it() } } }
    internal fun submit(job: Job): Boolean {
        init(context)
        if (active != null) return false
        active = job; cancelled.set(false); warnings.clear()
        state = TaskState(title(job), "准备中"); publish()
        try { ContextCompat.startForegroundService(context, Intent(context, TaskService::class.java)) }
        catch (error: Exception) { active = null; state = null; publish(); throw error }
        return true
    }
    private fun title(job: Job): String = when (job.kind) {
        "list" -> "打开 ${job.sources.first().name}"; "extract" -> "解压 ${job.sources.first().name}"
        "test" -> "检查 ${job.sources.first().name}"; "zip" -> "打包 ${job.sources.size} 项"; "export" -> "导出结果"; else -> job.kind
    }
    fun cancel() { cancelled.set(true); state = state?.copy(phase = "正在取消…"); future?.complete(PromptReply()); publish() }
    fun reply(id: String, reply: PromptReply) {
        if (prompt?.id == id) { if (future?.complete(reply) != true) reply.password?.fill('\u0000') }
        else reply.password?.fill('\u0000')
    }
    fun clearHistory() { records = emptyList(); saveRecords(); publish() }
    @Synchronized private fun saveRecords() = runCatching {
        val a = JSONArray()
        records.forEach { a.put(JSONObject().put("id", it.id).put("title", it.title).put("time", it.time).put("code", it.code).put("detail", it.detail).put("output", it.output ?: "")) }
        val file = android.util.AtomicFile(File(context.filesDir, "task-history.json"))
        val stream = file.startWrite()
        try { stream.write(a.toString().toByteArray()); file.finishWrite(stream) } catch (error: Exception) { file.failWrite(stream) }
    }
    private fun progress(phase: String, done: Long = 0, total: Long = 0, force: Boolean = false) {
        val now = SystemClock.elapsedRealtime()
        if (force || now - lastNotify >= 150) {
            lastNotify = now; state = state?.copy(phase = phase, done = done, total = total.coerceAtLeast(0), waiting = false); publish()
        }
        if (cancelled.get()) throw CancellationException()
    }
    private fun ask(request: Prompt): PromptReply {
        if (cancelled.get()) return PromptReply()
        val answer = CompletableFuture<PromptReply>()
        future = answer; prompt = request
        state = state?.copy(waiting = true, phase = if (request.branches == null) "等待输入密码" else "等待选择内层压缩包"); publish()
        try {
            while (!cancelled.get()) {
                try { return answer.get(250, TimeUnit.MILLISECONDS) } catch (_: TimeoutException) { }
            }
            return PromptReply()
        } finally {
            future = null; prompt = null; state = state?.copy(waiting = false); publish()
        }
    }
    private fun passwords(): Array<CharArray> {
        val saved = try { PasswordBook(context).read() } catch (error: Exception) { warnings.add("密码本无法读取，请在密码本页面检查；可以手动输入密码。"); emptyList() }
        return (saved.map { it.toCharArray() } + sessionPasswords.map { it.copyOf() }).take(64).toTypedArray()
    }
    private val callbacks = object : NativeCallbacks {
        override fun onProgress(done: Long, total: Long): Boolean {
            if (cancelled.get()) return true
            // Exceptions cannot escape a native callback with a pending JNI exception.
            runCatching { progress(if (active?.kind == "test") "检查数据完整性" else "处理压缩包", done, total) }
            return cancelled.get()
        }
        override fun onPassword(path: String, rejected: Boolean): CharArray? {
            val result = ask(Prompt(path = path, rejected = rejected))
            typedPassword?.fill('\u0000'); typedPassword = result.password?.copyOf(); rememberTyped = result.remember
            return result.password
        }
        override fun onPasswordAccepted(password: CharArray) {
            if (password.isEmpty()) return
            if (sessionPasswords.none { it.contentEquals(password) }) sessionPasswords.add(password.copyOf())
            if (rememberTyped && typedPassword?.contentEquals(password) == true) {
                try { PasswordBook(context).addVerified(String(password)) }
                catch (error: Exception) { warnings.add(error.message ?: "密码保存失败") }
                rememberTyped = false
            }
        }
        override fun onChoose(paths: Array<String>): Int = ask(Prompt(branches = paths.toList())).choice
    }
    internal fun execute(onFinish: (TaskRecord?) -> Unit) {
        if (running) return
        val job = active ?: run { onFinish(null); return }
        running = true
        worker.execute {
            val storage = Storage(context); val scratch = storage.jobDirectory()
            var code = 0; var detail = ""; var output: File? = null; var keepScratch = false
            try {
                val engine = context.applicationInfo.nativeLibraryDir + "/lib7z.so"
                when (job.kind) {
                    "list", "test", "extract" -> {
                        val source = job.sources.first()
                        val file = job.staged?.takeIf { it.exists() } ?: storage.stage(source, job.siblings, scratch, cancelled::get) { phase, n, total -> progress(phase, n, total) }
                        progress("读取压缩包", force = true)
                        val response = when (job.kind) {
                            "list" -> ArchiverNative.nativeList(file.absolutePath, engine, passwords(), job.prefix, callbacks)
                            "test" -> ArchiverNative.nativeTest(file.absolutePath, engine, passwords(), callbacks)
                            else -> {
                                val parent = job.parent ?: storage.results
                                val preferred = job.name.ifEmpty { Rules.resultName(source.name) }
                                output = storage.uniqueDirectory(parent, preferred)
                                val remaining = (output!!.usableSpace - (32L shl 20)).coerceAtLeast(0)
                                require(remaining > 0) { "存储空间不足" }
                                val cap = minOf(job.settings.maxBytes, remaining)
                                ArchiverNative.nativeExtractNested(file.absolutePath, output!!.absolutePath, engine, passwords(),
                                    if (job.settings.recursive) job.settings.depth else 1, cap, job.settings.keep, job.settings.ask, callbacks)
                            }
                        }
                        val json = JSONObject(response); code = json.getInt("code"); detail = json.optString("detail")
                        if (code == 0 && job.kind == "list") {
                            val rows = json.getJSONArray("entries")
                            val previousInput = page?.staged
                            page = ArchivePage(source, file, job.prefix, (0 until rows.length()).map { i -> rows.getJSONObject(i).let {
                                val path = it.getString("path")
                                Node(path, path.substringAfterLast('/'), it.getBoolean("directory"), it.getLong("size"), it.optLong("modified", -1) * 1000)
                            } }, json.getLong("count"), json.getLong("size"), json.getLong("packed"), json.optBoolean("truncated"))
                            if (previousInput != null && previousInput != file) {
                                val oldJob = previousInput.parentFile?.takeIf { it.name == "input" }?.parentFile
                                if (oldJob?.parentFile == File(context.cacheDir, "jobs")) oldJob.deleteRecursively()
                            }
                            pageVersion++; keepScratch = file.toPath().startsWith(scratch.toPath())
                            detail = "${json.getLong("count")} 项，解压后 ${Rules.size(json.getLong("size"))}"
                        }
                        if (code == 10) detail += "\n请让所有分卷处于同一文件夹，授权该文件夹，或用“选择文件”一次选中全部分卷。"
                        if (code == 0 && job.kind == "test") detail = "压缩包完整性检查通过。"
                    }
                    "zip" -> {
                        val folder = storage.uniqueDirectory(job.parent ?: storage.results, Rules.resultName(job.name.removeSuffix(".zip"), "").ifEmpty { "打包结果" })
                        output = folder
                        ZipCreator(storage).create(job.sources, File(folder, Rules.safeName(job.name)), cancelled::get) { phase, n, total -> progress(phase, n, total) }
                        detail = "已创建 ${job.name}（ZIP 存储模式，不压缩）"
                    }
                    "export" -> output = job.sources.first().file
                }
                if (output != null && job.exportTree != null && !cancelled.get() && code in listOf(0, 13)) {
                    progress("保存到选定文件夹", force = true)
                    val exported = storage.export(output!!, android.net.Uri.parse(job.exportTree), cancelled::get) { phase, n, total -> progress(phase, n, total) }
                    detail += "\n已保存到：$exported\n本地结果：${output!!.absolutePath}"
                }
            } catch (_: CancellationException) { code = 9; detail += "\n任务已取消，已完成的解压文件保留在结果目录。" }
            catch (error: Throwable) {
                code = if (cancelled.get()) 9 else 2
                detail += "\n" + (error.message ?: error.javaClass.simpleName)
            } finally {
                sessionPasswords.forEach { it.fill('\u0000') }; sessionPasswords.clear()
                typedPassword?.fill('\u0000'); typedPassword = null; rememberTyped = false
                if (!keepScratch) scratch.deleteRecursively()
                if (output != null) detail += "\n结果目录：${output!!.absolutePath}"
                if (warnings.isNotEmpty()) detail += "\n" + warnings.distinct().joinToString("\n")
                val record = TaskRecord(UUID.randomUUID().toString(), title(job), System.currentTimeMillis(), code, detail.trim().take(65000), output?.absolutePath)
                if (job.kind == "list" && code in listOf(3, 8)) openFailure = OpenFailure(record.id, job.sources.first())
                records = (listOf(record) + records).take(50); saveRecords()
                prompt = null
                main.post {
                    active = null; state = null; running = false
                    onFinish(record); publish()
                }
            }
        }
    }
}

class TaskService : Service() {
    private val channel = "archive-tasks"
    private var wake: PowerManager.WakeLock? = null
    private var lastUpdate = 0L
    private val observer: () -> Unit = { if (SystemClock.elapsedRealtime() - lastUpdate > 750 || Tasks.state?.waiting == true) {
        lastUpdate = SystemClock.elapsedRealtime(); notifyIfAllowed(1, notification())
    } }
    override fun onCreate() {
        super.onCreate(); Tasks.init(this)
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(NotificationChannel(channel, "压缩任务", NotificationManager.IMPORTANCE_LOW))
        Tasks.listeners.add(observer)
    }
    private fun notifyIfAllowed(id: Int, notification: Notification) {
        if (Build.VERSION.SDK_INT < 33 || checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).notify(id, notification)
        }
    }
    private fun notification(record: TaskRecord? = null): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java).putExtra("tasks", true), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val s = Tasks.state
        return NotificationCompat.Builder(this, channel).setSmallIcon(R.drawable.ic_milkzip).setContentTitle(record?.let { Rules.resultLabel(it.code) } ?: s?.title ?: "MilkZip")
            .setContentText(record?.title ?: s?.phase ?: "准备中").setContentIntent(open).setOnlyAlertOnce(true).setOngoing(record == null)
            .apply {
                if (record == null) {
                    val cancel = PendingIntent.getService(this@TaskService, 1, Intent(this@TaskService, TaskService::class.java).setAction("cancel"), PendingIntent.FLAG_IMMUTABLE)
                    addAction(0, "取消", cancel)
                    if (s?.waiting != true) setProgress(1000, if (s != null && s.total > 0) (s.done.toDouble() / s.total * 1000).toInt().coerceIn(0, 1000) else 0, s == null || s.total <= 0)
                }
            }.build()
    }
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == "cancel") { Tasks.cancel(); return START_NOT_STICKY }
        startForeground(1, notification())
        wake = (getSystemService(POWER_SERVICE) as PowerManager).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MilkZip:archive").apply { acquire(TimeUnit.HOURS.toMillis(6)) }
        Tasks.execute { record ->
            stopForeground(STOP_FOREGROUND_REMOVE)
            if (record != null) notifyIfAllowed(2, notification(record))
            stopSelf()
        }
        return START_NOT_STICKY
    }
    override fun onTimeout(startId: Int, fgsType: Int) { Tasks.cancel(); stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(startId) }
    override fun onDestroy() { Tasks.listeners.remove(observer); wake?.let { if (it.isHeld) it.release() }; super.onDestroy() }
    override fun onBind(intent: Intent?) = null
}
