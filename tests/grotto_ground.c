/* Check the exposed shelf on the approach. Isolating the terrain pass keeps a
   passing fish or a dark coral silhouette from being mistaken for a stripe. */
#include "render.c"
#include <stdio.h>

int main(void){
    const int seconds[]={8,11,14};
    uint8_t pixels[LCD_H][52];
    generate_world();
    prepare_renderer();
    for(unsigned i=0;i<sizeof seconds/sizeof *seconds;i++){
        Diver d={0};
        d.time=4*SHOT_SECONDS+seconds[i];
        demo_pose(&d,5,seconds[i]);
        render(&d,pixels[0],sizeof pixels[0]);
        water();
        terrain();
        int groundBlocks=0;
        for(int y=184;y<LCD_H;y+=4){
            int consecutive=0;
            for(int band=144/BAND;band<BANDS;band++){
                if(y<roofMax[ringCount][band]){consecutive=0;continue;}
                groundBlocks++;
                int light=0;
                for(int dy=0;dy<4;dy++)light+=__builtin_popcount(pixels[y+dy][band]);
                consecutive=light<=4?consecutive+1:0;
                if(consecutive>=7){
                    fprintf(stderr,"Dark ground stripe at %d s, row %d\n",seconds[i],y);
                    return 1;
                }
            }
        }
        if(!groundBlocks){fprintf(stderr,"Grotto shelf out of view at %d s\n",seconds[i]);return 1;}
    }
    puts("Grotto shelf is visible without long dark strips beneath the coral.");
    return 0;
}
