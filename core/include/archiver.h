/* Original code in this tree is MIT. See LICENSE.
   7-Zip is not MIT: LICENSES/7-zip-license.txt and LICENSES/lgpl-2.1.txt.
   RAR decoding is not MIT: LICENSES/unrar-license.txt. */

#ifndef LOCAL_ARCHIVER_ARCHIVER_H
#define LOCAL_ARCHIVER_ARCHIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(ARCHIVER_BUILDING)
#    define ARCHIVER_API __declspec(dllexport)
#  else
#    define ARCHIVER_API __declspec(dllimport)
#  endif
#else
#  define ARCHIVER_API __attribute__((visibility("default")))
#endif

#define ARCHIVER_VERSION "0.6.0"

typedef enum ArchiverResult {
  ARCHIVER_OK = 0,
  ARCHIVER_ERR_ARGUMENT = 1,
  ARCHIVER_ERR_IO = 2,
  ARCHIVER_ERR_FORMAT = 3,
  ARCHIVER_ERR_ENGINE = 4,
  ARCHIVER_ERR_PASSWORD_REQUIRED = 5,
  ARCHIVER_ERR_PASSWORD = 6,
  ARCHIVER_ERR_PATH = 7,
  ARCHIVER_ERR_UNSUPPORTED = 8,
  ARCHIVER_ERR_CANCELLED = 9,
  ARCHIVER_ERR_MISSING_VOLUME = 10,
  ARCHIVER_ERR_NEED_CHOICE = 11,
  ARCHIVER_ERR_LIMIT = 12,
  ARCHIVER_ERR_PARTIAL = 13 /* some independent branches completed, others failed */
} ArchiverResult;

typedef struct ArchiverArchive ArchiverArchive;

typedef struct ArchiverEntry {
  char *path;
  int is_directory;
  int is_symlink;
  uint64_t size;
  /* Added after the original fields. packed_size is 0 when the handler
     does not report one. modified_unix is seconds since 1970, or -1. */
  uint64_t packed_size;
  int64_t modified_unix;
} ArchiverEntry;

/* password is used only for this call. NULL means the user did not type one.
   The library does not write it to disk or to any log.
   engine_path is 7z.so, 7z.dll, or lib7z.dylib. NULL searches next to this
   library and the ARCHIVER_7ZIP variable.
   On failure *out may still be set so archiver_message() can be read.
   If *out is set, the caller must archiver_close() it. */
ARCHIVER_API ArchiverResult archiver_open(const char *archive_path,
                                           const char *engine_path,
                                           const char *password,
                                           ArchiverArchive **out);

ARCHIVER_API ArchiverResult archiver_entry_count(const ArchiverArchive *archive,
                                                  uint32_t *count);
ARCHIVER_API ArchiverResult archiver_entry_at(const ArchiverArchive *archive,
                                               uint32_t index,
                                               ArchiverEntry *out);
ARCHIVER_API void archiver_entry_free(ArchiverEntry *entry);

ARCHIVER_API ArchiverResult archiver_extract(ArchiverArchive *archive,
                                              const char *dest_dir,
                                              const char *password);

/* Runs synchronously on the caller's thread. The callback must not call back
   into this archive. Return nonzero to cancel; the current incomplete file is
   removed, while completed files/directories may remain. No callback is kept
   after this function returns. total may be zero when it is not yet known. */
typedef int (*ArchiverProgressCallback)(uint64_t completed, uint64_t total, void *user);
ARCHIVER_API ArchiverResult archiver_extract_with_progress(
    ArchiverArchive *archive, const char *dest_dir, const char *password,
    ArchiverProgressCallback progress, void *user);

ARCHIVER_API void archiver_close(ArchiverArchive *archive);

/* Stable English token such as "ok" or "password-required". */
ARCHIVER_API const char *archiver_result_string(ArchiverResult result);

/* Empty string when there is no extra detail. Never contains a password. */
ARCHIVER_API const char *archiver_message(const ArchiverArchive *archive);

ARCHIVER_API ArchiverResult archiver_check_entry_path(const char *path);
ARCHIVER_API ArchiverResult archiver_check_symlink(const char *link_path,
                                                    const char *target);

/* Try passwords[0..count) in order. Header check only: the first password
   that opens the archive (and decrypts a small encrypted item, for formats
   that do not fail at Open) is kept. *matched_index is that index, or -1
   when no password was needed. On failure the result is the same as
   archiver_open, including PASSWORD_REQUIRED when every password fails.
   This function does not prompt and does not write the passwords anywhere. */
ARCHIVER_API ArchiverResult archiver_open_with_passwords(const char *archive_path,
                                                          const char *engine_path,
                                                          const char *const *passwords,
                                                          uint32_t password_count,
                                                          int32_t *matched_index,
                                                          ArchiverArchive **out);

/* Check archive bytes without writing them. */
ARCHIVER_API ArchiverResult archiver_test(ArchiverArchive *archive, const char *password);

/* Store the named files into a new ZIP. Names inside the ZIP are the file
   basenames. This does not create RAR. */
ARCHIVER_API ArchiverResult archiver_create_zip(const char *zip_path,
                                                 const char *const *files,
                                                 uint32_t file_count);

/* Extract archive layers until the payload is not an archive.
   Stops on a bad password, a missing volume, more than one archive in a
   layer when chosen_inner is NULL, or a depth / byte / file-count / ratio limit.
   chosen_inner is the relative path of the archive to continue, or NULL.
   message_buf receives a short detail and never a password. */
ARCHIVER_API ArchiverResult archiver_extract_nested(
    const char *archive_path, const char *engine_path, const char *dest_dir,
    const char *const *passwords, uint32_t password_count, const char *chosen_inner,
    ArchiverProgressCallback progress, void *progress_user, char *message_buf,
    uint32_t message_cap);


/* Callbacks run synchronously on the worker/calling thread. choose returns an
   index, -1 to stop successfully, -2 to cancel, or -3 to unpack all branches.
   password writes a NUL-terminated UTF-8 password and returns 1; 0 cancels.
   Saved passwords are always tried before asking. No callback is retained. */
typedef int (*ArchiverNestedChoose)(const char *const *paths, uint32_t count, void *user);
typedef int (*ArchiverNestedPassword)(const char *path, int rejected, char *buffer,
                                    uint32_t capacity, void *user);
typedef struct ArchiverNestedOptions {
  uint32_t max_depth;          /* 1..32; reaching this chosen depth is success */
  uint64_t max_output_bytes;   /* cumulative across layers, 0 = 64 GiB */
  int keep_intermediate;      /* original input is always kept */
  int all_branches;           /* independent branches continue after recoverable errors;
                                multiple archives get separate .unpacked folders */
  ArchiverNestedChoose choose;
  ArchiverNestedPassword password;
  void (*password_accepted)(const char *password, void *user); /* verified; do not log */
  void *user;
} ArchiverNestedOptions;
/* message_buf includes completed archive paths and failed branch details.
   Cancellation, unsafe paths, engine errors and global limits stop immediately.
   Known document/application containers are preserved during automatic recursion;
   explicitly opening one still allows extraction. */
ARCHIVER_API ArchiverResult archiver_extract_nested_ex(
    const char *archive_path, const char *engine_path, const char *dest_dir,
    const char *const *passwords, uint32_t password_count,
    const ArchiverNestedOptions *options, ArchiverProgressCallback progress,
    void *progress_user, char *message_buf, uint32_t message_cap);

#ifdef __cplusplus
}
#endif

#endif
