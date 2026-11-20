#pragma once
#include <functional>

namespace PNS {
    bool QueueLayerRestore(std::function<void()> restore);
}
