#include "platform/levilamina/bds/pubsub.h"

namespace spark::levilamina::bds::pubsub {

// Keep this translation unit so the private ABI view has a stable compiled
// home when the host target is linked with the generated Bedrock import lib.
template class ConnectorView<ChunkLoadedSignature>;
template class ConnectorView<ChunkDiscardedSignature>;
template class ConnectorView<DimensionCreatedSignature>;

}  // namespace spark::levilamina::bds::pubsub
