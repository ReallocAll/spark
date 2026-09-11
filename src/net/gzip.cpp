#include "net/gzip.h"

#include <zlib.h>

#include <algorithm>
#include <stdexcept>
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
#include <functional>
#endif

namespace spark {

#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
// NOLINTNEXTLINE(misc-use-internal-linkage): shared with the lifecycle fixture.
thread_local std::function<void()> GzipStepForTesting;
#endif

std::string gzipCompress(const std::string &input)
{
    return gzipCompress(input, {});
}

std::string gzipCompress(const std::string &input, const CancellationToken &cancellation)
{
    if (cancellation.stopRequested()) {
        throw std::runtime_error("gzip: cancelled");
    }
    z_stream zs{};
    // windowBits 15 + 16 selects a gzip wrapper.
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("gzip: deflateInit2 failed");
    }

    struct DeflateGuard {
        z_stream &stream;
        ~DeflateGuard() { deflateEnd(&stream); }
    } guard{zs};

    std::string out;
    char buffer[16384];
    int ret;
    std::size_t offset = 0;
    do {
        if (cancellation.stopRequested()) {
            throw std::runtime_error("gzip: cancelled");
        }
        if (zs.avail_in == 0 && offset < input.size()) {
            const auto count = std::min(input.size() - offset, sizeof(buffer));
            zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data() + offset));
            zs.avail_in = static_cast<uInt>(count);
            offset += count;
        }
        zs.next_out = reinterpret_cast<Bytef *>(buffer);
        zs.avail_out = sizeof(buffer);
        ret = deflate(&zs, offset == input.size() ? Z_FINISH : Z_NO_FLUSH);
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (GzipStepForTesting) {
            GzipStepForTesting();
        }
#endif
        out.append(buffer, sizeof(buffer) - zs.avail_out);
    } while (ret == Z_OK);

    if (cancellation.stopRequested()) {
        throw std::runtime_error("gzip: cancelled");
    }
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("gzip: deflate failed");
    }
    return out;
}

}  // namespace spark
