#include "garden.h"
#include "stone.h"
#include "coral.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The light buffer is linear monochrome. Only the final pass converts it to LCD bits. */
enum { LCD_W=WIDTH*2, LCD_H=HEIGHT*2 };
static uint8_t light[HEIGHT][WIDTH], wave[256];
static uint8_t openWater[HEIGHT/2][WIDTH/2];
/* Each terrain ray owns two background columns. Keep one depth and one
   tone per ray until the foreground grid is initialized. */
static uint16_t groundDepth[HEIGHT][WIDTH/2];
static uint8_t groundLight[HEIGHT][WIDTH/2],waterTile[HEIGHT/2][WIDTH/2];
typedef struct {uint8_t lane[WIDTH],row[HEIGHT],waterX[WIDTH/2],waterY[HEIGHT/2];} GridMap;
static GridMap gridMaps[3];
/* Only small, distant life fades into the opaque surface behind it. */
static int smallPass;
static uint8_t bedLight[HEIGHT][WIDTH];
enum { CLEAR_RANGE=12, REEF_RANGE=40, TERRAIN_RANGE=78, FOG_BINS=512 };
typedef struct {uint16_t water,reef;} Attenuation;
static Attenuation attenuation[FOG_BINS];


static int detailLevel=DETAIL_FULL,viewWidth=WIDTH,viewHeight=HEIGHT,pixelSize=2;
static float meshScale=1;
void set_render_detail(int detail){
    detailLevel=detail;pixelSize=4-detail;
    meshScale=2.f/pixelSize;
    viewWidth=(LCD_W+pixelSize-1)/pixelSize;viewHeight=LCD_H/pixelSize;
}
static uint16_t depth[HEIGHT][WIDTH];
static const int focal=110;
static const Diver *view;
static float headingX,headingZ,horizon;
static int tick;

static float clampf(float v,float lo,float hi){return fminf(hi,fmaxf(lo,v));}
static int min2(int a,int b){return a<b?a:b;}
static int max2(int a,int b){return a>b?a:b;}
static int sine(int i){return wave[(unsigned)i&255];}
/* One LCD stamp per source sample. Tone and both local slopes select a
   dithered plane. Each byte of the stamp is a separate LCD scanline. */
enum { SLOPES=9, RAMPS=32*SLOPES*SLOPES, PHASES=4+16+1 };
static const int gradient[SLOPES]={-112,-72,-40,-16,0,16,40,72,112};
static uint32_t stamps[PHASES*RAMPS];
static uint16_t slopeX[511],slopeY[511];
static uint8_t whiteThreshold[4][4];
/* Sort stamps by gradient, then tone and horizontal Bayer phase. Adjacent
   tones and their alternating pixel patterns now share cache lines. */
static int stamp_band(int size,int y){
    if(size==4)return 20*RAMPS;
    if(size==3)return (4+4*(y&3))*RAMPS;
    return 2*(y&1)*RAMPS;
}
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
static void init_tables(void){
    for(int i=0;i<TRIG_STEPS;i++)motionTable[i]=sinf(i*(6.283185307f/TRIG_STEPS));
    motionTable[TRIG_STEPS]=motionTable[0];
    static const int bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    int ink[256];
    for(int i=0;i<256;i++){
        wave[i]=(uint8_t)(127.5f+127.5f*sinf(i*.02454369f));
        float t=clampf((i-26)/208.f,0,1);
        ink[i]=(int)(255*t*t*(3-2*t));
    }
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){
        int tone=0;while(ink[tone]<=16*bayer[y][x]+7)tone++;
        whiteThreshold[y][x]=(uint8_t)tone;
    }
    for(int d=-255;d<=255;d++){
        int nearest=0;
        for(int k=1;k<SLOPES;k++)
            if(abs(d-2*gradient[k])<abs(d-2*gradient[nearest]))nearest=k;
        slopeX[d+255]=(uint16_t)(nearest*SLOPES*32);
        slopeY[d+255]=(uint16_t)(nearest*32);
    }
    for(int size=2;size<=4;size++){
        int phases=size==4?1:size==2?2:4;
        for(int y=0;y<phases;y++)for(int x=0;x<phases;x++){
            uint32_t *table=stamps+stamp_band(size,y)+x;
            for(int tone=0;tone<32;tone++)for(int gx=0;gx<SLOPES;gx++)for(int gy=0;gy<SLOPES;gy++){
                uint32_t block=0;
                for(int b=0;b<size;b++)for(int a=0;a<size;a++){
                    int delta=gradient[gx]*(2*a+1-size)+gradient[gy]*(2*b+1-size);
                    int value=tone*8+4+delta/(2*size);
                    if(value>=whiteThreshold[(y*size+b)&3][(x*size+a)&3])
                        block|=1u<<(8*b+size-1-a);
                }
                table[((gx*SLOPES+gy)*32+tone)*phases]=block;
            }
        }
    }
}
/* All positions are evaluated from the current camera. The one repeating tile
   stores wave intensity, and the depth slices store camera-independent distances. */
enum { MAX_SLICES=90 };
typedef struct {float z,inv; int key,haze;} Slice;
static Slice slices[3][MAX_SLICES];
static int sliceCount[3];
static uint8_t seaTile[64][64];
/* Bilinear patch coefficients in quarter metres, sampled continuously as a
   ray crosses a two-metre terrain cell. The material uses its own metre grid. */
