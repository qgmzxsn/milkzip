#include "archiver.h"
#include "utf.hpp"

#include <jni.h>

#include <string.h>

#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <new>
#include <exception>

namespace {

void wipe(std::string &s) {
  if (!s.empty()) {
    volatile char *p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  }
  s.clear();
}

std::string from_chars(JNIEnv *env, jcharArray chars) {
  if (!chars) return std::string();
  jsize n = env->GetArrayLength(chars);
  if (n <= 0) return std::string();
  jchar *data = env->GetCharArrayElements(chars, nullptr);
  if (!data) return std::string();
  std::string out = archiver::utf16_to_utf8(data, static_cast<size_t>(n));
  volatile jchar *clear = data;
  for (jsize i = 0; i < n; ++i) clear[i] = 0;
  env->ReleaseCharArrayElements(chars, data, 0);
  return out;
}

std::string from_jstring(JNIEnv *env, jstring value) {
  if (!value) return std::string();
  const jchar *chars = env->GetStringChars(value, nullptr);
  if (!chars) return std::string();
  std::string out = archiver::utf16_to_utf8(chars, env->GetStringLength(value));
  env->ReleaseStringChars(value, chars);
  return out;
}

jstring to_jstring(JNIEnv *env, const std::string &value) {
  std::u16string text = archiver::utf8_to_utf16(value);
  std::vector<jchar> chars(text.begin(), text.end());
  return env->NewString(chars.data(), static_cast<jsize>(chars.size()));
}

jstring result_text(JNIEnv *env, ArchiverResult result, ArchiverArchive *archive) {
  std::string text = archiver_result_string(result);
  const char *detail = archiver_message(archive);
  if (detail && detail[0]) {
    text.push_back('\n');
    text += detail;
  }
  return to_jstring(env, text);
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_app_localarchiver_ArchiverNative_listArchive(JNIEnv *env, jobject, jstring archive_path,
                                                   jstring engine_path, jcharArray password) {
  std::string path = from_jstring(env, archive_path);
  std::string engine = from_jstring(env, engine_path);
  std::string pw = from_chars(env, password);
  if (env->ExceptionCheck()) { wipe(pw); return nullptr; }
  const char *pw_ptr = password ? pw.c_str() : nullptr;
  ArchiverArchive *archive = nullptr;
  ArchiverResult result =
      archiver_open(path.c_str(), engine.empty() ? nullptr : engine.c_str(), pw_ptr, &archive);
  wipe(pw);
  if (result != ARCHIVER_OK) {
    jstring text = result_text(env, result, archive);
    archiver_close(archive);
    return text;
  }
  std::string text = "ok";
  uint32_t count = 0;
  archiver_entry_count(archive, &count);
  for (uint32_t i = 0; i < count; ++i) {
    ArchiverEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (archiver_entry_at(archive, i, &entry) != ARCHIVER_OK) continue;
    text.push_back('\n');
    text += entry.path ? entry.path : "";
    archiver_entry_free(&entry);
  }
  archiver_close(archive);
  return to_jstring(env, text);
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_localarchiver_ArchiverNative_extractArchive(JNIEnv *env, jobject, jstring archive_path,
                                                      jstring dest_path, jstring engine_path,
                                                      jcharArray password) {
  std::string path = from_jstring(env, archive_path);
  std::string dest = from_jstring(env, dest_path);
  std::string engine = from_jstring(env, engine_path);
  std::string pw = from_chars(env, password);
  if (env->ExceptionCheck()) { wipe(pw); return nullptr; }
  const char *pw_ptr = password ? pw.c_str() : nullptr;
  ArchiverArchive *archive = nullptr;
  ArchiverResult result =
      archiver_open(path.c_str(), engine.empty() ? nullptr : engine.c_str(), pw_ptr, &archive);
  if (result == ARCHIVER_OK) {
    result = archiver_extract(archive, dest.c_str(), pw_ptr);
  }
  wipe(pw);
  jstring text = result_text(env, result, archive);
  archiver_close(archive);
  return text;
}

namespace {
std::string json_string(const std::string &text) {
  const char *hex = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}
std::string result_json(ArchiverResult code, const std::string &detail, const std::string &extra = {}) {
  return "{\"code\":" + std::to_string(code) + ",\"detail\":" + json_string(detail) + extra + "}";
}
struct Secrets {
  std::vector<std::string> values;
  std::vector<const char *> pointers;
  ~Secrets() { for (auto &value : values) wipe(value); }
  void read(JNIEnv *env, jobjectArray source) {
    const jsize count = source ? std::min<jsize>(env->GetArrayLength(source), 64) : 0;
    values.reserve(count);
    for (jsize i = 0; i < count; ++i) {
      auto chars = static_cast<jcharArray>(env->GetObjectArrayElement(source, i));
      values.push_back(from_chars(env, chars));
      if (chars) env->DeleteLocalRef(chars);
    }
    for (const auto &value : values) pointers.push_back(value.c_str());
  }
};
struct AndroidCallbacks {
  JavaVM *vm = nullptr;
  std::recursive_mutex mutex;
  jthrowable error = nullptr;
  struct Environment {
    JavaVM *vm; JNIEnv *env = nullptr; bool attached = false;
    explicit Environment(JavaVM *v) : vm(v) {
      if (!vm) return;
      if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
#if defined(__ANDROID__)
        attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
#else
        attached = vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) == JNI_OK;
#endif
      }
    }
    ~Environment() { if (attached) vm->DetachCurrentThread(); }
  };
  bool capture(JNIEnv *env) {
    if (env && env->ExceptionCheck()) {
      jthrowable local = env->ExceptionOccurred(); env->ExceptionClear();
      if (!error) error = static_cast<jthrowable>(env->NewGlobalRef(local));
      env->DeleteLocalRef(local);
    }
    return !env || error;
  }
  bool rethrow(JNIEnv *env) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (error) env->Throw(error);
    return env->ExceptionCheck();
  }
  jobject object;
  jmethodID progress_id = nullptr, password_id = nullptr, accepted_id = nullptr, choose_id = nullptr;
  AndroidCallbacks(JNIEnv *env, jobject o) : object(nullptr) {
    env->GetJavaVM(&vm);
    if (o) object = env->NewGlobalRef(o);
    if (!o) return;
    jclass cls = env->GetObjectClass(o);
    progress_id = env->GetMethodID(cls, "onProgress", "(JJ)Z");
    password_id = env->GetMethodID(cls, "onPassword", "(Ljava/lang/String;Z)[C");
    accepted_id = env->GetMethodID(cls, "onPasswordAccepted", "([C)V");
    choose_id = env->GetMethodID(cls, "onChoose", "([Ljava/lang/String;)I");
    env->DeleteLocalRef(cls);
  }
  ~AndroidCallbacks() {
    Environment scope(vm);
    if (scope.env) { if (object) scope.env->DeleteGlobalRef(object); if (error) scope.env->DeleteGlobalRef(error); }
  }
  bool cancelled(uint64_t done = 0, uint64_t total = 0) {
    std::lock_guard<std::recursive_mutex> lock(mutex); Environment scope(vm); JNIEnv *env = scope.env;
    if (capture(env)) return true;
    if (!object) return false;
    const bool stop = env->CallBooleanMethod(object, progress_id, static_cast<jlong>(done), static_cast<jlong>(total));
    return capture(env) || stop;
  }
  std::string ask(const char *path, bool rejected, bool &cancelled) {
    std::lock_guard<std::recursive_mutex> lock(mutex); Environment scope(vm); JNIEnv *env = scope.env;
    if (capture(env)) { cancelled = true; return {}; }
    jstring name = to_jstring(env, path);
    auto chars = static_cast<jcharArray>(env->CallObjectMethod(object, password_id, name, static_cast<jboolean>(rejected)));
    env->DeleteLocalRef(name);
    if (capture(env) || !chars) { cancelled = true; return {}; }
    std::string out = from_chars(env, chars);
    env->DeleteLocalRef(chars);
    return out;
  }
  void accepted(const char *value) {
    std::lock_guard<std::recursive_mutex> lock(mutex); Environment scope(vm); JNIEnv *env = scope.env;
    if (capture(env)) return;
    const std::u16string text = archiver::utf8_to_utf16(value);
    jcharArray chars = env->NewCharArray(static_cast<jsize>(text.size()));
    if (!chars || capture(env)) return;
    env->SetCharArrayRegion(chars, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jchar *>(text.data()));
    env->CallVoidMethod(object, accepted_id, chars);
    if (!capture(env)) {
      std::vector<jchar> zero(text.size());
      env->SetCharArrayRegion(chars, 0, static_cast<jsize>(zero.size()), zero.data());
    }
    env->DeleteLocalRef(chars);
  }
  int choose(const char *const *paths, uint32_t count) {
    std::lock_guard<std::recursive_mutex> lock(mutex); Environment scope(vm); JNIEnv *env = scope.env;
    if (capture(env)) return -2;
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray items = env->NewObjectArray(static_cast<jsize>(count), string_class, nullptr);
    env->DeleteLocalRef(string_class);
    for (uint32_t i = 0; i < count; ++i) {
      jstring item = to_jstring(env, paths[i]);
      env->SetObjectArrayElement(items, static_cast<jsize>(i), item); env->DeleteLocalRef(item);
    }
    const int result = env->CallIntMethod(object, choose_id, items);
    env->DeleteLocalRef(items);
    return capture(env) ? -2 : result;
  }
};
int android_progress(uint64_t done, uint64_t total, void *user) {
  return static_cast<AndroidCallbacks *>(user)->cancelled(done, total) ? 1 : 0;
}
struct AuthArchive {
  ArchiverArchive *handle = nullptr;
  std::string password;
  bool typed = false;
  ~AuthArchive() { archiver_close(handle); wipe(password); }
};
ArchiverResult authenticated_open(const std::string &path, const std::string &engine,
    Secrets &book, AndroidCallbacks &callbacks, AuthArchive &out) {
  if (callbacks.cancelled()) return ARCHIVER_ERR_CANCELLED;
  int32_t matched = -1;
  ArchiverResult result = archiver_open_with_passwords(path.c_str(), engine.c_str(),
      book.pointers.data(), static_cast<uint32_t>(book.pointers.size()), &matched, &out.handle);
  if (matched >= 0) out.password = book.values[matched];
  bool rejected = false;
  while ((result == ARCHIVER_ERR_PASSWORD || result == ARCHIVER_ERR_PASSWORD_REQUIRED) && callbacks.object) {
    bool cancelled = false;
    wipe(out.password);
    out.password = callbacks.ask(path.c_str(), rejected, cancelled);
    if (cancelled) return ARCHIVER_ERR_CANCELLED;
    if (out.password.size() > 1024) return ARCHIVER_ERR_ARGUMENT;
    archiver_close(out.handle); out.handle = nullptr;
    const char *view = out.password.c_str();
    result = archiver_open_with_passwords(path.c_str(), engine.c_str(), &view, 1, &matched, &out.handle);
    out.typed = true; rejected = true;
  }
  return result;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_localarchiver_ArchiverNative_nativeList(JNIEnv *env, jobject, jstring archive_path,
    jstring engine_path, jobjectArray passwords, jstring prefix_string, jobject listener) try {
  const std::string path = from_jstring(env, archive_path), engine = from_jstring(env, engine_path);
  const std::string prefix = from_jstring(env, prefix_string);
  Secrets book; book.read(env, passwords);
  AndroidCallbacks callbacks(env, listener); AuthArchive archive;
  if (env->ExceptionCheck()) return nullptr;
  ArchiverResult code = authenticated_open(path, engine, book, callbacks, archive);
  if (env->ExceptionCheck()) return nullptr;
  if (callbacks.rethrow(env)) return nullptr;
  if (code != ARCHIVER_OK) return to_jstring(env, result_json(code, archiver_message(archive.handle)));
  uint32_t count = 0; archiver_entry_count(archive.handle, &count);
  struct Row { std::string path; bool directory = false, symlink = false; uint64_t size = 0, packed = 0; int64_t modified = -1; };
  std::map<std::string, Row> rows;
  uint64_t size = 0, packed = 0;
  bool truncated = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (i % 256 == 0 && callbacks.cancelled()) return to_jstring(env, result_json(ARCHIVER_ERR_CANCELLED, {}));
    ArchiverEntry entry = {};
    if (archiver_entry_at(archive.handle, i, &entry) != ARCHIVER_OK) continue;
    const std::string name = entry.path ? entry.path : "";
    size += entry.size; packed += entry.packed_size;
    if (name.compare(0, prefix.size(), prefix) == 0 && name.size() > prefix.size()) {
      const std::string rest = name.substr(prefix.size());
      const size_t slash = rest.find('/');
      const std::string child = slash == std::string::npos ? rest : rest.substr(0, slash);
      if (!child.empty()) {
        const std::string key = prefix + child;
        if (rows.size() < 50000 || rows.count(key)) {
          Row &row = rows[key]; row.path = key;
          row.directory = row.directory || slash != std::string::npos || entry.is_directory;
          if (slash == std::string::npos) { row.size = entry.size; row.packed = entry.packed_size; row.modified = entry.modified_unix; row.symlink = entry.is_symlink; }
        } else truncated = true;
      }
    }
    archiver_entry_free(&entry);
  }
  std::string items = ",\"entries\":["; bool first = true;
  for (const auto &pair : rows) {
    const auto &r = pair.second;
    if (!first) items += ',';
    first = false;
    items += "{\"path\":" + json_string(r.path) + ",\"directory\":" + (r.directory ? "true" : "false") +
        ",\"symlink\":" + (r.symlink ? "true" : "false") + ",\"size\":" + std::to_string(r.size) +
        ",\"packed\":" + std::to_string(r.packed) + ",\"modified\":" + std::to_string(r.modified) + "}";
  }
  items += "],\"count\":" + std::to_string(count) + ",\"size\":" + std::to_string(size) +
      ",\"packed\":" + std::to_string(packed) + ",\"truncated\":" + (truncated ? "true" : "false");
  if (archive.typed && callbacks.object) callbacks.accepted(archive.password.c_str());
  return callbacks.rethrow(env) ? nullptr : to_jstring(env, result_json(ARCHIVER_OK, {}, items));
} catch (const std::bad_alloc &) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":12,\"detail\":\"decoder memory allocation failed\"}");
} catch (const std::exception &) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":4,\"detail\":\"native decoder exception\"}");
} catch (...) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":4,\"detail\":\"unknown native decoder exception\"}");
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_localarchiver_ArchiverNative_nativeExtractNested(JNIEnv *env, jobject, jstring archive_path,
    jstring dest_path, jstring engine_path, jobjectArray passwords, jint depth, jlong bytes,
    jboolean keep, jboolean ask, jobject listener) try {
  const std::string path = from_jstring(env, archive_path), dest = from_jstring(env, dest_path), engine = from_jstring(env, engine_path);
  Secrets book; book.read(env, passwords); AndroidCallbacks callbacks(env, listener);
  if (env->ExceptionCheck()) return nullptr;
  ArchiverNestedOptions options = {};
  options.max_depth = static_cast<uint32_t>(depth); options.max_output_bytes = static_cast<uint64_t>(bytes);
  options.keep_intermediate = keep; options.all_branches = 1; options.user = &callbacks;
  if (listener) {
    options.password = [](const char *path, int rejected, char *buffer, uint32_t capacity, void *user) {
      bool cancelled = false;
      std::string value = static_cast<AndroidCallbacks *>(user)->ask(path, rejected != 0, cancelled);
      if (cancelled || value.size() > 1024 || value.size() >= capacity) { wipe(value); return 0; }
      memcpy(buffer, value.c_str(), value.size() + 1); wipe(value); return 1;
    };
    options.password_accepted = [](const char *value, void *user) { static_cast<AndroidCallbacks *>(user)->accepted(value); };
    if (ask) options.choose = [](const char *const *paths, uint32_t count, void *user) { return static_cast<AndroidCallbacks *>(user)->choose(paths, count); };
  }
  std::vector<char> message(65536);
  const ArchiverResult code = archiver_extract_nested_ex(path.c_str(), engine.c_str(), dest.c_str(),
      book.pointers.data(), static_cast<uint32_t>(book.pointers.size()), &options,
      android_progress, &callbacks, message.data(), static_cast<uint32_t>(message.size()));
  return callbacks.rethrow(env) ? nullptr : to_jstring(env, result_json(code, message.data()));
} catch (const std::bad_alloc &) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":12,\"detail\":\"decoder memory allocation failed\"}");
} catch (const std::exception &) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":4,\"detail\":\"native decoder exception\"}");
} catch (...) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":4,\"detail\":\"unknown native decoder exception\"}");
}

