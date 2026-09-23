#include "platform/levilamina/bds/pubsub.h"

namespace spark::levilamina::bds::pubsub {

// Keep compiled ConnectorView instantiations in this translation unit.
template class ConnectorView<ChunkLoadedSignature>;
template class ConnectorView<ChunkDiscardedSignature>;
template class ConnectorView<DimensionCreatedSignature>;

}  // namespace spark::levilamina::bds::pubsub
