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
import javax.crypto.spec.SecretKeySpec

/** Supplies the AES key used to seal the password book. Production uses Android Keystore. */
internal interface VaultKeys {
    /** Current key, or a newly generated one when no entry exists. */
    fun key(): SecretKey
    /** Drops the current entry so the next [key] call cannot reuse it. */
    fun delete()
}

/** Keystore-backed encryption, atomic replacement, no plaintext preferences or backups. */
class PasswordBook internal constructor(
    private val file: AtomicFile,
    private val keys: VaultKeys,
    private val maxPlaintextBytes: Int,
) {
    constructor(context: Context) : this(
        AtomicFile(File(context.filesDir, FILE_NAME)),
        AndroidVaultKeys(ALIAS),
        MAX_PLAINTEXT_BYTES,
    )

    init {
        require(maxPlaintextBytes in 1..MAX_PLAINTEXT_BYTES)
    }

    companion object {
        private val guard = Any()
        private const val ALIAS = "milkzip.password-book.v1"
        private const val FILE_NAME = "password-book.bin"
        private const val GCM_IV_LENGTH = 12
        private const val GCM_TAG_LENGTH = 16
        private const val DATA_KEY_BYTES = 32
        private const val WRAPPED_KEY_LENGTH = DATA_KEY_BYTES + GCM_TAG_LENGTH
        private const val ENVELOPE_VERSION = 2
        /** Legacy direct Keystore ciphertext: IV-length byte + IV + tag. */
        internal const val LEGACY_OVERHEAD = 1 + GCM_IV_LENGTH + GCM_TAG_LENGTH
        /** Version byte + wrapped data key + payload IV and tag. */
        internal const val ENVELOPE_OVERHEAD = 1 + GCM_IV_LENGTH + WRAPPED_KEY_LENGTH + GCM_IV_LENGTH + GCM_TAG_LENGTH
        /**
         * Largest UTF-8 `JSONArray` of 64 valid passwords. Android org.json turns
         * most C0 controls into `\u00XX` (6 bytes). A 300 KB ciphertext cap is
         * below that size, so a book that just saved is rejected on read.
         */
        internal const val MAX_PLAINTEXT_BYTES =
            Rules.PASSWORD_LIMIT * (1024 * 6 + 2) + (Rules.PASSWORD_LIMIT - 1) + 2
        internal const val MAX_FILE_BYTES = MAX_PLAINTEXT_BYTES + ENVELOPE_OVERHEAD

        /** 64 distinct 1024-byte passwords whose JSON form is the largest legal book. */
        internal fun worstCasePasswords(): List<String> {
            // Two-byte escapes (\b \t \n \f \r) are smaller. These controls all become `\u00XX`.
            val controls = charArrayOf(
                '\u0001', '\u0002', '\u0003', '\u0004', '\u0005', '\u0006', '\u0007', '\u000B',
                '\u000E', '\u000F', '\u0010', '\u0011', '\u0012', '\u0013', '\u0014', '\u0015',
                '\u0016', '\u0017', '\u0018', '\u0019', '\u001A', '\u001B', '\u001C', '\u001D', '\u001E', '\u001F',
            )
            return List(Rules.PASSWORD_LIMIT) { index ->
                "\u0001".repeat(1022) + controls[index / controls.size] + controls[index % controls.size]
            }
        }
    }

    fun read(): List<String> {
        synchronized(guard) {
            if (!file.baseFile.exists()) return emptyList()
            val bytes = file.openRead().use { it.readBytes() }
            try {
                require(bytes.size >= LEGACY_OVERHEAD && bytes.size <= MAX_FILE_BYTES) { "密码本文件损坏" }
                val plain = when (bytes[0].toInt()) {
                    GCM_IV_LENGTH -> decryptLegacy(bytes)
                    ENVELOPE_VERSION -> decryptEnvelope(bytes)
                    else -> throw IllegalArgumentException("密码本版本不支持")
                }
                try {
                    val array = JSONArray(String(plain, Charsets.UTF_8))
                    return (0 until array.length()).map { array.getString(it) }.also { values ->
                        require(values.size <= Rules.PASSWORD_LIMIT && values.all(Rules::validPassword)) { "密码本格式无效" }
                    }
                } finally { plain.fill(0) }
            } finally { bytes.fill(0) }
        }
    }

    fun write(values: List<String>) {
        synchronized(guard) { writeLocked(values) }
    }

    /**
     * Forgets an unusable keystore entry and its ciphertext, then writes an
     * empty book under a new key. [write] alone keeps using the old entry.
     */
    fun reset() {
        synchronized(guard) {
            keys.delete()
            deleteFiles()
            keys.key()
            writeLocked(emptyList())
        }
    }

    fun addVerified(value: String) {
        synchronized(guard) {
            val values = read().toMutableList()
            if (value !in values) { require(values.size < Rules.PASSWORD_LIMIT) { "密码本已满，本次密码仅在任务中使用" }; values.add(value); write(values) }
        }
    }

    fun saveEdited(original: List<String>, edited: List<String>): List<String> = synchronized(guard) {
        // A background task may have verified a new password while the editor was open.
        val additions = read().filter { it !in original }
        val merged = (edited + additions).distinct()
        write(merged)
        merged
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

    private fun writeLocked(values: List<String>) {
        require(values.size <= Rules.PASSWORD_LIMIT && values.all(Rules::validPassword)) { "最多 64 条，每条 1–1024 字节，不含空字符" }
        require(values.distinct().size == values.size) { "密码重复" }
        val plain = JSONArray(values).toString().toByteArray(Charsets.UTF_8)
        // API 26 Keystore can return a short ciphertext for a few hundred KB.
        // Only the 32-byte data key goes through Keystore; the book stays local AES-GCM.
        val dataKey = KeyGenerator.getInstance("AES").apply { init(DATA_KEY_BYTES * 8) }.generateKey()
        val rawKey = dataKey.encoded
        var encrypted = ByteArray(0)
        var wrapped = ByteArray(0)
        var payload = ByteArray(0)
        try {
            // Reject before startWrite so a too-large book cannot replace a good file.
            require(plain.size <= maxPlaintextBytes) { "密码本内容过大，未能保存" }
            require(rawKey != null && rawKey.size == DATA_KEY_BYTES) { "密码本密钥不可用" }
            val payloadCipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, dataKey) }
            require(payloadCipher.iv.size == GCM_IV_LENGTH) { "密码本密钥不可用" }
            encrypted = payloadCipher.doFinal(plain)
            require(encrypted.size == plain.size + GCM_TAG_LENGTH) { "密码本内容过大，未能保存" }
            val keyCipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, keys.key()) }
            require(keyCipher.iv.size == GCM_IV_LENGTH) { "密码本密钥不可用" }
            wrapped = keyCipher.doFinal(rawKey)
            require(wrapped.size == WRAPPED_KEY_LENGTH) { "密码本密钥不可用" }
            payload = ByteBuffer.allocate(ENVELOPE_OVERHEAD + plain.size)
                .put(ENVELOPE_VERSION.toByte()).put(keyCipher.iv).put(wrapped).put(payloadCipher.iv).put(encrypted).array()
            require(payload.size <= MAX_FILE_BYTES) { "密码本内容过大，未能保存" }
            val output = file.startWrite()
            try {
                output.write(payload)
                file.finishWrite(output)
            } catch (error: Throwable) {
                file.failWrite(output)
                throw error
            }
        } finally {
            plain.fill(0); rawKey?.fill(0); encrypted.fill(0); wrapped.fill(0); payload.fill(0)
        }
    }

    private fun decryptLegacy(bytes: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, keys.key(), GCMParameterSpec(128, bytes.copyOfRange(1, 1 + GCM_IV_LENGTH)))
        return cipher.doFinal(bytes, 1 + GCM_IV_LENGTH, bytes.size - 1 - GCM_IV_LENGTH)
    }

    private fun decryptEnvelope(bytes: ByteArray): ByteArray {
        require(bytes.size >= ENVELOPE_OVERHEAD) { "密码本文件损坏" }
        val wrappedStart = 1 + GCM_IV_LENGTH
        val dataIvStart = wrappedStart + WRAPPED_KEY_LENGTH
        val bodyStart = dataIvStart + GCM_IV_LENGTH
        val keyCipher = Cipher.getInstance("AES/GCM/NoPadding")
        keyCipher.init(Cipher.DECRYPT_MODE, keys.key(), GCMParameterSpec(128, bytes.copyOfRange(1, wrappedStart)))
        val rawKey = keyCipher.doFinal(bytes, wrappedStart, WRAPPED_KEY_LENGTH)
        try {
            require(rawKey.size == DATA_KEY_BYTES) { "密码本文件损坏" }
            val payload = Cipher.getInstance("AES/GCM/NoPadding")
            payload.init(Cipher.DECRYPT_MODE, SecretKeySpec(rawKey, "AES"), GCMParameterSpec(128, bytes.copyOfRange(dataIvStart, bodyStart)))
            return payload.doFinal(bytes, bodyStart, bytes.size - bodyStart)
        } finally { rawKey.fill(0) }
    }

    /** Removes the committed book and both AtomicFile generations (`.bak` and `.new`). */
    private fun deleteFiles() {
        file.delete()
        val base = file.baseFile
        File(base.path + ".bak").delete()
        File(base.path + ".new").delete()
        check(!base.exists() && !File(base.path + ".bak").exists() && !File(base.path + ".new").exists()) { "密码本无法重置" }
    }
}

private class AndroidVaultKeys(private val alias: String) : VaultKeys {
    override fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }

    override fun delete() {
        fun store() = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        // Invalidated entries can make the first delete throw; a second look
        // distinguishes "already gone" from "still unusable".
        runCatching { store().deleteEntry(alias) }
        if (store().containsAlias(alias)) store().deleteEntry(alias)
    }
}
