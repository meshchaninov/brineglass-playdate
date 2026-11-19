#include "garden.h"
#include "wreck.h"
#include "stone.h"
#include "coral.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Projection is expressed in the original camera's 200-unit film plane.
   Coverage, depth and the one-bit output all use native LCD coordinates. */
enum { LCD_W=400, LCD_H=240, CLEAR_RANGE=12, REEF_RANGE=37, TERRAIN_RANGE=74 };
static const int viewWidth=LCD_W,viewHeight=LCD_H,focal=110;
static const float meshScale=2;

static uint8_t wave[256],ink[256][4],waterRow[LCD_H];
static uint8_t *lcd;
static int pitchBytes,tick,baking;
static const Diver *view;
static float headingX,headingZ,horizon;
static float clampf(float v,float lo,float hi){return fminf(hi,fmaxf(lo,v));}
static int min2(int a,int b){return a<b?a:b;}
static int max2(int a,int b){return a>b?a:b;}
static int sine(int i){return wave[(unsigned)i&255];}
/* Linear interpolation keeps the phase continuous for moving animals. */
enum { TRIG_STEPS=1024 };
static float motionTable[TRIG_STEPS+1];
static inline float motion_wave(float index){
    int i=(int)floorf(index);
    float frac=index-i;
    i=(unsigned)i&(TRIG_STEPS-1);
    return motionTable[i]+frac*(motionTable[i+1]-motionTable[i]);
}
static inline float motion_sin(float angle){return motion_wave(angle*(TRIG_STEPS/6.283185307f));}
static inline float motion_cos(float angle){return motion_wave(angle*(TRIG_STEPS/6.283185307f)+TRIG_STEPS/4);}
static uint8_t seaTile[64][64],waterTone[LCD_H/4][LCD_W/8];
/* Eight native samples of a linear light ramp at the LCD's Bayer phase. */
enum { RAMP_RADIUS=12 };
static uint8_t rampInk[2*RAMP_RADIUS+1][256][4];
/* A stationary screen-door pattern, independent of the four-pixel ink pattern.
   Within every Bayer phase all sixteen coarse ranks occur once per tile. */
enum { COVER_STEPS=64 };
typedef struct {uint8_t low,high;} Coverage;
static const Coverage opaque={0,COVER_STEPS};
static Coverage detailCover={0,COVER_STEPS},drawCover={0,COVER_STEPS};
static uint16_t coverRows[COVER_STEPS+1][16];
static uint8_t landmarkCover[4096],reefCover[4096];
static const uint8_t *rangeCover=landmarkCover;

static int visibility(float z,float clear,float limit){
    float t=clampf((limit-z)/(limit-clear),0,1);
    return (int)(COVER_STEPS*t*t*(3-2*t)+.5f);
}
static void init_tables(void){
    static const uint8_t bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    for(int y=0;y<16;y++)for(int x=0;x<16;x++){
        int phase=bayer[y&3][x&3];
        int rank=4*((bayer[y>>2][x>>2]+phase*7)&15)+(phase>>2);
        uint16_t bit=(uint16_t)(128u>>(x&7))<<((x>>3)*8);
        for(int level=rank+1;level<=COVER_STEPS;level++)coverRows[level][y]|=bit;
    }
    for(int i=0;i<TRIG_STEPS;i++)motionTable[i]=sinf(i*(6.283185307f/TRIG_STEPS));
    motionTable[TRIG_STEPS]=motionTable[0];
    for(int i=0;i<256;i++){
        wave[i]=(uint8_t)(127.5f+127.5f*sinf(i*.02454369f));
        float t=clampf((i-26)/208.f,0,1);
        int level=(int)(255*t*t*(3-2*t));
        for(int y=0;y<4;y++)for(int x=0;x<8;x++)
            if(level>16*bayer[y][x&3]+7)ink[i][y]|=128u>>x;
    }
    for(int g=-RAMP_RADIUS;g<=RAMP_RADIUS;g++)for(int tone=0;tone<256;tone++)
        for(int y=0;y<4;y++)for(int x=0;x<8;x++){
            int value=min2(255,max2(0,tone+((2*x-7)*g)/4));
            rampInk[g+RAMP_RADIUS][tone][y]|=ink[value][y]&(128u>>x);
        }
    for(int z=0;z<64;z++)for(int x=0;x<64;x++){
        int a=sine(x*4+sine(z*4)/6),b=sine(z*4+a/4);
        seaTile[z][x]=(uint8_t)(a/4+b/7+max2(0,35-abs(a-b))*2);
    }
}
/* Project the waves at the corners of an 8 by 4 patch. Interpolate the
   light on each LCD row and threshold each of its eight native pixels. */
static void water(void){
    int phase=(int)(view->time*16),angle=(int)(view->yaw*108);
    int shafts[276];
    for(int i=0;i<276;i++){
        int a=i+angle,group=sine(a*2+phase/6)+sine(a*4-phase/8);
        shafts[i]=(group*group/4000)*(80+sine(5*a+phase/5))/340;
    }
    uint8_t corners[2][LCD_W/8+1];
    for(int y=0;y<=LCD_H;y+=4){
        uint8_t *cur=corners[(y/4)&1];
        int sceneY=y/2;
        float elevation=(horizon-sceneY)/focal;
        int ambient=161+(int)(44*clampf(elevation+.35f,-.8f,1.5f)+view->p.y*.28f);
        float dist=-view->p.y/fmaxf(.05f,elevation),step=dist/focal;
        int surf=(int)(72*clampf(elevation*2,0,1)*clampf((175-dist)/125,0,1));
        float wx=view->p.x+dist*headingX-100*step*headingZ;
        float wz=view->p.z+dist*headingZ+100*step*headingX;
        int u=(int)((wx*3.3f+wz*6.2f)*64)+phase*64,v=(int)((wz*9.7f-wx*2.5f)*64)-phase*64;
        int du=(int)((headingZ*3.3f-headingX*6.2f)*step*256);
        int dv=(int)((-headingX*9.7f-headingZ*2.5f)*step*256);
        int shift=(119-sceneY)*3/5;
        if(y<LCD_H)memset(waterRow+y,ambient,4);
        for(int col=0;col<=LCD_W/8;col++,u+=du,v+=dv){
            int ripple=0;
            if(surf){
                int broad=seaTile[(v>>8)&63][(u>>8)&63];
                int cross=seaTile[((u>>9)-(phase>>1))&63][((v>>9)+(phase>>2))&63];
                ripple=surf*(3*broad+cross-228)/384;
            }
            int base=ambient+ripple, gain=176-sceneY;
            int a=min2(255,base+((shafts[4*col+shift]*gain)>>7));
            cur[col]=a+((176-a)>>3);
        }
        if(!y)continue;
        const uint8_t *prev=corners[((y/4)&1)^1];
        for(int col=0;col<LCD_W/8;col++){
            int l=prev[col],r=prev[col+1];
            int grad=min2(RAMP_RADIUS,max2(-RAMP_RADIUS,((r-l)+(cur[col+1]-cur[col]))/8));
            int tone=(l+r)*4;
            int dy=cur[col]+cur[col+1]-l-r;
            waterTone[y/4-1][col]=(tone+2*dy+4)>>3;
            const uint8_t (*ramp)[4]=rampInk[grad+RAMP_RADIUS];
            uint8_t *out=lcd+(y-4)*pitchBytes+col;
            for(int j=0;j<4;j++,tone+=dy)out[j*pitchBytes]=ramp[(tone+(dy/2)+4)>>3][j];
        }
    }
}
enum { RINGS=32, BAND=8, BANDS=LCD_W/BAND };
typedef struct {float z,inv; int key,fog,coverage;} Ring;
static Ring rings[RINGS];
static int ringCount;
static uint8_t roof[RINGS+1][LCD_W],roofMin[RINGS+1][BANDS],roofMax[RINGS+1][BANDS];
static uint8_t ringForKey[4096],fogForKey[4096];
static int8_t strata[64][64];
typedef struct {int16_t y,tone,phase,rock;} Knot;
static Knot knots[RINGS][BANDS+1];

static void terrain_tables(void){
    for(float z=1.2f;ringCount<RINGS;){
        z=fminf(z,TERRAIN_RANGE);
        rings[ringCount++]=(Ring){z,220.f/z,(int)(32760/z),(int)(clampf((z-12)/62,0,1)*230),visibility(z,45,TERRAIN_RANGE-1)};
        if(z>=TERRAIN_RANGE)break;
        z+=1.0f+z*.105f;
    }
    for(int i=0;i<4096;i++){
        int key=i*8+4,k=0;
        while(k<ringCount && rings[k].key>key)k++;
        ringForKey[i]=k;
        float z=32760.f/key;
        landmarkCover[i]=visibility(z,45,TERRAIN_RANGE-1);
        reefCover[i]=visibility(z,19,REEF_RANGE-2);
        float t=clampf((z-CLEAR_RANGE)/(TERRAIN_RANGE-CLEAR_RANGE),0,1);
        fogForKey[i]=(uint8_t)(248*t*t*(3-2*t));
    }
    memset(roof[0],LCD_H,LCD_W);memset(roofMin[0],LCD_H,BANDS);memset(roofMax[0],LCD_H,BANDS);
    for(int z=0;z<64;z++)for(int x=0;x<64;x++)
        strata[z][x]=(int8_t)(stone_sample(x*128,z*150,x*73+z*13)*.63f+(sine(z*19+sine(x*4)/8)-127)*.06f);
}
static void terrain_knots(void){
    for(int k=0;k<ringCount;k++){
        Ring s=rings[k];
        float dx=s.z*(BAND/220.f)*headingZ,dz=-s.z*(BAND/220.f)*headingX;
        float wx=view->p.x+s.z*headingX-dx*(BANDS*.5f);
        float wz=view->p.z+s.z*headingZ-dz*(BANDS*.5f);
        int gx=(int)((wx+EXTENT)*1024),gz=(int)((wz+EXTENT)*1024);
        int stepX=(int)(dx*1024),stepZ=(int)(dz*1024);
        for(int i=0;i<=BANDS;i++,gx+=stepX,gz+=stepZ){
            Knot *n=&knots[k][i];
            if((unsigned)gx>=(GRID-1)*2048u || (unsigned)gz>=(GRID-1)*2048u){
                *n=(Knot){LCD_H*4,160,0,0};continue;
            }
            int ix=gx>>11,iz=gz>>11,fx=(gx>>3)&255,fz=(gz>>3)&255;
            const int16_t *a=&seabed[iz][ix],*b=a+GRID;
            int h0=a[0]*256+(a[1]-a[0])*fx,h1=b[0]*256+(b[1]-b[0])*fx;
            float h=(h0+(((h1-h0)*fz)>>8))*(1.f/1024);
            n->y=(int16_t)clampf((2*horizon+(view->p.y-h)*s.inv)*4,-8000,8000);
            const uint8_t *p=&groundTone[gz>>10][gx>>10],*q=p+GROUND_SAMPLES;
            int tx=(gx>>6)&15,tz=(gz>>6)&15;
            int t0=p[0]*16+(p[1]-p[0])*tx,t1=q[0]*16+(q[1]-q[0])*tx;
            int caustic=seaTile[((gz>>8)-(tick>>2))&63][((gx>>8)+(tick>>2))&63];
            n->tone=(t0+(((t1-t0)*tz)>>4))/16+(caustic>>3)+max2(0,caustic-100)/3-7;
            n->phase=((gx>>8)+(gz>>9))&63;
            n->rock=(int)clampf((h+42)*18,0,170);
        }
    }
}
static inline uint8_t edge_bits(const uint8_t *edge,int y){
#if defined(__ARM_FEATURE_DSP) && defined(__thumb__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    /* Compare four unsigned LCD column heights at once. SEL takes the
       corresponding bit only when the height is at least y+1. */
    uint32_t a,b,tmp,u,v,level=(y+1)*0x01010101u;
    memcpy(&a,edge,4);memcpy(&b,edge+4,4);
    __asm__("usub8 %0,%3,%5\n\tsel %1,%6,%8\n\tusub8 %0,%4,%5\n\tsel %2,%7,%8"
            : "=&r"(tmp),"=&r"(u),"=&r"(v)
            : "r"(a),"r"(b),"r"(level),"r"(0x10204080u),"r"(0x01020408u),"r"(0u) : "cc");
    u|=v;u|=u>>16;return (uint8_t)(u|(u>>8));
#else
    unsigned bits=0;for(int i=0;i<8;i++)bits=(bits<<1)|(y<edge[i]);return bits;
#endif
}
/* Each band advances away from the eye. Only the newly exposed part of
   each slope is written. The previous horizon is also its foreground clip. */
