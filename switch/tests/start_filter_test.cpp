// Host check of NxStartFilter: c++ -std=c++20 tests/start_filter_test.cpp
#include <cassert>
#include <cstdio>

#include "../nx_pad.h"

int main() {
    // + held alone: Start after the delay.
    NxStartFilter held;
    assert(held.apply(NX_PLUS, 1000) == 0);
    assert(held.apply(NX_PLUS, 1050) == 0);
    assert(held.apply(NX_PLUS, 1100) == NX_PLUS);
    assert(held.apply(0, 1200) == 0);

    // A 40 ms tap is replayed as Start after release, for DELAY_MS.
    NxStartFilter tap;
    assert(tap.apply(NX_PLUS, 2000) == 0);
    assert(tap.apply(0, 2040) == NX_PLUS);
    assert(tap.apply(0, 2100) == NX_PLUS);
    assert(tap.apply(0, 2140) == 0);

    // - and + together, in either order: never Start, not even on release.
    NxStartFilter combo;
    assert(combo.apply(NX_PLUS, 3000) == 0);
    assert(combo.apply(NX_PLUS | NX_MINUS, 3030) == NX_MINUS);
    assert((combo.apply(NX_PLUS | NX_MINUS, 3500) & NX_PLUS) == 0);
    assert(combo.apply(0, 3600) == 0);
    assert(combo.apply(NX_MINUS, 4000) == NX_MINUS);
    assert(combo.apply(NX_MINUS | NX_PLUS, 4020) == NX_MINUS);
    assert(combo.apply(0, 4050) == 0);
    std::puts("start_filter: OK");
    return 0;
}
