package app.localarchiver

import android.util.AtomicFile
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.json.JSONArray
import java.io.File
import java.nio.ByteBuffer
import java.security.UnrecoverableKeyException
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

@RunWith(AndroidJUnit4::class)
class PasswordBookTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var dir: File

    @Before fun setup() { dir = File(context.cacheDir, "password-book-${System.nanoTime()}").apply { mkdirs() } }
    @After fun cleanup() { dir.deleteRecursively() }

    @Test fun worstCaseEscapesRoundTripAndStayWithinTheReadLimit() {
        val book = vault(SoftwareVaultKeys())
        val values = PasswordBook.worstCasePasswords()
        val plaintext = JSONArray(values).toString().toByteArray(Charsets.UTF_8)
        assertEquals(PasswordBook.MAX_PLAINTEXT_BYTES, plaintext.size)
        assertTrue(plaintext.size > 300_000)
        book.write(values)
        val stored = bookFile()
        assertTrue(stored.length() > 300_000)
        assertEquals(2, stored.readBytes()[0].toInt())
        assertEquals((plaintext.size + PasswordBook.ENVELOPE_OVERHEAD).toLong(), stored.length())
        assertTrue(stored.length() <= PasswordBook.MAX_FILE_BYTES)
        assertEquals(values, book.read())
    }

    @Test fun oversizedSerializationDoesNotReplaceTheExistingBook() {
        val book = vault(SoftwareVaultKeys(), maxPlaintextBytes = 32)
        book.write(listOf("kept"))
        val before = bookFile().readBytes()
        val error = assertThrows(IllegalArgumentException::class.java) {
            book.write(listOf("\u0001".repeat(32)))
        }
        assertEquals("密码本内容过大，未能保存", error.message)
        assertArrayEquals(before, bookFile().readBytes())
        assertFalse(File(bookFile().path + ".bak").exists())
        assertFalse(File(bookFile().path + ".new").exists())
        assertEquals(listOf("kept"), book.read())
    }

    @Test fun legacyDirectCiphertextStillReads() {
        val keys = SoftwareVaultKeys()
        val book = vault(keys)
        val values = listOf("legacy \u0001")
        val plain = JSONArray(values).toString().toByteArray(Charsets.UTF_8)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, keys.key()) }
        val encrypted = cipher.doFinal(plain)
        bookFile().writeBytes(ByteBuffer.allocate(1 + cipher.iv.size + encrypted.size).put(cipher.iv.size.toByte()).put(cipher.iv).put(encrypted).array())
        assertEquals(values, book.read())
        book.write(values)
        assertEquals(2, bookFile().readBytes()[0].toInt())
        assertEquals(values, book.read())
    }

    @Test fun readRejectsCiphertextAboveTheWorstCaseLimit() {
        val stored = bookFile()
        stored.writeBytes(ByteArray(PasswordBook.MAX_FILE_BYTES + 1))
        val error = assertThrows(IllegalArgumentException::class.java) { vault(SoftwareVaultKeys()).read() }
        assertEquals("密码本文件损坏", error.message)
        assertEquals(PasswordBook.MAX_FILE_BYTES + 1L, stored.length())
    }

    @Test fun resetReplacesAnUnusableKeyAndLeavesAnEmptyUsableBook() {
        val keys = SoftwareVaultKeys()
        val book = vault(keys)
        book.write(listOf("secret"))
        val before = bookFile().readBytes()
        keys.mode = SoftwareVaultKeys.Mode.UNUSABLE_ENTRY
        assertThrows(Exception::class.java) { book.read() }
        assertThrows(Exception::class.java) { book.write(emptyList()) }
        assertEquals(0, keys.deletions)
        assertArrayEquals(before, bookFile().readBytes())
        book.reset()
        assertEquals(1, keys.deletions)
        assertEquals(SoftwareVaultKeys.Mode.HEALTHY, keys.mode)
        assertEquals(emptyList<String>(), book.read())
        book.write(listOf("after-reset"))
        assertEquals(listOf("after-reset"), book.read())
    }

    @Test fun resetRecoversWhenLoadingTheKeyThrows() {
        val keys = SoftwareVaultKeys()
        val book = vault(keys)
        book.write(listOf("secret"))
        File(bookFile().path + ".bak").writeBytes(byteArrayOf(9))
        File(bookFile().path + ".new").writeBytes(byteArrayOf(8))
        keys.mode = SoftwareVaultKeys.Mode.GET_THROWS
        assertThrows(UnrecoverableKeyException::class.java) { book.write(emptyList()) }
        book.reset()
        assertFalse(File(bookFile().path + ".bak").exists())
        assertFalse(File(bookFile().path + ".new").exists())
        assertEquals(emptyList<String>(), book.read())
        book.write(listOf("replaced"))
        assertEquals(listOf("replaced"), book.read())
    }

    @Test fun deviceKeystoreResetAndWorstCaseBookStayReadable() {
        val book = PasswordBook(context)
        val original = runCatching { book.read() }.getOrElse { emptyList() }
        try {
            book.write(listOf("quote\"slash\\solidus/\u0001"))
            assertEquals(listOf("quote\"slash\\solidus/\u0001"), PasswordBook(context).read())
            val worst = PasswordBook.worstCasePasswords()
            val plainSize = JSONArray(worst).toString().toByteArray(Charsets.UTF_8).size
            book.write(worst)
            val stored = File(context.filesDir, "password-book.bin")
            assertEquals("plaintext=$plainSize", (plainSize + PasswordBook.ENVELOPE_OVERHEAD).toLong(), stored.length())
            assertTrue(stored.length() > 300_000)
            assertTrue(stored.length() <= PasswordBook.MAX_FILE_BYTES)
            assertEquals(worst, book.read())
            File(stored.path + ".bak").writeBytes(byteArrayOf(1))
            File(stored.path + ".new").writeBytes(byteArrayOf(2))
            book.reset()
            assertFalse(File(stored.path + ".bak").exists())
            assertFalse(File(stored.path + ".new").exists())
            assertEquals(emptyList<String>(), book.read())
            book.write(listOf("重置后"))
            assertEquals(listOf("重置后"), PasswordBook(context).read())
        } finally {
            book.write(original)
        }
    }

    private fun bookFile() = File(dir, "book.bin")
    private fun vault(keys: VaultKeys, maxPlaintextBytes: Int = PasswordBook.MAX_PLAINTEXT_BYTES) =
        PasswordBook(AtomicFile(bookFile()), keys, maxPlaintextBytes)
}

private class SoftwareVaultKeys : VaultKeys {
    enum class Mode { HEALTHY, UNUSABLE_ENTRY, GET_THROWS }
    var mode = Mode.HEALTHY
    var deletions = 0
    private var current: SecretKey? = null

    override fun key(): SecretKey {
        current?.let { existing ->
            if (mode == Mode.GET_THROWS) throw UnrecoverableKeyException("invalidated")
            if (mode == Mode.UNUSABLE_ENTRY) return UnusableKey()
            return existing
        }
        current = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        mode = Mode.HEALTHY
        return current!!
    }

    override fun delete() {
        deletions++
        current = null
        mode = Mode.HEALTHY
    }
}

private class UnusableKey : SecretKey {
    private val encoded = byteArrayOf(1, 2, 3)
    override fun getAlgorithm() = "AES"
    override fun getFormat() = "RAW"
    override fun getEncoded() = encoded
}
