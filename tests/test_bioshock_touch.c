/* BioShock live-movie input context against bounded synthetic UE2 metadata. */
#define _GNU_SOURCE
#ifdef _WIN32
#include "touch_host_win.h"
#include "bioshock_guest_state_fixture.h"
#define mmap(address,length,protection,flags,fd,offset) VirtualAlloc((address),(length),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)
#else
#include <sys/mman.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
char *void_wstr_utf8(uint32_t va);
uint32_t void_gstrdup(const char *text);
#include "../android/host/bioshock_touch.c"
static int relayouts, picker_calls, picker_reveals;
static uint64_t now_ns;
uint64_t void_monotonic_ns(void) { return now_ns; }
void void_bioshock_flash_relayout(void) { ++relayouts; }
void void_bioshock_movie_call(uint32_t movie, const char *method, const char *key, const char *value)
{ (void)movie; (void)key; (void)value; ++picker_calls;
  if (!strcmp(method, "PortPickerReveal")) ++picker_reveals; }

enum { TEST_SIZE=0x03000000, ENGINE=0x100000, CLIENT=0x110000, VIEWPORT=0x120000,
       PLAYER=0x130000, PAWN=0x140000, GUI=0x150000, HUD=0x160000, PAUSE=0x170000,
       MOVIES=0x180000, SCENE=0x190000, RANGES=0x1A0000, VTABLE=0x1B0000, AXIS_ARGS=0x1C0000, ROOT_VTABLE=0x1D0000 };
static uint32_t next_class=0x200000, next_property=0x300000, next_name=0x400000, name_index;
static int owned=1;
static int axis_calls,allocations,change_scene,fail_allocation;
static float brightness_bounds[4] = {50, 5, 100, 10};
static float settings_row = NAN, settings_page, settings_frame = 9, settings_section = 8;
static float settings_mouse[2] = {1032, 416}, settings_modal = 1;
static float settings_hit[4] = {781.525f, 364.8999f, 1061.775f, 448.9012f};
static int variable_failure;
static void_regs live_regs;
#ifdef RECOMP_X87_WINDOW
double g_st_win[RECOMP_X87_SLOTS];
int64_t g_st_i64_win[RECOMP_X87_SLOTS];
int g_fp_sp;
#endif

