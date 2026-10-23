package app.localarchiver

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.IBinder
import android.os.Process
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/** A native decoder failure must not take the file browser and password book down with it. */
class NativeWorkerService : Service() {
    private val cancelled = AtomicBoolean(false)
    private val busy = AtomicBoolean(false)
    private val binder = object : INativeWorker.Stub() {
        override fun cancel() { cancelled.set(true) }
        override fun run(arguments: Bundle, events: INativeEvents) {
            check(busy.compareAndSet(false, true)) { "解压引擎正在工作" }
            cancelled.set(false)
            Thread(null, {
                val passwords = Array(arguments.getInt("passwordCount")) { arguments.getCharArray("password$it") ?: charArrayOf() }
                var failure = ""
                try {
                    events.onStarted(Process.myPid())
                    val listener = object : NativeCallbacks {
                        override fun onProgress(done: Long, total: Long) = cancelled.get() || events.onProgress(done, total)
                        override fun onPassword(path: String, rejected: Boolean) = if (cancelled.get()) null else events.onPassword(path, rejected)
                        override fun onPasswordAccepted(password: CharArray) { events.onPasswordAccepted(password) }
                        override fun onChoose(paths: Array<String>) = if (cancelled.get()) -2 else events.onChoose(paths)
                    }
                    val path = requireNotNull(arguments.getString("path"))
                    val engine = applicationInfo.nativeLibraryDir + "/lib7z.so"
                    val result = when (arguments.getString("kind")) {
                        "list" -> ArchiverNative.nativeList(path, engine, passwords, arguments.getString("prefix") ?: "", listener)
                        "test" -> ArchiverNative.nativeTest(path, engine, passwords, listener)
                        "extract" -> ArchiverNative.nativeExtractNested(path, requireNotNull(arguments.getString("dest")), engine, passwords,
                            arguments.getInt("depth"), arguments.getLong("bytes"), arguments.getBoolean("keep"), arguments.getBoolean("ask"), listener)
                        else -> error("未知解压操作")
                    }
                    // Archive listings can exceed Binder's transaction limit. Only small control messages use Binder.
                    File(requireNotNull(arguments.getString("response"))).writeText(result)
                } catch (error: Throwable) {
                    failure = error.message ?: error.javaClass.simpleName
                } finally {
                    passwords.forEach { it.fill('\u0000') }; arguments.clear(); busy.set(false)
                }
                runCatching { events.onComplete(failure) }
            }, "milkzip-native", 8L shl 20).start()
        }
    }
    override fun onBind(intent: Intent): IBinder = binder
    override fun onUnbind(intent: Intent): Boolean { cancelled.set(true); return false }
}
