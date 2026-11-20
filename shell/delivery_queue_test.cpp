#include "delivery_queue.h"

#include <cstdlib>
#include <string>

static void check(bool condition) {
    if (!condition) std::abort();
}

int main() {
    ainput::DeliveryQueue<int, std::string> queue;
    queue.remember(1, 10);
    queue.remember(2, 10);
    queue.remember(3, 20);
    check(queue.finish(2, "second").empty());
    auto other = queue.finish(3, "other context");
    check(other.size() == 1 && other[0].id == 3);
    auto ready = queue.finish(1, "timeout");
    check(ready.size() == 2 && ready[0].id == 1 && ready[1].id == 2);
    check(*ready[0].outcome == "timeout" && *ready[1].outcome == "second");

    queue.remember(4, 10);
    queue.remember(5, 10);
    check(queue.finish(5, "buffered before Escape").empty());
    check(queue.discard().size() == 2 && queue.size() == 0);
    queue.remember(6, 10);
    check(queue.finish(4, "late after Escape").empty());
    check(queue.finish(5, "late duplicate").empty());
    auto fresh = queue.finish(6, "fresh");
    check(fresh.size() == 1 && *fresh[0].outcome == "fresh");
}
