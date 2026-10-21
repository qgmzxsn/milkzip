package app.localarchiver

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import org.json.JSONArray
import java.io.File
import java.nio.ByteBuffer
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Keystore-backed encryption, atomic replacement, no plaintext preferences or backups. */
class PasswordBook(private val context: Context) {
    private val file = AtomicFile(File(context.filesDir, "password-book.bin"))
    private val alias = "milkzip.password-book.v1"
    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }
    @Synchronized fun read(): List<String> {
        if (!file.baseFile.exists()) return emptyList()
        val bytes = file.openRead().use { it.readBytes() }
        require(bytes.size >= 29 && bytes.size <= 300_000) { "密码本文件损坏" }
        val ivSize = bytes[0].toInt()
        require(ivSize == 12) { "密码本版本不支持" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes.copyOfRange(1, 13)))
        val plain = cipher.doFinal(bytes, 13, bytes.size - 13)
        try {
            val array = JSONArray(String(plain, Charsets.UTF_8))
            return (0 until array.length()).map { array.getString(it) }.also { values ->
                require(values.size <= Rules.PASSWORD_LIMIT && values.all(Rules::validPassword)) { "密码本格式无效" }
            }
        } finally { plain.fill(0); bytes.fill(0) }
    }
    @Synchronized fun write(values: List<String>) {
        require(values.size <= Rules.PASSWORD_LIMIT && values.all(Rules::validPassword)) { "最多 64 条，每条 1–1024 字节，不含空字符" }
        require(values.distinct().size == values.size) { "密码重复" }
        val plain = JSONArray(values).toString().toByteArray(Charsets.UTF_8)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key()) }
        val encrypted = try { cipher.doFinal(plain) } finally { plain.fill(0) }
        val output = file.startWrite()
        try {
            output.write(ByteBuffer.allocate(1 + cipher.iv.size + encrypted.size).put(cipher.iv.size.toByte()).put(cipher.iv).put(encrypted).array())
            file.finishWrite(output)
        } catch (error: Throwable) { file.failWrite(output); throw error }
    }
    @Synchronized fun addVerified(value: String) {
        val values = read().toMutableList()
        if (value !in values) { require(values.size < Rules.PASSWORD_LIMIT) { "密码本已满，本次密码仅在任务中使用" }; values.add(value); write(values) }
    }
    fun import(text: String): Pair<List<String>, Int> {
        val values = read().toMutableList(); var added = 0
        for (line in text.removePrefix("\uFEFF").split('\n')) {
            val value = line.removeSuffix("\r")
            if (value.isEmpty() || value in values) continue
            require(Rules.validPassword(value)) { "导入中存在超过 1024 字节或含空字符的密码" }
            require(values.size < Rules.PASSWORD_LIMIT) { "超过 64 条，请先清理密码本" }
            values.add(value); added++
        }
        return values to added // Explicit Save in the editor commits this proposal.
    }
}