static void terrain(void){
    terrain_knots();
    for(int k=0;k<ringCount;k++){
        int texRow[LCD_H/4];
        float step=rings[k].z/220.f;
        float height=(view->p.y+2*horizon*step)*4;
        for(int y=0;y<LCD_H/4;y++,height-=16*step)texRow[y]=(int)height&63;
        for(int band=0;band<BANDS;band++){
            Knot l=knots[k][band],r=knots[k][band+1];
            const uint8_t *previous=roof[k]+band*8;
            uint8_t *top=roof[k+1]+band*8;
            int low=roofMin[k][band],high=roofMax[k][band];
            int nextLow=low,nextHigh=high;
            if(min2(l.y,r.y)+2>=high*4){
                memcpy(top,previous,8);
            }else{
                int ys=l.y*16+32+(r.y-l.y),dy=(r.y-l.y)*2;
                nextLow=LCD_H;nextHigh=0;
                for(int x=0;x<8;x++,ys+=dy){
                    int edge=min2(previous[x],max2(0,ys>>6));
                    top[x]=edge;
                    nextLow=min2(nextLow,edge);nextHigh=max2(nextHigh,edge);
                }
                int base=(l.tone+r.tone)/2,rock=(l.rock+r.rock)/2,fog=rings[k].fog;
                int phase=l.phase;
                int gradient=min2(RAMP_RADIUS,max2(-RAMP_RADIUS,((r.tone-l.tone)*(256-fog))>>10));
                const uint8_t (*ramp)[4]=rampInk[gradient+RAMP_RADIUS];
                uint8_t *out=lcd+nextLow*pitchBytes+band;
                for(int y=nextLow;y<high;){
                    int end=min2(high,(y|3)+1);
                    int tone=base+((strata[texRow[y/4]][phase]*rock)>>8);
                    tone+=((waterTone[y/4][band]-tone)*fog)>>8;
                    const uint8_t *pattern=ramp[min2(255,max2(0,tone))];
                    for(;y<end;y++,out+=pitchBytes){
                        unsigned mask=(y<low?255:edge_bits(previous,y));
                        if(y<nextHigh)mask&=~edge_bits(top,y);
                        if(rings[k].coverage<COVER_STEPS)
                            mask&=coverRows[rings[k].coverage][y&15]>>(8*(band&1));
                        *out^=(*out^pattern[y&3])&mask;
                    }
                }
            }
            roofMin[k+1][band]=nextLow;roofMax[k+1][band]=nextHigh;
        }
    }
}
typedef struct {float x,y,z;} Camera;
typedef struct {float x,y,iz;} Screen;
static Camera camera(Vec p){
    float x=p.x-view->p.x,z=p.z-view->p.z;
    return (Camera){x*headingZ-z*headingX,p.y-view->p.y,x*headingX+z*headingZ};
}
static Screen project(Camera p){
    float inv=1/p.z;
    return (Screen){meshScale*(100+focal*p.x*inv),meshScale*(horizon-focal*p.y*inv),32760*inv};
}


/* Generated once from the same ribbon geometry used for the plant silhouette. */
static uint8_t bakedLeaf[80][64],leafMask[80][64];
typedef struct {int ring,clearUntil;const uint8_t *pattern;} Brush;
static inline Brush brush(int key,int tone,int y){
    int bin=min2(4095,max2(0,key)>>3);
    tone+=((waterRow[min2(239,max2(0,y))]-tone)*fogForKey[bin])>>8;
    return (Brush){ringForKey[bin],0,ink[tone]};
}
static void bound_brush(Brush *pen,int x0,int x1){
    int limit=LCD_H;
    const uint8_t *edge=roofMin[pen->ring];
    for(int i=max2(0,x0)>>3;i<=min2(LCD_W-1,x1)>>3;i++)limit=min2(limit,edge[i]);
    pen->clearUntil=limit;
}
static inline void stroke(int y,int left,int right,Brush pen){
    if(left>=right)return;
    unsigned pattern=pen.pattern[y&3],mask=255u>>(left&7);
    int a=left>>3,b=(right-1)>>3;
    unsigned last=(255u<<(7-((right-1)&7)))&255;
    uint8_t *row=lcd+y*pitchBytes;
    if(drawCover.low || drawCover.high<COVER_STEPS){
        uint16_t holes=coverRows[drawCover.high][y&15]^coverRows[drawCover.low][y&15];
        for(int i=a;i<=b;i++){
            unsigned bits=mask & (holes>>(8*(i&1)));
            if(i==b)bits&=last;
            if(y>=pen.clearUntil && y>=roofMin[pen.ring][i]){
                if(y>=roofMax[pen.ring][i])bits=0;
                else bits&=edge_bits(roof[pen.ring]+i*8,y);
            }
            row[i]^=(row[i]^pattern)&bits;
            mask=255;
        }
        return;
    }
    if(y<pen.clearUntil){
        if(a==b){row[a]^=(row[a]^pattern)&(mask&last);return;}
        row[a]^=(row[a]^pattern)&mask;
        for(int i=a+1;i<b;i++)row[i]=pattern;
        row[b]^=(row[b]^pattern)&last;
        return;
    }
    const uint8_t *lo=roofMin[pen.ring],*hi=roofMax[pen.ring];
    for(int i=a;i<=b;i++){
        unsigned bits=mask;
        if(i==b)bits&=last;
        if(y<lo[i])row[i]^=(row[i]^pattern)&bits;
        else if(y<hi[i]){
            bits&=edge_bits(roof[pen.ring]+i*8,y);
            row[i]^=(row[i]^pattern)&bits;
        }
        mask=255;
    }
}
static inline void flat_span(int y,int left,int right,int key,int tone){
    stroke(y,left,right,brush(key,tone,y));
}
typedef struct {float x,y,scale,iz,z;} Sprite;
enum { TRI_CMD, OVAL_CMD, KELP_CMD, CORAL_CMD, SNOW_CMD, JELLY_CMD, BUCKETS=1024, MAX_COMMANDS=8192 };
typedef struct {
    int16_t next;
    uint8_t type,tone;
    Coverage coverage;
    union {
        Screen v[3];
        struct {Sprite s;float x,y,rx,ry;} oval;
        struct {Sprite s;float h;int phase;} kelp;
        struct {const Prop *p;Camera base;int variant;} coral;
        struct {int x,y,key;} snow;
        struct {Sprite s;float phase;} jelly;
    };
} Command;
static Command commands[MAX_COMMANDS];
static int16_t bucketHead[BUCKETS],bucketTail[BUCKETS];
static int commandCount;
static Command *submit(int type,float key,int tone){
    int binKey=min2(4095,max2(0,(int)key)>>3);
    int high=min2(detailCover.high,rangeCover[binKey]);
    if(high<=detailCover.low || commandCount==MAX_COMMANDS)return NULL;
    /* Logarithmic inverse-depth buckets retain fine ordering close to the eye. */
    uint32_t bits;memcpy(&bits,&key,sizeof bits);
    int bin=min2(BUCKETS-1,max2(0,(int)(bits>>17)-(134<<6)));
    int id=commandCount++;
    Command *cmd=&commands[id];cmd->type=type;cmd->tone=tone;cmd->next=-1;
    cmd->coverage=(Coverage){detailCover.low,high};
    if(bucketHead[bin]<0)bucketHead[bin]=id;
    else commands[bucketTail[bin]].next=id;
    bucketTail[bin]=id;
    return cmd;
}
static void raster_triangle(Screen a,Screen b,Screen c,int color){
    /* Walk both edges in Q12, with the same top-left fill rule on shared seams. */
    if(b.y<a.y){Screen s=a;a=b;b=s;}
    if(c.y<a.y){Screen s=a;a=c;c=s;}
    if(c.y<b.y){Screen s=b;b=c;c=s;}
    if(c.y-a.y<.01f)return;
    int first=max2(0,(int)ceilf(a.y)),last=min2(viewHeight,(int)ceilf(c.y));
    if(first>=last || (a.x<0 && b.x<0 && c.x<0) ||
       (a.x>=viewWidth && b.x>=viewWidth && c.x>=viewWidth))return;
    float inv=1/(c.y-a.y);
    float slope=(c.x-a.x)*inv;
    int longStep=(int)clampf(slope*4096,-1e8f,1e8f);
    Brush pen=brush((int)((a.iz+b.iz+c.iz)*(1.f/3)),color,(first+last)/2);
    bound_brush(&pen,(int)fminf(a.x,fminf(b.x,c.x))-1,(int)fmaxf(a.x,fmaxf(b.x,c.x))+1);
    for(int half=0;half<2;half++){
        Screen p=half?b:a,q=half?c:b;
        int y0=max2(first,(int)ceilf(p.y)),y1=min2(last,(int)ceilf(q.y));
        if(y0>=y1)continue;
        float shortSlope=(q.x-p.x)/(q.y-p.y);
        int shortStep=(int)clampf(shortSlope*4096,-1e8f,1e8f);
        int x1=(int)clampf((a.x+(y0-a.y)*slope)*4096,-1e8f,1e8f);
        int x2=(int)clampf((p.x+(y0-p.y)*shortSlope)*4096,-1e8f,1e8f);
        for(int y=y0;y<y1;y++,x1+=longStep,x2+=shortStep){
            int left=max2(0,(min2(x1,x2)+4095)>>12);
            int right=min2(LCD_W,(max2(x1,x2)+4095)>>12);
            if(baking){
                if(y<80)for(int x=left;x<min2(64,right);x++){bakedLeaf[y][x]=color;leafMask[y][x]=1;}
            }else stroke(y,left,right,pen);
        }
    }
}
static void triangle(Screen a,Screen b,Screen c,int tone){
    if(baking){raster_triangle(a,b,c,tone);return;}
    if((a.x<0 && b.x<0 && c.x<0) || (a.x>=LCD_W && b.x>=LCD_W && c.x>=LCD_W))return;
    if((a.y<0 && b.y<0 && c.y<0) || (a.y>=LCD_H && b.y>=LCD_H && c.y>=LCD_H))return;
    Command *cmd=submit(TRI_CMD,(a.iz+b.iz+c.iz)*(1.f/3),tone);
    if(cmd){cmd->v[0]=a;cmd->v[1]=b;cmd->v[2]=c;}
}
/* Static landmarks keep shared vertices in world coordinates. Projection is
   performed once per vertex for the current camera, including free swimming. */