int void_machine_is_owned(void) { return owned; }
static int test_player_fov=75, test_weapon_fov=60;
int void_bioshock_fov(int foreground) { return foreground ? test_weapon_fov : test_player_fov; }
int void_vm_readable(uint32_t address,uint32_t size)
{ return address && size && address<=TEST_SIZE-size; }
int void_vm_writable(uint32_t address,uint32_t size) { return void_vm_readable(address,size); }
void void_log(const char *format,...) { (void)format; }
void void_bioshock_flash_trace(void) {}
void void_regs_save(void_regs *out) { memcpy(out,&live_regs,sizeof *out); }
void void_regs_load(const void_regs *in) { memcpy(&live_regs,in,sizeof *in); }
uint32_t void_galloc(uint32_t bytes) { assert(bytes==4 && !allocations);if(fail_allocation)return 0;++allocations;return AXIS_ARGS; }
uint32_t void_gstrdup(const char *text) {
    assert(strlen(text)<64 && !allocations);if(fail_allocation)return 0;
    strcpy(GPTR(AXIS_ARGS),text);++allocations;return AXIS_ARGS;
}
void void_gfree(uint32_t address) { assert(address==AXIS_ARGS && allocations==1);--allocations; }
uint32_t void_call_guest_this(uint32_t address,uint32_t self,int nargs,const uint32_t *args)
{
    if(address==0x10BF6690u){
        assert(owned&&self==SCENE&&nargs==1&&args[0]==AXIS_ARGS);
        const char *paths[]={"_xmouse","_ymouse","_width","_height"};int found=-1;
        for(int i=0;i<4;++i)if(strstr(GPTR(args[0]),paths[i]))found=i;
        float value = NAN;
        if(!strcmp(GPTR(args[0]),"selectorPos"))value=settings_row;
        else if(!strcmp(GPTR(args[0]),"PortPage"))value=settings_page;
        else if(!strcmp(GPTR(args[0]),"_currentframe"))value=settings_frame;
        else if(!strcmp(GPTR(args[0]),"PortSectionFrame"))value=settings_section;
        else if(!strcmp(GPTR(args[0]),"ConfirmBox._currentframe"))value=settings_modal;
        else if(!strcmp(GPTR(args[0]),"_xmouse"))value=settings_mouse[0];
        else if(!strcmp(GPTR(args[0]),"_ymouse"))value=settings_mouse[1];
        else if(!strncmp(GPTR(args[0]),"PortFovSlider",sizeof("PortFovSlider")-1)) {
            const char *hit_fields[]={"left","top","right","bottom"};
            int row;char field[16];assert(sscanf(GPTR(args[0]),"PortFovSlider%d_%15s",&row,field)==2);
            assert(row==3||row==7);
            float hit[]={row==3?260:720,452,row==3?560:1020,506};
            for(int i=0;i<4;++i)if(!strcmp(field,hit_fields[i]))value=hit[i];
        }
        else if(!strncmp(GPTR(args[0]),"PortSlider",sizeof("PortSlider")-1)) {
            const char *hit_fields[]={"left","top","right","bottom"};
            assert(!strchr(GPTR(args[0]),'.')); // Original get_variable cannot traverse plain AS objects.
            int frame,row;char field[16];assert(sscanf(GPTR(args[0]),"PortSlider%d_%d_%15s",&frame,&row,field)==3);
            assert(frame==(int)settings_frame);
            float hit[4];memcpy(hit,settings_hit,sizeof hit);
            if((frame==7&&row==9)||(frame==9&&row==3)){hit[1]+=100;hit[3]+=100;}
            else if((frame==7&&row==10)||(frame==9&&row==4)){hit[0]-=1.25f;hit[2]-=1.25f;hit[1]+=200;hit[3]+=200;}
            else if(frame==7&&row==11){hit[0]=500.0249f;hit[1]=184.8499f;hit[2]=780.2751f;hit[3]=268.8512f;}
            else if((frame==8&&row==3)||(frame==9&&row==5)||(frame==10&&row==1)){
                hit[0]=498.2749f;hit[1]=188.9499f;hit[2]=778.5251f;hit[3]=272.9512f;
            }else assert((frame==7&&row==8)||(frame==9&&row==2));
            for(int i=0;i<4;++i)if(!strcmp(field,hit_fields[i]))value=hit[i];
        }
        else {
            assert(found>=0);
            if(!strncmp(GPTR(args[0]),"BrightnessScrollBar.slider_mc.",sizeof("BrightnessScrollBar.slider_mc.")-1))value=brightness_bounds[found];
        }
        if(variable_failure==1)return 0;
        if(variable_failure==2)memset(GPTR(AXIS_ARGS+256),'x',64);
        else snprintf(GPTR(AXIS_ARGS+256),64,"%g",(double)value);
        memset(&live_regs,0xA5,sizeof live_regs);
#ifdef RECOMP_X87_WINDOW
        memset(g_st_win,0x5A,sizeof g_st_win);memset(g_st_i64_win,0xA5,sizeof g_st_i64_win);g_fp_sp=12;
#endif
        return AXIS_ARGS+256;
    }
    assert(owned && address==0x10A65A60u && self==PAUSE && nargs==3);
    assert(args[0]==AXIS_ARGS && args[1]==AXIS_ARGS+1 && MEM8(args[1])==4);
    float delta;memcpy(&delta,&args[2],4);assert(isfinite(delta) && delta==(int)delta);
    unsigned axis=MEM8(args[0])==0xE4?0:1;assert(MEM8(args[0])==(axis?0xE5:0xE4));
    int value=(int)MEM32(self+240+axis*4)+(axis?-(int)delta:(int)delta);
    int bound=(int)MEM32(self+104+axis*4);assert(value>=0 && value<=bound);
    MEM32(self+240+axis*4)=(uint32_t)value;++axis_calls;
    memset(&live_regs,0xA5,sizeof live_regs);
#ifdef RECOMP_X87_WINDOW
    memset(g_st_win,0x5A,sizeof g_st_win);memset(g_st_i64_win,0xA5,sizeof g_st_i64_win);g_fp_sp=12;
#endif
    if(change_scene){menu_scene=0;scene_client=0;change_scene=0;}
    return 0;
}

