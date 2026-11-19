/* Keep the keel embedded, with the exposed hull and rigging clear on approach. */
#include "render.c"
#include <stdio.h>

enum { ROW_BYTES=LCD_W/8, SUPPORT=128, UPPER_HULL=255 };
typedef uint8_t Frame[LCD_H*ROW_BYTES];
static Frame scene,clipped,unobstructed;

static int above_bilge(Vec v){
    float along,across;
    wreck_coordinates(v.x,v.z,&along,&across);
    return v.y >= ship(along,.17f*fabsf(along)-4.1f,across).y;
}

static int mark_wreck(void){
    /* Identify the original hull facets before the appended outcrops. */
    static Model hull;
    Diver reference={0};
    visit(&reference,1);view=&reference;
    building=&hull;wreck();building=NULL;
    Model *site=&landmarks[0];
    if(hull.overflow || site->overflow || hull.nfaces>=site->nfaces)return 0;
    for(int i=0;i<site->nfaces;i++){
        Facet *f=&site->faces[i];
        if(i>=hull.nfaces){f->tone=SUPPORT;continue;}
        f->tone=above_bilge(site->positions[f->a]) && above_bilge(site->positions[f->b]) &&
                above_bilge(site->positions[f->c]) ? UPPER_HULL : 0;
    }
    return 1;
}

static void ship_mask(Frame output,int includeRocks){
    memset(output,0,sizeof(Frame));lcd=output;
    memset(bucketHead,0xff,sizeof bucketHead);commandCount=0;
    draw_model(&landmarks[0]);
    for(int b=0;b<BUCKETS;b++)for(int i=bucketHead[b];i>=0;i=commands[i].next){
        const Command *f=&commands[i];
        if(!includeRocks && f->tone==SUPPORT)continue;
        raster_triangle(f->v[0],f->v[1],f->v[2],f->tone==UPPER_HULL?255:0);
    }
}

int main(void){
    generate_world();prepare_renderer();
    for(float along=-14;along<=14;along+=.5f){
        Vec keel=ship(along,.17f*fabsf(along)-6.86f,0);
        float burial=floor_at(keel.x,keel.z)-keel.y;
        if(burial<.2f || burial>1.8f){
            fprintf(stderr,"Keel burial %.2f m at station %.1f\n",burial,along);return 1;
        }
    }
    if(!mark_wreck()){fputs("Incomplete wreck model\n",stderr);return 1;}
    memset(fogForKey,0,sizeof fogForKey);
    for(int n=0;n<(int)(SHOT_SECONDS*30);n++){
        Diver d={0};d.time=n/30.f;demo_pose(&d,1,d.time);
        render(&d,scene,ROW_BYTES);
        ship_mask(clipped,1);
        memset(roof,LCD_H,sizeof roof);
        memset(roofMin,LCD_H,sizeof roofMin);memset(roofMax,LCD_H,sizeof roofMax);
        ship_mask(unobstructed,0);
        if(!memchr(unobstructed,255,sizeof(Frame)))return 1;
        for(size_t i=0;i<sizeof(Frame);i++)if(unobstructed[i]&~clipped[i]){
            fprintf(stderr,"Shoal obscures upper hull at %.2fs, x=%zu y=%zu\n",d.time,8*(i%ROW_BYTES),i/ROW_BYTES);
            return 1;
        }
    }
    puts("Keel rests on the shoal; upper planking, deck and rigging remain clear throughout the approach.");
    return 0;
}
