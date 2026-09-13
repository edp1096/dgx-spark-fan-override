#define main watcher_main
#include "nvfanwatch.c"
#undef main
#include <assert.h>
int main(void)
{
    for (int p = 1; p <= 3; ++p) {
        assert(!choose_max(on_temp[p] - 1, false, p));
        assert(choose_max(on_temp[p], false, p));
        assert(choose_max(on_temp[p] - 5000, true, p));
        assert(!choose_max(on_temp[p] - 5001, true, p));
        assert(choose_max(100000, false, p));
        assert(!choose_max(30000, true, p));
    }
    puts("All three profiles: rise, fall, hysteresis and jumps passed.");
    return 0;
}