extern "C" JNIEXPORT jstring JNICALL
Java_app_localarchiver_ArchiverNative_nativeTest(JNIEnv *env, jobject, jstring archive_path,
    jstring engine_path, jobjectArray passwords, jobject listener) try {
  const std::string path = from_jstring(env, archive_path), engine = from_jstring(env, engine_path);
  Secrets book; book.read(env, passwords); AndroidCallbacks callbacks(env, listener); AuthArchive archive;
  if (env->ExceptionCheck()) return nullptr;
  ArchiverResult code = authenticated_open(path, engine, book, callbacks, archive);
  if (code == ARCHIVER_OK) code = archiver_test_with_progress(archive.handle,
      archive.password.empty() ? nullptr : archive.password.c_str(), android_progress, &callbacks);
  if (code == ARCHIVER_OK && archive.typed && listener) callbacks.accepted(archive.password.c_str());
  return callbacks.rethrow(env) ? nullptr : to_jstring(env, result_json(code, archiver_message(archive.handle)));
} catch (const std::bad_alloc &) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":12,\"detail\":\"decoder memory allocation failed\"}");
} catch (const std::exception &) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":4,\"detail\":\"native decoder exception\"}");
} catch (...) {
  return env->ExceptionCheck() ? nullptr : env->NewStringUTF("{\"code\":4,\"detail\":\"unknown native decoder exception\"}");
}

#if defined(ARCHIVER_JNI_TEST_CALLBACKS)
// Only present in the host test library, never in the Android APK.
extern "C" JNIEXPORT void JNICALL
Java_app_localarchiver_ArchiverNative_checkThreadCallbacks(JNIEnv *env, jobject, jobject listener) {
  AndroidCallbacks callbacks(env, listener);
  std::thread worker([&] {
    callbacks.cancelled(1, 2);
    bool stopped = false;
    std::string password = callbacks.ask("thread.zipSC", false, stopped);
    callbacks.accepted(password.c_str()); wipe(password);
    const char *paths[] = {"one.zip", "two.zip"}; callbacks.choose(paths, 2);
  });
  worker.join(); callbacks.rethrow(env);
}
#endif