typedef struct {int16_t base,dx,dz,twist;} HeightPatch;
static HeightPatch field[GRID-1][GRID-1];
static int8_t strata[64][64];
static void prepare_field(void){
    for(int z=0;z<GRID-1;z++)for(int x=0;x<GRID-1;x++){
        int a=seabed[z][x],b=seabed[z][x+1],c=seabed[z+1][x],d=seabed[z+1][x+1];
        field[z][x]=(HeightPatch){a,b-a,c-a,a-b-c+d};
    }
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        int n=stone_sample(x*128,y*150,x*73+y*13);
        strata[y][x]=(int8_t)(n*.63f+(sine(y*19+sine(x*4)/8)-127)*.06f);
    }
}
static int fade_weight(float z,float start,float end){
    float t=clampf((z-start)/(end-start),0,1);
    return (int)(256*t*t*(3-2*t)+.5f);
}
static void prepare_environment(void){
    for(int q=0;q<3;q++){
        GridMap *m=&gridMaps[q];
        int dst=4-q,src=4;
        int lanes=((LCD_W+src-1)/src)/2,rows=LCD_H/src;
        for(int x=0;x<(LCD_W+dst-1)/dst;x++){
            int col=(2*x+1)*dst/(2*src);
            m->lane[x]=min2(lanes-1,col/2);
            if(x<(LCD_W+dst-1)/dst/2)m->waterX[x]=min2(lanes-1,col);
        }
        for(int y=0;y<LCD_H/dst;y++){
            int row=(2*y+1)*dst/(2*src);
            m->row[y]=min2(rows-1,row);
            if(y<LCD_H/dst/2)m->waterY[y]=min2(rows/2-1,row);
        }
    }
    for(int i=0;i<FOG_BINS;i++){
        float z=32760.f/(i*8+4);
        attenuation[i]=(Attenuation){fade_weight(z,CLEAR_RANGE,TERRAIN_RANGE-2),
                                     fade_weight(z,REEF_RANGE-10,REEF_RANGE-1)};
    }
    for(int z=0;z<64;z++)for(int x=0;x<64;x++){
        int a=sine(x*4+sine(z*4)/6),b=sine(z*4+a/4);
        seaTile[z][x]=(uint8_t)(a/4+b/7+max2(0,35-abs(a-b))*2);
    }
    for(int q=0;q<3;q++){
        for(float z=1.2f;sliceCount[q]<MAX_SLICES;){
            z=fminf(z,TERRAIN_RANGE);
            Slice *s=&slices[q][sliceCount[q]++];
            s->z=z;s->inv=focal/z;s->key=(int)(32760/z);s->haze=fade_weight(z,CLEAR_RANGE,TERRAIN_RANGE-2);
            if(z>=TERRAIN_RANGE)break;
            float step=.55f+(2-q)*.22f+z*(.054f+(2-q)*.012f);
            z+=step+fmaxf(0,(z-30)*.10f);
        }
    }
}
static void water(void){
    int phase=(int)(view->time*16),angle=(int)(view->yaw*108);
    int shafts[WIDTH+96];
    for(int i=0;i<WIDTH+96;i++){
        int a=i+angle;
        int group=sine(a*2+phase/6)+sine(a*4-phase/8);
        shafts[i]=(group*group/4000)*(80+sine(5*a+phase/5))/340;
    }
    for(int y=0;y<viewHeight;y+=2){
        int sceneY=(int)(y/meshScale);
        float elevation=(horizon-sceneY)/focal;
        int ambient=161+(int)(44*clampf(elevation+.35f,-.8f,1.5f)+view->p.y*.28f);
        float dist=-view->p.y/fmaxf(.05f,elevation),step=dist/focal;
        int surf=(int)(72*clampf(elevation*2,0,1)*clampf((175-dist)/125,0,1));
        float wx=view->p.x+dist*headingX-100*step*headingZ;
        float wz=view->p.z+dist*headingZ+100*step*headingX;
        int u=(int)((wx*3.3f+wz*6.2f)*64)+phase*64,v=(int)((wz*9.7f-wx*2.5f)*64)-phase*64;
        int du=(int)((headingZ*3.3f-headingX*6.2f)*step*128/meshScale);
        int dv=(int)((-headingX*9.7f-headingZ*2.5f)*step*128/meshScale);
        unsigned shift=(HEIGHT-1-sceneY)*3/5;
        for(int x=0;x<viewWidth;x+=2,u+=du,v+=dv){
            int shaft=(shafts[x*pixelSize/2+shift]*(176-sceneY))>>7;
            int ripple=0;
            if(surf){
                int broad=seaTile[(v>>8)&63][(u>>8)&63];
                int cross=seaTile[((u>>9)-(phase>>1))&63][((v>>9)+(phase>>2))&63];
                ripple=surf*(3*broad+cross-228)/384;
            }
            int clearWater=ambient+shaft+ripple;
            uint8_t value=(uint8_t)min2(255,clearWater+((176-clearWater)>>3));
            waterTile[y/2][x/2]=value;
        }
        int lanes=viewWidth/2;
        memcpy(groundLight[y],waterTile[y/2],lanes);
        memcpy(groundLight[y+1],waterTile[y/2],lanes);
        memset(groundDepth[y],0,lanes*sizeof(uint16_t));
        memset(groundDepth[y+1],0,lanes*sizeof(uint16_t));
    }
}

