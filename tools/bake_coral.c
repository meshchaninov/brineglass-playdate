/* Construct two continuous branching distance fields, then sphere-trace orthographic
   views. Only the depth and lighting of the first surface reach the Playdate. */
#include "coral.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
enum { N=80 };
static const float spacing=2.f/(N-1),band=.13f;
static float field[N][N][N];
typedef struct {float x,y,z;} Point;
static Point add(Point a,Point b){return (Point){a.x+b.x,a.y+b.y,a.z+b.z};}
static Point mul(Point p,float t){return (Point){p.x*t,p.y*t,p.z*t};}
static float dot(Point a,Point b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static Point unit(Point p){return mul(p,1.f/sqrtf(fmaxf(dot(p,p),1e-10f)));}
static float limit(float v,float a,float b){return fminf(b,fmaxf(a,v));}
static uint32_t seed;
static float random01(void){seed=1664525u*seed+1013904223u;return (seed>>8)/16777216.f;}
static int cell(float x){return (int)limit(floorf((x+1)/spacing),0,N-1);}
static void branch(Point a,Point b,float r,float tip){
    Point ab=add(b,mul(a,-1));float inv=1/fmaxf(dot(ab,ab),1e-7f),pad=r+band;
    for(int z=cell(fminf(a.z,b.z)-pad);z<=cell(fmaxf(a.z,b.z)+pad);z++)
    for(int y=cell(fminf(a.y,b.y)-pad);y<=cell(fmaxf(a.y,b.y)+pad);y++)
    for(int x=cell(fminf(a.x,b.x)-pad);x<=cell(fmaxf(a.x,b.x)+pad);x++){
        Point p={x*spacing-1-a.x,y*spacing-1-a.y,z*spacing-1-a.z};
        float t=limit(dot(p,ab)*inv,0,1);
        Point off=add(p,mul(ab,-t));float d=sqrtf(dot(off,off))-(r+t*(tip-r));
        float old=field[z][y][x],join=.013f;
        float blend=fmaxf(0,join-fabsf(old-d));
        field[z][y][x]=fminf(old,d)-blend*blend/(4*join);
    }
}
static void grow(Point p,Point direction,float length,float radius,int order,int fan){
    Point end=add(p,mul(direction,length));branch(p,end,radius,radius*.74f);
    if(order==0)return;
    float angle=random01()*6.2831853f;
    Point spread=fan?(Point){1,0,.15f*sinf(angle)}:(Point){cosf(angle),0,sinf(angle)};
    float fork=.40f+random01()*.27f;
    for(int i=-1;i<=1;i+=2){
        Point dir=add(direction,mul(spread,i*fork));
        dir.y=fmaxf(.24f,dir.y+.16f);dir.z+=fan?.07f*sinf(angle+i):.15f*cosf(angle+i);
        grow(end,unit(dir),length*(.65f+.09f*random01()),radius*.73f,order-1,fan);
    }
}
static void shape(int fan){
    seed=fan?1301:713;
    for(int z=0;z<N;z++)for(int y=0;y<N;y++)for(int x=0;x<N;x++)field[z][y][x]=band;
    Point root={0,-.76f,0};
    if(fan){
        Point trunk={.012f,-.49f,.03f};branch(root,trunk,.112f,.083f);
        for(int k=-3;k<=3;k++){
            Point dir=unit((Point){k*.35f,1-.07f*abs(k),(random01()-.5f)*.27f});
            grow(trunk,dir,.37f+.06f*random01(),.057f,4,1);
        }
    }else{
        for(int k=0;k<9;k++){
            float angle=k*2.4f,reach=.15f+.70f*(k%4)/3;
            Point start=add(root,(Point){.12f*cosf(angle),0,.12f*sinf(angle)});
            Point dir=unit((Point){reach*cosf(angle),1,reach*sinf(angle)});
            branch(root,start,.105f,.075f);
            grow(start,dir,.42f+.09f*random01(),.076f,3,0);
        }
    }
}
static float sample(Point p){
    if(fabsf(p.x)>=.9999f || fabsf(p.y)>=.9999f || fabsf(p.z)>=.9999f)return band;
    float gx=(p.x+1)/spacing,gy=(p.y+1)/spacing,gz=(p.z+1)/spacing;
    int x=(int)gx,y=(int)gy,z=(int)gz;float u=gx-x,v=gy-y,w=gz-z,sum=0;
    for(int k=0;k<2;k++)for(int j=0;j<2;j++){
        float *row=&field[z+k][y+j][x];sum+=(row[0]+u*(row[1]-row[0]))*(j?v:1-v)*(k?w:1-w);
    }
    return sum;
}
static unsigned trace(Point across,Point up,Point front,float u,float v){
    Point plane=add(mul(across,u),mul(up,v));float travel=1.42f;
    for(int i=0;i<135 && travel> -1.42f;i++){
        Point p=add(plane,mul(front,travel));float d=sample(p);
        if(d<.004f){
            float e=.014f;
            Point n=unit((Point){sample(add(p,(Point){e,0,0}))-sample(add(p,(Point){-e,0,0})),
                                 sample(add(p,(Point){0,e,0}))-sample(add(p,(Point){0,-e,0})),
                                 sample(add(p,(Point){0,0,e}))-sample(add(p,(Point){0,0,-e}))});
            Point sun={-.46f,.81f,.35f};
            float shadow=limit(sample(add(p,mul(sun,.13f)))/.12f,0,1);
            float pores=sinf(113*p.x+33*p.z)*sinf(95*p.y-57*p.z);
            int tone=(int)limit(74+57*dot(n,sun)+23*shadow+17*p.y+7*pores,37,183);
            return (unsigned)(tone*256+(int)limit(128+travel*83,1,255));
        }
        travel-=fmaxf(.004f,d*.80f);
    }
    return 0;
}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"w");if(!f)return 1;
    fprintf(f,"#include \"coral.h\"\nconst uint16_t coralAtlas[CORAL_FORMS][CORAL_TILTS][CORAL_ANGLES][CORAL_TEXELS][CORAL_TEXELS]={\n");
    for(int form=0;form<CORAL_FORMS;form++){
        shape(form);fputs("{\n",f);
        for(int t=0;t<CORAL_TILTS;t++){
            float tilt=(t-1)*.56f;fputs("{\n",f);
            for(int a=0;a<CORAL_ANGLES;a++){
                float angle=a*(6.2831853f/CORAL_ANGLES);
                Point front={sinf(angle)*cosf(tilt),sinf(tilt),cosf(angle)*cosf(tilt)};
                Point across={-cosf(angle),0,sinf(angle)},up={-sinf(angle)*sinf(tilt),cosf(tilt),-cosf(angle)*sinf(tilt)};
                fputs("{\n",f);
                for(int y=0;y<CORAL_TEXELS;y++){
                    fputs("{",f);
                    for(int x=0;x<CORAL_TEXELS;x++)fprintf(f,"%u,",trace(across,up,front,(x+.5f)*2/CORAL_TEXELS-1,1-(y+.5f)*2/CORAL_TEXELS));
                    fputs("},\n",f);
                }
                fputs("},\n",f);
            }
            fputs("},\n",f);
        }
        fputs("},\n",f);
    }
    fputs("};\n",f);return fclose(f)!=0;
}