static void put(uint32_t address,uint32_t value) { memcpy(GPTR(address),&value,4); }
static void name(uint32_t object,const char *text)
{
    put(object+36,++name_index);put(0x1000+name_index*4,next_name);
    for (unsigned i=0;;++i) { uint16_t c=(unsigned char)text[i];memcpy(GPTR(next_name+16+2*i),&c,2);if(!c)break; }
    next_name+=0x100;
}
char *void_wstr_utf8(uint32_t va)
{
    if (!va) return NULL;
    char *result=malloc(129);
    for (unsigned i=0;i<128;++i) {
        uint16_t c;memcpy(&c,GPTR(va+2*i),2);result[i]=(char)c;
        if (!c) return result;
    }
    result[128]=0;return result;
}
static uint32_t class(const char *text,uint32_t super)
{ uint32_t object=next_class;next_class+=0x1000;name(object,text);put(object+56,super);return object; }
static uint32_t property(uint32_t owner,const char *text,uint32_t kind,uint32_t offset,uint32_t next)
{
    uint32_t object=next_property;next_property+=0x1000;
    name(object,text);put(object+44,kind);put(object+108,offset);put(object+60,next ? next : MEM32(owner+84));
    put(owner+84,object);
    return object;
}
static uint32_t read_context(uint32_t out[4])
{
    assert(void_touch_game_read(out)==1);return out[1];
}
static void mouse_scale(uint32_t movie,float x,float y)
{ memcpy(GPTR(movie+248),&x,4);memcpy(GPTR(movie+252),&y,4); }
static void notify_mouse(uint32_t movie)
{
    float scale[2];memcpy(scale,GPTR(movie+248),sizeof scale);
    uint32_t root=MEM32(movie+268);
    for(unsigned i=0;i<2;++i){put(root+56+4*i,MEM32(root+44+4*i));put(root+44+4*i,(uint32_t)((double)(int)MEM32(movie+240+4*i)*scale[i]));}
}
static void absolute_pointer(void)
{
    uint32_t xy[2];void_regs expected;
    memset(&expected,0x3C,sizeof expected);memcpy(&live_regs,&expected,sizeof expected);
#ifdef RECOMP_X87_WINDOW
    double saved[RECOMP_X87_SLOTS];int64_t integers[RECOMP_X87_SLOTS];
    for(unsigned i=0;i<RECOMP_X87_SLOTS;++i){g_st_win[i]=i+.25;g_st_i64_win[i]=i*19;}
    memcpy(saved,g_st_win,sizeof saved);memcpy(integers,g_st_i64_win,sizeof integers);g_fp_sp=128;
#endif
    const int targets[][2]={{2779,1263},{0,0},{1390,827},{1,1260},{2500,3},{960,540}};
    const float scales[][2]={{1,1},{.5f,.5f},{1280.0f/2780,720.0f/1264},{2,3},{1.0f/16,1.0f/8}};
    for(unsigned s=0;s<sizeof scales/sizeof *scales;++s)for(unsigned t=0;t<sizeof targets/sizeof *targets;++t){
        put(PAUSE+240,80);put(PAUSE+244,90);mouse_scale(PAUSE,scales[s][0],scales[s][1]);notify_mouse(PAUSE);notify_mouse(PAUSE);
        int calls=axis_calls;
        assert(void_touch_ui_position(targets[t][0],targets[t][1]));
        assert(MEM32(PAUSE+240)==(uint32_t)targets[t][0] && MEM32(PAUSE+244)==(uint32_t)targets[t][1]);
        assert(axis_calls-calls<=2 && !allocations && !memcmp(&live_regs,&expected,sizeof expected));
        assert(void_touch_ui_mouse(xy)==1 && xy[0]!=MEM32(PAUSE+240));
        notify_mouse(PAUSE);assert(void_touch_ui_mouse(xy)==1 && xy[0]!=MEM32(PAUSE+240));
        notify_mouse(PAUSE);assert(void_touch_ui_mouse(xy)==1 && xy[0]==MEM32(PAUSE+240) && xy[1]==MEM32(PAUSE+244));
        calls=axis_calls;assert(void_touch_ui_position(targets[t][0],targets[t][1]) && calls==axis_calls);
#ifdef RECOMP_X87_WINDOW
        assert(g_fp_sp==128 && !memcmp(g_st_win,saved,sizeof saved) && !memcmp(g_st_i64_win,integers,sizeof integers));
#endif
    }
    int calls=axis_calls;owned=0;assert(!void_touch_ui_position(10,20));owned=1;
    uint32_t old_x=MEM32(PAUSE+240),old_y=MEM32(PAUSE+244);
    fail_allocation=1;assert(!void_touch_ui_position(10,20)&&calls==axis_calls&&!allocations);
    assert(MEM32(PAUSE+240)==old_x&&MEM32(PAUSE+244)==old_y&&!memcmp(&live_regs,&expected,sizeof expected));fail_allocation=0;
    assert(!void_touch_ui_position(-1,20) && !void_touch_ui_position(3000,20) && calls==axis_calls);
    put(VTABLE+228,0);assert(!void_touch_ui_position(10,20) && calls==axis_calls);put(VTABLE+228,0x10A65A60u);
    mouse_scale(PAUSE,NAN,1);assert(!void_touch_ui_mouse(xy));mouse_scale(PAUSE,1,1);
    change_scene=1;calls=axis_calls;assert(!void_touch_ui_position(100,200) && axis_calls==calls+1 && !allocations);
    menu_scene=PAUSE;
    put(SCENE+112,0);assert(void_touch_ui_button_state()==0);put(SCENE+112,1);assert(void_touch_ui_button_state()==1);
    put(SCENE+112,2);assert(void_touch_ui_button_state()==-1);put(SCENE+112,0);
    owned=0;assert(void_touch_ui_button_state()==-1);owned=1;
    put(ROOT_VTABLE+76,0);assert(void_touch_ui_button_state()==-1);put(ROOT_VTABLE+76,0x10BF6330);
    puts("PASS BioShock exact original axis calls: 30 far jumps/scales, <=2 callbacks, completed-Advance pointer ACK and original processed LMB validation, neutral/no-op/bounds/stale scene, full guest registers and x87 window preserved");
}

