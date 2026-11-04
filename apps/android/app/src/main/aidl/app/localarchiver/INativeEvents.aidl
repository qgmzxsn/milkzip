package app.localarchiver;
interface INativeEvents {
    void onStarted(int pid);
    boolean onProgress(long done, long total);
    char[] onPassword(String path, boolean rejected);
    void onPasswordAccepted(in char[] password);
    int onChoose(in String[] paths);
    void onComplete(String error);
}
