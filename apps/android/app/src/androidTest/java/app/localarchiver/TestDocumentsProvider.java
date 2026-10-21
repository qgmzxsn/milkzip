package app.localarchiver;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/** Standalone test-package process: deliberately uses only Android/Java APIs. */
public class TestDocumentsProvider extends DocumentsProvider {
    private static final String[] COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS};
    @Override public boolean onCreate() { return true; }
    private File base() throws FileNotFoundException {
        File directory = new File(getContext().getCacheDir(), "test-documents");
        directory.mkdirs();
        if (!directory.isDirectory()) throw new FileNotFoundException("No test directory");
        return directory;
    }
    private File file(String id) throws FileNotFoundException {
        File base = base();
        File file = id.equals("root") ? base : new File(base, id.startsWith("root/") ? id.substring(5) : id);
        try {
            String path = file.getCanonicalPath(), root = base.getCanonicalPath();
            if (!path.equals(root) && !path.startsWith(root + "/")) throw new FileNotFoundException("Unsafe test path");
        } catch (IOException error) { throw new FileNotFoundException(error.getMessage()); }
        return file;
    }
    private String id(File file) throws FileNotFoundException {
        File base = base();
        return file.equals(base) ? "root" : "root/" + file.getAbsolutePath().substring(base.getAbsolutePath().length() + 1);
    }
    private void add(MatrixCursor cursor, File file) throws FileNotFoundException {
        String[] columns = cursor.getColumnNames(); Object[] values = new Object[columns.length];
        for (int i = 0; i < columns.length; ++i) {
            switch (columns[i]) {
                case Document.COLUMN_DOCUMENT_ID: values[i] = id(file); break;
                case Document.COLUMN_DISPLAY_NAME: values[i] = file.getName(); break;
                case Document.COLUMN_MIME_TYPE: values[i] = file.isDirectory() ? Document.MIME_TYPE_DIR : "application/octet-stream"; break;
                case Document.COLUMN_SIZE: values[i] = file.length(); break;
                case Document.COLUMN_LAST_MODIFIED: values[i] = file.lastModified(); break;
                case Document.COLUMN_FLAGS: values[i] = Document.FLAG_SUPPORTS_WRITE | Document.FLAG_SUPPORTS_DELETE | (file.isDirectory() ? Document.FLAG_DIR_SUPPORTS_CREATE : 0); break;
            }
        }
        cursor.addRow(values);
    }
    @Override public Cursor queryRoots(String[] projection) {
        String[] columns = projection != null ? projection : new String[]{Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_FLAGS};
        MatrixCursor cursor = new MatrixCursor(columns); Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; ++i) {
            switch (columns[i]) {
                case Root.COLUMN_ROOT_ID: case Root.COLUMN_DOCUMENT_ID: row[i] = "root"; break;
                case Root.COLUMN_TITLE: row[i] = "MilkZip test documents"; break;
                case Root.COLUMN_FLAGS: row[i] = Root.FLAG_SUPPORTS_CREATE; break;
            }
        }
        cursor.addRow(row); return cursor;
    }
    @Override public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : COLUMNS); add(cursor, file(documentId)); return cursor;
    }
    @Override public Cursor queryChildDocuments(String parentId, String[] projection, String sortOrder) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : COLUMNS);
        File[] children = file(parentId).listFiles();
        if (children != null) for (File child : children) add(cursor, child);
        return cursor;
    }
    @Override public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal) throws FileNotFoundException {
        return ParcelFileDescriptor.open(file(documentId), ParcelFileDescriptor.parseMode(mode));
    }
    @Override public String createDocument(String parentId, String mimeType, String name) throws FileNotFoundException {
        if (name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\")) throw new FileNotFoundException("Unsafe name");
        File file = new File(file(parentId), name);
        try {
            if (!(Document.MIME_TYPE_DIR.equals(mimeType) ? file.mkdir() : file.createNewFile())) throw new IOException("Already exists");
        } catch (IOException error) { throw new FileNotFoundException(error.getMessage()); }
        return id(file);
    }
    private void remove(File file) throws FileNotFoundException {
        if (file.isDirectory()) { File[] children = file.listFiles(); if (children != null) for (File child : children) remove(child); }
        if (!file.delete()) throw new FileNotFoundException("Cannot delete test file");
    }
    @Override public void deleteDocument(String documentId) throws FileNotFoundException {
        File file = file(documentId); if (file.equals(base())) throw new FileNotFoundException("Cannot delete root"); remove(file);
    }
    @Override public boolean isChildDocument(String parentId, String documentId) {
        try { return file(documentId).getAbsolutePath().startsWith(file(parentId).getAbsolutePath() + "/"); }
        catch (FileNotFoundException error) { return false; }
    }
}
