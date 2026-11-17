package app.localarchiver;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

// The same JNI class/method signatures as the Kotlin object, tested on a host JVM.
public class ArchiverNative {
    public native String listArchive(String archive, String engine, char[] password);
    public native String extractArchive(String archive, String dest, String engine, char[] password);

    public native void checkThreadCallbacks(Object listener);
    public native String nativeExtractNested(String archive, String dest, String engine, char[][] passwords,
        int depth, long bytes, boolean keep, boolean ask, Object listener);
    public static class Listener {
        int calls;
        public boolean onProgress(long done, long total) { calls++; return false; }
        public char[] onPassword(String path, boolean rejected) { return null; }
        public void onPasswordAccepted(char[] password) { }
        public int onChoose(String[] paths) { return -3; }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        System.load(args[1]);
        ArchiverNative api = new ArchiverNative();
        Listener probe = new Listener();
        api.checkThreadCallbacks(probe);
        try {
            api.checkThreadCallbacks(new Listener() {
                @Override public boolean onProgress(long done, long total) { throw new IllegalStateException("callback failure"); }
            });
            throw new AssertionError("callback exception was lost");
        } catch (IllegalStateException expected) { }
        if (probe.calls == 0) throw new AssertionError("native thread callback missing");
        // Construct Unicode paths in Java: some Windows Java launchers convert
        // command-line arguments through the legacy system code page.
        String archive = Path.of(args[3], "压缩😀.zip").toString();
        String encrypted = Path.of(args[3], "加密🔑" + args[4]).toString();
        String dest = Path.of(args[3], "输出😀").toString();
        if (!Files.exists(Path.of(archive))) throw new AssertionError("missing test archive");
        String listed = api.listArchive(archive, args[2], null);
        if (!listed.equals("ok\n目录😀/文件🦊.txt")) {
            throw new AssertionError("Unexpected JNI list result (code points): " +
                listed.codePoints().mapToObj(Integer::toHexString)
                    .collect(java.util.stream.Collectors.joining(",")));
        }
        String result = api.extractArchive(archive, dest, args[2], null);
        if (!result.equals("ok")) throw new AssertionError(result);
        Path extracted = Path.of(dest, "目录😀", "文件🦊.txt");
        if (!Files.readString(extracted).equals("unicode")) throw new AssertionError("contents");
        char[] password = new String(System.in.readAllBytes(), StandardCharsets.UTF_8).toCharArray();
        try {
            result = api.extractArchive(encrypted, dest + "-secret", args[2], password);
            if (!result.equals("ok")) throw new AssertionError(result);
            Path secret = Path.of(dest + "-secret", "secret.txt");
            if (!Files.readString(secret).equals("secret")) throw new AssertionError("password");
        } finally {
            Arrays.fill(password, '\0');
        }
        result = api.extractArchive(Path.of(args[3], "damaged.zip").toString(),
            dest + "-damaged", args[2], new char[]{'x'});
        if (!result.startsWith("format")) {
            throw new AssertionError("Unencrypted damage mislabeled as a password error: " + result);
        }
        Path threaded = Path.of(args[3], "threaded.7z");
        if (Files.exists(threaded)) {
            Listener listener = new Listener();
            result = api.nativeExtractNested(threaded.toString(), dest + "-threaded", args[2], new char[0][],
                32, 64L << 20, true, false, listener);
            if (!result.contains("\"code\":0") || listener.calls == 0) throw new AssertionError(result);
        }
        result = api.nativeExtractNested(Path.of(args[3], "zipsc-split.7z.001").toString(), dest + "-zipsc", args[2],
            new char[][] { "batch-secret".toCharArray() }, 32, 64L << 20, true, false, new Listener());
        if (!result.contains("\"code\":0") || Files.size(Path.of(dest + "-zipsc", "payload.bin")) != 131072) throw new AssertionError(result);
        System.out.println("JNI Unicode paths, listing, and password checks passed");
    }
}
