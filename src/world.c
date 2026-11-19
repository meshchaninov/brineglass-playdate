#include "garden.h"
#include "wreck.h"
#include "stone.h"
#include <math.h>
#include <string.h>

const Site sites[SITE_COUNT]={
    {"Whale fluke",-8,12,-5,-4,2.7f,.29f},
    {"Shipwreck",57,40,59.5f,6,28,-.11f},
    {"Pillar grove",-26,-100,0,-108,10,.08f},
    {"Sponge canyon",-55,-18,-60,8,7,-.04f},
    {"Kelp forest",10,-62,6,-90,9,.02f},
    {"Coral grotto",3,62,1,100,7,.04f}
};
/* Each rail pairs a cubic camera path with a separate moving look-at target.
   Coordinates share the same world as free swimming and the paper chart. */
typedef struct { Vec eye[4],look[4]; } Flyby;
static const Flyby flights[SITE_COUNT]={
    {{{-19,-36,29},{-16,-40,18},{-6,-39,9},{2,-36,10}},
     {{-7,-34,-5},{-6,-34,-4},{-5,-33,-4},{-4,-33,-4}}},
    {{{51,-18,52},{58,-16,43},{66,-15,33},{70,-14,24}},
     {{53,-23,8},{56,-19,7},{63,-19,6},{61,-20,8}}},
    {{{-37,-36,-78},{-35,-34,-92},{-25,-30,-107},{-22,-25,-124}},
     {{-14,-28,-100},{-7,-23,-104},{-6,-19,-109},{3,-18,-112}}},
    {{{-57,-34,-30},{-63,-36,-15},{-55,-37,1},{-61,-34,22}},
     {{-60,-35,-4},{-55,-37,9},{-59,-34,22},{-50,-32,38}}},
    {{{19,-31,-52},{15,-35,-68},{-2,-32,-79},{-13,-28,-91}},
     {{5,-27,-78},{3,-28,-89},{-21,-25,-83},{-42,-27,-82}}},
    {{{1,-37,52},{6,-39,68},{5,-35,86},{0,-35,99}},
     {{9,-34,77},{2,-34,91},{-2,-38,98},{13,-38,108}}}
};
enum { RAIL_STEPS=24 };
static Vec rail[SITE_COUNT][RAIL_STEPS+1];
static float distanceOnRail[SITE_COUNT][RAIL_STEPS+1];
static Vec cubic(const Vec knots[4],float t){
    float s=1-t,a=s*s*s,b=3*s*s*t,c=3*s*t*t,d=t*t*t;
    return (Vec){a*knots[0].x+b*knots[1].x+c*knots[2].x+d*knots[3].x,
                 a*knots[0].y+b*knots[1].y+c*knots[2].y+d*knots[3].y,
                 a*knots[0].z+b*knots[1].z+c*knots[2].z+d*knots[3].z};
}
static void prepare_rails(void){
    for(int s=0;s<SITE_COUNT;s++){
        float length=0;
        for(int j=0;j<=RAIL_STEPS;j++){
            Vec point=cubic(flights[s].eye,(float)j/RAIL_STEPS);
            if(j){
                Vec last=rail[s][j-1];
                float dx=point.x-last.x,dy=point.y-last.y,dz=point.z-last.z;
                length+=sqrtf(dx*dx+dy*dy+dz*dz);
            }
            rail[s][j]=point;
            distanceOnRail[s][j]=length;
        }
        for(int j=1;j<=RAIL_STEPS;j++)distanceOnRail[s][j]/=length;
    }
}
/* Blend a shelf away from the lens only where the generated terrain intersects the route. */
static void open_passages(void){
    for(int s=0;s<SITE_COUNT;s++)for(int k=0;k<=RAIL_STEPS;k++){
        Vec camera=rail[s][k];
        int gx=(int)((camera.x+EXTENT)*.5f),gz=(int)((camera.z+EXTENT)*.5f);
        for(int j=gz-4;j<=gz+4;j++)for(int i=gx-4;i<=gx+4;i++){
            float dx=2*i-EXTENT-camera.x,dz=2*j-EXTENT-camera.z;
            float cap=camera.y-3.5f+fmaxf(0,dx*dx+dz*dz-6)*.60f;
            if(seabed[j][i]>cap*4)seabed[j][i]=(int16_t)floorf(cap*4);
        }
    }
}
static float distance_to_flight(Vec p){
    float best=1e8f;
    for(int s=0;s<SITE_COUNT;s++)for(int j=0;j<=RAIL_STEPS;j++){
        float dx=p.x-rail[s][j].x,dz=p.z-rail[s][j].z;
        best=fminf(best,dx*dx+dz*dz);
    }
    return sqrtf(best);
}
const Arch arches[3]={
    {-23,-9.5f,-16,.3f,4.2f,1.4f,1.2f},
    {0,-45,-34,1.57f,14,2.7f,1.2f},
    {-40,-45,44,1.57f,10,2.2f,1.3f}
};
static const float mesas[][6]={
    {-37,-42,-33,-22,9,-7},{-29,-15,-33,5,12,-14},{-36,11,-30,27,9,-5},
    {30,-45,35,-27,9,-9},{37,-20,31,-3,11,-16},{32,5,35,9,7,-8},
    {19,-15,19,-15,4.4f,-6},{6,-88,6,-88,10,-34},{0,-106,0,-106,8,0},
    {44,6,47,-8,7,-17},{33,29,37,12,9,-19},{74,14,74,14,8,-15},
    {-84,-34,-80,-12,10,-7},{-78,-6,-83,12,12,-13},{-82,18,-79,32,9,-8},
    {-17,62,-19,78,10,-6},{-21,84,-18,100,11,-9},{22,67,24,80,7,-11},
    {-16,122,34,122,9,-27}
};
Vec habitats[HABITAT_COUNT];
Prop props[PROP_COUNT];
int16_t seabed[GRID][GRID];
uint8_t groundTone[GROUND_SAMPLES][GROUND_SAMPLES];
static uint32_t seed;
static float random01(void){
    seed=1664525u*seed+1013904223u;
    return (float)(seed>>8)/16777216.f;
}
static float clamp(float n,float lo,float hi){return fminf(hi,fmaxf(lo,n));}
/* Deterministic value noise sampled only while building the heightfield. */
static float rock_corner(int x,int z){
    uint32_t n=(uint32_t)x*0x7feb352du+(uint32_t)z*0x846ca68bu+0x91e10da5u;
    n^=n>>16;n*=0x85ebca6bu;n^=n>>13;
    return (float)(n>>16)*(2.f/65535.f)-1.f;
}
static float rock_noise(float x,float z){
    int i=(int)floorf(x),j=(int)floorf(z);
    float u=x-i,v=z-j;u=u*u*(3-2*u);v=v*v*(3-2*v);
    float a=rock_corner(i,j),b=rock_corner(i+1,j);
    float c=rock_corner(i,j+1),d=rock_corner(i+1,j+1);
    return (1-v)*(a+(b-a)*u)+v*(c+(d-c)*u);
}
/* Distance to the rectangle swept by the hull planks, in ship coordinates. */
static float hull_distance(float along,float across){
    float dx=fmaxf(0,fmaxf(-16-along,along-17));
    float dz=fmaxf(0,fabsf(across)-5.6f);
    return sqrtf(dx*dx+dz*dz);
}
/* Bake contact into the tone map before lighting it. Keep the strongest
   footprint so overlapping colonies cannot turn a shelf into a black strip. */