int main(void)
{
    assert(mmap((void *)(uintptr_t)VOID_GUEST_BASE,TEST_SIZE,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)(uintptr_t)VOID_GUEST_BASE);
    assert(mmap((void *)(uintptr_t)(VOID_GUEST_BASE+0x11000000),0x400000,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)(uintptr_t)(VOID_GUEST_BASE+0x11000000));
    put(0x11297070,0x1000);put(0x11297074,1024);put(0x111BE314,ENGINE);
    uint32_t object_kind=class("ObjectProperty",0),array_kind=class("ArrayProperty",0),bool_kind=class("BoolProperty",0);
    uint32_t engine_class=class("Engine",0),client_class=class("Client",0),viewport_class=class("Viewport",0);
    uint32_t player_class=class("PlayerController",0),shock_class=class("ShockPlayerController",player_class);
    uint32_t pawn_class=class("Pawn",0),gui_class=class("FlashGUIController",0);
    uint32_t int_kind=class("IntProperty",0);
    uint32_t string_kind=class("StrProperty",0), float_kind=class("FloatProperty",0);
    property(pawn_class,"LastPlayerInputContext",string_kind,2400,0);
    put(property(pawn_class,"HudElementsDisabled",bool_kind,2420,0)+148,2);
    put(PAWN+2420,2);
    const char *fov_properties[]={"DefaultFOV","DesiredFOV","FovAngle","DefaultForegroundFOV","ForegroundFovAngle"};
    for(int i=0;i<5;++i)property(player_class,fov_properties[i],float_kind,2500+4*i,0);
    MEMF(PLAYER+2500)=75;MEMF(PLAYER+2504)=75;MEMF(PLAYER+2508)=75;
    MEMF(PLAYER+2512)=60;MEMF(PLAYER+2516)=60;
    property(shock_class,"HeadbobContextStates",int_kind,1800,0);
    property(shock_class,"CurrentUseFocus",object_kind,1900,0);
    property(shock_class,"CurrentUseFocus",bool_kind,1904,0); /* Same-name wrong-type decoy is ignored. */
    uint32_t interact_class=class("Actor",0), unrelated_class=class("Material",0);
    put(0x1F0000+44,interact_class);
    property(pawn_class,"ActiveHoldable",object_kind,2200,0);
    property(pawn_class,"LastWeapon",object_kind,2204,0);
    property(pawn_class,"AvailableAbilities",array_kind,2208,0);
    uint32_t weapon_class=class("PlayerWeapon",0);
    uint32_t zoom_field=property(weapon_class,"CanBeZoomed",bool_kind,700,0);
    put(zoom_field+148,32);put(0x1E0000+44,weapon_class);
    property(client_class,"UseController",bool_kind,212,0);
    property(engine_class,"Client",object_kind,68,0);
    property(viewport_class,"Actor",object_kind,64,0);
    property(viewport_class,"GUIController",object_kind,116,0);
    property(player_class,"Player",object_kind,1364,0);
    property(player_class,"Pawn",object_kind,1300,0);
    property(pawn_class,"Controller",object_kind,1000,0);
    property(gui_class,"PlayingMovies",array_kind,280,0);
    name(ENGINE,"GameEngine");name(CLIENT,"WindowsClient");name(VIEWPORT,"WindowsViewport");
    name(PLAYER,"ShockPlayerController");name(PAWN,"ShockPlayer");name(GUI,"FlashGUIController");
    name(HUD,"HUD");name(PAUSE,"Pause");
    put(ENGINE+44,engine_class);put(CLIENT+44,client_class);put(VIEWPORT+44,viewport_class);
    put(PLAYER+44,shock_class);put(PAWN+44,pawn_class);put(GUI+44,gui_class);
    put(ENGINE+68,CLIENT);put(CLIENT+196,VIEWPORT);put(VIEWPORT+64,PLAYER);put(VIEWPORT+116,GUI);
    put(PLAYER+1364,VIEWPORT);put(PLAYER+1300,PAWN);put(PAWN+1000,PLAYER);
    put(GUI+280,MOVIES);put(GUI+284,1);put(MOVIES,HUD);
    put(RANGES,0x0000E5E4);put(PAUSE+228,RANGES);put(PAUSE+232,1);
    put(PAUSE+268,SCENE);put(SCENE+268,1);put(SCENE,ROOT_VTABLE);put(ROOT_VTABLE+76,0x10BF6330);put(PAUSE+240,123);put(PAUSE+244,456);
    put(PAUSE,VTABLE);put(VTABLE+228,0x10A65A60u);put(PAUSE+104,2780);put(PAUSE+108,1264);
    mouse_scale(PAUSE,1,1);notify_mouse(PAUSE);notify_mouse(PAUSE);
    uint32_t out[4],epoch=read_context(out);
    assert(out[0]==1 && out[3]==PLAYER && MEM32(CLIENT+212)==1);
    uint32_t details[2];
    put(HUD+268,SCENE);read_context(out);
    put(PLAYER+1900,0x1F0000);
    void_bioshock_touch_context(SCENE,1|2|4|8|16);
    assert(void_touch_game_details(details)==1 && details[1]==(31u<<4|1u<<9));
    void_bioshock_touch_context(SCENE,0);void_bioshock_touch_context(SCENE,32);
    assert(void_touch_game_details(details)==1 && details[1]==(1u<<4|1u<<9)); /* Empty UseFocus text retains eligible target. */
    void_bioshock_touch_context(SCENE,3);
    void_bioshock_touch_context(SCENE+4096,2);
    void_bioshock_touch_context(0,0);
    assert(void_touch_game_details(details)==1 && details[1]==(3u<<4|1u<<9));
    put(PLAYER+1900,0);
    assert(void_touch_game_details(details)==1 && details[1]==(2u<<4|1u<<9)); /* Stale HUD text cannot invent a target. */
    put(PLAYER+1900,UINT32_MAX);
    assert(void_touch_game_details(details)==1 && !(details[1]&(1u<<4)));
    put(PLAYER+1900,0x1F0000);put(0x1F0000+44,unrelated_class);
    assert(void_touch_game_details(details)==1 && !(details[1]&(1u<<4)));
    put(0x1F0000+44,interact_class);put(PLAYER+1900,0);
    uint32_t bad_focus_class=class("BadFocusController",shock_class);
    property(bad_focus_class,"CurrentUseFocus",object_kind,65535,0);
    put(PLAYER+44,bad_focus_class);put(PLAYER+1900,0x1F0000);read_context(out);
    assert(void_touch_game_details(details)==1 && !(details[1]&(1u<<4)));
    put(PLAYER+44,shock_class);read_context(out);
    assert(void_touch_game_details(details)==1 && (details[1]&(1u<<4)));
    put(PLAYER+1900,0);
    void_bioshock_touch_context(SCENE,0);
    put(PLAYER+1804,1);assert(void_touch_game_details(details)==1 && !(details[1]&(1u<<10)));
    put(PLAYER+1808,2);assert(void_touch_game_details(details)==1 && (details[1]&(1u<<10)));
    put(PLAYER+1808,UINT32_MAX);assert(void_touch_game_details(details)==1 && !(details[1]&(1u<<10)));
    put(PLAYER+1808,0);
    put(PAWN+2200,0x1E0000);put(PAWN+2204,0x1E0000);
    put(0x1E0000+700,32);assert(void_touch_game_details(details)==1 && (details[1]&(1u<<11)));
    put(0x1E0000+700,16);assert(void_touch_game_details(details)==1 && !(details[1]&(1u<<11)));
    put(PAWN+2200,0);assert(void_touch_game_details(details)==1 && !(details[1]&(1u<<11)));
    void_bioshock_touch_options(0,1);read_context(out);
    assert(out[0]==1 && !void_touch_gamepad_present() && !MEM32(CLIENT+212));
    assert(void_touch_game_details(details)==1 && !details[1]);
    void_bioshock_touch_options(1,1);read_context(out);
    assert(void_touch_gamepad_present() && MEM32(CLIENT+212)==1);
    put(MOVIES+4,PAUSE);put(GUI+284,2);
    assert(read_context(out)>epoch && out[0]==2 && !MEM32(CLIENT+212) && !void_touch_gamepad_present());epoch=out[1];
    assert(void_touch_game_details(details)==1 && !details[1]);
    assert(void_touch_ui_control()==1);
    uint32_t xy[2];assert(void_touch_ui_mouse(xy)==1 && xy[0]==123 && xy[1]==456);
    absolute_pointer();
    put(ROOT_VTABLE+88,0x10BF6690u);settings_row=2;
    assert(void_touch_ui_control()==(1|4)); // Original knob is 36.2px above its invisible measurement bar.
    settings_mouse[1]=452.2f;assert(void_touch_ui_control()==1); // Measurement bar alone is outside the actual input hitbox.
    settings_mouse[1]=416;settings_mouse[0]=781;assert(void_touch_ui_control()==1);settings_mouse[0]=1032;
    settings_modal=6;assert(void_touch_ui_control()==1);settings_modal=1; // Modal covers the underlying graphics/input widget.
    settings_hit[2]=settings_hit[0];assert(void_touch_ui_control()==1);settings_hit[2]=1061.775f;
    settings_hit[3]=NAN;assert(void_touch_ui_control()==1);settings_hit[3]=448.9012f;
    settings_row=0;assert(void_touch_ui_control()==(1|4)); // Stale highlight cannot reject the visible Audio knob.
    settings_mouse[0]=500;settings_mouse[1]=350;assert(void_touch_ui_control()==1);
    settings_mouse[0]=1032;settings_mouse[1]=416;
    settings_row=2;settings_page=2;settings_frame=8;
    assert(void_touch_ui_control()==1); // Additional controls hide the underlying original sliders.
    settings_frame=9;assert(void_touch_ui_control()==(1|4)); // Stale PortPage must not block original Audio.
    settings_row=2.5f;assert(void_touch_ui_control()==(1|4));
    settings_row=INFINITY;assert(void_touch_ui_control()==(1|4));
    settings_row=33;assert(void_touch_ui_control()==(1|4));
    settings_row=0;settings_page=0;settings_frame=7;settings_section=7;
    assert(void_touch_ui_control()==(1|4)); // Actual Graphics failure: row0 still selected over quality row8.
    settings_page=1;settings_mouse[0]=410;settings_mouse[1]=480;assert(void_touch_ui_control()==(1|4));
    settings_mouse[0]=870;assert(void_touch_ui_control()==(1|4));
    settings_mouse[0]=640;assert(void_touch_ui_control()==1); // The gap between FOV sliders belongs to neither.
    settings_page=0;
    const int pages[]={7,7,7,8,9,9,9,10};
    const float points[][2]={{1032,516},{1032,616},{600,220},{600,230},{1032,516},{1032,616},{600,230},{600,230}};
    for(unsigned i=0;i<sizeof pages/sizeof *pages;++i){
        settings_frame=(float)pages[i];settings_mouse[0]=points[i][0];settings_mouse[1]=points[i][1];
        assert(void_touch_ui_control()==(1|4)); // Every candidate, including nonfirst rows, uses actual geometry.
    }
    settings_frame=NAN;assert(void_touch_ui_control()==1);
    settings_frame=7.5f;assert(void_touch_ui_control()==1);
    settings_frame=25;assert(void_touch_ui_control()==1);
    settings_frame=6;assert(void_touch_ui_control()==1);
    settings_frame=9;settings_mouse[0]=1032;settings_mouse[1]=416;
    settings_row=NAN;settings_page=0;settings_frame=6;
    put(GUI+284,1);assert(read_context(out)>epoch && out[0]==1 && MEM32(CLIENT+212)==1);epoch=out[1];
    put(MOVIES+4,HUD);put(GUI+284,2);assert(read_context(out)==epoch && out[0]==1);
    put(MOVIES+4,PAUSE);name(PAUSE,"PCWeaponSelection");assert(read_context(out)>epoch && out[0]==2);epoch=out[1];
    assert(void_touch_ui_control()==1);
    assert(picker_reveals==0);
    now_ns+=134000000u;assert(read_context(out)==epoch && picker_reveals==0);
    now_ns+=1000000u;assert(read_context(out)==epoch && picker_reveals==1);
    now_ns+=135000000u;assert(read_context(out)==epoch && picker_reveals==1);
    const char *drag_movies[]={"Hacking","Maps","PlasmidEquip"};
    for (unsigned i=0;i<sizeof drag_movies/sizeof *drag_movies;++i) {
        name(PAUSE,drag_movies[i]);assert(read_context(out)>epoch && out[0]==2);epoch=out[1];
        assert(void_touch_ui_control()==(1|4|16));
    }
    name(PAUSE,"PlasmiNow");assert(read_context(out)>epoch && out[0]==2);epoch=out[1];
    assert(void_touch_ui_control()==1);
    name(PAUSE,"NTSCColorBars");assert(read_context(out)>epoch && out[0]==2);epoch=out[1];
    put(ROOT_VTABLE+88,0x10BF6690u);void_regs expected;memset(&expected,0x3C,sizeof expected);live_regs=expected;
#ifdef RECOMP_X87_WINDOW
    double brightness_saved[RECOMP_X87_SLOTS];int64_t brightness_integers[RECOMP_X87_SLOTS];int brightness_sp=g_fp_sp;
    memcpy(brightness_saved,g_st_win,sizeof brightness_saved);memcpy(brightness_integers,g_st_i64_win,sizeof brightness_integers);
#endif
    assert(void_touch_ui_control()==(1|4)&&!allocations&&!memcmp(&live_regs,&expected,sizeof expected));
#ifdef RECOMP_X87_WINDOW
    assert(g_fp_sp==brightness_sp&&!memcmp(g_st_win,brightness_saved,sizeof brightness_saved)&&
           !memcmp(g_st_i64_win,brightness_integers,sizeof brightness_integers));
#endif
    brightness_bounds[0]=-1;assert(void_touch_ui_control()==1);brightness_bounds[0]=50;
    brightness_bounds[1]=11;assert(void_touch_ui_control()==1);brightness_bounds[1]=5;
    brightness_bounds[2]=0;assert(void_touch_ui_control()==1);brightness_bounds[2]=100;
    brightness_bounds[3]=NAN;assert(void_touch_ui_control()==1);brightness_bounds[3]=10;
    variable_failure=1;assert(void_touch_ui_control()==1&&!allocations);
    variable_failure=2;assert(void_touch_ui_control()==1&&!allocations);variable_failure=0;
    fail_allocation=1;assert(void_touch_ui_control()==1&&!allocations);fail_allocation=0;
    put(ROOT_VTABLE+88,0);assert(void_touch_ui_control()==1);put(ROOT_VTABLE+88,0x10BF6690u);
    owned=0;assert(void_touch_ui_control()==1);owned=1;
    put(PAUSE+256,2);assert(read_context(out)>epoch && out[0]==1);epoch=out[1];
    put(PAUSE+256,0);assert(read_context(out)>epoch && out[0]==2);epoch=out[1];
    put(PAUSE+232,0);name(PAUSE,"FadeIn");assert(read_context(out)>epoch && out[0]==1);epoch=out[1];
    name(PAUSE,"ProgressBar");assert(read_context(out)==epoch && out[0]==1);
    put(PAUSE+232,257);assert(read_context(out)==epoch && out[0]==1);
    put(PAUSE+232,1);put(PAUSE+228,UINT32_MAX-1);assert(read_context(out)==epoch && out[0]==1);
    put(PAUSE+228,RANGES);name(PAUSE,"ComboLock");assert(read_context(out)>epoch && out[0]==2);epoch=out[1];
    assert(void_touch_ui_control()==1);
    put(GUI+284,0);assert(read_context(out)>epoch && out[0]==1);epoch=out[1];
    put(GUI+284,17);assert(read_context(out)==epoch && out[0]==1);
    put(GUI+284,1);put(GUI+280,0);assert(read_context(out)==epoch && out[0]==1);
    put(GUI+280,MOVIES);put(MOVIES,0);assert(read_context(out)==epoch && out[0]==1);
    put(MOVIES,HUD);put(HUD+36,4096);assert(read_context(out)==epoch && out[0]==1);
    put(GUI+44,0);assert(read_context(out)==epoch && out[0]==1);
    put(GUI+44,gui_class);put(GUI+284,2);put(MOVIES+4,PAUSE);name(PAUSE,"Hacking");
    assert(read_context(out)>epoch && out[0]==2 && void_touch_ui_control()==(1|4|16));epoch=out[1];
    put(PLAYER+1300,0);name(HUD,"Pause");put(HUD+228,RANGES);put(HUD+232,1);
    put(HUD+268,SCENE);put(HUD+240,9);put(HUD+244,10);put(GUI+284,1);
    mouse_scale(HUD,1,1);notify_mouse(HUD);notify_mouse(HUD);
    assert(read_context(out)>epoch && out[0]==2 && out[3]==VIEWPORT && void_touch_ui_control()==1);
    assert(void_touch_ui_mouse(xy)==1 && xy[0]==9 && xy[1]==10);
    owned=0;out[0]=99;assert(void_touch_game_read(out)==-1 && out[0]==99);
    owned=1;name(HUD,"HUD");put(PLAYER+1300,PAWN);
    const char stale_context[]="Default PauseUIActive ";
    put(PAWN+2400,0x1F1000);put(PAWN+2404,sizeof stale_context);
    for(unsigned i=0;i<sizeof stale_context;++i)MEM16(0x1F1000+2*i)=(uint8_t)stale_context[i];
    const uint32_t input=0x1F2000;
    put(VIEWPORT+124,input);put(input,0x1103CB3C);
    const char *contexts[]={"Default","MovementOnly","NullInput","Default"};
    for(int mode=0;mode<4;++mode){
        name(input,contexts[mode]);put(input+1956,MEM32(input+36));
        put(PAWN+2420,mode==3?0:2);
        read_context(out);assert(out[0]==1);
        assert(void_touch_game_details(details)==1);
        assert(!!(details[1]&(1u<<12))==(mode==2));
        assert(!!(details[1]&(1u<<13))==(mode==3));
        assert(!!(details[1]&(1u<<14))==(mode==1));
        assert(void_bioshock_scripted_scene()==(mode==2));
    }
    test_player_fov=100;test_weapon_fov=68;
    MEMF(PLAYER+2516)=45; // A scripted weapon angle is active.
    read_context(out);
    assert(MEMF(PLAYER+2500)==100 && MEMF(PLAYER+2504)==100 && MEMF(PLAYER+2508)==100);
    assert(MEMF(PLAYER+2512)==68 && MEMF(PLAYER+2516)==45);
    puts("PASS BioShock movie context: input-bearing visible menus, stable gameplay for HUD/fades/progress, two-axis mini-game drags, bounded ranges/arrays, missing/bad names, invalid GUI class and ownership gate");
    return 0;
}