enum { MAX_MODEL_VERTS=512, MAX_MODEL_FACES=640 };
typedef struct {uint16_t a,b,c;uint8_t tone;int8_t winding;} Facet;
static int winding;
enum { GROUP_FACES=16, MAX_GROUPS=(MAX_MODEL_FACES+GROUP_FACES-1)/GROUP_FACES, VERT_WORDS=(MAX_MODEL_VERTS+31)/32 };
typedef struct {Vec center,half;} ModelBounds;
typedef struct {ModelBounds bounds;uint32_t vertices[VERT_WORDS];} FaceGroup;
typedef struct {ModelBounds bounds;FaceGroup groups[MAX_GROUPS];int nverts,nfaces,overflow;Vec positions[MAX_MODEL_VERTS];Facet faces[MAX_MODEL_FACES];} Model;
static Model landmarks[2+3],*building;
static int model_vertex(Vec v){
    for(int i=0;i<building->nverts;i++){
        Vec a=building->positions[i];
        if(a.x==v.x && a.y==v.y && a.z==v.z)return i;
    }
    if(building->nverts>=MAX_MODEL_VERTS){building->overflow=1;return 0;}
    building->positions[building->nverts]=v;
    return building->nverts++;
}
static void face(Vec a,Vec b,Vec c,int tone){
    if(building){
        if(building->nfaces>=MAX_MODEL_FACES){building->overflow=1;return;}
        Facet *f=&building->faces[building->nfaces++];
        f->a=model_vertex(a);f->b=model_vertex(b);f->c=model_vertex(c);f->tone=tone;f->winding=winding;
        return;
    }
    Camera v[3]={camera(a),camera(b),camera(c)},poly[4];
    if(v[0].z>=.9f && v[1].z>=.9f && v[2].z>=.9f){
        triangle(project(v[0]),project(v[1]),project(v[2]),tone);
        return;
    }
    int n=0;
    for(int i=0;i<3;i++){
        Camera p=v[i],q=v[(i+1)%3];
        if(p.z>=.9f)poly[n++]=p;
        if((p.z<.9f)!=(q.z<.9f)){
            float u=(.9f-p.z)/(q.z-p.z);
            poly[n++]=(Camera){p.x+u*(q.x-p.x),p.y+u*(q.y-p.y),.9f};
        }
    }
    if(n<3)return;
    int col=tone;
    for(int i=2;i<n;i++)triangle(project(poly[0]),project(poly[i-1]),project(poly[i]),col);
}
static void quad(Vec a,Vec b,Vec c,Vec d,int tone){face(a,b,c,tone);face(a,c,d,tone);}
static void pole(Vec from,Vec to,float w,int tone){
    /* A camera-facing strip keeps yards and diagonal stays visible edge-on. */
    float dx=to.x-from.x,dy=to.y-from.y,dz=to.z-from.z;
    float rx=from.x-view->p.x,ry=from.y-view->p.y,rz=from.z-view->p.z;
    Vec n={dy*rz-dz*ry,dz*rx-dx*rz,dx*ry-dy*rx};
    float length=sqrtf(n.x*n.x+n.y*n.y+n.z*n.z);
    if(length<.001f)return;
    float k=w/length;
    n.x*=k;n.y*=k;n.z*=k;
    Vec a={from.x-n.x,from.y-n.y,from.z-n.z},b={from.x+n.x,from.y+n.y,from.z+n.z};
    Vec c={to.x+n.x,to.y+n.y,to.z+n.z},d={to.x-n.x,to.y-n.y,to.z-n.z};
    quad(a,b,c,d,tone);
}
/* Hull stations, planks and spars use the proportions of the original wreck. */
static void wreck(void){
    const float stations[]={-16,-13,-8,0,8,13,17};
    const float widths[]={2.6f,4.6f,5.3f,5.6f,4.8f,3.2f,.15f};
    for(int i=0;i<6;i++){
        float x=stations[i],xx=stations[i+1],b=widths[i],bb=widths[i+1];
        float sheer=fabsf(x)*.17f,ss=fabsf(xx)*.17f;
        quad(ship(x,sheer,b),ship(xx,ss,bb),ship(xx,ss,-bb),ship(x,sheer,-b),154);
        for(int side=-1;side<=1;side+=2){
            pole(ship(x,sheer,side*b),ship(xx,ss,side*bb),.16f,175);
            for(int k=0;k<5;k++){
                if(side==1 && i==2 && (k==2 || k==3))continue;
                float t=k*.2f,u=t+.18f;
                quad(ship(x,sheer-7*t,side*b*(1-t*t)),ship(xx,ss-7*t,side*bb*(1-t*t)),
                     ship(xx,ss-7*u,side*bb*(1-u*u)),ship(x,sheer-7*u,side*b*(1-u*u)),28+((i+k)%3)*7+k*4);
            }
            pole(ship(x,sheer,side*b),ship(x,sheer-5.8f,side*b*.34f),.18f,98);
        }
    }
    /* Rigging, broken ribs, and a torn strip of canvas separate mast from yard. */
    for(int i=0;i<4;i++){
        float x=-9+i*3;
        pole(ship(x,1.2f,4.5f),ship(-5.8f,10,.8f),.075f,40);
        pole(ship(x+7,1,4),ship(6.9f,14,.6f),.085f,48);
    }
    for(int i=0;i<5;i++){
        float z=-4+i*1.6f;
        Vec a=ship(6.7f+z*.6f,11,z),b=ship(6.7f+(z+1.6f)*.6f,11,z+1.6f);
        Vec hem=ship(5.1f+z*.6f,7.8f+(i%3)*.75f,z+.1f);
        face(a,b,hem,139+i*9);
    }
    pole(ship(-5,0,0),ship(-6,12,1),.35f,27);
    pole(ship(6,.5f,0),ship(7,17,.7f),.37f,21);
    pole(ship(3.7f,11,-5),ship(9.7f,11,5),.26f,31);
    pole(ship(5.2f,15,-3),ship(8.8f,15,3),.20f,34);
    pole(ship(16,2,0),ship(23,5,0),.22f,32);
    pole(ship(-14,3,1),ship(7,16,.7f),.09f,47);
    pole(ship(7,16,.7f),ship(22,5,0),.09f,47);
    quad(ship(-15,2,2.8f),ship(-8,2,4.4f),ship(-8,5,4.4f),ship(-15,5.6f,2.8f),46);
    quad(ship(-15,5.6f,-2.8f),ship(-8,5,-4.4f),ship(-8,5,4.4f),ship(-15,5.6f,2.8f),89);
}
static void outcrop(Vec center,float radius,float height,int seed,int light);

/* Broken bedrock protrudes through the sand on both sides of the keel.
   Build it with the wreck so the static projection and culling are shared. */
static void wreck_site(void){
    static const struct {float along,across,radius,height;} stones[]={
        {-14.6f,2.2f,3.8f,1.35f}, {-12.3f,4.9f,2.0f,.75f},
        {-6.4f,3.1f,4.9f,1.7f}, {-2.6f,5.6f,2.3f,.70f},
        {6.5f,1.8f,5.1f,.64f}, {10.4f,4.4f,2.25f,.85f},
        {15.6f,1.8f,2.3f,.8f}, {-16.8f,6.4f,1.8f,.75f},
        {-9,-3.3f,4.2f,1.4f}, {3.8f,-3.3f,4.9f,.95f}
    };
    wreck();
    for(unsigned i=0;i<sizeof stones/sizeof *stones;i++){
        Vec p=ship(stones[i].along,0,stones[i].across);
        p.y=floor_at(p.x,p.z);
        outcrop(p,stones[i].radius,stones[i].height,17+i*73,16);
    }
}

