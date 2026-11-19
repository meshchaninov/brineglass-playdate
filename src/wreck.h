#ifndef WRECK_H
#define WRECK_H

#include "garden.h"

#define WRECK_X 59.5f
#define WRECK_Y (-23.f)
#define WRECK_Z 6.f
#define WRECK_FORWARD_X .90f
#define WRECK_FORWARD_Z .43f

/* Local x runs stern to bow, local z runs across the deck. */
static inline Vec ship(float x,float y,float z){
    return (Vec){WRECK_X+x*WRECK_FORWARD_X-z*WRECK_FORWARD_Z,
                 WRECK_Y+y-.12f*x+.14f*z,
                 WRECK_Z+x*WRECK_FORWARD_Z+z*WRECK_FORWARD_X};
}
static inline void wreck_coordinates(float x,float z,float *along,float *across){
    const float scale=WRECK_FORWARD_X*WRECK_FORWARD_X+WRECK_FORWARD_Z*WRECK_FORWARD_Z;
    float dx=x-WRECK_X,dz=z-WRECK_Z;
    *along=(dx*WRECK_FORWARD_X+dz*WRECK_FORWARD_Z)/scale;
    *across=(dz*WRECK_FORWARD_X-dx*WRECK_FORWARD_Z)/scale;
}
#endif
