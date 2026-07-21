#ifndef ENDSTONE_SPARK_BYTEBIN_H
#define ENDSTONE_SPARK_BYTEBIN_H

#include <string>

namespace spark {

struct UploadResult {
    bool ok = false;
    std::string key;    // bytebin content key (append to the viewer URL)
    std::string error;  // populated when !ok
};

// POST a raw payload to a bytebin instance and return the content key.
// Request-side Content-Encoding is intentionally omitted: the viewer must receive
// the protobuf payload, not a second gzip envelope.
UploadResult uploadToBytebin(const std::string &body, const std::string &bytebin_url,
                             const std::string &content_type, const std::string &user_agent);

}  // namespace spark

#endif  // ENDSTONE_SPARK_BYTEBIN_H