/* Span runs across the two lobes; positive depth faces the Whale fluke viewpoint. */
static Vec fluke_point(float ground,float span,float height,float depthOffset){
    return (Vec){-5+.967f*span-.255f*depthOffset,ground+height,-4+.255f*span+.967f*depthOffset};
}
static void fluke(void){
    const float ground=floor_at(-5,-4);
    /* The submerged tail stock bends away from the viewer and narrows at the joint. */
    static const float stock[][4]={
        /* height, centre depth, half-width, half-thickness */
        {-2,2.1f,2.55f,1.93f},{.6f,1.72f,2.12f,1.66f},{2.5f,.87f,1.48f,1.34f},
        {4.4f,.20f,1.06f,.95f},{5.5f,-.05f,.96f,.78f},{6.6f,-.14f,1.38f,.62f}
    };
    static const float ovalX[]={1,.707f,0,-.707f,-1,-.707f,0,.707f};
    static const float ovalZ[]={0,.707f,1,.707f,0,-.707f,-1,-.707f};
    static const int stockTone[]={76,87,77,59,44,50,57,67};
    Vec last[8],ring[8];
    winding=-1;
    for(unsigned i=0;i<sizeof stock/sizeof *stock;i++){
        const float *section=stock[i];
        float bend=fmaxf(0,1-section[0]/5.5f);
        for(int j=0;j<8;j++)
            ring[j]=fluke_point(ground,section[2]*ovalX[j]-1.6f*bend*bend,section[0],section[1]+section[3]*ovalZ[j]);
        if(i)for(int j=0;j<8;j++)
            quad(last[j],last[(j+1)%8],ring[(j+1)%8],ring[j],stockTone[j]);
        memcpy(last,ring,sizeof ring);
    }
    winding=0;
    for(int j=1;j<7;j++)face(ring[0],ring[j],ring[j+1],80);
    /* Cross sections follow the rounded leading edge and the thinner scalloped trailing
       edge. Both lobes share the centre section, which cuts a V into the trailing edge. */
    static const float sections[][5]={
        /* span, leading height, trailing height, sweep in depth, half-thickness */
        {0,5.28f,8.25f,0,.87f},{.55f,5.45f,9.34f,-.06f,.86f},
        {1.35f,5.95f,10.07f,-.20f,.78f},{2.5f,6.52f,10.25f,-.42f,.67f},
        {3.8f,6.94f,10.15f,-.74f,.56f},{5.1f,7.35f,10.02f,-1.12f,.44f},
        {6.25f,7.98f,10.16f,-1.52f,.33f},{7.25f,8.83f,10.52f,-1.98f,.23f},
        {8.03f,9.72f,11.02f,-2.45f,.15f},{8.58f,10.61f,11.53f,-2.94f,.09f},
        {8.93f,11.44f,11.96f,-3.35f,.045f},{9.10f,12.29f,12.29f,-3.68f,0}
    };
    static const float chord[]={0,.28f,.75f,1,.50f};
    static const float bulge[]={0,.94f,.55f,0,-.75f};
    static const int bladeTone[]={43,68,87,46,40};
    for(int side=-1;side<=1;side+=2){
        for(unsigned i=0;i<sizeof sections/sizeof *sections;i++){
            if(i==1 || i==3 || i==6 || i==9)continue;
            winding=-side;
            const float *section=sections[i];
            float span=side*section[0],roll=.060f*span;
            for(int j=0;j<5;j++){
                float height=section[1]+(section[2]-section[1])*chord[j]+roll;
                float z=section[3]-.29f*(section[2]-section[1])*chord[j]+section[4]*bulge[j];
                ring[j]=fluke_point(ground,span,height,z);
            }
            if(i)for(int j=0;j<5;j++){
                quad(last[j],last[(j+1)%5],ring[(j+1)%5],ring[j],bladeTone[j]);
            }
            memcpy(last,ring,5*sizeof(Vec));
        }
    }
    winding=0;
}
static void stone_arch(const Arch *a){
    winding=-1;
    Vec previous[5],ring[5];
    float c=cosf(a->yaw),s=sinf(a->yaw);
    for(int i=0;i<=16;i++){
        float angle=i*6.283185f/16,ca=cosf(angle),sa=sinf(angle);
        for(int j=0;j<5;j++){
            float cross=j*6.283185f/5,along=(a->radius+a->tube*cosf(cross))*ca;
            float normal=a->tube*sinf(cross);
            ring[j]=(Vec){a->x+along*c-normal*s,a->y+(a->radius+a->tube*cosf(cross))*sa*a->stretch,a->z+along*s+normal*c};
        }
        if(i)for(int j=0;j<5;j++)quad(previous[j],previous[(j+1)%5],ring[(j+1)%5],ring[j],42+j*10+(int)(sa*12));
        memcpy(previous,ring,sizeof ring);
    }
    winding=0;
}
static ModelBounds model_bounds(Model *m,int first,int end,uint32_t *mask){
    Vec low=m->positions[m->faces[first].a],high=low;
    for(int i=first;i<end;i++){
        Facet f=m->faces[i];int ids[3]={f.a,f.b,f.c};
        for(int k=0;k<3;k++){
            int id=ids[k];Vec p=m->positions[id];
            if(mask)mask[id/32]|=1u<<(id%32);
            low=(Vec){fminf(low.x,p.x),fminf(low.y,p.y),fminf(low.z,p.z)};
            high=(Vec){fmaxf(high.x,p.x),fmaxf(high.y,p.y),fmaxf(high.z,p.z)};
        }
    }
    return (ModelBounds){{(low.x+high.x)*.5f,(low.y+high.y)*.5f,(low.z+high.z)*.5f},
                         {(high.x-low.x)*.5f+.005f,(high.y-low.y)*.5f+.005f,(high.z-low.z)*.5f+.005f}};
}
static void build_landmarks(void){
    Diver reference={0};visit(&reference,1);view=&reference;
    building=&landmarks[0];wreck_site();
    building=&landmarks[1];fluke();
    for(int i=0;i<3;i++){building=&landmarks[2+i];stone_arch(&arches[i]);}
    building=NULL;
    for(int j=0;j<5;j++){
        Model *m=&landmarks[j];if(!m->nfaces || m->overflow)continue;
        m->bounds=model_bounds(m,0,m->nfaces,NULL);
        for(int i=0;i<m->nfaces;i+=GROUP_FACES){
            FaceGroup *g=&m->groups[i/GROUP_FACES];
            g->bounds=model_bounds(m,i,min2(i+GROUP_FACES,m->nfaces),g->vertices);
        }
    }
}
static int hidden(int x0,int y0,int x1,int y1,int key);
/* Project an enclosing box into conservative screen and inverse-depth bounds.
   While it crosses the near plane, keep it for the usual clipping pass. */
enum { MODEL_PARTIAL, MODEL_HIDDEN, MODEL_INSIDE };
static int classify_bounds(const ModelBounds *box){
    Camera p=camera(box->center);Vec h=box->half;
    float rx=fabsf(headingZ)*h.x+fabsf(headingX)*h.z;
    float rz=fabsf(headingX)*h.x+fabsf(headingZ)*h.z;
    float far=p.z+rz,near=p.z-rz;
    if(far<.9f)return MODEL_HIDDEN;
    if(near<.9f)return MODEL_PARTIAL;
    float nearScale=meshScale*focal/near,farScale=meshScale*focal/far;
    float l=p.x-rx,r=p.x+rx,t=p.y+h.y,b=p.y-h.y;
    int x0=(int)floorf(meshScale*100+l*(l<0?nearScale:farScale)) - 1;
    int x1=(int)ceilf(meshScale*100+r*(r>0?nearScale:farScale)) + 1;
    int y0=(int)floorf(meshScale*horizon-t*(t>0?nearScale:farScale)) - 1;
    int y1=(int)ceilf(meshScale*horizon-b*(b<0?nearScale:farScale)) + 1;
    if(x1<0 || x0>=viewWidth || y1<0 || y0>=viewHeight)return MODEL_HIDDEN;
    if(hidden(x0,y0,x1,y1,(int)ceilf(32760/near)+2+(viewWidth+15)/16))return MODEL_HIDDEN;
    return x0>=0 && x1<viewWidth && y0>=0 && y1<viewHeight ? MODEL_INSIDE:MODEL_PARTIAL;
}
static void draw_model(const Model *model){
    if(model->overflow){
        int id=(int)(model-landmarks);
        if(id==0)wreck_site();else if(id==1)fluke();else stone_arch(&arches[id-2]);
        return;
    }
    int visibility=classify_bounds(&model->bounds);
    if(visibility==MODEL_HIDDEN)return;
    static Screen screen[MAX_MODEL_VERTS];
    static uint8_t clip[MAX_MODEL_VERTS];
    uint32_t needed[VERT_WORDS]={0};uint8_t skip[MAX_GROUPS];
    int groups=(model->nfaces+GROUP_FACES-1)/GROUP_FACES,any=0;
    for(int j=0;j<groups;j++){
        const FaceGroup *g=&model->groups[j];skip[j]=visibility==MODEL_PARTIAL && classify_bounds(&g->bounds)==MODEL_HIDDEN;
        if(skip[j])continue;
        any=1;
        if(visibility==MODEL_PARTIAL)for(int w=0;w<(model->nverts+31)/32;w++)needed[w]|=g->vertices[w];
    }
    if(!any)return;
    for(int w=0;w<(model->nverts+31)/32;w++){
        uint32_t word=needed[w];
        if(visibility==MODEL_INSIDE){
            int count=min2(32,model->nverts-32*w);word=UINT32_MAX>>(32-count);
        }
        for(uint32_t mask=word;mask;mask&=mask-1){
            int i=32*w+__builtin_ctz(mask);
            Camera c=camera(model->positions[i]);
            if(c.z<.9f){clip[i]=16;continue;}
            Screen v=project(c);screen[i]=v;
            clip[i]=(v.x<0)|((v.x>=viewWidth)<<1)|((v.y<0)<<2)|((v.y>=viewHeight)<<3);
        }
    }
    for(int j=0;j<groups;j++){
        if(skip[j])continue;
        int count=min2(GROUP_FACES,model->nfaces-j*GROUP_FACES);
        uint32_t visible=(1u<<count)-1u;
        for(;visible;visible&=visible-1){
            int i=j*GROUP_FACES+__builtin_ctz(visible);
            Facet f=model->faces[i];
            if(clip[f.a]&clip[f.b]&clip[f.c])continue;
            if((clip[f.a]|clip[f.b]|clip[f.c])&16)
                face(model->positions[f.a],model->positions[f.b],model->positions[f.c],f.tone);
            else{
                Screen a=screen[f.a],b=screen[f.b],c=screen[f.c];
                if(f.winding && f.winding*((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))<=0)continue;
                triangle(a,b,c,f.tone);
            }
        }
    }
}

static Screen vertex(Sprite s,float x,float y){return (Screen){s.x+x*s.scale,s.y-y*s.scale,s.iz};}
static void raster_oval(Sprite s,float x,float y,float rx,float ry,int tone){
    Screen p=vertex(s,x,y);
    float w=fmaxf(.6f,rx*s.scale),h=fmaxf(.6f,ry*s.scale);
    int y0=max2(0,(int)ceilf(p.y-h)),y1=min2(viewHeight-1,(int)(p.y+h));
    Brush pen=brush((int)s.iz,tone,(y0+y1)/2);
    bound_brush(&pen,(int)(p.x-w)-1,(int)(p.x+w)+1);
    for(int row=y0;row<=y1;row++){
        float dy=(row-p.y)/h,extent=w*sqrtf(fmaxf(0,1-dy*dy));
        int left=max2(0,(int)ceilf(p.x-extent)),right=min2(viewWidth-1,(int)(p.x+extent));
        stroke(row,left,right+1,pen);
    }
}
static void oval(Sprite s,float x,float y,float rx,float ry,int tone){
    Command *cmd=submit(OVAL_CMD,s.iz,tone);
    if(cmd){cmd->oval.s=s;cmd->oval.x=x;cmd->oval.y=y;cmd->oval.rx=rx;cmd->oval.ry=ry;}
}
/* Tapered ribbon with a twisted midrib. The current lifts the outer half of each leaf. */
static void kelp_leaf(Sprite s,float x,float y,float dx,float dy,float curl,int dark,int lit){
    Screen base=vertex(s,x,y);
    Screen lower=vertex(s,x+dx*.32f,y+dy*.08f);
    Screen upper=vertex(s,x+dx*.24f,y+dy*.55f);
    Screen ridge=vertex(s,x+dx*.65f,y+dy*.57f);
    Screen rim=vertex(s,x+dx*.68f,y+dy*.85f);
    Screen tip=vertex(s,x+dx,y+dy+curl);
    triangle(base,lower,ridge,dark);triangle(base,ridge,upper,lit);
    triangle(upper,ridge,rim,lit);triangle(rim,ridge,tip,dark);
}
/* The skirt follows sampled floor heights around the footprint. On an incline a
   screen-facing quad with a horizontal base would hang out over the downhill side. */
