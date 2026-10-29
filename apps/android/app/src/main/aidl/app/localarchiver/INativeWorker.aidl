package app.localarchiver;
import android.os.Bundle;
import app.localarchiver.INativeEvents;
interface INativeWorker {
    void run(in Bundle arguments, INativeEvents events);
    void cancel();
}
