#ifndef GARDEN_H
#define GARDEN_H
#include <stdint.h>
enum { SHOT_SECONDS=16, WIDTH=200, HEIGHT=120, GRID=161, EXTENT=160, SITE_COUNT=6, WILD_PROPS=760, PROP_COUNT=WILD_PROPS+20*SITE_COUNT };
typedef struct { float x,y,z; } Vec;
typedef struct { Vec p; float yaw, pitch, time, velocity; } Diver;
typedef struct { const char *name; float x,z, aimX,aimZ, altitude,pitch; } Site;
typedef enum { KELP, SPONGE, CORAL, BOULDER, BUBBLES } Species;
typedef struct { Vec p; float size, phase; Species kind; } Prop;
typedef struct { float x,y,z,yaw,radius,tube,stretch; } Arch;
extern const Site sites[SITE_COUNT];
extern const Arch arches[3];
enum { HABITAT_COUNT=2*SITE_COUNT };
extern Vec habitats[HABITAT_COUNT];
extern Prop props[PROP_COUNT];
extern int16_t seabed[GRID][GRID]; /* quarter metres, indexed by (coordinate + EXTENT) / 2 */
enum { GROUND_SAMPLES=EXTENT*2+1 };
extern uint8_t groundTone[GROUND_SAMPLES][GROUND_SAMPLES];
void generate_world(void);
float floor_at(float x,float z);
void visit(Diver *d,int site);
void swim(Diver *d,float forward,float steer,float rise,float look,float dt);
void demo_pose(Diver *d,int site,float elapsed);
enum { DETAIL_LOW, DETAIL_MIXED, DETAIL_FULL };
void set_render_detail(int detail);
void prepare_renderer(void);
void render(const Diver *d, uint8_t *frame, int stride);
#endif