static const float rockX[6]={1,.50f,-.50f,-1,-.50f,.50f};
static const float rockZ[6]={0,.866f,.866f,0,-.866f,-.866f};
static void outcrop(Vec center,float radius,float height,int seed,int light){
    Vec foot[6],rim[6];
    Vec peak={center.x+radius*.12f,center.y+height,center.z-radius*.08f};
    for(int j=0;j<6;j++){
        float wobble=.86f+sine(seed+j*61)*.0011f;
        float dx=radius*wobble*rockX[j],dz=radius*wobble*rockZ[j]*.78f;
        float x=center.x+dx,z=center.z+dz;
        foot[j]=(Vec){x,floor_at(x,z)-.30f,z};
        x=center.x+dx*.72f;z=center.z+dz*.72f;
        rim[j]=(Vec){x,floor_at(x,z)+height*(.45f+.18f*rockZ[j]),z};
    }
    for(int j=0;j<6;j++){
        int k=(j+1)%6;
        int sun=(int)(rockX[j]*8+rockZ[j]*15);
        quad(foot[j],foot[k],rim[k],rim[j],77+light-sun);
        face(rim[j],rim[k],peak,112+light-sun);
    }
}
static void sponge(const Prop *p){
    const float offsets[3][2]={{-.44f,-.14f},{.08f,.19f},{.49f,-.21f}};
    for(int n=0;n<3;n++){
        float size=p->size,height=size*(1-n*.16f),r=size*.27f;
        float x=p->p.x+offsets[n][0]*size,z=p->p.z+offsets[n][1]*size;
        float y=floor_at(x,z),lipY=y+height;
        Vec base[6],lip[6],inner[6],well={x+.13f*height,lipY-r*.86f,z};
        for(int j=0;j<6;j++){
            float dx=r*rockX[j],dz=r*rockZ[j];
            float bx=x+dx*.70f,bz=z+dz*.70f;
            base[j]=(Vec){bx,floor_at(bx,bz)-.24f,bz};
            lip[j]=(Vec){well.x+dx,lipY+.12f*dx,well.z+dz};
            inner[j]=(Vec){well.x+dx*.66f,lipY+.08f*dx,well.z+dz*.66f};
        }
        for(int j=0;j<6;j++){
            int k=(j+1)%6;
            int sun=(int)(rockX[j]*10+rockZ[j]*20);
            quad(base[j],base[k],lip[k],lip[j],83-sun);
            quad(lip[j],lip[k],inner[k],inner[j],151-sun/2);
            face(inner[j],inner[k],well,42+sun/2);
        }
    }
}
static uint32_t coralMask[CORAL_FORMS][CORAL_TILTS][CORAL_ANGLES][CORAL_TEXELS][2];
static void index_coral(void){
    for(int form=0;form<CORAL_FORMS;form++)for(int tilt=0;tilt<CORAL_TILTS;tilt++)
        for(int a=0;a<CORAL_ANGLES;a++)for(int y=0;y<CORAL_TEXELS;y++)
            for(int x=0;x<CORAL_TEXELS;x++)if(coralAtlas[form][tilt][a][y][x])
                coralMask[form][tilt][a][y][x/32]|=1u<<(x&31);
}
static void raster_coral(const Prop *p,Camera base,int variant){
    float size=p->size,dx=view->p.x-p->p.x,dz=view->p.z-p->p.z;
    float range=sqrtf(dx*dx+dz*dz),dy=view->p.y-p->p.y-.76f*size;
    float azimuth=atan2f(dx,dz)-p->phase;
    int angle=((int)floorf(azimuth*(CORAL_ANGLES/6.2831853f)+.5f))&(CORAL_ANGLES-1);
    int tilt=dy>range*.29f?2:dy< -range*.29f?0:1;
    const uint16_t (*atlas)[CORAL_TEXELS]=coralAtlas[variant%CORAL_FORMS][tilt][angle];
    base.y+=.76f*size;
    Screen center=project(base);
    float radius=size*meshScale*focal/base.z;
    float rx=radius*range/base.z,ry=radius*sqrtf(1+dy*dy/(range*range));
    float top=center.y-ry,left=center.x-rx;
    float stepY=CORAL_TEXELS/(2*ry);
    int y0=max2(0,(int)ceilf(top-.5f)),y1=min2(viewHeight,(int)ceilf(center.y+ry-.5f));
    if(y0>=y1 || rx<.25f || ry<.25f)return;
    float toward=size*base.z/(83.f*sqrtf(range*range+dy*dy));
    int keys[32];
    for(int i=0;i<32;i++){
        float z=base.z-(8*i+4-128)*toward;
        keys[i]=z>=1?(int)(32760.f/z):0;
    }
    const uint32_t (*mask)[2]=coralMask[variant%CORAL_FORMS][tilt][angle];
    int xEdge[CORAL_TEXELS+1];uint32_t active[2]={0};
    for(int x=0;x<=CORAL_TEXELS;x++){
        xEdge[x]=min2(LCD_W,max2(0,(int)ceilf(left+x*(2*rx/CORAL_TEXELS)-.5f)));
        if(x && xEdge[x]>xEdge[x-1])active[(x-1)/32]|=1u<<((x-1)&31);
    }
    for(int y=y0;y<y1;y++){
        int sy=min2(CORAL_TEXELS-1,(int)((y+.5f-top)*stepY));
        for(int half=0;half<2;half++){
            for(uint32_t bits=mask[sy][half]&active[half];bits;bits&=bits-1){
                int x=32*half+__builtin_ctz(bits);
                uint16_t sample=atlas[sy][x];
                int key=keys[(sample&255)/8];
                if(key)flat_span(y,xEdge[x],xEdge[x+1],key,sample>>8);
            }
        }
    }
}
/* Each row of a frond stores its opaque spans. Project them at the current
   world location and bend the upper rows with the current. */
