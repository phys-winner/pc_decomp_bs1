/* Production Flash settings bridge with bounded guest memory and original-call fixtures. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "touch_host_win.h"
#include "bioshock_settings_fixture.h"
static void_regs live;
#define g_eax live.eax
#define g_esp live.esp
#include "../android/host/bioshock_settings.c"

enum { TEST_SIZE=0x11300000, TABLE=0x100000, ENTRY=0x101000, MOVIE=0x110000,
       STACK=0x200000, COMMAND=0x300000, ARGUMENT=0x301000 };
static int owned=1,original_calls,guest_calls,backend_calls,active_allocations,fail_allocation,relayouts;
static uint64_t time_ns=1;
static unsigned serial;
static uint32_t active_movie=MOVIE;
static char last_method[64],last_key[64],last_value[4096];
static void_fn registered;
double g_st_win[RECOMP_X87_SLOTS];
int64_t g_st_i64_win[RECOMP_X87_SLOTS];
int g_fp_sp;
int void_machine_is_owned(void) { return owned; }
uint64_t void_monotonic_ns(void) { return time_ns; }
uint32_t void_bioshock_touch_movie(void) { return active_movie; }
int void_vm_readable(uint32_t a,uint32_t n) { return a>=0x10000 && n && a<=TEST_SIZE-n; }
void void_log(const char *s,...) { (void)s; }
_Noreturn void void_fatal(const char *s,...) { (void)s;abort(); }
void void_regs_save(void_regs *r) { memcpy(r,&live,sizeof live); }
void void_regs_load(const void_regs *r) { memcpy(&live,r,sizeof live); }
void sub_10A65370(void) { ++original_calls; }
void void_bioshock_flash_relayout(void) { ++relayouts; }
static int hdr_wanted=-1,render_polls;
static unsigned context_calls,context_value,context_source;
void void_bioshock_touch_context(uint32_t source,uint32_t flags) { ++context_calls;context_source=source;context_value=flags; }
void void_bioshock_render_hdr(int wanted) { hdr_wanted=wanted; }
void void_bioshock_render_poll(void) { ++render_polls; }
void_fn void_dispatch_find(uint32_t a) { assert(a==0x10A65370);return sub_10A65370; }
void void_dispatch_add(uint32_t a,void_fn f) { assert(a==0x10A65370);registered=f; }
uint32_t void_gstrdup(const char *s) {
    if(fail_allocation && --fail_allocation==0)return 0;
    assert(strlen(s)<0x2000 && serial<256);
    uint32_t a=0x500000+(serial++)*0x2000;strcpy(GPTR(a),s);++active_allocations;return a;
}
void void_gfree(uint32_t a) { assert(a>=0x500000 && active_allocations>0);--active_allocations; }
uint32_t void_call_guest(uint32_t a,int n,const uint32_t *args,int pop) {
    assert(owned && a==0x10A66CD0 && args[0]==MOVIE && !pop);
    strcpy(last_method,GPTR(args[1]));
    assert((n==3 && (!strcmp(last_method,"PortOpenAdditional") || !strcmp(last_method,"PortOpenControls")) && !*(char *)GPTR(args[2])) ||
           (n==5 && (!strcmp(last_method,"PortSet") || !strcmp(last_method,"PortLanguage") ||
                     !strcmp(last_method,"PortGraphicsSet")) && !strcmp(GPTR(args[2]),"%s,%s")));
    if(n==5){strcpy(last_key,GPTR(args[3]));strcpy(last_value,GPTR(args[4]));}
    ++guest_calls;memset(&live,0xA5,sizeof live);memset(g_st_win,0x5A,sizeof g_st_win);
    memset(g_st_i64_win,0xA5,sizeof g_st_i64_win);g_fp_sp=17;return 0;
}
int void_android_port_setting(const char *key,const char *value,char *out,size_t cap) {
    assert(setting_key(key)>=0 && (cap==4096 || cap==256));++backend_calls;
    snprintf(out,cap,"%s",value?value:(!strcmp(key,"renderer")?"dxvk-turnip":"0"));return 0;
}
static void setup(void) {
    MEM32(0x11297070)=TABLE;MEM32(0x11297074)=2;MEM32(TABLE+4)=ENTRY;
    MEM32(MOVIE+36)=1;MEM32(MOVIE+268)=0x120000;
    const uint16_t name[]={'P','a','u','s','e',0};memcpy(GPTR(ENTRY+16),name,sizeof name);
}
static void fs(const char *command,const char *argument) {
    memset(&live,0x3C,sizeof live);g_esp=STACK;
    strcpy(GPTR(COMMAND),command);strcpy(GPTR(ARGUMENT),argument);
    MEM32(STACK+4)=0x120000;MEM32(STACK+8)=COMMAND;MEM32(STACK+12)=ARGUMENT;registered();
}
int main(void) {
    assert(VirtualAlloc((void *)(VOID_GUEST_BASE),TEST_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)==(void *)VOID_GUEST_BASE);
    setup();void_bioshock_settings_register();assert(registered);
    fs("PortTouchContext","31");assert(context_calls==1 && context_value==31 && context_source==0x120000 && g_eax==0 && g_esp==STACK+4);
    const char *invalid_contexts[]={"32","-1"," 1","1x","","9999999999999999999999999999999999999"};
    for(unsigned i=0;i<sizeof invalid_contexts/sizeof *invalid_contexts;++i)fs("PortTouchContext",invalid_contexts[i]);
    assert(context_calls==1 && !backend_calls && !original_calls);
    fs("PortTouchContext","0");assert(context_calls==2 && context_value==0);
    fs("ResumeSelected","");assert(original_calls==1 && !backend_calls && g_esp==STACK);
    fs("PortSettings","refresh");assert(backend_calls==37 && guest_calls==40 && !active_allocations && !relayouts && hdr_wanted==0);
    assert(g_eax==0 && g_esp==STACK+4 && !strcmp(last_key,"cheats"));
    fs("PortSettings","renderer=gles");assert(backend_calls==38 && !strcmp(last_value,"gles") && !relayouts);
    fs("PortSettings","fpsChoices=60");fs("PortSettings","unknown=1");fs("PortSettings","renderer");
    assert(backend_calls==38);
    MEM32(MOVIE+268)=0;fs("PortSettings","renderer=gles");assert(backend_calls==38);setup();
    fs("PortSettings","editLayout=1");assert(backend_calls==39 && !strcmp(last_key,"editLayout"));
    fs("PortSettings","uiScale=1.2");assert(backend_calls==40 && relayouts==1 && !strcmp(last_value,"1.2"));
    fs("PortSettings","touchMore=1");assert(backend_calls==41 && relayouts==1 && !strcmp(last_key,"touchMore"));
    fs("PortSettings","hdr=1");assert(backend_calls==42 && hdr_wanted==1 && relayouts==1);
    fs("PortSettings","hdr=0");assert(backend_calls==43 && hdr_wanted==0);

    void_regs expected;memset(&expected,0x36,sizeof expected);memcpy(&live,&expected,sizeof live);
    double saved[RECOMP_X87_SLOTS];int64_t integers[RECOMP_X87_SLOTS];
    for(unsigned i=0;i<RECOMP_X87_SLOTS;++i){g_st_win[i]=i+.25;g_st_i64_win[i]=i*19;}
    memcpy(saved,g_st_win,sizeof saved);memcpy(integers,g_st_i64_win,sizeof integers);g_fp_sp=128;
    void_bioshock_movie_call(MOVIE,"PortSet","renderer","dxvk");
    assert(!memcmp(&live,&expected,sizeof live) && g_fp_sp==128 && !memcmp(saved,g_st_win,sizeof saved) &&
           !memcmp(integers,g_st_i64_win,sizeof integers) && !active_allocations);
    assert(!memcmp(&live,&expected,sizeof live) && g_fp_sp==128 && !active_allocations);
    int calls=guest_calls;
    for(int i=1;i<=4;++i){fail_allocation=i;void_bioshock_movie_call(MOVIE,"PortSet","renderer","gles");
        assert(guest_calls==calls && !active_allocations && !memcmp(&live,&expected,sizeof live));}

    settings_deadline=10;settings_requested=1;owned=0;void_bioshock_settings_poll();assert(settings_requested && guest_calls==calls && !render_polls);
    hdr_wanted=-1;int backend=backend_calls;owned=1;active_movie=0;void_bioshock_settings_poll();
    assert(hdr_wanted==0 && backend_calls==backend+1 && render_polls==1);
    void_bioshock_settings_poll();assert(backend_calls==backend+1 && render_polls==2);owned=0;
    owned=1;active_movie=0;void_bioshock_settings_poll();assert(settings_requested && guest_calls==calls);
    active_movie=MOVIE;void_bioshock_settings_poll();
    assert(!settings_requested && guest_calls==calls+5 && !strcmp(last_method,"PortOpenAdditional"));
    void_bioshock_settings_poll();assert(guest_calls==calls+5);
    settings_requested=1;active_movie=0;time_ns=10;void_bioshock_settings_poll();assert(!settings_requested);
    active_movie=MOVIE;settings_requested=2;void_bioshock_settings_poll();
    assert(guest_calls==calls+10 && !strcmp(last_method,"PortOpenControls") && !strcmp(last_key,"renderScale"));
    MEM32(MOVIE+36)=2;assert(!pause_movie());MEM32(MOVIE+36)=1;
    MEM32(TABLE+4)=UINT32_MAX-8;assert(!pause_movie());setup();
    char result[8];memset(GPTR(ARGUMENT),'x',8);assert(!guest_string(ARGUMENT,result,sizeof result));
    assert(!guest_string(UINT32_MAX-2,result,sizeof result));
    puts("PASS original Flash settings: cdecl/delegation, whitelist/refresh, bounded strings/movie, full state, allocation failure, gameplay-to-Pause and stale request cancellation");
    return 0;
}
