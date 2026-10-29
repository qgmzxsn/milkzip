package app.localarchiver

import org.junit.Assert.*
import org.junit.Test

class ArchivePlanTest {
    private fun file(path: String) = Node(path, path.substringAfterLast('/'), false)
    @Test fun splitVolumesBecomeOneRootAndOtherFoldersRemainIndependent() {
        val files = listOf(file("/one/a.7z.002"), file("/one/a.7z.001"), file("/one/b.zip"), file("/two/a.7z.001"))
        assertEquals(listOf("/one/a.7z.001", "/one/b.zip", "/two/a.7z.001"), ArchivePlan.roots(files, files).map { it.location })
        assertEquals(2, ArchivePlan.companions(files.first(), files).size)
        assertEquals("/one/a.7z.001", ArchivePlan.roots(listOf(files.first()), files).single().location)
    }
    @Test fun oldRarAndZipVolumesUseTheActualLeadingFile() {
        for (names in listOf(listOf("a.r00", "a.RAR"), listOf("b.z01", "b.zip"), listOf("x.part002.rar", "x.part001.rar"))) {
            val files = names.map { file("/files/$it") }
            assertEquals(names.last(), ArchivePlan.roots(files, files).single().name)
        }
    }
    @Test fun nestedSelectionDoesNotDeleteOrPackTheSameDescendantTwice() {
        val parent = Node("/files/dir", "dir", true)
        assertEquals(listOf(parent, file("/files/other.zip")), ArchivePlan.deletionRoots(listOf(parent, file("/files/dir/a.zip"), file("/files/other.zip"))))
    }
    @Test fun documentVolumeGroupsRespectDocumentParent() {
        fun doc(id: String) = Node("content://provider/tree/root/document/" + id.replace("/", "%2F"), id.substringAfterLast('/'), false)
        val files = listOf(doc("root/one/a.7z.001"), doc("root/one/a.7z.002"), doc("root/two/a.7z.001"))
        assertEquals(2, ArchivePlan.roots(files, files).size)
        assertEquals(2, ArchivePlan.companions(files.first(), files).size)
    }
}