enum { LEAF_W=64, LEAF_H=80, LEAF_FORMS=4 };
typedef struct {uint8_t left,right,tone;} LeafSpan;
typedef struct {int n; LeafSpan spans[LEAF_W];} LeafRow;
static LeafRow canopy[LEAF_FORMS][LEAF_H];
static void build_canopy(void){
    for(int variant=0;variant<LEAF_FORMS;variant++){
        int phase=variant*67,toneShift=variant*3,dark=41+toneShift,lit=95+toneShift;
        float h=12;
        Sprite s={32,79,6.3f,3000,10};
        memset(leafMask,0,sizeof leafMask);
        float x=0,y=0;
        int count=8;
        for(int k=1;k<=count;k++){
            float t=(float)k/count;
            float nx=t*(sine(phase+k*15+tick)-127)*.012f*h*.13f;
            float ny=h*t;
            float w=.11f+h*.011f*(1-t);
            triangle(vertex(s,x-w,y),vertex(s,x+w,y),vertex(s,nx+w,ny),dark);
            triangle(vertex(s,x-w,y),vertex(s,nx+w,ny),vertex(s,nx-w,ny),dark);
            float reach=(1.3f+h*.10f)*(1.12f-t*.7f);
            float side=((k+phase)&1)?1.f:-1.f;
            float curl=(sine(phase+k*37+tick)-127)*.005f;
            float attach=nx-(nx-x)*(.09f*count);
            kelp_leaf(s,attach,ny-h*.09f,side*reach*1.26f,h*.19f,curl,dark,lit);
            x=nx;y=ny;
        }
        for(int row=0;row<LEAF_H;row++){
            LeafRow *r=&canopy[variant][row];
            for(int x=0;x<LEAF_W;){
                if(!leafMask[row][x]){x++;continue;}
                int left=x,tone=bakedLeaf[row][x++];
                while(x<LEAF_W && leafMask[row][x] && bakedLeaf[row][x]==tone)x++;
                r->spans[r->n++]=(LeafSpan){left,x,tone};
            }
        }
    }
}
static void raster_kelp(Sprite s,float h,int phase){
    float scale=s.scale*h/(6.3f*12),inv=1.f/scale;
    float top=s.y-79*scale,left=s.x-32*scale;
    int start=max2(0,(int)ceilf(top)),end=min2(viewHeight,(int)ceilf(s.y+scale));
    int variant=phase&3;
    Brush dark=brush((int)s.iz,41+variant*3,(start+end)/2);
    Brush shine=brush((int)s.iz,95+variant*3,(start+end)/2);
    bound_brush(&dark,(int)(left-6*scale),(int)(left+70*scale)+1);shine.clearUntil=dark.clearUntil;
    int offsets[LEAF_W+1];
    for(int i=0;i<=LEAF_W;i++)offsets[i]=(int)(scale*i*1024);
    float sway=scale*(sine(phase+tick*2)-127)*.040f;
    for(int y=start;y<end;y++){
        int row=min2(79,(int)((y-top)*inv));
        const LeafRow *r=&canopy[variant][row];
        float bend=(79-row)*(1.f/79),origin=left+sway*bend*bend;
        int originFixed=(int)(origin*1024)+1023;
        for(int n=0;n<r->n;n++){
            LeafSpan span=r->spans[n];
            int x0=max2(0,(originFixed+offsets[span.left])>>10);
            int x1=min2(viewWidth,(originFixed+offsets[span.right])>>10);
            stroke(y,x0,x1,span.tone<80?dark:shine);
        }
    }
}
static void sponge_lod(Sprite s,float h){
    for(int n=0;n<3;n++){
        float cx=(n-1)*.46f*h,top=h*(1-.16f*n),r=h*.27f;
        int wall=74+n*6;
        Screen l=vertex(s,cx-r*.7f,-.12f*h),rr=vertex(s,cx+r*.7f,-.12f*h);
        Screen a=vertex(s,cx-r+top*.13f,top),b=vertex(s,cx+r+top*.13f,top);
        triangle(l,rr,b,wall);triangle(l,b,a,wall+7);
        oval(s,cx+top*.13f,top,r,r*.34f,151);
        oval(s,cx+top*.13f,top,r*.61f,r*.20f,48);
    }
}
/* The nearest depth of the bounding volume selects a conservative terrain horizon. */
static int hidden(int x0,int y0,int x1,int y1,int key){
    if(x1<0 || x0>=LCD_W || y1<0 || y0>=LCD_H)return 1;
    const uint8_t *sky=roof[ringForKey[min2(4095,max2(0,key)>>3)]];
    for(int x=max2(0,x0);x<=min2(LCD_W-1,x1);x++)if(y0<sky[x])return 0;
    return 1;
}
static void distant_rock(Sprite s,float h,int phase){
    float w=h*(.83f+.001f*sine(phase));
    Screen a=vertex(s,-w,-h*.16f),b=vertex(s,w,-h*.16f);
    Screen c=vertex(s,w*.70f,h*.45f),d=vertex(s,-w*.43f,h*.62f),e=vertex(s,-w*.13f,h*.29f);
    int col=83;
    triangle(a,b,e,col);triangle(b,c,e,col+10);triangle(c,d,e,col+25);triangle(d,a,e,col-4);
}
static void plant(const Prop *p){
    Camera c=camera(p->p);
    if(c.z>REEF_RANGE)return;
    if(c.z<=1.5f){
        /* World-space facets can straddle the lens even when their anchor
           is behind it. Let the normal near-plane clipper trim these. */
        if(c.z < -p->size || fabsf(c.x)>2+p->size*1.2f)return;
        if(p->kind==BOULDER)outcrop(p->p,p->size*.90f,p->size*.65f,(int)(p->phase*40),0);
        else if(p->kind==SPONGE)sponge(p);
        return;
    }
    int nearLens=COVER_STEPS-visibility(c.z,1.5f,3.5f);
    Screen pos=project(c);
    Sprite s={pos.x,pos.y,meshScale*focal/c.z,pos.iz,c.z};
    float h=p->size;
    int phase=(int)(p->phase*40);
    float reach=p->kind==BUBBLES?20:p->kind==CORAL?1.85f*h:h*1.2f;
    float radius=p->kind==BUBBLES?1.2f:p->kind==KELP?.46f*h+.6f:h*1.1f;
    float bounds=s.scale*radius*(1+radius/fmaxf(1,c.z-radius));
    if(c.z>radius+1 && hidden((int)(s.x-bounds)-2,(int)(s.y-reach*s.scale-bounds)-2,
                              (int)(s.x+bounds)+2,(int)(s.y+bounds)+2,(int)(32760/(c.z-radius))))return;
    if(p->kind==BUBBLES){
        for(int i=0;i<5;i++){
            float t=fmodf(view->time*1.05f+i*4+p->phase,20);
            float x=(sine((int)(t*9)+phase)-127)*.005f;
            detailCover.high=min2(nearLens,COVER_STEPS-visibility(fminf(t,20-t),0,2));
            oval(s,x,t,.09f+t*.009f,.11f+t*.008f,216);
        }
        detailCover=opaque;return;
    }
    if(pos.y+bounds<0 || pos.y-reach*s.scale-bounds>LCD_H)return;
    /* Both LODs occupy complementary pixels during the change. */
    int near=visibility(c.z,p->kind==BOULDER?5:2.5f,p->kind==BOULDER?11:6);
    int plantCover=(p->kind==KELP || p->kind==CORAL)?nearLens:COVER_STEPS;
    detailCover=(Coverage){0,min2(near,plantCover)};
    switch(p->kind){
    case KELP:
        if(near)outcrop(p->p,.45f+h*.026f,.45f+h*.024f,phase,0);
        detailCover=(Coverage){0,plantCover};
        {Command *cmd=submit(KELP_CMD,s.iz,0);if(cmd){cmd->kelp.s=s;cmd->kelp.h=h;cmd->kelp.phase=phase;}}
        break;
    case SPONGE:
        if(near)sponge(p);
        detailCover=(Coverage){near,COVER_STEPS};
        if(near<COVER_STEPS)sponge_lod(s,h);
        break;
    case CORAL:
        if(near)outcrop(p->p,h*.48f,h*.28f,phase,0);
        detailCover=(Coverage){0,plantCover};
        {Command *cmd=submit(CORAL_CMD,s.iz,0);if(cmd){cmd->coral.p=p;cmd->coral.base=c;cmd->coral.variant=phase;}}
        break;
    case BOULDER:
        if(near)outcrop(p->p,h*.90f,h*.65f,phase,0);
        detailCover=(Coverage){near,COVER_STEPS};
        if(near<COVER_STEPS)distant_rock(s,h,phase);
        break;
    default:break;
    }
    detailCover=opaque;
}
enum { CELL_WIDTH=20, CELL_COUNT=2*EXTENT/CELL_WIDTH };
static int16_t firstProp[CELL_COUNT][CELL_COUNT],nextProp[PROP_COUNT];
static void index_props(void){
    memset(firstProp,0xff,sizeof firstProp);
    for(int i=0;i<PROP_COUNT;i++){
        if(props[i].kind==EMPTY_PROP)continue;
        const Vec p=props[i].p;
        int x=max2(0,min2(CELL_COUNT-1,(int)((p.x+EXTENT)/CELL_WIDTH)));
        int z=max2(0,min2(CELL_COUNT-1,(int)((p.z+EXTENT)/CELL_WIDTH)));
        nextProp[i]=firstProp[z][x];firstProp[z][x]=(int16_t)i;
    }
}
static void draw_plants(void){
    float limit=REEF_RANGE;
    for(int z=0;z<CELL_COUNT;z++)for(int x=0;x<CELL_COUNT;x++){
        int first=firstProp[z][x];if(first<0)continue;
        Vec center={x*CELL_WIDTH-EXTENT+CELL_WIDTH/2,0,z*CELL_WIDTH-EXTENT+CELL_WIDTH/2};
        Camera c=camera(center);
        if(c.z< -15 || c.z>limit+15 || fabsf(c.x)>c.z*.95f+35)continue;
        for(int i=first;i>=0;i=nextProp[i])plant(&props[i]);
    }
}
static void marine_snow(void){
    const int period=32;
    for(int i=0;i<144;i++){
        unsigned key=2654435761u*(unsigned)(i+1);
        float x=(float)(key&1023)/32+view->time*.09f;
        float y=(float)((key>>11)&1023)/32-view->time*.045f;
        float z=(float)((key>>20)&1023)/32;
        x+=period*floorf((view->p.x-x)/period+.5f);
        y+=period*floorf((view->p.y-y)/period+.5f);
        z+=period*floorf((view->p.z-z)/period+.5f);
        Camera cam=camera((Vec){x,y,z});
        if(cam.z<1.6f || cam.z>15 || fabsf(cam.x)>cam.z)continue;
        float dx=x-view->p.x,dy=y-view->p.y,dz=z-view->p.z;
        /* Finish fading before any axis of the periodic particle field wraps. */
        int visible=min2(visibility(dx*dx+dy*dy+dz*dz,9*9,15*15),
                         COVER_STEPS-visibility(cam.z,1.6f,3.f));
        if(!visible)continue;
        Screen v=project(cam);
        int sx=(int)v.x,sy=(int)v.y;
        if(sx>=0 && sx<meshScale*WIDTH && sy>=0 && sy<meshScale*HEIGHT){
            detailCover=(Coverage){0,visible};
            Command *cmd=submit(SNOW_CMD,v.iz,182+(i%4)*12);
            detailCover=opaque;
            if(cmd){cmd->snow.x=sx;cmd->snow.y=sy;cmd->snow.key=(int)v.iz;}
        }
    }
}
/* Build the silhouette in world space so that a turn of the camera also
   turns a side view into a head-on or tail-on view. */
