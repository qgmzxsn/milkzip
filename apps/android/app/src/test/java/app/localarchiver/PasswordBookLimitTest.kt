package app.localarchiver

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PasswordBookLimitTest {
    @Test fun fileLimitCoversWorstCaseJsonEscaping() {
        // 64 × 1024 U+0001 passwords become `\u0001` (6 bytes) inside a compact JSON array.
        assertEquals(393409, PasswordBook.MAX_PLAINTEXT_BYTES)
        assertEquals(89, PasswordBook.ENVELOPE_OVERHEAD)
        assertEquals(393498, PasswordBook.MAX_FILE_BYTES)
        assertTrue(PasswordBook.MAX_FILE_BYTES > 300_000)
        val worst = PasswordBook.worstCasePasswords()
        assertEquals(Rules.PASSWORD_LIMIT, worst.distinct().size)
        assertTrue(worst.all(Rules::validPassword))
        assertEquals(PasswordBook.MAX_PLAINTEXT_BYTES, androidJsonArraySize(worst))
        assertEquals(PasswordBook.MAX_PLAINTEXT_BYTES, androidJsonArraySize(List(Rules.PASSWORD_LIMIT) { "\u0001".repeat(1024) }))
        val mixed = listOf("\"\\/\u0001\b\n".repeat(100), "ok")
        assertTrue(androidJsonArraySize(mixed) < PasswordBook.MAX_PLAINTEXT_BYTES)
    }

    /** Compact escaping used by Android `org.json.JSONStringer` (no extra whitespace). */
    private fun androidJsonArraySize(values: List<String>): Int {
        val encoded = StringBuilder("[")
        values.forEachIndexed { index, value ->
            if (index > 0) encoded.append(',')
            encoded.append('"')
            value.forEach { character ->
                when (character) {
                    '"', '\\', '/' -> encoded.append('\\').append(character)
                    '\b' -> encoded.append("\\b")
                    '\t' -> encoded.append("\\t")
                    '\n' -> encoded.append("\\n")
                    '\u000C' -> encoded.append("\\f")
                    '\r' -> encoded.append("\\r")
                    else -> if (character.code <= 0x1F) encoded.append("\\u%04x".format(character.code)) else encoded.append(character)
                }
            }
            encoded.append('"')
        }
        encoded.append(']')
        return encoded.toString().toByteArray(Charsets.UTF_8).size
    }
}
