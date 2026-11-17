package app.localarchiver

import org.junit.Assert.*
import org.junit.Test

class RulesTest {
    @Test fun volumeGroupsKeepCompanionsAndExcludeUnrelatedFiles() {
        assertEquals(Rules.volumeGroup("Book.PART001.RAR"), Rules.volumeGroup("book.part009.rar"))
        assertEquals(Rules.volumeGroup("book.rar"), Rules.volumeGroup("book.r00"))
        assertEquals(Rules.volumeGroup("book.zip"), Rules.volumeGroup("book.z01"))
        assertEquals(Rules.volumeGroup("book.7z.001"), Rules.volumeGroup("BOOK.7z.099"))
        assertNotEquals(Rules.volumeGroup("other.7z.001"), Rules.volumeGroup("book.7z.001"))
        assertNull(Rules.volumeGroup("normal.txt"))
    }
    @Test fun passwordLimitsAreUtf8AndPreserveSpaces() {
        assertTrue(Rules.validPassword(" 密码 "))
        assertTrue(Rules.validPassword("中".repeat(341)))
        assertFalse(Rules.validPassword("中".repeat(342)))
        assertFalse(Rules.validPassword("")); assertFalse(Rules.validPassword("a\u0000b"))
    }
    @Test fun longChineseAndEmojiOutputNamesFitFilesystem() {
        for (name in listOf("中".repeat(80) + ".zip", "😀".repeat(80) + ".7z")) {
            val result = Rules.resultName(name)
            assertTrue(result.endsWith("_解压"))
            assertTrue(result.toByteArray(Charsets.UTF_8).size <= 240)
            assertFalse(result.contains('\uFFFD'))
            assertEquals(result, result.toByteArray(Charsets.UTF_8).toString(Charsets.UTF_8))
        }
    }
    @Test fun noPathTraversalInUserOutputNames() {
        listOf("..", ".", "a/b", "a\\b", "bad\u0000name", "\n").forEach { value -> assertThrows(IllegalArgumentException::class.java) { Rules.safeName(value) } }
        assertEquals("你好.zip", Rules.safeName("你好.zip"))
    }
    @Test fun directoriesStayFirstWithNumericSizeOrdering() {
        val nodes = listOf(Node("a", "a", false, 2), Node("b", "b", false, 100), Node("z", "z", true))
        assertEquals(listOf("z", "b", "a"), Rules.sorted(nodes, "size").map { it.name })
    }
}