static void draw_fish(Vec center,Vec velocity){
    guide_swimmer(&center,&velocity,1.9f);
    Camera f=camera(center);
    if(f.z<1 || f.z>REEF_RANGE || fabsf(f.x)>f.z+1)return;
    float flat=sqrtf(velocity.x*velocity.x+velocity.z*velocity.z);
    float length=sqrtf(flat*flat+velocity.y*velocity.y);
    if(length<.0001f)return;
    Vec forward={velocity.x/length,velocity.y/length,velocity.z/length};
    Vec side=flat>.0001f?(Vec){velocity.z/flat,0,-velocity.x/flat}:(Vec){1,0,0};
    int near=visibility(f.z,10,14);
    if(near<COVER_STEPS){
        detailCover=(Coverage){near,COVER_STEPS};
        /* Most schooling fish occupy only a few samples. Project the tangent
           into the image plane so the nose keeps leading as the camera turns. */
        Screen mid=project(f);
        float lateral=forward.x*headingZ-forward.z*headingX;
        float along=forward.x*headingX+forward.z*headingZ;
        float k=meshScale*focal/f.z;
        float vx=k*(lateral-f.x/f.z*along),vy=-k*(forward.y-f.y/f.z*along);
        float half=fmaxf(.42f,.13f*k);
        Screen nose={mid.x+.40f*vx,mid.y+.40f*vy,mid.iz};
        Screen rear={mid.x-.30f*vx,mid.y-.30f*vy,mid.iz};
        Screen upper={mid.x,mid.y-half,mid.iz},lower={mid.x,mid.y+half,mid.iz};
        int tone=41;
        triangle(nose,upper,lower,tone);triangle(rear,lower,upper,tone);
        Screen a={mid.x-.64f*vx,mid.y-.64f*vy-half,mid.iz};
        Screen b={a.x,a.y+2*half,mid.iz};triangle(rear,a,b,tone);
        detailCover=opaque;
        if(!near)return;
    }
    detailCover=(Coverage){0,near};
    Vec up={forward.y*side.z,forward.z*side.x-forward.x*side.z,-forward.y*side.x};
    Vec nose={center.x+.40f*forward.x,center.y+.40f*forward.y,center.z+.40f*forward.z};
    Vec rear={center.x-.30f*forward.x,center.y-.30f*forward.y,center.z-.30f*forward.z};
    Vec fin={center.x-.64f*forward.x,center.y-.64f*forward.y,center.z-.64f*forward.z};
    Vec ring[4]={
        {center.x+.09f*side.x,center.y,center.z+.09f*side.z},
        {center.x+.13f*up.x,center.y+.13f*up.y,center.z+.13f*up.z},
        {center.x-.09f*side.x,center.y,center.z-.09f*side.z},
        {center.x-.13f*up.x,center.y-.13f*up.y,center.z-.13f*up.z}
    };
    for(int j=0;j<4;j++){
        int next=(j+1)%4;
        face(nose,ring[j],ring[next],41);
        face(rear,ring[next],ring[j],41);
    }
    Vec top={fin.x+.17f*up.x,fin.y+.17f*up.y,fin.z+.17f*up.z};
    Vec bottom={fin.x-.17f*up.x,fin.y-.17f*up.y,fin.z-.17f*up.z};
    face(rear,top,bottom,41);
    detailCover=opaque;
}
/* Local coordinates on a swimming turtle: right, height and forward. */
static Vec turtle_point(Vec p,float x,float y,float z,float sineYaw,float cosineYaw){
    return (Vec){p.x+x*cosineYaw+z*sineYaw,p.y+y,p.z-x*sineYaw+z*cosineYaw};
}
static void turtle(Vec p,float yaw,float beat){
    static const float rimX[]={0,.58f,.76f,.63f,0,-.63f,-.76f,-.58f};
    static const float rimZ[]={1.12f,.76f,-.14f,-.80f,-1.10f,-.80f,-.14f,.76f};
    float s=motion_sin(yaw),c=motion_cos(yaw);
    Vec rim[8],plate[8],top=turtle_point(p,0,.42f,.05f,s,c);
    Vec belly=turtle_point(p,0,-.14f,.05f,s,c);
    for(int j=0;j<8;j++){
        rim[j]=turtle_point(p,rimX[j],0,rimZ[j],s,c);
        plate[j]=turtle_point(p,rimX[j]*.55f,.34f,rimZ[j]*.56f,s,c);
    }
    int near=visibility(camera(p).z,10,14);
    for(int j=0;j<8;j++){
        int k=(j+1)%8;
        if(near){
            detailCover=(Coverage){0,near};
            quad(rim[j],rim[k],plate[k],plate[j],56+(j%3)*10);
            face(plate[j],plate[k],top,88+(j%2)*10);
        }
        if(near<COVER_STEPS){
            detailCover=(Coverage){near,COVER_STEPS};face(rim[j],rim[k],top,64+(j%3)*10);
        }
        detailCover=opaque;
        if(view->p.y<p.y+.2f)face(rim[k],rim[j],belly,107);
    }
    /* Broad front flippers hinge at the shoulder; short rear paddles steer. */
    for(int side=-1;side<=1;side+=2){
        Vec shoulder=turtle_point(p,side*.54f,0,.69f,s,c);
        Vec elbow=turtle_point(p,side*1.07f,.22f*beat,.77f,s,c);
        Vec tip=turtle_point(p,side*1.65f,.69f*beat,-.06f,s,c);
        Vec heel=turtle_point(p,side*.68f,-.04f,.15f,s,c);
        quad(shoulder,elbow,tip,heel,58);
        face(rim[4],turtle_point(p,side*.74f,-.07f,-.54f,s,c),
             turtle_point(p,side*.93f,-.16f,-1.31f,s,c),57);
    }
    Vec head=turtle_point(p,0,.07f,1.48f,s,c);
    pole(turtle_point(p,0,.05f,.90f,s,c),head,.17f,72);
    Camera cam=camera(head);
    if(cam.z>1){
        Screen v=project(cam);Sprite sp={v.x,v.y,meshScale*focal/cam.z,v.iz,cam.z};
        oval(sp,0,0,.23f,.18f,80);
    }
}
static void jelly(Vec p,float phase,float size){
    Camera cam=camera(p);
    if(cam.z<1.3f || cam.z>REEF_RANGE || fabsf(cam.x)>cam.z+2)return;
    Screen v=project(cam);
    Sprite sp={v.x,v.y,meshScale*focal/cam.z*size,v.iz,cam.z};
    Command *cmd=submit(JELLY_CMD,v.iz,0);
    if(cmd){cmd->jelly.s=sp;cmd->jelly.phase=phase;}
}
static void raster_jelly(Sprite s,float phase){
    float pulse=motion_sin(phase),r=(.66f+.11f*pulse)*s.scale,h=(.52f-.07f*pulse)*s.scale;
    if(r<.5f || h<.3f)return;
    int base=(int)ceilf(s.y),y0=max2(0,(int)ceilf(s.y-h)),y1=min2(LCD_H,base+1);
    Brush crown=brush((int)s.iz,163,base),rim=brush((int)s.iz,86,base);
    for(int y=y0;y<y1;y++){
        float t=(s.y-y)/h,span=r*sqrtf(fmaxf(0,1-t*t));
        stroke(y,max2(0,(int)ceilf(s.x-span)),min2(LCD_W,(int)ceilf(s.x+span)),y==base?rim:crown);
    }
    for(int j=0;j<5;j++){
        float length=s.scale*1.4f;
        if(j&1)length*=clampf((r-5)*.25f,0,1);
        if(length<1)continue;
        int end=min2(LCD_H,(int)(s.y+length));
        float x=s.x+(j*.25f-.5f)*r*1.45f;
        float bend=motion_sin(phase+j*1.1f)*s.scale*.24f;
        for(int y=max2(0,base+1);y<end;y++){
            float t=(y-s.y)/length;
            int px=(int)(x+bend*t*(3*t-1.5f));
            stroke(y,max2(0,px),min2(LCD_W,px+1+(r>13)),rim);
        }
    }
}
static void reef_life(void){
    for(int i=0;i<HABITAT_COUNT;i++){
        Vec home=habitats[i];
        Camera cam=camera(home);
        if(cam.z < -8 || cam.z>REEF_RANGE+8 || fabsf(cam.x)>cam.z+13 || fabsf(home.y-view->p.y)>42)continue;
        float angle=view->time*.16f+i*1.81f;
        if(i%2==0){
            /* A second, tighter school wheels across the path as a group. */
            for(int k=0;k<18;k++){
                float t=angle+.13f*(k/3),s=motion_sin(t),c=motion_cos(t);
                float radius=4.3f+(k%3)*.67f;
                Vec fish={home.x+radius*c,home.y+.26f*(k%5)+.45f*motion_sin(t*2+k),home.z+radius*s};
                draw_fish(fish,(Vec){-radius*s,.90f*motion_cos(t*2+k),radius*c});
            }
        }else if(i==3 || i==7 || i==9){
            float x=home.x+4.7f*motion_cos(angle),z=home.z+3.3f*motion_sin(angle);
            float y=fmaxf(home.y+.45f*motion_sin(angle*2),floor_at(x,z)+1.3f);
            if(y<-.9f)turtle((Vec){x,y,z},atan2f(-4.7f*motion_sin(angle),3.3f*motion_cos(angle)),motion_sin(view->time*2.6f+i));
        }else{
            for(int k=0;k<5;k++){
                float phase=view->time*1.8f+k+i;
                float x=home.x+2.3f*motion_cos(angle*.40f+k*2.4f),z=home.z+2.9f*motion_sin(angle*.40f+k*2.4f);
                float y=fmaxf(home.y+(k%3)*.95f+.48f*motion_sin(phase*.5f),floor_at(x,z)+2.4f);
                if(y<-1)jelly((Vec){x,y,z},phase,.70f+(k%3)*.20f);
            }
        }
    }
}

/* Each encounter stays in world space during both the tour and free swim.
   A swimmer's local +z follows the tangent to its path. */
