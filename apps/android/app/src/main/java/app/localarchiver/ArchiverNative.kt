package app.localarchiver

/** Invoked synchronously on the task worker, never on the UI thread. */
interface NativeCallbacks {
    fun onProgress(done: Long, total: Long): Boolean
    fun onPassword(path: String, rejected: Boolean): CharArray?
    fun onPasswordAccepted(password: CharArray)
    // -1 finish this layer, -2 cancel, -3 recurse all, or a branch index.
    fun onChoose(paths: Array<String>): Int
}

object ArchiverNative {
    init { System.loadLibrary("archiver_jni") }
    external fun listArchive(archivePath: String, enginePath: String, password: CharArray?): String
    external fun extractArchive(archivePath: String, destPath: String, enginePath: String, password: CharArray?): String
    external fun nativeList(archivePath: String, enginePath: String, passwords: Array<CharArray>, prefix: String, listener: NativeCallbacks): String
    external fun nativeExtractNested(archivePath: String, destPath: String, enginePath: String, passwords: Array<CharArray>, depth: Int, bytes: Long, keep: Boolean, ask: Boolean, listener: NativeCallbacks): String
    external fun nativeTest(archivePath: String, enginePath: String, passwords: Array<CharArray>, listener: NativeCallbacks): String
}
