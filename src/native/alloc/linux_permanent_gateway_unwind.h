#ifndef ENDSTONE_SPARK_LINUX_PERMANENT_GATEWAY_UNWIND_H
#define ENDSTONE_SPARK_LINUX_PERMANENT_GATEWAY_UNWIND_H

#include "native/alloc/linux_permanent_gateway_registry.h"

namespace spark::gateway::permanent {

HostUnwinder resolveHost(const void *anchor);
bool registerHost(Directory &directory, Deadline deadline, bool &quarantined);
bool verifyHost(const Directory &directory, Deadline deadline = Deadline::max());
bool registerPrivate(const Directory &directory, const void *anchor);
bool unregisterHost(const Directory &directory, Deadline deadline);
void unregisterPrivate();

}  // namespace spark::gateway::permanent

#endif
