/* Production game tweaks (android/host/bioshock_tweaks.c) against bounded guest memory. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "touch_host_win.h"
static uint32_t g_eax, g_ecx, g_esp;
#include "../android/host/bioshock_tweaks.c"

enum { TEST_SIZE=0x11300000, ENGINE=0x100000, CLIENT=0x101000, VIEWPORTS=0x102000, VIEWPORT=0x103000,
       MANAGER=0x110000, CONTAINER=0x111000, SLOT=0x112000, STACK_ITEM=0x113000, STACK=0x200000 };
static int owned=1;
static char setting_physics[8]="30", setting_fix[8]="1", setting_cap[8]="1", setting_raw[8]="0", setting_aniso[8]="16";
int void_machine_is_owned(void) { return owned; }
static int scripted_scene=-1;
int void_bioshock_scripted_scene(void) { return scripted_scene; }
int void_vm_readable(uint32_t a,uint32_t n) { return a>=0x10000 && n && a<=TEST_SIZE-n; }
int void_vm_writable(uint32_t a,uint32_t n) { return void_vm_readable(a,n); }
void void_log(const char *s,...) { (void)s; }
int void_android_port_setting(const char *key,const char *value,char *out,size_t cap) {
    assert(!value);
    const char *v=!strcmp(key,"physicsRate")?setting_physics:!strcmp(key,"fixScrounger")?setting_fix:
        !strcmp(key,"cutsceneFps")?setting_cap:!strcmp(key,"mouseRaw")?setting_raw:!strcmp(key,"anisotropy")?setting_aniso:NULL;
    if(!v)return 2;
    snprintf(out,cap,"%s",v);return 0;
}
static float step(void) { float f; memcpy(&f,GPTR(PHYSICS_STEP_VA),4); return f; }
int main(void) {
    assert(VirtualAlloc((void *)(VOID_GUEST_BASE),TEST_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)==(void *)VOID_GUEST_BASE);
    MEM32(PHYSICS_STEP_VA)=PHYSICS_STEP_ORIGINAL;
    /* No engine yet: no cinematic, the original step stays bit-exact. */
    void_bioshock_tweaks_poll();
    assert(MEM32(PHYSICS_STEP_VA)==PHYSICS_STEP_ORIGINAL && void_bioshock_frame_cap()==0.0f);
    assert(void_bioshock_anisotropy_level()==16);
    /* Physics: 60 Hz live, back to the exact original bits. */
    void_bioshock_tweak("physicsRate","60");void_bioshock_tweaks_poll();assert(step()==1.0f/60.0f);
    void_bioshock_tweak("physicsRate","45");void_bioshock_tweaks_poll();assert(MEM32(PHYSICS_STEP_VA)==PHYSICS_STEP_ORIGINAL);
    /* Not owned: nothing is touched. */
    void_bioshock_tweak("physicsRate","60");owned=0;void_bioshock_tweaks_poll();assert(MEM32(PHYSICS_STEP_VA)==PHYSICS_STEP_ORIGINAL);
    owned=1;void_bioshock_tweaks_poll();assert(step()==1.0f/60.0f);
    /* A value the port did not write disables the setting for good. */
    float other=0.02f;memcpy(GPTR(PHYSICS_STEP_VA),&other,4);void_bioshock_tweak("physicsRate","30");void_bioshock_tweaks_poll();
    assert(step()==0.02f);void_bioshock_tweak("physicsRate","60");void_bioshock_tweaks_poll();assert(step()==0.02f);
    /* Cutscenes: Viewports[0]+352 of the engine's client caps the limiter at 60 while the setting is on. */
    MEM32(0x111BE314)=ENGINE;MEM32(ENGINE+68)=CLIENT;MEM32(CLIENT+60)=VIEWPORTS;MEM32(CLIENT+64)=1;MEM32(VIEWPORTS)=VIEWPORT;
    MEM32(VIEWPORT+352)=1;void_bioshock_tweaks_poll();assert(void_bioshock_frame_cap()==60.0f);
    void_bioshock_tweak("cutsceneFps","0");assert(void_bioshock_frame_cap()==0.0f);
    void_bioshock_tweak("cutsceneFps","1");assert(void_bioshock_frame_cap()==60.0f);
    MEM32(VIEWPORT+352)=0;void_bioshock_tweaks_poll();assert(void_bioshock_frame_cap()==0.0f);
    MEM32(VIEWPORT+352)=1;MEM32(CLIENT+64)=0;void_bioshock_tweaks_poll();assert(void_bioshock_frame_cap()==0.0f);
    MEM32(CLIENT+64)=1;MEM32(VIEWPORTS)=UINT32_MAX-100;void_bioshock_tweaks_poll();assert(void_bioshock_frame_cap()==0.0f);
    /* A level script locked input with NullInput. */
    scripted_scene=1;void_bioshock_tweaks_poll();assert(void_bioshock_is_cinematic() && void_bioshock_frame_cap()==60.0f);
    void_bioshock_tweak("cutsceneFps","0");assert(void_bioshock_is_cinematic() && void_bioshock_frame_cap()==0.0f);
    void_bioshock_tweak("cutsceneFps","1");
    scripted_scene=0;void_bioshock_tweaks_poll();assert(void_bioshock_frame_cap()==0.0f);
    scripted_scene=-1;void_bioshock_tweaks_poll();assert(void_bioshock_frame_cap()==0.0f);
    MEM32(0x111C1438u)=0x123000;void_bioshock_tweaks_poll();assert(void_bioshock_is_cinematic());
    MEM32(0x111C1438u)=0;void_bioshock_tweaks_poll();assert(!void_bioshock_is_cinematic());
    assert(void_bioshock_fov(0)==75 && void_bioshock_fov(1)==60);
    void_bioshock_tweak("playerFov","100");assert(void_bioshock_fov(0)==100 && void_bioshock_fov(1)==60);
    void_bioshock_tweak("weaponFov","72");void_bioshock_tweak("weaponFov","101");
    assert(void_bioshock_fov(0)==100 && void_bioshock_fov(1)==72);
    /* Scrounger: CanReRoll is refused (false, retired thiscall) only while the container holds an item. */
    MEM32(MANAGER+0x48)=CONTAINER;for(int i=0;i<3;++i)MEM32(CONTAINER+56+4*i)=SLOT+0x100*i;
    for(int i=0;i<3;++i)MEM32(SLOT+0x100*i+60)=0;
    g_ecx=MANAGER;g_esp=STACK;g_eax=0x77;assert(!void_bioshock_reroll_blocked() && g_esp==STACK && g_eax==0x77);
    MEM32(SLOT+0x200+60)=STACK_ITEM;MEM32(STACK_ITEM+76)=0;assert(!void_bioshock_reroll_blocked());
    MEM32(STACK_ITEM+76)=1;assert(void_bioshock_reroll_blocked() && g_esp==STACK+4 && g_eax==0);
    g_esp=STACK;g_eax=0x77;void_bioshock_tweak("fixScrounger","0");assert(!void_bioshock_reroll_blocked() && g_esp==STACK);
    void_bioshock_tweak("fixScrounger","1");MEM32(MANAGER+0x48)=0;assert(!void_bioshock_reroll_blocked() && g_esp==STACK);
    MEM32(MANAGER+0x48)=CONTAINER;MEM32(CONTAINER+56)=UINT32_MAX-8;assert(void_bioshock_reroll_blocked());
    g_ecx=UINT32_MAX-4;g_esp=STACK;assert(!void_bioshock_reroll_blocked() && g_esp==STACK);
    assert(!void_bioshock_mouse_raw());void_bioshock_tweak("mouseRaw","1");assert(void_bioshock_mouse_raw());
    puts("PASS game tweaks: live Havok step with exact original restore and foreign-value guard, cutscene cap, Scrounger re-roll guard, bounded reads");
    return 0;
}