typedef struct { Vec p; float forwardX,forwardZ,scale,pitchSin,pitchCos; } Swimmer;
static Vec body_point(Swimmer s,float x,float y,float z){
    float forward=s.pitchCos*z-s.pitchSin*y;
    return (Vec){s.p.x+s.scale*(s.forwardZ*x+s.forwardX*forward),
                 s.p.y+s.scale*(s.pitchSin*z+s.pitchCos*y),
                 s.p.z+s.scale*(s.forwardZ*forward-s.forwardX*x)};
}
/* verticalSlope is dy per metre along the original horizontal orbit. */
static void follow_reef(Swimmer *s,float verticalSlope,float clearance){
    Vec tangent={s->forwardX,verticalSlope,s->forwardZ};
    guide_swimmer(&s->p,&tangent,clearance);
    s->pitchCos=1.f/sqrtf(1+tangent.y*tangent.y);
    s->pitchSin=tangent.y*s->pitchCos;
}
static int swimmer_visible(Vec p,float radius){
    Camera c=camera(p);
    if(c.z+radius<.9f || c.z>REEF_RANGE || fabsf(c.x)>c.z*.92f+radius*1.5f)return 0;
    if(c.z<=radius+1)return 1;
    Screen v=project(c);
    float extent=220.f*radius/(c.z-radius);
    return !hidden((int)(v.x-extent)-1,(int)(v.y-extent)-1,
                   (int)(v.x+extent)+1,(int)(v.y+extent)+1,(int)(32760/(c.z-radius))+2);
}
static void reef_shark(Swimmer s,float phase){
    /* Snout, shoulder, waist and caudal peduncle. The travelling lateral
       wave gets wider towards the tail while the head barely moves. */
    static const float sections[][4]={
        {1.88f,.08f,.06f,.01f},{1.18f,.34f,.29f,.025f},{.28f,.41f,.37f,.055f},
        {-.83f,.26f,.25f,.15f},{-1.61f,.09f,.10f,.32f}
    };
    Vec ring[5][4];float bend[5];
    for(int i=0;i<5;i++){
        const float *v=sections[i];
        bend[i]=v[3]*motion_sin(phase-i*.52f);
        ring[i][0]=body_point(s,bend[i],v[2],v[0]);
        ring[i][1]=body_point(s,bend[i]+v[1],0,v[0]);
        ring[i][2]=body_point(s,bend[i],-v[2],v[0]);
        ring[i][3]=body_point(s,bend[i]-v[1],0,v[0]);
        if(i)for(int j=0;j<4;j++){
            int k=(j+1)&3;
            quad(ring[i-1][j],ring[i][j],ring[i][k],ring[i-1][k],j==1||j==2?127:62);
        }
    }
    face(ring[0][0],ring[0][1],ring[0][2],81);face(ring[0][0],ring[0][2],ring[0][3],81);
    face(ring[2][0],body_point(s,bend[2],1.02f,-.20f),body_point(s,bend[3],.21f,-.55f),45);
    for(int side=-1;side<=1;side+=2){
        face(body_point(s,side*.30f,-.07f,.67f),body_point(s,side*1.08f,-.27f,-.36f),
             body_point(s,side*.27f,-.12f,-.11f),51);
    }
    float tail=.60f*motion_sin(phase-2.65f);
    Vec fork=body_point(s,tail*.86f,.05f,-1.97f);
    face(ring[4][0],body_point(s,tail,.90f,-2.44f),fork,48);
    face(ring[4][2],fork,body_point(s,tail,-.50f,-2.25f),59);
    face(ring[4][0],fork,ring[4][2],57);
}
static void squid(Swimmer s,float pulse){
    /* Mantle first on the power stroke. The swept fins and two long arms
       stay legible when the animal turns broadside to the camera. */
    float width=.25f+.055f*pulse;
    Vec tip=body_point(s,0,.08f,1.09f+.12f*pulse);
    Vec collar=body_point(s,0,-.02f,-.38f);
    Vec rim[4]={body_point(s,0,width,.23f),body_point(s,width,0,.23f),
                body_point(s,0,-width,.23f),body_point(s,-width,0,.23f)};
    for(int j=0;j<4;j++){
        int k=(j+1)&3;
        face(tip,rim[j],rim[k],j==0||j==3?104:57);
        face(collar,rim[k],rim[j],j==0||j==3?84:48);
    }
    for(int side=-1;side<=1;side+=2){
        face(tip,body_point(s,side*.49f,.03f+.08f*pulse,.42f),
             body_point(s,side*.19f,0,.04f),76);
    }
    Vec head=body_point(s,0,-.01f,-.62f);
    /* The eye sits on the visible cheek, just behind the mantle opening. */
    float side=(view->p.x-s.p.x)*s.forwardZ-(view->p.z-s.p.z)*s.forwardX;
    Camera eye=camera(body_point(s,side<0?-.15f:.15f,.015f,-.33f));
    if(eye.z>1 && eye.z<22){
        Screen v=project(eye);
        Sprite disc={v.x,v.y,220.f*s.scale/eye.z,v.iz,eye.z};
        detailCover.high=visibility(eye.z,16,22);
        oval(disc,0,0,.13f,.125f,179);
        oval(disc,0,0,.074f,.072f,38);
        detailCover=opaque;
    }
    for(int j=-1;j<=1;j++){
        Vec root=body_point(s,.12f*j,.07f,-.31f);
        Vec knee=body_point(s,j*(.18f+.07f*pulse),-.08f-.06f*j,-.81f);
        Vec end=body_point(s,j*(.26f+.11f*pulse),.08f*pulse,-1.26f+.19f*(j==0));
        face(root,head,knee,55);
        face(head,knee,end,61);
    }
}
typedef enum { SHARK_PATROL, SQUID_POD, SILVER_SCHOOL } EncounterKind;
typedef struct { Vec home; float axisX,axisZ,phase; EncounterKind kind; } Encounter;
static const Encounter encounters[]={
    {{57,-13,25},1,0,1.1f,SHARK_PATROL},          /* Over the wreck's bow */
    {{-11,-26,-79},.52f,-.854f,-1.6f,SHARK_PATROL}, /* Across the kelp channel */
    {{-13,-33,3},1,0,-4.9f,SILVER_SCHOOL},        /* Beside the stone fluke */
    {{-59,-32.7f,12},1,0,-12.8f,SQUID_POD},           /* Through the sponge canyon */
    {{3,-33.5f,85.5f},1,0,-16.5f,SQUID_POD},             /* Above the coral shelf */
    {{-17,-24,-110},.18f,.984f,-12.2f,SILVER_SCHOOL}
};
static void encounters_at_reef(void){
    for(unsigned i=0;i<sizeof encounters/sizeof *encounters;i++){
        const Encounter *e=&encounters[i];
        Camera c=camera(e->home);
        if(c.z< -15 || c.z>REEF_RANGE+15 || fabsf(c.x)>c.z+25 || fabsf(c.y)>44)continue;
        int count=e->kind==SHARK_PATROL?1:e->kind==SQUID_POD?5:28;
        float speed=e->kind==SHARK_PATROL?.21f:e->kind==SQUID_POD?.23f:.29f;
        for(int n=0;n<count;n++){
            int rank=n/4,lane=n%4;
            float t=view->time*speed+e->phase-rank*.18f;
            float beat=view->time*(e->kind==SQUID_POD?3.8f:2.9f)+i+1.4f*n;
            if(e->kind==SQUID_POD)t+=.19f*(n-2)+.042f*motion_sin(beat);
            float along=motion_sin(t),across=motion_cos(t);
            float major=e->kind==SHARK_PATROL?8.5f:6.4f;
            float minor=3.f+(count>1?.48f*lane:0);
            float u=major*along,w=minor*across;
            float du=major*across,dw=-minor*along,inv=1/sqrtf(du*du+dw*dw);
            Swimmer s={{e->home.x+e->axisX*u-e->axisZ*w,
                        e->home.y+.51f*motion_sin(1.6f*t+.44f*n),
                        e->home.z+e->axisZ*u+e->axisX*w},
                       (e->axisX*du-e->axisZ*dw)*inv,(e->axisZ*du+e->axisX*dw)*inv,1,0,1};
            if(e->kind==SQUID_POD){
                static const float height[]={-.9f,.48f,-.28f,1.34f,.22f};
                static const float offset[]={-.75f,.8f,-.4f,.45f,-.9f};
                s.p.x-=e->axisZ*offset[n];s.p.z+=e->axisX*offset[n];
                s.p.y+=height[n];
            }
            if(e->kind==SILVER_SCHOOL)s.p.y+=.35f*lane;
            float clearance=e->kind==SHARK_PATROL?4.4f:e->kind==SQUID_POD?2.9f:1.9f;
            follow_reef(&s,.51f*1.6f*motion_cos(1.6f*t+.44f*n)*inv,clearance);
            if(e->kind==SHARK_PATROL){
                s.scale=1.15f;
                if(s.p.y< -1.4f && swimmer_visible(s.p,3))reef_shark(s,beat);
            }else if(e->kind==SQUID_POD){
                s.scale=1.02f+.12f*lane;
                if(s.p.y< -1 && swimmer_visible(s.p,1.7f))squid(s,motion_sin(beat));
            }else if(s.p.y<-.8f && swimmer_visible(s.p,.7f)){
                /* A school banks as a wave: each flank catches the light
                   just after its neighbour. Tail and nose follow the orbit. */
                Vec nose=body_point(s,0,0,.45f),tail=body_point(s,0,0,-.35f);
                Vec ridge=body_point(s,0,.105f,0),belly=body_point(s,0,-.115f,0);
                Vec flank=body_point(s,.075f*motion_sin(t),0,.06f);
                int flash=(int)(49*fmaxf(0,motion_cos(3*t+.36f*lane)));
                face(nose,ridge,tail,67);face(nose,belly,flank,135+flash);
                face(flank,belly,tail,119+flash);
                face(tail,body_point(s,0,.14f,-.57f),body_point(s,0,-.14f,-.57f),63);
            }
        }
    }
}

static void animals(void){
    /* The widest orbits turn inside the submerged side of the kelp,
       canyon and grotto shelves, leaving room for the full wingspan. */
    static const struct { Vec school; float rayX,rayZ,radius,phase; } paths[]={
        {{1,-28,3.5f},1,3.5f,14,3.012f}, {{49,-24,24.5f},49,24.5f,10,.6f},
        {{0,-24,-80},0,-66,18,.3f}, {{-56,-29,18},-54,34,12,5},
        {{10,-28,86},9,92,12,2.2f}, {{20,-20,-20},20,-20,38,4.2f}
    };
    for(int i=0;i<6;i++){
        Vec home=paths[i].school;
        float a=view->time*.069f+paths[i].phase,c=motion_cos(a),s=motion_sin(a);
        Vec m={paths[i].rayX+paths[i].radius*c,home.y+motion_sin(a*2)*.7f,paths[i].rayZ+paths[i].radius*s};
        Swimmer ray={m,-s,c,1,0,1};
        float halfSpan=4.4f+.22f*i;
        follow_reef(&ray,1.4f*motion_cos(a*2)/paths[i].radius,halfSpan+3.4f);
        Camera cam=camera(ray.p);
        if(cam.z>-6 && cam.z<REEF_RANGE+5 && fabsf(cam.x)<cam.z+7){
            Vec nose=body_point(ray,0,.3f,1.6f),back=body_point(ray,0,0,-1.7f);
            Vec tail=body_point(ray,0,-.3f,-4.5f);
            for(int side=-1;side<=1;side+=2){
                float flap=motion_sin(view->time*1.65f+i),w=halfSpan*side;
                Vec joint=body_point(ray,w*.43f,.7f,0);
                Vec wing=body_point(ray,w,flap*1.8f,-.8f);
                Vec leading=body_point(ray,w*.57f,.65f+flap*.48f,.64f);
                face(nose,joint,back,23);face(nose,leading,joint,76);
                face(leading,wing,joint,62);face(back,joint,wing,40);
                Vec root=body_point(ray,side*.28f,0,-1.7f);
                face(ray.p,root,tail,26);
            }
        }
        /* A school moves on its own path below each ray. */
        float driftSin=motion_sin(a*.9f),driftCos=motion_cos(a*.9f);
        Camera school=camera((Vec){home.x+9*driftSin,home.y-5,home.z+9*driftCos});
        if(school.z < -8 || school.z>REEF_RANGE+8 || fabsf(school.x)>school.z+16)continue;
        for(int k=0;k<15;k++){
            float angle=a*1.5f+k*2.4f;
            float turnSin=motion_sin(angle),turnCos=motion_cos(angle);
            Vec fish={home.x+9*driftSin+5*turnCos+(k%3)*1.1f,
                      home.y-5+1.3f*motion_sin(angle*1.3f),
                      home.z+9*driftCos+4*turnSin};
            /* Tangent to this fish's full path, including the school's drift. */
            Vec velocity={9*.9f*driftCos-5*1.5f*turnSin,
                          1.3f*1.3f*1.5f*motion_cos(angle*1.3f),
                          -9*.9f*driftSin+4*1.5f*turnCos};
            draw_fish(fish,velocity);
        }
    }
}

void prepare_renderer(void){
    static int ready;
    if(ready)return;
    init_tables();terrain_tables();
    baking=1;build_canopy();baking=0;
    build_landmarks();index_props();index_coral();ready=1;
}
void render(const Diver *d,uint8_t *frame,int stride){
    prepare_renderer();
    lcd=frame;pitchBytes=stride;
    view=d;headingX=sinf(d->yaw);headingZ=cosf(d->yaw);
    horizon=60+focal*tanf(d->pitch)+sinf(d->time*1.3f)*.65f;
    tick=(int)(d->time*19);
    water();terrain();
    memset(bucketHead,0xff,sizeof bucketHead);commandCount=0;
    Camera shipPos=camera((Vec){WRECK_X,WRECK_Y,WRECK_Z}),flukePos=camera((Vec){-5,-38,-4});
    if(shipPos.z>-23 && shipPos.z<TERRAIN_RANGE+23 && fabsf(shipPos.x)<shipPos.z+42)draw_model(&landmarks[0]);
    if(flukePos.z>-10 && flukePos.z<TERRAIN_RANGE+10 && fabsf(flukePos.x)<flukePos.z+18)draw_model(&landmarks[1]);
    for(int i=0;i<3;i++){
        Camera p=camera((Vec){arches[i].x,arches[i].y,arches[i].z});
        if(p.z>-18 && p.z<TERRAIN_RANGE+18 && fabsf(p.x)<p.z+36)draw_model(&landmarks[2+i]);
    }
    rangeCover=reefCover;
    draw_plants();animals();reef_life();encounters_at_reef();marine_snow();
    rangeCover=landmarkCover;
    for(int b=0;b<BUCKETS;b++)for(int id=bucketHead[b];id>=0;id=commands[id].next){
        const Command *cmd=&commands[id];
        drawCover=cmd->coverage;
        switch(cmd->type){
        case TRI_CMD:raster_triangle(cmd->v[0],cmd->v[1],cmd->v[2],cmd->tone);break;
        case OVAL_CMD:raster_oval(cmd->oval.s,cmd->oval.x,cmd->oval.y,cmd->oval.rx,cmd->oval.ry,cmd->tone);break;
        case KELP_CMD:raster_kelp(cmd->kelp.s,cmd->kelp.h,cmd->kelp.phase);break;
        case CORAL_CMD:raster_coral(cmd->coral.p,cmd->coral.base,cmd->coral.variant);break;
        case JELLY_CMD:raster_jelly(cmd->jelly.s,cmd->jelly.phase);break;
        case SNOW_CMD:flat_span(cmd->snow.y,cmd->snow.x,cmd->snow.x+1,cmd->snow.key,cmd->tone);break;
        }
    }
    drawCover=opaque;
}
