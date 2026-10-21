#ifndef MILKZIP_LZ4_STREAM_HPP
#define MILKZIP_LZ4_STREAM_HPP

#include "archiver.h"
#include "safe_write.hpp"
#include <cstdio>
#include <string>

namespace archiver {
// The caller owns the input and commits output only after successful decoding.
// A null output validates the stream without writing it.
ArchiverResult decode_lz4(FILE *input, SafeDest::File *output,
    ArchiverProgressCallback progress, void *user, std::string &message);
}
#endif