static void bake_contacts(void){
    memset(groundTone,0,sizeof groundTone);
    for(int i=0;i<PROP_COUNT;i++){
        const Prop *p=&props[i];
        if(p->kind==BUBBLES || p->kind==EMPTY_PROP)continue;
        float r=p->kind==KELP?.9f+p->size*.035f:.8f+p->size*.45f;
        int strength=p->kind==BOULDER?19:14;
        float cx=p->p.x+EXTENT,cz=p->p.z+EXTENT;
        int loX=(int)clamp(floorf(cx-r),0,GROUND_SAMPLES-1),hiX=(int)clamp(ceilf(cx+r),0,GROUND_SAMPLES-1);
        int loZ=(int)clamp(floorf(cz-r),0,GROUND_SAMPLES-1),hiZ=(int)clamp(ceilf(cz+r),0,GROUND_SAMPLES-1);
        for(int j=loZ;j<=hiZ;j++)for(int k=loX;k<=hiX;k++){
            float dx=k-cx,dz=j-cz,weight=1-(dx*dx+dz*dz)/(r*r);
            if(weight<=0)continue;
            int shade=(int)(strength*weight*weight+.5f);
            if(shade>groundTone[j][k])groundTone[j][k]=(uint8_t)shade;
        }
    }
}

static void shade_ground(void){
    bake_contacts();
    for(int j=0;j<GROUND_SAMPLES;j++)for(int i=0;i<GROUND_SAMPLES;i++){
        float x=i-EXTENT,z=j-EXTENT,h=floor_at(x,z);
        float dx=(floor_at(x+1,z)-floor_at(x-1,z))*.5f;
        float dz=(floor_at(x,z+1)-floor_at(x,z-1))*.5f;
        float slope=dx*dx+dz*dz,rock=clamp(slope*.7f+(h+40)*.06f,0,1);
        float sun=(.55f+.52f*dx+.65f*dz)/sqrtf(1+slope);
        float grain=sinf(x*.95f+z*.28f+1.7f*sinf(z*.49f))+
                    .65f*cosf(.41f*x-1.1f*z);
        float tone=124-21*rock+sun*(16+10*rock)+grain*(3+3*rock);
        tone+=.47f*rock*stone_sample((int)(x*STONE_UNITS),(int)(h*STONE_UNITS),(int)(z*STONE_UNITS));
        /* Fine pale sediment settles on the grotto floor. The steep walls
           retain their rock material, even in the middle of the coral bed. */
        float gx=(x-5)/29.f,gz=(z-86)/39.f;
        float sand=clamp(1-gx*gx-gz*gz,0,1)/(1+.8f*slope);
        tone+=sand*(154+6*sun+3*grain-tone);
        groundTone[j][i]=(uint8_t)clamp(tone-groundTone[j][i],36,163);
    }
}
static float ground_slope(float x,float z,float radius){
    float h=floor_at(x,z),maxRise=0;
    const float dx[]={1,-1,0,0},dz[]={0,0,1,-1};
    for(int i=0;i<4;i++)
        maxRise=fmaxf(maxRise,fabsf(floor_at(x+radius*dx[i],z+radius*dz[i])-h));
    return maxRise/radius;
}
/* Keep a colony on a ledge wide enough for its footprint. At a vertical face,
   even a correctly anchored billboard is mostly buried or hangs over the edge. */