typedef struct {float y;int slice,firstRow,shade;} Crest;
static Crest crest[WIDTH/2];
static float continuous_height(int gx,int gz){
    const HeightPatch *m=&field[gz>>11][gx>>11];
    int fx=(gx>>3)&255,fz=(gz>>3)&255;
    int height=m->base*256+m->dx*fx+m->dz*fz+((m->twist*fx*fz)>>8);
    return height*(1.f/1024);
}
static void bottom(void){
    int top[WIDTH];
    float previousY[WIDTH];
    for(int x=0;x<WIDTH;x++){top[x]=viewHeight;previousY[x]=viewHeight;}
    for(int i=0;i<viewWidth/2;i++)crest[i]=(Crest){viewHeight,-1,viewHeight,0};
    const int stepX=2;
    int previousKey=0;
    for(int k=0;k<sliceCount[detailLevel];k++){
        const Slice *s=&slices[detailLevel][k];
        float step=s->z/focal;
        float start=(.5f*stepX/meshScale-100)*step;
        int gx=(int)((view->p.x+EXTENT+s->z*headingX+start*headingZ)*1024);
        int gz=(int)((view->p.z+EXTENT+s->z*headingZ-start*headingX)*1024);
        int dx=(int)(step*stepX/meshScale*headingZ*1024),dz=(int)(-step*stepX/meshScale*headingX*1024);
        for(int x=0;x<viewWidth;x+=stepX,gx+=dx,gz+=dz){
            if(!top[x] || (unsigned)gx>=2u*(GRID-1)*1024 || (unsigned)gz>=2u*(GRID-1)*1024)continue;
            float h=continuous_height(gx,gz);
            float screen=(horizon+(view->p.y-h)*s->inv)*meshScale;
            if(screen<crest[x/2].y){crest[x/2].y=screen;crest[x/2].slice=k;}
            float prev=previousY[x];previousY[x]=screen;
            int y=max2(0,(int)ceilf(screen));
            if(y>=top[x])continue;
            int ix=gx>>10,iz=gz>>10,tx=(gx>>6)&15,tz=(gz>>6)&15;
            const uint8_t *n=groundTone[iz]+ix,*south=n+GROUND_SAMPLES;
            int a=n[0]*16+tx*(n[1]-n[0]),b=south[0]*16+tx*(south[1]-south[0]);
            int tone=(a*16+(b-a)*tz)>>8;
            int rock=(int)clampf((h+42)*20,0,180);
            int caustic=seaTile[((gz>>8)-(tick>>2))&63][((gx>>8)+(tick>>2))&63];
            int value=tone+(caustic>>3)+max2(0,caustic-100)/3-7;
            int span=top[x]-y;
            float slope=previousKey && prev>screen+.05f?(previousKey-s->key)/(prev-screen):0;
            int dk=(int)(slope*256),key=(int)((s->key+(y-screen)*slope)*256);
            int tu=((gx>>8)+(gz>>9))&63;
            float dy=s->z/(focal*meshScale);
            int tv=(int)((view->p.y+(horizon*meshScale-y)*dy)*1024)+(gz>>3);
            int dv=(int)(dy*1024);
            for(int row=y;row<y+span;row++,key+=dk,tv-=dv){
                int relief=(rock*strata[(tv>>8)&63][tu])>>8;
                uint8_t tone=(uint8_t)max2(12,min2(239,value+relief));
                if(s->haze)tone+=(int)((waterTile[row/2][x/2]-tone)*s->haze)>>8;
                uint16_t hit=(uint16_t)(key>>8);
                groundLight[row][x/2]=tone;
                groundDepth[row][x/2]=hit;
            }
            top[x]=y;
        }
        previousKey=s->key;
    }
    for(int i=0;i<viewWidth/2;i++){
        int row=top[2*i];
        crest[i].firstRow=row;
        if(row<viewHeight)crest[i].shade=groundLight[min2(viewHeight-1,row+1)][i];
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
static inline void paint_sample(int y,int x,int key,int tone){
    if(key<32760/CLEAR_RANGE){
        Attenuation f=attenuation[key>>3];
        tone+=((openWater[y/2][x/2]-tone)*f.water)>>8;
        if(smallPass && f.reef){
            if(f.reef==256)return;
            tone+=((bedLight[y][x]-tone)*f.reef)>>8;
        }
    }
    depth[y][x]=(uint16_t)key;light[y][x]=(uint8_t)tone;
}
static void triangle(Screen a,Screen b,Screen c,int color){
    /* Increment the inverse depth across each scanline; horizontal edges need no special division. */
    if(b.y<a.y){Screen s=a;a=b;b=s;}
    if(c.y<a.y){Screen s=a;a=c;c=s;}
    if(c.y<b.y){Screen s=b;b=c;c=s;}
    if(c.y-a.y<.01f)return;
    int first=max2(0,(int)ceilf(a.y)),last=min2(viewHeight,(int)ceilf(c.y));
    if(first>=last || (a.x<0 && b.x<0 && c.x<0) ||
       (a.x>=viewWidth && b.x>=viewWidth && c.x>=viewWidth))return;
    float slope=(c.x-a.x)/(c.y-a.y);
    float zSlope=(c.iz-a.iz)/(c.y-a.y);
    float across=b.x-(a.x+(b.y-a.y)*slope);
    float dz=fabsf(across)>.0001f?(b.iz-a.iz-(b.y-a.y)*zSlope)/across:0;
    int dzFixed=(int)(clampf(dz,-524287.f,524287.f)*16);
    for(int half=0;half<2;half++){
        Screen p=half?b:a,q=half?c:b;
        int y0=max2(first,(int)ceilf(p.y)),y1=min2(last,(int)ceilf(q.y));
        if(y0>=y1)continue;
        float shortSlope=(q.x-p.x)/(q.y-p.y);
        float x1=a.x+(y0-a.y)*slope,x2=p.x+(y0-p.y)*shortSlope;
        float z=a.iz+(y0-a.y)*zSlope;
        for(int y=y0;y<y1;y++,x1+=slope,x2+=shortSlope,z+=zSlope){
            int left=max2(0,(int)ceilf(fminf(x1,x2)));
            int right=min2(viewWidth,(int)ceilf(fmaxf(x1,x2)));
            if(left>=right)continue;
            int key=(int)(clampf(z+(left-x1)*dz,0,65535)*16);
            for(int x=left;x<right;x++,key+=dzFixed){
                int k=key>>4;
                if(k>=depth[y][x])paint_sample(y,x,k,color);
            }
        }
    }
}
/* Static landmarks keep shared vertices in world coordinates. Projection is
   performed once per vertex for the current camera, including free swimming. */
enum { MAX_MODEL_VERTS=512, MAX_MODEL_FACES=640 };
typedef struct {uint16_t a,b,c,tone;} Facet;
enum { GROUP_FACES=16, MAX_GROUPS=(MAX_MODEL_FACES+GROUP_FACES-1)/GROUP_FACES, VERT_WORDS=(MAX_MODEL_VERTS+31)/32 };
typedef struct {Vec center,half;} ModelBounds;
typedef struct {ModelBounds bounds;uint32_t vertices[VERT_WORDS];} FaceGroup;
typedef struct {ModelBounds bounds;FaceGroup groups[MAX_GROUPS];int nverts,nfaces,overflow;Vec positions[MAX_MODEL_VERTS];Facet faces[MAX_MODEL_FACES];} Model;
static Model landmarks[2+3],*building;
typedef struct {Vec normal;float offset;} FacingPlane;
static FacingPlane flukeInterior[MAX_MODEL_FACES];
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
        f->a=model_vertex(a);f->b=model_vertex(b);f->c=model_vertex(c);f->tone=tone;
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
static Vec ship(float x,float y,float z){
    return (Vec){59.5f+x*.90f-z*.43f,-23+y-.12f*x+.14f*z,6+x*.43f+z*.90f};
}
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
    for(unsigned i=0;i<sizeof stock/sizeof *stock;i++){
        const float *section=stock[i];
        float bend=fmaxf(0,1-section[0]/5.5f);
        for(int j=0;j<8;j++)
            ring[j]=fluke_point(ground,section[2]*ovalX[j]-1.6f*bend*bend,section[0],section[1]+section[3]*ovalZ[j]);
        if(i)for(int j=0;j<8;j++)
            quad(last[j],last[(j+1)%8],ring[(j+1)%8],ring[j],stockTone[j]);
        memcpy(last,ring,sizeof ring);
    }
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
    static const float chord[]={0,.18f,.53f,.85f,1,.60f,.19f};
    static const float bulge[]={0,.94f,.72f,.30f,0,-.54f,-.77f};
    static const int bladeTone[]={43,65,87,64,46,38,42};
    for(int side=-1;side<=1;side+=2){
        for(unsigned i=0;i<sizeof sections/sizeof *sections;i++){
            const float *section=sections[i];
            float span=side*section[0],roll=.060f*span;
            for(int j=0;j<7;j++){
                float height=section[1]+(section[2]-section[1])*chord[j]+roll;
                float z=section[3]-.29f*(section[2]-section[1])*chord[j]+section[4]*bulge[j];
                ring[j]=fluke_point(ground,span,height,z);
            }
            if(i)for(int j=0;j<7;j++){
                int start=building?building->nfaces:0;
                quad(last[j],last[(j+1)%7],ring[(j+1)%7],ring[j],bladeTone[j]);
                /* Only the broad inner blade has enough thickness to
                   resolve front and back in the quantized depth buffer. */
                if(building && i<=7 && (j==1 || j==2 || j==5)){
                    for(int k=start;k<building->nfaces;k++){
                        Facet f=building->faces[k];
                        Vec a=building->positions[f.a],b=building->positions[f.b],c=building->positions[f.c];
                        Vec u={b.x-a.x,b.y-a.y,b.z-a.z},v={c.x-a.x,c.y-a.y,c.z-a.z};
                        Vec n={u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
                        float scale=-side/fmaxf(.0001f,sqrtf(n.x*n.x+n.y*n.y+n.z*n.z));
                        n=(Vec){n.x*scale,n.y*scale,n.z*scale};
                        flukeInterior[k]=(FacingPlane){n,-(n.x*a.x+n.y*a.y+n.z*a.z)};
                    }
                }
            }
            memcpy(last,ring,7*sizeof(Vec));
        }
    }
}
static void stone_arch(const Arch *a){
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
    building=&landmarks[0];wreck();
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
/* A plane rejected throughout this box of world-space camera positions stays rejected
   as the diver moves within it. Rebuild only when the eye leaves the box. */
static uint32_t *fluke_mask(const Model *m){
    static Vec eye;
    static uint32_t mask[MAX_GROUPS];
    static int valid;
    Vec e=view->p;
    if(valid && fabsf(e.x-eye.x)<=1 && fabsf(e.y-eye.y)<=1 && fabsf(e.z-eye.z)<=1)return mask;
    eye=e;valid=1;
    Vec d={e.x-m->bounds.center.x,e.y-m->bounds.center.y,e.z-m->bounds.center.z};
    float margin=-.5f*sqrtf(d.x*d.x+d.y*d.y+d.z*d.z)-3;
    for(int i=0;i<m->nfaces;i++){
        if(i%GROUP_FACES==0)mask[i/GROUP_FACES]=0;
        FacingPlane p=flukeInterior[i];
        if(e.x*p.normal.x+e.y*p.normal.y+e.z*p.normal.z+p.offset>=margin)
            mask[i/GROUP_FACES]|=1u<<(i%GROUP_FACES);
    }
    return mask;
}
static void draw_model(const Model *model){
    if(model->overflow){
        int id=(int)(model-landmarks);
        if(id==0)wreck();else if(id==1)fluke();else stone_arch(&arches[id-2]);
        return;
    }
    int visibility=classify_bounds(&model->bounds);
    if(visibility==MODEL_HIDDEN)return;
    Camera center=camera(model->bounds.center);
    int flukeOutside=model==&landmarks[1] && center.z>11 && center.z<48 &&
                     (fabsf(view->p.x+5)>11 || fabsf(view->p.z+4)>8);
    const uint32_t *facing=flukeOutside?fluke_mask(model):NULL;

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
        uint32_t visible=facing?facing[j]:(1u<<count)-1u;
        for(;visible;visible&=visible-1){
            int i=j*GROUP_FACES+__builtin_ctz(visible);
            Facet f=model->faces[i];
            if(clip[f.a]&clip[f.b]&clip[f.c])continue;
            if((clip[f.a]|clip[f.b]|clip[f.c])&16)
                face(model->positions[f.a],model->positions[f.b],model->positions[f.c],f.tone);
            else triangle(screen[f.a],screen[f.b],screen[f.c],f.tone);
        }
    }
}
typedef struct { float x,y,scale,iz,z; } Sprite;
static Screen vertex(Sprite s,float x,float y){return (Screen){s.x+x*s.scale,s.y-y*s.scale,s.iz};}
static void oval(Sprite s,float x,float y,float rx,float ry,int tone){
    Screen p=vertex(s,x,y);
    float w=fmaxf(.6f,rx*s.scale),h=fmaxf(.6f,ry*s.scale);
    int y0=max2(0,(int)ceilf(p.y-h)),y1=min2(viewHeight-1,(int)(p.y+h));
    for(int row=y0;row<=y1;row++){
        float dy=(row-p.y)/h,extent=w*sqrtf(fmaxf(0,1-dy*dy));
        int left=max2(0,(int)ceilf(p.x-extent)),right=min2(viewWidth-1,(int)(p.x+extent));
        for(int i=left;i<=right;i++)if(s.iz>=depth[row][i]){
            paint_sample(row,i,(int)s.iz,tone);
        }
    }
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
static void outcrop(Vec center,float radius,float height,int seed){
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
        quad(foot[j],foot[k],rim[k],rim[j],77-sun);
        face(rim[j],rim[k],peak,112-sun);
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
static void draw_coral(const Prop *p,Camera base,int variant){
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
    float stepX=CORAL_TEXELS/(2*rx),stepY=CORAL_TEXELS/(2*ry);
    int y0=max2(0,(int)ceilf(top-.5f)),y1=min2(viewHeight,(int)ceilf(center.y+ry-.5f));
    int x0=max2(0,(int)ceilf(left-.5f)),x1=min2(viewWidth,(int)ceilf(center.x+rx-.5f));
    float toward=size*base.z/(83.f*sqrtf(range*range+dy*dy));
    for(int y=y0;y<y1;y++){
        const uint16_t *row=atlas[min2(CORAL_TEXELS-1,(int)((y+.5f-top)*stepY))];
        for(int x=x0;x<x1;x++){
            uint16_t sample=row[min2(CORAL_TEXELS-1,(int)((x+.5f-left)*stepX))];
            if(!sample)continue;
            float z=base.z-((sample&255)-128)*toward;
            if(z<1)continue;
            uint16_t key=(uint16_t)(32760.f/z);
            if(key<depth[y][x])continue;
            paint_sample(y,x,key,sample>>8);
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
        memset(depth,0,sizeof depth);
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
                if(!depth[row][x]){x++;continue;}
                int left=x,tone=light[row][x++];
                while(x<LEAF_W && depth[row][x] && light[row][x]==tone)x++;
                r->spans[r->n++]=(LeafSpan){left,x,tone};
            }
        }
    }
}
static void kelp_sprite(Sprite s,float h,int phase){
    float scale=s.scale*h/(6.3f*12),inv=1.f/scale;
    float top=s.y-79*scale,left=s.x-32*scale;
    int start=max2(0,(int)ceilf(top)),end=min2(viewHeight,(int)ceilf(s.y+scale));
    int variant=phase&3;
    int darkest=41+variant*3,shine=95+variant*3;
    float sway=scale*(sine(phase+tick*2)-127)*.040f;
    for(int y=start;y<end;y++){
        int row=min2(79,(int)((y-top)*inv));
        const LeafRow *r=&canopy[variant][row];
        float bend=(79-row)*(1.f/79),origin=left+sway*bend*bend;
        for(int n=0;n<r->n;n++){
            LeafSpan span=r->spans[n];
            int x0=max2(0,(int)ceilf(origin+span.left*scale));
            int x1=min2(viewWidth,(int)ceilf(origin+span.right*scale));
            int tone=span.tone<80?darkest:shine;
            for(int x=x0;x<x1;x++)if(s.iz>=depth[y][x]){
                paint_sample(y,x,(int)s.iz,tone);
            }
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
/* A tile stores the farthest opaque depth within it. Rejecting a prop
   requires every tile intersecting its bounds to be in front of it. */
enum { TILE=8, TILE_COLS=WIDTH/TILE, TILE_ROWS=HEIGHT/TILE };
static uint16_t tileZ[TILE_ROWS][TILE_COLS];
static void occlusion_tiles(void){
    /* Front-to-back spans fill upwards. Depth is farthest at the top row of
       each column, including zero in open water. Pairs share a ray. */
    const GridMap *m=&gridMaps[detailLevel];
    for(int y=0;y<viewHeight;y+=TILE)for(int x=0;x<viewWidth;x+=TILE){
        const uint16_t *ground=groundDepth[m->row[y]];
        int z=65535,last=m->lane[min2(viewWidth-1,x+TILE-1)];
        for(int i=m->lane[x];i<=last;i++)z=min2(z,ground[i]);
        tileZ[y/TILE][x/TILE]=(uint16_t)z;
    }
}
static int hidden(int x0,int y0,int x1,int y1,int key){
    x0=max2(0,x0)/TILE;x1=min2(viewWidth-1,x1)/TILE;
    y0=max2(0,y0)/TILE;y1=min2(viewHeight-1,y1)/TILE;
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)if(tileZ[y][x]<=key)return 0;
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
    if(c.z<1.5f || c.z>REEF_RANGE || fabsf(c.x)>c.z*.95f+6)return;
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
            oval(s,x,t,.09f+t*.009f,.11f+t*.008f,216);
        }
        return;
    }
    if(pos.y<0 || pos.y-(p->kind==CORAL?1.7f:1.f)*h*s.scale>meshScale*HEIGHT+2)return;
    switch(p->kind){
    case KELP:
        if(c.z<(detailLevel==DETAIL_FULL?14:5) && detailLevel>DETAIL_LOW)outcrop(p->p,.45f+h*.026f,.45f+h*.024f,phase);
        kelp_sprite(s,h,phase);
        break;
    case SPONGE:
        if(c.z<(detailLevel==DETAIL_FULL?12:4) && detailLevel>DETAIL_LOW)sponge(p);
        else sponge_lod(s,h);
        break;
    case CORAL:
        if(c.z<(detailLevel==DETAIL_FULL?14:5) && detailLevel>DETAIL_LOW)outcrop(p->p,h*.48f,h*.28f,phase);
        draw_coral(p,c,phase);
        break;
    case BOULDER:
        if(c.z<(detailLevel==DETAIL_FULL?13:6) && detailLevel>DETAIL_LOW)outcrop(p->p,h*.90f,h*.65f,phase);
        else distant_rock(s,h,phase);
        break;
    default:break;
    }
}
enum { CELL_WIDTH=20, CELL_COUNT=2*EXTENT/CELL_WIDTH };
static int16_t firstProp[CELL_COUNT][CELL_COUNT],nextProp[PROP_COUNT];
static void index_props(void){
    memset(firstProp,0xff,sizeof firstProp);
    for(int i=0;i<PROP_COUNT;i++){
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
        Screen v=project(cam);
        int sx=(int)v.x,sy=(int)v.y;
        if(sx>=0 && sx<meshScale*WIDTH && sy>=0 && sy<meshScale*HEIGHT && v.iz>depth[sy][sx]){
            light[sy][sx]=(uint8_t)(182+(i%4)*12);
        }
    }
}
/* Build the silhouette in world space so that a turn of the camera also
   turns a side view into a head-on or tail-on view. */
static void draw_fish(Vec center,Vec velocity){
    Camera f=camera(center);
    if(f.z<1 || f.z>REEF_RANGE || fabsf(f.x)>f.z+1)return;
    float flat=sqrtf(velocity.x*velocity.x+velocity.z*velocity.z);
    float length=sqrtf(flat*flat+velocity.y*velocity.y);
    if(length<.0001f)return;
    Vec forward={velocity.x/length,velocity.y/length,velocity.z/length};
    Vec side=flat>.0001f?(Vec){velocity.z/flat,0,-velocity.x/flat}:(Vec){1,0,0};
    if(f.z>12){
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
        return;
    }
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
    for(int j=0;j<8;j++){
        int k=(j+1)%8;
        quad(rim[j],rim[k],plate[k],plate[j],56+(j%3)*10);
        face(plate[j],plate[k],top,88+(j%2)*10);face(rim[k],rim[j],belly,107);
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
    float pulse=motion_sin(phase),radius=.66f+.11f*pulse,height=.52f-.07f*pulse;
    int fringe=91;
    /* Open bell, with a lit crown and a dark scalloped underside. */
    oval(sp,0,0,radius,.15f,fringe);
    Screen crest=vertex(sp,0,height);
    for(int i=0;i<8;i++){
        float a=i*3.141593f/8,b=(i+1)*3.141593f/8;
        Screen first=vertex(sp,radius*motion_cos(a),height*motion_sin(a));
        Screen last=vertex(sp,radius*motion_cos(b),height*motion_sin(b));
        triangle(vertex(sp,0,0),first,last,146+5*(i%3));
    }
    triangle(crest,vertex(sp,-radius*.40f,height*.76f),vertex(sp,0,height*.68f),195);
    for(int i=0;i<5;i++){
        float x=(i-2)*radius*.34f,y=0;
        for(int j=1;j<=3;j++){
            float nx=(i-2)*radius*.34f+.15f*motion_sin(phase-j*.9f+i*.7f)*j*.6f;
            float ny=-j*(.40f+.06f*(i%2));
            float w=fmaxf(.025f,.48f/sp.scale);
            triangle(vertex(sp,x-w,y),vertex(sp,x+w,y),vertex(sp,nx+w,ny),fringe);
            triangle(vertex(sp,x-w,y),vertex(sp,nx+w,ny),vertex(sp,nx-w,ny),fringe);
            x=nx;y=ny;
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
                if(fish.y<floor_at(fish.x,fish.z)+.5f || fish.y>-.5f)continue;
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

static void animals(void){
    static const float paths[][5]={{1,-28,3.5f,14,3.012f},{49,-24,24.5f,10,.6f},
        {0,-24,-80,28,.3f},{-50,-29,20,22,5},{10,-28,86,22,2.2f},{20,-20,-20,38,4.2f}};
    for(int i=0;i<6;i++){
        const float *p=paths[i];
        float a=view->time*.069f+p[4],c=motion_cos(a),s=motion_sin(a);
        Vec m={p[0]+p[3]*c,p[1]+motion_sin(a*2)*.7f,p[2]+p[3]*s};
        Camera cam=camera(m);
        if(cam.z>-6 && cam.z<REEF_RANGE+5 && fabsf(cam.x)<cam.z+7){
            Vec nose={m.x-s*1.6f,m.y+.3f,m.z+c*1.6f},back={m.x+s*1.7f,m.y,m.z-c*1.7f};
            Vec tail={m.x+s*4.5f,m.y-.3f,m.z-c*4.5f};
            for(int side=-1;side<=1;side+=2){
                float flap=motion_sin(view->time*1.65f+i),w=(4.4f+.22f*i)*side;
                Vec joint={m.x+c*w*.43f,m.y+.7f,m.z+s*w*.43f};
                Vec wing={m.x+c*w+s*.8f,m.y+flap*1.8f,m.z+s*w-c*.8f};
                Vec leading={m.x+c*w*.57f-s*.64f,m.y+.65f+flap*.48f,m.z+s*w*.57f+c*.64f};
                face(nose,joint,back,23);face(nose,leading,joint,76);
                face(leading,wing,joint,62);face(back,joint,wing,40);
                Vec root={back.x+c*side*.28f,back.y,back.z+s*side*.28f};
                face(m,root,tail,26);
            }
        }
        /* A school moves on its own path below each ray. */
        float driftSin=motion_sin(a*.9f),driftCos=motion_cos(a*.9f);
        Camera school=camera((Vec){p[0]+9*driftSin,p[1]-5,p[2]+9*driftCos});
        if(school.z < -8 || school.z>REEF_RANGE+8 || fabsf(school.x)>school.z+16)continue;
        for(int k=0;k<15;k++){
            float angle=a*1.5f+k*2.4f;
            float turnSin=motion_sin(angle),turnCos=motion_cos(angle);
            Vec fish={p[0]+9*driftSin+5*turnCos+(k%3)*1.1f,
                      p[1]-5+1.3f*motion_sin(angle*1.3f),
                      p[2]+9*driftCos+4*turnSin};
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
    int quality=detailLevel;
    set_render_detail(DETAIL_FULL);
    init_tables();prepare_field();prepare_environment();build_canopy();build_landmarks();index_props();
    set_render_detail(quality);ready=1;
}
/* The coarse march identifies the depth slice forming the skyline. Probe
   the angular height again between slices along a new ray at a steep edge. */
static float crest_probe(float u,int slice,int bgLevel){
    float scale=2.f/(4-bgLevel);
    int height=LCD_H/(4-bgLevel);
    if(slice<0)return height;
    const Slice *ring=slices[bgLevel];
    int last=sliceCount[bgLevel]-1;
    float center=ring[slice].z;
    float z[3]={.5f*(ring[max2(0,slice-1)].z+center),center,.5f*(center+ring[min2(last,slice+1)].z)};
    float dx=headingX+u*headingZ,dz=headingZ-u*headingX,best=height;
    for(int i=0;i<3;i++){
        int gx=(int)((view->p.x+EXTENT+dx*z[i])*1024),gz=(int)((view->p.z+EXTENT+dz*z[i])*1024);
        if((unsigned)gx>=2u*(GRID-1)*1024 || (unsigned)gz>=2u*(GRID-1)*1024)continue;
        float y=(horizon+(view->p.y-continuous_height(gx,gz))*focal/z[i])*scale;
        best=fminf(best,y);
    }
    return best;
}
static void refine_skyline(uint8_t *frame,int stride,int bgLevel){
    int bgPixel=4-bgLevel,bgHeight=LCD_H/bgPixel;
    float bgScale=2.f/bgPixel;
    int cols=((LCD_W+bgPixel-1)/bgPixel)/2;
    for(int i=-1;i<cols;i++){
        Crest l=crest[max2(0,i)],r=crest[min2(cols-1,i+1)];
        if((l.slice<0 && r.slice<0) || (l.y< -2 && r.y< -2))continue;
        float left=clampf(l.y,-bgHeight,bgHeight),right=clampf(r.y,-bgHeight,bgHeight);
        float mid=.5f*(left+right);
        if(i>=0 && i+1<cols && fabsf(left-right)>1){
            float u=((2*i+2)/bgScale-100)/focal;
            mid=crest_probe(u,l.slice,bgLevel);
            if(l.slice!=r.slice)mid=fminf(mid,crest_probe(u,r.slice,bgLevel));
            mid=clampf(mid,-bgHeight,bgHeight);
        }
        int start=(2*i+1)*bgPixel;
        int last=min2(LCD_W,start+2*bgPixel);
        for(int x=max2(0,start);x<last;x++){
            int offset=x-start;
            float t=((offset%bgPixel)+.5f)/bgPixel;
            float a=offset<bgPixel?left:mid,b=offset<bgPixel?mid:right;
            float edge=(a+(b-a)*t)*bgPixel;
            int sx=x/pixelSize;
            int lane=gridMaps[detailLevel].lane[sx];
            Crest source=crest[lane];
            float raw=source.firstRow*bgPixel;
            if(fabsf(edge-raw)<.75f)continue;
            int lo=max2(0,(int)floorf(fminf(edge,raw))-bgPixel);
            int hi=min2(LCD_H,(int)ceilf(fmaxf(edge,raw))+bgPixel);
            float invWidth=1/fmaxf(1,fabsf(b-a));
            int leftTone=l.slice>=0?l.shade:r.shade,rightTone=r.slice>=0?r.shade:l.shade;
            float mix=(offset+.5f)/(2*bgPixel);
            int rim=(int)(leftTone+(rightTone-leftTone)*mix);
            if(lo>=hi)continue;
            int alpha=(int)((.5f+(lo+.5f-edge)*invWidth)*4096);
            int advance=(int)(invWidth*4096+.5f);
            int sxByte=x/8,bit=128>>(x&7),endCell=(hi-1)/pixelSize;
            int nearest=l.slice<0?r.slice:r.slice<0?l.slice:min2(l.slice,r.slice);
            int crestKey=nearest<0?0:slices[bgLevel][nearest].key;
            for(int sy=lo/pixelSize,y=lo;sy<=endCell;sy++){
                int end=min2(hi,(sy+1)*pixelSize);
                int water=openWater[sy/2][sx/2];
                int object=depth[sy][sx]!=groundDepth[gridMaps[detailLevel].row[sy]][lane];
                /* A foreground animal or frond owns its original LCD sample.
                   A newly exposed rock edge may cover a more distant object. */
                if(object && depth[sy][sx]>=crestKey){
                    alpha+=(end-y)*advance;y=end;continue;
                }
                int rock=!object && (sy*pixelSize+pixelSize/2)>=source.firstRow*bgPixel?light[sy][sx]:rim;
                if(object)water=light[sy][sx];
                uint8_t *byte=frame+y*stride+sxByte;
                for(;y<end;y++,byte+=stride,alpha+=advance){
                    int blend=max2(0,min2(4096,alpha));
                    int value=water+(((rock-water)*blend)>>12);
                    if(value>=whiteThreshold[y&3][x&3])*byte|=(uint8_t)bit;
                    else *byte&=(uint8_t)~bit;
                }
            }
        }
    }
}
static void write_stamp(uint8_t *out,int stride,uint32_t block,int size){
    out[0]=(uint8_t)block;out[stride]=(uint8_t)(block>>8);
    if(size>=3)out[2*stride]=(uint8_t)(block>>16);
    if(size==4)out[3*stride]=(uint8_t)(block>>24);
}
static inline uint32_t sample_stamp(const uint32_t *table,int phases,int x,
                                    int center,int left,int right,int up,int down){
    int tone=(20*center+3*(left+right+up+down))>>8;
    int code=slopeX[255+right-left]+slopeY[255+down-up]+tone;
    return table[code*phases+(x&(phases-1))];
}
static inline __attribute__((always_inline)) void pack_frame(uint8_t *frame,int stride,int size,int phases){
    uint32_t row[WIDTH];
    int w=(LCD_W+size-1)/size,h=LCD_H/size;
    for(int y=0;y<h;y++){
        const uint8_t *line=light[y],*up=light[max2(0,y-1)],*down=light[min2(h-1,y+1)];
        const uint32_t *table=stamps+stamp_band(size,y);
        row[0]=sample_stamp(table,phases,0,line[0],line[0],line[1],up[0],down[0]);
        for(int x=1;x<w-1;x++)
            row[x]=sample_stamp(table,phases,x,line[x],line[x-1],line[x+1],up[x],down[x]);
        row[w-1]=sample_stamp(table,phases,w-1,line[w-1],line[w-2],line[w-1],up[w-1],down[w-1]);
        uint8_t *out=frame+y*size*stride;
        if(size==4){
            for(int i=0;i<w;i+=2)write_stamp(out++,stride,(row[i]<<4)|row[i+1],4);
        }else if(size==2){
            for(int i=0;i<w;i+=4)
                write_stamp(out++,stride,(row[i]<<6)|(row[i+1]<<4)|(row[i+2]<<2)|row[i+3],2);
        }else{
            for(int i=0;i<w;i+=8){
                uint32_t *p=row+i;
                write_stamp(out++,stride,(p[0]<<5)|(p[1]<<2)|((p[2]>>1)&0x030303),3);
                write_stamp(out++,stride,((p[2]&0x010101)<<7)|(p[3]<<4)|(p[4]<<1)|((p[5]>>2)&0x010101),3);
                if(i+7<w)
                    write_stamp(out++,stride,((p[5]&0x030303)<<6)|(p[6]<<3)|p[7],3);
            }
        }
    }
}
static void display_frame(uint8_t *frame,int stride){
    if(pixelSize==2)pack_frame(frame,stride,2,2);
    else if(pixelSize==3)pack_frame(frame,stride,3,4);
    else pack_frame(frame,stride,4,1);
}
/* A terrain ray covers 2, alternately 2 or 3, or 4 foreground samples. */
static void compose_background(int bgLevel){
    const GridMap *m=&gridMaps[detailLevel];
    int lanes=((LCD_W+3-bgLevel)/(4-bgLevel))/2;
    if(detailLevel==DETAIL_FULL){
        for(int y=0;y<viewHeight;y+=2)for(int i=0;i<lanes;i++){
            int x=4*i;
            uint32_t tone=groundLight[y/2][i]*0x01010101u;
            uint32_t z=groundDepth[y/2][i]*0x00010001u;
            for(int j=0;j<2;j++){
                memcpy(&light[y+j][x],&tone,4);
                memcpy(&depth[y+j][x],&z,4);
                memcpy(&depth[y+j][x+2],&z,4);
            }
        }
        for(int y=0;y<viewHeight/2;y+=2)for(int x=0;x<viewWidth/2;x+=2){
            uint8_t w=waterTile[y/2][x/2];
            openWater[y][x]=openWater[y][x+1]=openWater[y+1][x]=openWater[y+1][x+1]=w;
        }
        return;
    }
    for(int y=0;y<viewHeight;y++){
        int sy=m->row[y];
        uint8_t *l=light[y];uint16_t *d=depth[y];
        if(y && sy==m->row[y-1]){
            memcpy(l,light[y-1],viewWidth);memcpy(d,depth[y-1],viewWidth*sizeof(*d));continue;
        }
        const uint8_t *tone=groundLight[sy];const uint16_t *z=groundDepth[sy];
        int x=0,i=0;
        if(detailLevel==DETAIL_LOW){
            for(;i<lanes;i++,x+=2){
                l[x]=l[x+1]=tone[i];d[x]=d[x+1]=z[i];
            }
        }else{
            for(;i+2<lanes;i+=3,x+=8){
                l[x]=l[x+1]=l[x+2]=tone[i];d[x]=d[x+1]=d[x+2]=z[i];
                l[x+3]=l[x+4]=tone[i+1];d[x+3]=d[x+4]=z[i+1];
                l[x+5]=l[x+6]=l[x+7]=tone[i+2];d[x+5]=d[x+6]=d[x+7]=z[i+2];
            }
        }
        for(;x<viewWidth;x++){l[x]=tone[m->lane[x]];d[x]=z[m->lane[x]];}
    }
    for(int y=0;y<viewHeight/2;y++){
        const uint8_t *src=waterTile[m->waterY[y]];
        if(detailLevel==DETAIL_LOW)memcpy(openWater[y],src,viewWidth/2);
        else for(int x=0;x<viewWidth/2;x++)openWater[y][x]=src[m->waterX[x]];
    }
}
void render(const Diver *d,uint8_t *frame,int stride){
    prepare_renderer();
    view=d;headingX=sinf(d->yaw);headingZ=cosf(d->yaw);
    horizon=60+focal*tanf(d->pitch)+sinf(d->time*1.3f)*.65f;
    tick=(int)(d->time*19);
    int sceneLevel=detailLevel,bgLevel=DETAIL_LOW;
    set_render_detail(bgLevel);
    water();bottom();
    set_render_detail(sceneLevel);
    compose_background(bgLevel);
    occlusion_tiles();
    Camera shipPos=camera((Vec){59.5f,-23,6}),flukePos=camera((Vec){-5,-38,-4});
    if(shipPos.z>-23 && shipPos.z<TERRAIN_RANGE+23 && fabsf(shipPos.x)<shipPos.z+42)draw_model(&landmarks[0]);
    if(flukePos.z>-10 && flukePos.z<TERRAIN_RANGE+10 && fabsf(flukePos.x)<flukePos.z+18)draw_model(&landmarks[1]);
    for(int i=0;i<3;i++){
        Camera p=camera((Vec){arches[i].x,arches[i].y,arches[i].z});
        if(p.z>-18 && p.z<TERRAIN_RANGE+18 && fabsf(p.x)<p.z+36)draw_model(&landmarks[2+i]);
    }
    /* Preserve reef and landmark shading for the short-range fade. */
    memcpy(bedLight,light,viewHeight*sizeof light[0]);
    smallPass=1;
    draw_plants();
    animals();reef_life();marine_snow();
    smallPass=0;
    display_frame(frame,stride);
    refine_skyline(frame,stride,bgLevel);
}
