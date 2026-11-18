#include "stone.h"
#include <math.h>

int8_t stoneLight[STONE_SIDE][STONE_SIDE][STONE_SIDE];

static float corner(int x,int y,int z,int mask){
    uint32_t n=(uint32_t)(x&mask)*73856093u^(uint32_t)(y&mask)*19349663u^(uint32_t)(z&mask)*83492791u;
    n^=n>>15;n*=0x2c1b3c6du;n^=n>>12;
    return (float)(n>>16)*(2.f/65535.f)-1.f;
}
static float noise(float x,float y,float z,int cells){
    float scale=(float)cells/STONE_SIDE;
    x*=scale;y*=scale;z*=scale;
    int i=(int)floorf(x),j=(int)floorf(y),k=(int)floorf(z);
    float u=x-i,v=y-j,w=z-k;
    u=u*u*(3-2*u);v=v*v*(3-2*v);w=w*w*(3-2*w);
    float result=0;
    for(int dz=0;dz<2;dz++)for(int dy=0;dy<2;dy++){
        float a=corner(i,j+dy,k+dz,cells-1),b=corner(i+1,j+dy,k+dz,cells-1);
        result+=(a+(b-a)*u)*(dy?v:1-v)*(dz?w:1-w);
    }
    return result;
}
static void relief_slice(float dst[STONE_SIDE][STONE_SIDE],int z){
    for(int y=0;y<STONE_SIDE;y++)for(int x=0;x<STONE_SIDE;x++){
        float large=noise(x,y,z,4);
        float medium=noise(x+3*large,y-2*large,z+1.5f*large,8);
        float fine=noise(x+5,y+11,z+7,16);
        /* Fractured plates with a recessed seam and slanted bedding planes. */
        float joint=fmaxf(0,1-3.6f*fabsf(large+.27f*medium));
        float bed=cosf((y*4+x+z)*(.19634954f)+3.7f*large+.58f*medium);
        dst[y][x]=large*16+medium*8+fine*3.5f-joint*joint*14+fmaxf(0,bed)*5;
    }
}
void bake_stone(void){
    /* Two adjacent slices suffice to differentiate relief along the light. */
    float slices[2][STONE_SIDE][STONE_SIDE];
    relief_slice(slices[0],0);
    for(int z=0;z<STONE_SIDE;z++){
        float (*a)[STONE_SIDE]=slices[z&1],(*b)[STONE_SIDE]=slices[(z+1)&1];
        relief_slice(b,(z+1)&STONE_MASK);
        for(int y=0;y<STONE_SIDE;y++)for(int x=0;x<STONE_SIDE;x++){
            float h=a[y][x];
            float bevel=1.3f*(a[y][(x+1)&STONE_MASK]-h)+
                        2.8f*(a[(y+1)&STONE_MASK][x]-h)+1.1f*(b[y][x]-h);
            stoneLight[z][y][x]=(int8_t)fminf(57,fmaxf(-58,1.23f*h+bevel));
        }
    }
}