static Vec attachment(float x,float z,Species kind,float size){
    float radius=kind==KELP?.65f:fmaxf(.8f,size*.8f);
    float ax=x,az=z,best=ground_slope(x,z,radius);
    if(kind!=BUBBLES && best>.85f){
        for(int dz=-6;dz<=6;dz+=2)for(int dx=-6;dx<=6;dx+=2){
            float score=ground_slope(x+dx,z+dz,radius)+.012f*(dx*dx+dz*dz);
            if(score<best){best=score;ax=x+dx;az=z+dz;}
        }
    }
    return (Vec){ax,floor_at(ax,az)-.2f,az};
}
/* Give each ridge a rounded approach on every side. A quadratic falloff
   keeps high cliffs from lifting animals across the whole neighbouring bay.
   The separable maximum works on signed quarter-metre heights. */
static int16_t swimBed[GRID][GRID];

static void spread_ridge(int16_t line[GRID]){
    int16_t expanded[GRID];
    for(int i=0;i<GRID;i++){
        int best=line[i];
        int first=i>10?i-10:0,last=i+10<GRID?i+10:GRID-1;
        for(int j=first;j<=last;j++){
            int distance=i-j;
            int candidate=line[j]-3*distance*distance;
            if(candidate>best)best=candidate;
        }
        expanded[i]=best;
    }
    for(int i=0;i<GRID;i++)line[i]=expanded[i];
}

static void prepare_swim_bed(void){
    for(int j=0;j<GRID;j++){
        for(int i=0;i<GRID;i++)swimBed[j][i]=seabed[j][i];
        spread_ridge(swimBed[j]);
    }
    int16_t column[GRID];
    for(int i=0;i<GRID;i++){
        for(int j=0;j<GRID;j++)column[j]=swimBed[j][i];
        spread_ridge(column);
        for(int j=0;j<GRID;j++)swimBed[j][i]=column[j];
    }
}

/* Central quadratic B-spline: a continuous tangent through grid boundaries.
   Return the height and its derivative in the horizontal travel direction. */
