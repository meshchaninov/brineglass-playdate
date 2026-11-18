#ifndef GARDEN_STONE_H
#define GARDEN_STONE_H
#include <stdint.h>

enum { STONE_SIDE=32, STONE_MASK=STONE_SIDE-1, STONE_FRAC=256, STONE_UNITS=2*STONE_FRAC };
extern int8_t stoneLight[STONE_SIDE][STONE_SIDE][STONE_SIDE];
void bake_stone(void);

/* Q8 coordinates wrap in all three axes. Sampling a solid material keeps its
   veins anchored through vertical faces and across projection directions. */
static inline int stone_sample(int u,int v,int w){
    unsigned x=((unsigned)u>>8)&STONE_MASK,y=((unsigned)v>>8)&STONE_MASK,z=((unsigned)w>>8)&STONE_MASK;
    unsigned x1=(x+1)&STONE_MASK,y1=(y+1)&STONE_MASK,z1=(z+1)&STONE_MASK;
    int fx=u&255,fy=v&255,fz=w&255;
    const int8_t *a=stoneLight[z][y],*b=stoneLight[z][y1];
    const int8_t *c=stoneLight[z1][y],*d=stoneLight[z1][y1];
    int top=a[x]*256+(a[x1]-a[x])*fx, bottom=b[x]*256+(b[x1]-b[x])*fx;
    int front=top+((bottom-top)*fy>>8);
    top=c[x]*256+(c[x1]-c[x])*fx;bottom=d[x]*256+(d[x1]-d[x])*fx;
    int back=top+((bottom-top)*fy>>8);
    return (front+((back-front)*fz>>8))>>8;
}
#endif
