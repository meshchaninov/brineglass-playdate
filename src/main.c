#include "pd_api.h"
#include "garden.h"

#include <math.h>
#include <string.h>

static PlaydateAPI *pd;
static Diver diver;
static PDMenuItem *soundItem,*fpsItem,*tourItem;
static PDSynth *voice,*bell;
static int chart,selected=1,showFPS,sound=1,phrase;
static float audioTimer=1,caption=2;
static const int route[SITE_COUNT]={1,4,0,3,5,2};
static int touring=1,shot,detail=DETAIL_LOW;
static float shotTime;
static int stable,promotionDelay=90;
static float average;
static void camera_cut(void){
    /* The previous view's cost says little about newly visible geometry. */
    detail=DETAIL_LOW;set_render_detail(detail);
    stable=0;promotionDelay=18;
}
/* Hysteresis uses actual render time, independent of the display's frame limiter. */
static void budget(uint32_t ms){
    if(!stable)average=(float)ms;
    else average+=((float)ms-average)*.125f;
    if(stable<180)stable++;
    /* Medium resolution can use a little more of the 33 ms refresh
       interval before falling all the way back to 100 x 60. */
    int peakLimit=detail==DETAIL_MIXED?36:35;
    float meanLimit=detail==DETAIL_MIXED?31.4f:30.f;
    if(detail>DETAIL_LOW && (ms>=peakLimit || (stable>=4 && average>meanLimit))){
        set_render_detail(--detail);
        stable=0;
    }else if(detail<DETAIL_FULL && stable>=promotionDelay && ms<=26 &&
             average<(detail==DETAIL_LOW?25.0f:22.0f)){
        set_render_detail(++detail);
        stable=0;promotionDelay=40;
    }
}
#ifdef TARGET_SIMULATOR
#define PLATFORM "sim"
#else
#define PLATFORM "device"
#endif
static unsigned timing[101],samples,totalMs,peakRenderMs,totalWork,qualityFrames[3],overBudget;
static void report_frame(uint32_t delta,uint32_t work){
    if(!touring)return;
    ++timing[delta<100?delta:100];++samples;totalMs+=delta;totalWork+=work;
    qualityFrames[detail]++;overBudget+=work>33;
    if(work>peakRenderMs)peakRenderMs=work;
}
static void finish_shot(void){
    if(samples && totalMs){
        unsigned p95=0,count=0;
        while(p95<100 && count+timing[p95]<(samples*95+99)/100){count+=timing[p95];p95++;}
        pd->system->logToConsole(PLATFORM " %s | %u frames / %u ms | p95 interval %u ms | peak render %u ms | total render %u ms | 2x/3x/4x %u/%u/%u | over 33 ms %u",
            sites[route[shot]].name,samples,totalMs,p95,peakRenderMs,totalWork,qualityFrames[2],qualityFrames[1],qualityFrames[0],overBudget);
    }
    memset(qualityFrames,0,sizeof qualityFrames);overBudget=0;
    memset(timing,0,sizeof timing);samples=totalMs=peakRenderMs=totalWork=0;
}
static void start_shot(int index,float elapsed){
    finish_shot();camera_cut();
    shot=(index+SITE_COUNT)%SITE_COUNT;
    shotTime=elapsed;selected=route[shot];caption=1.5f;
}
static void tour_site(int site){
    for(int i=0;i<SITE_COUNT;i++)if(route[i]==site){start_shot(i,0);return;}
}
static void change_tour(void *context){
    (void)context;
    touring=pd->system->getMenuItemValue(tourItem);
    if(touring){chart=0;tour_site(selected);}
}
static uint32_t previous;
static void silence(void){
    pd->sound->synth->stop(voice);pd->sound->synth->stop(bell);
}
static void change_sound(void *context){
    (void)context;sound=pd->system->getMenuItemValue(soundItem);
    if(!sound)silence();
}
static void change_stats(void *context){
    (void)context;showFPS=pd->system->getMenuItemValue(fpsItem);
}
static void ambience(float dt){
    if(!sound)return;
    audioTimer-=dt;
    if(audioTimer>0)return;
    const float tones[]={130.81f,196.f,164.81f,146.83f,220.f,174.61f};
    if(phrase%3==0){
        pd->sound->synth->playNote(voice,tones[phrase]/2,.21f,2.3f,0);
        pd->sound->synth->playNote(bell,tones[phrase]*1.5f,.07f,1.7f,0);
    }else pd->sound->synth->playNote(bell,tones[phrase]*2,.09f,.65f,0);
    audioTimer=3.1f+(phrase%3)*.57f;phrase=(phrase+1)%6;
}
static void text_at(const char *text,int x,int y){
    pd->graphics->drawText(text,strlen(text),kASCIIEncoding,x,y);
}
static int chart_x(float x){return 117+(int)(x*.65f);}
static int chart_y(float z){return 115-(int)(z*.63f);}
static void draw_chart(void){
    const struct playdate_graphics *g=pd->graphics;
    g->clear(kColorWhite);
    /* Bathymetric contours are sampled from the same floor used for swimming. */
    for(int y=13;y<215;y+=2)for(int x=7;x<227;x+=2){
        float wx=(x-117)/.65f,wz=(115-y)/.63f;
        int level=(int)floorf(floor_at(wx,wz)/5);
        if(level!=(int)floorf(floor_at(wx+3.08f,wz)/5) || level!=(int)floorf(floor_at(wx,wz+3.18f)/5))
            g->fillRect(x,y,2,1,kColorBlack);
    }
    g->drawRect(4,8,224,212,kColorBlack);
    if(touring){
        Diver point={0};demo_pose(&point,selected,0);
        for(int step=1;step<=12;step++){
            Vec last=point.p;
            demo_pose(&point,selected,SHOT_SECONDS*step/12.f);
            g->drawLine(chart_x(last.x),chart_y(last.z),chart_x(point.p.x),chart_y(point.p.z),2,kColorBlack);
        }
    }
    for(int i=0;i<SITE_COUNT;i++){
        int x=chart_x(sites[i].x),y=chart_y(sites[i].z);
        g->fillEllipse(x-4,y-4,9,9,0,360,kColorWhite);
        g->fillEllipse(x-2,y-2,5,5,0,360,kColorBlack);
        if(i==selected)g->drawEllipse(x-7,y-7,15,15,2,0,360,kColorBlack);
    }
    int x=chart_x(diver.p.x),y=chart_y(diver.p.z);
    int nx=(int)(sinf(diver.yaw)*8),ny=-(int)(cosf(diver.yaw)*8);
    g->drawLine(x,y,x+nx,y+ny,3,kColorWhite);g->drawLine(x,y,x+nx,y+ny,1,kColorBlack);
    text_at("Dive sites",237,13);
    for(int i=0;i<SITE_COUNT;i++){
        int row=48+i*26;
        if(i==selected){g->fillRect(232,row-3,164,24,kColorBlack);g->setDrawMode(kDrawModeFillWhite);}
        text_at(sites[i].name,239,row);
        g->setDrawMode(kDrawModeCopy);
    }
    text_at(touring?"Up/down select     A tour     B return":"Up/down select     A dive     B return",12,222);
}
static int update(void *ctx){
    (void)ctx;
    uint32_t now=pd->system->getCurrentTimeMilliseconds();
    uint32_t delta=now-previous;
    float dt=fminf(delta*.001f,.10f);previous=now;
    PDButtons held,press;pd->system->getButtonState(&held,&press,NULL);
    float crank=pd->system->getCrankChange();
    if(press&kButtonB){
        chart=!chart;
        if(chart){draw_chart();return 1;}
    }else if(chart){
        if(press&(kButtonDown|kButtonRight))selected=(selected+1)%SITE_COUNT;
        if(press&(kButtonUp|kButtonLeft))selected=(selected+SITE_COUNT-1)%SITE_COUNT;
        if(press&kButtonA){
            if(touring)tour_site(selected);else {visit(&diver,selected);camera_cut();}
            chart=0;caption=2;
        }else{
            if(press){draw_chart();return 1;}
            return 0;
        }
    }else if(touring){
        int next=!!(press&(kButtonRight|kButtonDown))-!!(press&(kButtonLeft|kButtonUp));
        if(next)start_shot(shot+next,0);
        else if(press&kButtonA){
            touring=0;pd->system->setMenuItemValue(tourItem,0);
        }
    }
    int move=!!(held&kButtonUp)-!!(held&kButtonDown);
    int steer=!!(held&kButtonRight)-!!(held&kButtonLeft);
    int vertical=!!(held&kButtonA);
    if(vertical && move && pd->system->isCrankDocked())diver.pitch*=fmaxf(0,1-3*dt);
    if(touring){
        shotTime+=dt;
        if(shotTime>=SHOT_SECONDS)start_shot(shot+1,shotTime-SHOT_SECONDS);
        selected=route[shot];
        demo_pose(&diver,selected,shotTime);
        diver.time=shot*SHOT_SECONDS+shotTime;
    }else swim(&diver,vertical?0:move,steer,vertical?move:0,crank*.006f,dt);
    uint32_t beforeRender=pd->system->getCurrentTimeMilliseconds();
    uint8_t *frame=pd->graphics->getFrame();
    render(&diver,frame,LCD_ROWSIZE);
    pd->graphics->markUpdatedRows(0,LCD_ROWS-1);
    if(caption>0){
        pd->graphics->fillRect(6,214,154,22,kColorWhite);
        text_at(sites[selected].name,12,216);
        if(touring){
            pd->graphics->fillRect(201,214,193,22,kColorWhite);
            text_at("<> View   A Swim   B Map",205,216);
        }else{
            pd->graphics->fillRect(318,214,76,22,kColorWhite);
            text_at("B  Chart",323,216);
        }
        caption-=dt;
    }
    if(showFPS)pd->system->drawFPS(3,3);
    ambience(dt);
    uint32_t work=pd->system->getCurrentTimeMilliseconds()-beforeRender;
    report_frame(delta,work);
    budget(work);
    return 1;
}
int eventHandler(PlaydateAPI *api,PDSystemEvent event,uint32_t arg){
    (void)arg;
    if(event==kEventInit){
        pd=api;generate_world();set_render_detail(detail);prepare_renderer();visit(&diver,selected);
        pd->display->setRefreshRate(30);
        voice=pd->sound->synth->newSynth();bell=pd->sound->synth->newSynth();
        pd->sound->synth->setAttackTime(voice,.85f);pd->sound->synth->setReleaseTime(voice,1.5f);
        pd->sound->synth->setAttackTime(bell,.21f);pd->sound->synth->setReleaseTime(bell,1.3f);
        tourItem=pd->system->addCheckmarkMenuItem("Demo tour",1,change_tour,NULL);
        soundItem=pd->system->addCheckmarkMenuItem("Sound",sound,change_sound,NULL);
        fpsItem=pd->system->addCheckmarkMenuItem("Show FPS",0,change_stats,NULL);
        previous=pd->system->getCurrentTimeMilliseconds();
        pd->system->setUpdateCallback(update,NULL);
    }else if(event==kEventPause || event==kEventLock)silence();
    else if(event==kEventResume || event==kEventUnlock)previous=pd->system->getCurrentTimeMilliseconds();
    return 0;
}
