#include "garden.h"
#include <stdio.h>

/* Count only the pixels affected by this frond, including foreground occlusion. */
static int kelp_pixels(Prop *kelp, int frame) {
    static uint8_t scene[240][52], background[240][52];
    Diver diver = {0};
    float elapsed = frame / 30.f;
    demo_pose(&diver, 3, elapsed);
    diver.time = 3 * SHOT_SECONDS + elapsed;
    render(&diver, scene[0], 52);
    kelp->kind = EMPTY_PROP;
    render(&diver, background[0], 52);
    kelp->kind = KELP;

    int changed = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 50; x++)
            changed += __builtin_popcount(scene[y][x] ^ background[y][x]);
    return changed;
}

int main(void) {
    generate_world();
    prepare_renderer();
    /* This frond crosses the 37 m draw limit between frames 262 and 263. */
    Prop *kelp = &props[447];
    if (kelp->kind != KELP) return 1;
    int outside = kelp_pixels(kelp, 262);
    int inside = kelp_pixels(kelp, 263);
    int distant = kelp_pixels(kelp, 300);
    int visible = kelp_pixels(kelp, 330);
    int close = kelp_pixels(kelp, 405);
    printf("Sponge Canyon kelp pixels: %d -> %d at cutoff, then %d, %d, %d\n",
           outside, inside, distant, visible, close);
    return !(outside == 0 && inside == 0 && distant > 0 && distant < visible
             && visible < close && close > 200);
}
