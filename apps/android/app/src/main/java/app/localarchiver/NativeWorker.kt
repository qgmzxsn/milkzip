package app.localarchiver

import android.app.ActivityManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.*
import java.io.File
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

internal class NativeWorkerException(message: String, val diagnostic: File? = null) : IOException(message)

/** Blocking worker-side client; all ServiceConnection callbacks remain on the main looper. */
internal object NativeWorker {
    @Volatile var pid: Int = 0; private set
    fun run(context: Context, arguments: Bundle, passwords: Array<CharArray>, response: File, listener: NativeCallbacks, cancelled: () -> Boolean = { false }, onStopped: () -> Unit = {}): String {
        val connected = CountDownLatch(1); val finished = CountDownLatch(1)
        val remote = AtomicReference<INativeWorker?>()
        val failure = AtomicReference<String?>()
        val replies = ConcurrentLinkedQueue<CharArray>()
        val gate = java.lang.Object()
        var closed = false
        var callbacks = 0
        fun <T> callback(fallback: T, action: () -> T): T {
            synchronized(gate) { if (closed) return fallback; callbacks++ }
            try { return action() }
            finally { synchronized(gate) { callbacks--; gate.notifyAll() } }
        }
        var linked: IBinder? = null
        pid = 0
        fun died() { if (finished.count == 0L) return; failure.compareAndSet(null, "解压引擎意外退出"); connected.countDown(); finished.countDown() }
        val death = IBinder.DeathRecipient { died() }
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, service: IBinder) {
                try { service.linkToDeath(death, 0); linked = service; remote.set(INativeWorker.Stub.asInterface(service)) }
                catch (_: RemoteException) { died() }
                connected.countDown()
            }
            override fun onServiceDisconnected(name: ComponentName) { died() }
            override fun onBindingDied(name: ComponentName) { died() }
            override fun onNullBinding(name: ComponentName) { died() }
        }
        var bound = false
        try {
            arguments.putString("response", response.path)
            arguments.putInt("passwordCount", passwords.size)
            passwords.forEachIndexed { index, value -> arguments.putCharArray("password$index", value) }
            bound = context.bindService(Intent(context, NativeWorkerService::class.java), connection, Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT)
            if (!bound || !connected.await(20, TimeUnit.SECONDS)) throw NativeWorkerException("无法启动解压引擎，请重试")
            val service = remote.get() ?: throw NativeWorkerException(failure.get() ?: "解压引擎未连接")
            service.run(arguments, object : INativeEvents.Stub() {
                override fun onStarted(value: Int) { pid = value }
                override fun onProgress(done: Long, total: Long) = callback(true) { listener.onProgress(done, total) }
                override fun onPassword(path: String, rejected: Boolean) = callback<CharArray?>(null) { listener.onPassword(path, rejected)?.also { replies.add(it) } }
                override fun onPasswordAccepted(password: CharArray) { try { callback(Unit) { listener.onPasswordAccepted(password) } } finally { password.fill('\u0000') } }
                override fun onChoose(paths: Array<String>) = callback(-2) { listener.onChoose(paths) }
                override fun onComplete(error: String) { if (error.isNotEmpty()) failure.compareAndSet(null, error); finished.countDown() }
            })
            var cancelTime = 0L
            while (!finished.await(250, TimeUnit.MILLISECONDS)) {
                if (cancelled()) {
                    if (cancelTime == 0L) { cancelTime = SystemClock.elapsedRealtime(); service.cancel() }
                    // Some damaged decoders stop calling progress. Cancellation still releases the worker.
                    if (pid > 0 && SystemClock.elapsedRealtime() - cancelTime > 5000) Process.killProcess(pid)
                }
            }
            failure.get()?.let { message ->
                val report = CrashDiagnostics.capture(context, pid)
                throw NativeWorkerException(message + report.summary + "\n已完成的文件保留。请保留此报告以便定位问题。", report.file)
            }
            return response.readText()
        } catch (error: RemoteException) { throw NativeWorkerException("解压引擎连接中断，已完成的文件保留。") }
        finally {
            synchronized(gate) { closed = true }
            // Engine death can interrupt a password/branch RPC. Release its prompt before retiring the job.
            onStopped()
            synchronized(gate) { while (callbacks > 0) gate.wait(250) }
            passwords.forEach { it.fill('\u0000') }; replies.forEach { it.fill('\u0000') }; replies.clear(); arguments.clear()
            linked?.let { runCatching { it.unlinkToDeath(death, 0) } }
            if (bound) context.unbindService(connection)
            response.delete(); pid = 0
        }
    }
}
