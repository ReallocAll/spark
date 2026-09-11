#ifndef ENDSTONE_SPARK_GZIP_H
#define ENDSTONE_SPARK_GZIP_H

#include <string>

#include "net/cancellation.h"

namespace spark {

// gzip-compress a buffer (zlib). Throws std::runtime_error on failure.
std::string gzipCompress(const std::string &input);
std::string gzipCompress(const std::string &input, const CancellationToken &cancellation);

}  // namespace spark

#endif  // ENDSTONE_SPARK_GZIP_H