static void swim_weights(float t,float w[3]){
    w[0]=.5f*(1-t)*(1-t);
    w[2]=.5f*t*t;
    w[1]=1-w[0]-w[2];
}

void guide_swimmer(Vec *p,Vec *velocity,float clearance){
    float gx=clamp((p->x+EXTENT)*.5f+.5f,1,GRID-1.001f);
    float gz=clamp((p->z+EXTENT)*.5f+.5f,1,GRID-1.001f);
    int i=(int)gx,j=(int)gz;
    float u=gx-i,v=gz-j,wx[3],wz[3];
    swim_weights(u,wx);swim_weights(v,wz);
    float row[3],dx=0;
    for(int k=0;k<3;k++){
        const int16_t *a=&swimBed[j+k-1][i-1];
        row[k]=a[0]*wx[0]+a[1]*wx[1]+a[2]*wx[2];
        dx+=wz[k]*((1-u)*(a[1]-a[0])+u*(a[2]-a[1]));
    }
    float bed=.25f*(row[0]*wz[0]+row[1]*wz[1]+row[2]*wz[2]);
    float dz=(1-v)*(row[1]-row[0])+v*(row[2]-row[1]);
    float rise=.125f*(dx*velocity->x+dz*velocity->z);
    /* Smooth maximum over two metres. Its derivative steers the nose into
       the climb and hands the original swimming motion back over open water. */
    float target=bed+clearance;
    float blend=clamp(.5f+.25f*(target-p->y),0,1);
    p->y+=(target-p->y)*blend+2*blend*(1-blend);
    velocity->y+=(rise-velocity->y)*blend;
}

