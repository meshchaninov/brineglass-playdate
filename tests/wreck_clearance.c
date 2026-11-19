/* Host-only regression for the ship approach. Use the production culling and
   triangle rasterizer, with white hull facets so dithering cannot hide a hole. */
#include "render.c"
#include <stdio.h>

enum { ROW_BYTES = LCD_W / 8, SHIPWRECK_SITE = 1 };
typedef uint8_t Frame[LCD_H * ROW_BYTES];
static Frame scene, clipped, unobstructed;

static void ship_silhouette(Frame output){
    memset(output, 0, sizeof(Frame));
    lcd = output;
    memset(bucketHead, 0xff, sizeof bucketHead);
    commandCount = 0;
    draw_model(&landmarks[0]);
    for(int i = 0; i < commandCount; i++){
        const Command *facet = &commands[i];
        raster_triangle(facet->v[0], facet->v[1], facet->v[2], 255);
    }
}

int main(void){
    generate_world();
    prepare_renderer();
    for(int n = 0; n < (int)(SHOT_SECONDS * 30); n++){
        Diver diver = {0};
        diver.time = n / 30.f;
        demo_pose(&diver, SHIPWRECK_SITE, diver.time);
        render(&diver, scene, ROW_BYTES);
        memset(waterRow, 255, sizeof waterRow);
        ship_silhouette(clipped);
        /* terrain() reconstructs every slice from the clear first row on the
           next frame. With all slices clear, the entire hull should appear. */
        memset(roof, LCD_H, sizeof roof);
        memset(roofMin, LCD_H, sizeof roofMin);
        memset(roofMax, LCD_H, sizeof roofMax);
        ship_silhouette(unobstructed);
        if(!memchr(unobstructed, 255, sizeof(Frame)) ||
           memcmp(clipped, unobstructed, sizeof(Frame)) != 0){
            fprintf(stderr, "Wreck silhouette failed at frame %d (%.2f s)\n", n, diver.time);
            return 1;
        }
    }
    puts("Full ship silhouette visible throughout the wreck flyby.");
    return 0;
}
