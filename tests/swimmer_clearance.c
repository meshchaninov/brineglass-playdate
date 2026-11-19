/* Recover the world vertices submitted by the real animal paths, before the
   terrain mask can conceal an intersection. Both fish LODs are exercised. */
#include "render.c"
#include <stdio.h>

static int check_point(Vec p){
    return p.y > floor_at(p.x,p.z) && p.y < 0;
}

static Vec world_vertex(Screen s){
    float z=32760.f/s.iz;
    float x=(s.x/meshScale-100)*z/focal;
    return (Vec){view->p.x + x*headingZ + z*headingX,
                 view->p.y + (horizon-s.y/meshScale)*z/focal,
                 view->p.z - x*headingX + z*headingZ};
}

int main(void){
    const int route[]={1,4,0,3,5,2};
    generate_world();
    prepare_renderer();
    memset(roof,LCD_H,sizeof roof);
    memset(roofMin,LCD_H,sizeof roofMin);
    memset(roofMax,LCD_H,sizeof roofMax);
    for(int shot=0;shot<SITE_COUNT;shot++){
        int facets=0;
        for(int n=0;n<SHOT_SECONDS*30;n++){
            Diver d={0};
            demo_pose(&d,route[shot],n/30.f);
            d.time=shot*SHOT_SECONDS+n/30.f;
            view=&d;
            headingX=sinf(d.yaw);headingZ=cosf(d.yaw);
            horizon=60+focal*tanf(d.pitch)+sinf(d.time*1.3f)*.65f;
            memset(bucketHead,0xff,sizeof bucketHead);
            commandCount=0;
            animals();reef_life();encounters_at_reef();
            for(int i=0;i<commandCount;i++){
                const Command *cmd=&commands[i];
                if(cmd->type!=TRI_CMD)continue;
                facets++;
                Vec center={0};
                for(int v=0;v<3;v++){
                    Vec p=world_vertex(cmd->v[v]);
                    if(!check_point(p)){
                        fprintf(stderr,"Swimmer intersects terrain or surface at %s %.2fs (%.2f %.2f %.2f)\n",
                                sites[route[shot]].name,n/30.f,p.x,p.y,p.z);
                        return 1;
                    }
                    center.x+=p.x/3;center.y+=p.y/3;center.z+=p.z/3;
                }
                if(!check_point(center)){
                    fprintf(stderr,"Submerged facet crosses terrain or surface at %s %.2fs\n",sites[route[shot]].name,n/30.f);
                    return 1;
                }
            }
        }
        if(!facets){fprintf(stderr,"No swimmers in %s\n",sites[route[shot]].name);return 1;}
    }
    puts("Swimmer geometry clears the seabed and surface across all six flybys.");
    return 0;
}