void generate_world(void){
    seed=72561;
    for(int j=0;j<GRID;j++) for(int i=0;i<GRID;i++){
        float x=i*2-EXTENT,z=j*2-EXTENT;
        float broad=sinf(x*.072f+cosf(z*.052f))*2.2f+cosf(z*.093f+x*.025f)*1.7f;
        float rock=sinf(.39f*x+.14f*z)+.55f*cosf(.52f*z-.16f*x);
        float base=-44+broad+rock*.32f, h=base;
        float block=rock_noise(x*.135f,z*.135f);
        float chip=rock_noise((x+.45f*z)*.31f,(z-.45f*x)*.31f);
        float fracture=2.15f*block+.83f*chip;
        float crown=2.6f*block+1.08f*chip;
        for(unsigned k=0;k<sizeof mesas/sizeof mesas[0];k++){
            const float *m=mesas[k];
            float dx=m[2]-m[0],dz=m[3]-m[1];
            float u=clamp(((x-m[0])*dx+(z-m[1])*dz)/(dx*dx+dz*dz+.01f),0,1);
            float cx=x-m[0]-u*dx,cz=z-m[1]-u*dz;
            if(fabsf(cx)>m[4]+14 || fabsf(cz)>m[4]+14)continue;
            float edge=sqrtf(cx*cx+cz*cz)+rock*.75f+fracture-m[4];
            float v=clamp(.73f-edge*.10f,0,1);
            h=fmaxf(h,base+(m[5]+broad*.45f+rock+crown-base)*v*v*(3-2*v));
        }
        if(h>base+2){
            float stratum=(h+block*1.9f+chip*.55f)/4.8f;
            float f=stratum-floorf(stratum);
            float shelf=clamp((f-.37f)*3.8f,0,1);
            h+=2.65f*(shelf-f)*clamp((h-base-2)*.19f,0,1);
        }
        /* A sandbank cradles the keel. Keep the approach open toward the deck. */
        float along,across;
        wreck_coordinates(x,z,&along,&across);
        float bank=clamp(1-(along*along/550+across*across/125),0,1);
        float clearing=clamp(1-(along*along/970+across*across/300),0,1);
        h=fmaxf(h-clearing*6,base+bank*(17-.12f*along));
        /* Leave a continuous sand sill under the keel. Recess the surrounding
           reef beyond the hull too, so an oblique depth slice cannot sample
           the high shoulder behind the ship and bring it through a plank. */
        float shore=hull_distance(along,across);
        if(shore<20){
            float keel=WRECK_Y+.17f*fabsf(along)-.12f*along-6.86f;
            float sill=fminf(keel,-30.f)-1.3f*fminf(8,fabsf(across))+.12f*block;
            float blend=clamp((shore-6)/14,0,1);
            blend=blend*blend*(3-2*blend);
            h-=(1-blend)*fmaxf(0,h-sill);
        }
        seabed[j][i]=(int16_t)(h*4);
    }
    prepare_rails();
    open_passages();
    prepare_swim_bed();
    const float beds[][4]={{0,-86,40,0},{72,-26,26,0},{-32,-5,15,0},
        {-60,5,20,1},{6,84,29,2},{5,-4,20,0},{57,14,23,0},{-40,44,24,0}};
    for(int i=0;i<WILD_PROPS;i++){
        const float *bed=beds[i%8];
        float angle=random01()*6.2832f,r=sqrtf(random01())*bed[2];
        float x=bed[0]+cosf(angle)*r,z=bed[1]+sinf(angle)*r;
        if(i%5==0){x=random01()*290-145;z=random01()*290-145;}
        /* Clear the foreground of each authored viewpoint and leave kelp as a frame. */
        for(int k=0;k<SITE_COUNT;k++){
            float dx=x-sites[k].x,dz=z-sites[k].z;
            float fx=sites[k].aimX-sites[k].x,fz=sites[k].aimZ-sites[k].z;
            float inv=1/sqrtf(fx*fx+fz*fz);
            float ahead=(dx*fx+dz*fz)*inv,across=(dx*fz-dz*fx)*inv;
            if(ahead>-3 && ahead<22 && fabsf(across)<4.5f+ahead*.17f){
                float side=across<0?-1:1;
                x+=side*9*fz*inv;z-=side*9*fx*inv;
            }
        }
        if(i%8==6 && fabsf(x-59)<15 && z>0 && z<38) {
            x+=x<59?-15:15;
        }
        float pick=random01();
        Species kind=pick<.10f?BUBBLES:pick<.27f?BOULDER:(Species)(int)bed[3];
        float size=kind==KELP?5+random01()*17:1.2f+random01()*3.5f;
        Vec anchor=attachment(x,z,kind,size);
        if(kind==KELP){
            /* Low kelp in the sightline, with tall fronds framing the flukes beyond the tips. */
            float dx=anchor.x+5,dz=anchor.z+4;
            float span=dx*.967f+dz*.255f,depth=-dx*.255f+dz*.967f;
            if(depth>-18 && depth<22){
                float margin=fabsf(span-1.f)-(11.f-.40f*depth);
                size=fminf(size,1.65f+fmaxf(0,margin)*3.6f);
            }
        }
        float clearance=distance_to_flight(anchor);
        size=fminf(size,1.25f+fmaxf(0,clearance-2.8f)*2);
        props[i]=(Prop){anchor,size,random01()*6.2832f,kind};
    }
    for(int site=0;site<SITE_COUNT;site++)for(int k=0;k<20;k++){
        float t=(k/2+.12f+.64f*random01())/10;
        Vec a=cubic(flights[site].eye,t),b=cubic(flights[site].eye,t+.02f);
        float dx=b.x-a.x,dz=b.z-a.z,side=(k%2?1.f:-1.f)*(6.5f+3*random01())/hypotf(dx,dz);
        float x=a.x+dz*side,z=a.z-dx*side;
        Species kind=site==3?SPONGE:site==5?CORAL:KELP;
        if(k%7==0)kind=BUBBLES;
        if(site==0 && kind==KELP)kind=k%3?BOULDER:CORAL;
        float scale=kind==KELP?6+5*random01():1.2f+1.5f*random01();
        Vec p=attachment(x,z,kind,scale);
        scale=fminf(scale,1.25f+fmaxf(0,distance_to_flight(p)-2.8f)*2);
        props[WILD_PROPS+site*20+k]=(Prop){p,scale,random01()*6.2832f,kind};
    }
    /* Placement can move a colony onto a neighbouring ledge. Check its final
       footprint before shading the floor or indexing visible props. */
    for(int i=0;i<PROP_COUNT;i++){
        Prop *p=&props[i];
        /* Thin the wild clusters and the planted borders before both the
           contact bake and the spatial index consume the final population. */
        if((i<WILD_PROPS && (i%3==1 || (p->kind==CORAL && i%3==2))) ||
           (i>=WILD_PROPS && p->kind==CORAL && i%3==1)){
            p->kind=EMPTY_PROP;
            continue;
        }
        if(p->kind==BUBBLES)continue;
        float along,across;
        wreck_coordinates(p->p.x,p->p.z,&along,&across);
        float radius=p->kind==KELP?.46f*p->size+.6f:1.1f*p->size;
        if(hull_distance(along,across)<radius+1)p->kind=EMPTY_PROP;
    }
    for(int i=0;i<HABITAT_COUNT;i++){
        Diver lens={0};
        demo_pose(&lens,i/2,SHOT_SECONDS*(i%2?.70f:.22f));
        float side=i%2?3.9f:-3.3f,ahead=i%2?12:15;
        float x=lens.p.x+ahead*sinf(lens.yaw)+side*cosf(lens.yaw);
        float z=lens.p.z+ahead*cosf(lens.yaw)-side*sinf(lens.yaw);
        float y=lens.p.y+ahead*tanf(lens.pitch)+(i%2?1.5f:-1.f);
        habitats[i]=(Vec){x,clamp(y,floor_at(x,z)+2.2f,-2),z};
    }
    bake_stone();
    shade_ground();
}
float floor_at(float x,float z){
    float gx=clamp((x+EXTENT)*.5f,0,GRID-1.001f),gz=clamp((z+EXTENT)*.5f,0,GRID-1.001f);
    int i=(int)gx,j=(int)gz;
    float u=gx-i,v=gz-j;
    return .25f*((1-v)*((1-u)*seabed[j][i]+u*seabed[j][i+1])+v*((1-u)*seabed[j+1][i]+u*seabed[j+1][i+1]));
}
void visit(Diver *d,int site){
    const Site *s=&sites[site];
    d->p=(Vec){s->x,fminf(-2,floor_at(s->x,s->z)+s->altitude),s->z};
    d->yaw=atan2f(s->aimX-s->x,s->aimZ-s->z);
    d->pitch=s->pitch; d->velocity=0;
}
static int arch_clear(Vec p){
    for(int i=0;i<3;i++){
        const Arch *a=&arches[i];
        float dx=p.x-a->x,dz=p.z-a->z;
        if(fabsf(dx)>20 || fabsf(dz)>20)continue;
        float c=cosf(a->yaw),s=sinf(a->yaw);
        float across=dx*c+dz*s,normal=-dx*s+dz*c,up=(p.y-a->y)/a->stretch;
        float ring=sqrtf(across*across+up*up)-a->radius;
        if(ring*ring+normal*normal<(a->tube+.55f)*(a->tube+.55f))return 0;
    }
    return 1;
}
void swim(Diver *d,float forward,float steer,float rise,float look,float dt){
    d->time=fmodf(d->time+dt,3600);
    d->pitch=clamp(d->pitch+look,-.86f,.82f);
    d->yaw=remainderf(d->yaw+steer*dt*1.4f,6.283185f);
    d->velocity+=(forward*10.f-d->velocity)*fminf(1,dt*4);
    float travel=d->velocity*dt,flat=travel*cosf(d->pitch);
    Vec p=d->p;
    p.y=clamp(p.y+travel*sinf(d->pitch)+rise*dt*7,floor_at(p.x,p.z)+1.4f,-.85f);
    if(arch_clear(p))d->p.y=p.y;
    for(int axis=0;axis<2;axis++){
        p=d->p;
        if(axis==0)p.x=clamp(p.x+flat*sinf(d->yaw),-153,153);
        else p.z=clamp(p.z+flat*cosf(d->yaw),-153,153);
        float floor=floor_at(p.x,p.z)+1.4f;
        if(floor>p.y+.5f || floor>-.85f)continue;
        p.y=fmaxf(floor,p.y);
        if(arch_clear(p))d->p=p;
    }
}

void demo_pose(Diver *d,int site,float elapsed){
    float distance=clamp(elapsed/SHOT_SECONDS,0,1);
    const float *arc=distanceOnRail[site];
    int j=1;
    while(j<RAIL_STEPS && arc[j]<distance)j++;
    float t=(j-1+(distance-arc[j-1])/(arc[j]-arc[j-1]))/RAIL_STEPS;
    d->p=cubic(flights[site].eye,t);
    Vec look=cubic(flights[site].look,t);
    float dx=look.x-d->p.x,dz=look.z-d->p.z;
    d->yaw=atan2f(dx,dz);
    d->pitch=atan2f(look.y-d->p.y,sqrtf(dx*dx+dz*dz));
    d->velocity=0;
}
