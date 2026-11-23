/* Standalone production touch/input assertions; no renderer or guest thread. */
#include "../android/host/input.c"
#include <assert.h>
#include <stdio.h>

static uint32_t ticks;
static int published;
static uint32_t publication[4];
int void_touch_game_read(uint32_t out[4])
{ if(published<0)return -1;if(!published)return 0;memcpy(out,publication,sizeof publication);return 1; }
void void_log(const char *format,...) { (void)format; }
uint32_t void_tick_ms(void) { return ++ticks; }
uint64_t void_monotonic_ns(void) { return (uint64_t)++ticks * 1000000; }
uint32_t void_window_main(void) { return 0; }
void void_post_message(uint32_t hwnd,uint32_t msg,uint32_t wp,uint32_t lp)
{ (void)hwnd;(void)msg;(void)wp;(void)lp; }
static uint32_t fixture_mouse_xy[2];
int void_machine_is_owned(void) { return 1; }
int void_touch_ui_mouse(uint32_t out[2])
{ out[0]=fixture_mouse_xy[0];out[1]=fixture_mouse_xy[1];return 1; }
static int gamepad_present;
int void_touch_gamepad_present(void) { return gamepad_present; }
static int controls_enabled=1;
int void_bioshock_touch_enabled(void) { return controls_enabled; }

int main(void)
{
    uint8_t pad[16],buttons[8],keys[256];int32_t dx,dy,dz;
    memset(pad,0xA5,16);int initial_connection=void_input_touch_pad(0,pad);
    printf("initial context=%llX active=%d connected=%d present=%d result=%d\n",
        (unsigned long long)touch_cursor_context,touch_game_active,touch_connected,gamepad_present,initial_connection);
    assert(!initial_connection);
    assert(!void_input_touch_pc_mouse());
    for(int i=0;i<16;++i)assert(pad[i]==0xA5);
    void_input_touch(0,0,0,0,1);void_input_touch(0,0,0,0,0);
    assert(!void_input_touch_pad(0,pad)); /* Unknown cursor clicks do not invent a pad. */
    void_input_touch_reset();
    void_input_touch(.5f,.5f,0,0,0);assert(void_input_touch_pad(0,pad));
    assert(!void_input_touch_pc_mouse()); /* Legacy/unknown is not our classified gameplay. */
    int16_t x,y;memcpy(&x,pad+8,2);memcpy(&y,pad+10,2);
    assert(x==y&&x>19000&&x<20000);assert(!void_input_touch_pad(1,pad));
    int arrivals=0;
    for(int i=0;i<pending_count;++i)if(pending[i].msg==0x0219) {
        ++arrivals;assert(pending[i].wparam==0x8000&&pending[i].lparam==0);
    }
    assert(arrivals==1);void_input_pump();
    assert(void_input_touch_pad(0,pad));
    for(int i=0;i<pending_count;++i)assert(pending[i].msg!=0x0219);
    void_input_touch(0,0,0,0,0);assert(void_input_touch_pad(0,pad));
    memcpy(&x,pad+8,2);memcpy(&y,pad+10,2);assert(!x&&!y);
    touch_game_seen=touch_game_active=1;
    void_input_touch_reset();memset(pad,0xA5,16);assert(!void_input_touch_pad(0,pad));
    for(int i=0;i<16;++i)assert(pad[i]==0xA5);
    void_input_touch(.5f,.5f,0,0,0);assert(void_input_touch_pad(0,pad));
    int reconnect_arrivals=0;
    for(int i=0;i<pending_count;++i)if(pending[i].msg==0x0219)++reconnect_arrivals;
    assert(reconnect_arrivals==1);void_input_pump();
    void_input_touch_reset();memset(pad,0xA5,16);assert(!void_input_touch_pad(0,pad));
    touch_game_seen=touch_game_active=0;
    touch_cursor_context=0;
    for(int i=0;i<16;++i)assert(pad[i]==0xA5);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    for(int i=0;i<4;++i)void_input_touch(0,0,.25f,-.25f,0);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    assert(dx==1&&dy==-1&&!dz);
    void_input_touch(0,0,0,0,16);void_input_touch(0,0,0,0,0);
    void_input_touch(0,0,0,0,16);void_input_touch(0,0,0,0,0);
    void_input_keyboard_snapshot(keys);assert(keys[void_vk_to_dik('E')]);
    void_input_keyboard_snapshot(keys);assert(!keys[void_vk_to_dik('E')]);
    void_input_event events[256];int n=void_input_keyboard_events(events,256,0),edges=0;
    for(int i=0;i<n;i++)if(events[i].ofs==void_vk_to_dik('E'))++edges;
    assert(edges==4);
    void_input_key('E',1);void_input_mouse_button(0,1);
    void_input_touch(0,0,0,0,17);void_input_touch_reset();void_input_touch_reset();
    assert(void_input_key_state('E')<0&&void_input_key_state(1)<0);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(buttons[0]);
    void_input_key('E',0);void_input_mouse_button(0,0);
    void_input_keyboard_snapshot(keys); /* Consume the deliberately preserved physical press latch. */
    n=void_input_keyboard_events(events,256,0);
    assert(n>=2); /* The physical DOWN/UP pair must survive touch cancellation too. */
    void_input_touch(0,0,0,0,17);void_input_touch_reset();void_input_touch_reset();
    void_input_keyboard_snapshot(keys);void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    assert(!keys[void_vk_to_dik('E')]&&!buttons[0]);
    n=void_input_keyboard_events(events,256,0);
    for(int i=0;i<n;i++)assert(events[i].data==0);
    void_input_touch(0,0,0,0,32);void_input_touch(0,0,0,0,0);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==120);
    void_input_touch_reset();void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==0);
    void_input_mouse_events(events,256,0);
    for(int i=0;i<3;i++){void_input_touch(0,0,0,0,64);void_input_touch(0,0,0,0,0);}
    published=-1;touch_weapon_slots=1u<<22;
    void_input_mouse_wheel(360);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==360);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==0);
    n=void_input_mouse_events(events,256,0);assert(n==1&&events[0].ofs==8&&events[0].data==360);
    touch_weapon_slots=0;
    n=void_input_mouse_events(events,256,1);assert(n==1&&events[0].ofs==8&&(int32_t)events[0].data==-120);
    assert(void_input_mouse_events(events,256,1)==1);assert(void_input_mouse_events(events,256,0)==1);
    touch_weapon_slots=1u<<22;ticks+=100;
    void_input_mouse_move(5,0);
    n=void_input_mouse_events(events,256,0);assert(n==1&&events[0].ofs==0&&events[0].data==5*MOUSE_SUBCOUNTS);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dx==5&&dz==-120);
    touch_weapon_slots=0;ticks+=100;
    n=void_input_mouse_events(events,256,0);assert(n==1&&events[0].ofs==8&&(int32_t)events[0].data==-120);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==-120);
    assert(void_input_mouse_events(events,256,0)==0);
    void_input_touch_reset();void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==0);
    published=0;
    for(int i=0;i<3;i++){void_input_touch(0,0,0,0,64);void_input_touch(0,0,0,0,0);}
    void_input_mouse_wheel(360);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==240);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==0);
    ticks+=100;
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==-120);
    ticks+=100;
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==-120);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==0);
    for(int i=0;i<2;i++){void_input_touch(0,0,0,0,32);void_input_touch(0,0,0,0,0);}
    ticks+=100;
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==120);
    void_input_touch_reset();void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(dz==0);
    void_input_touch(1,0,100,100,17);void_input_touch(NAN,0,0,0,0);
    void_input_keyboard_snapshot(keys);void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    assert(!dx&&!dy&&!keys[void_vk_to_dik('E')]&&!buttons[0]);
    for(int reset=0;reset<260;reset++) {
        void_input_touch(0,0,0,0,16);void_input_touch_reset();
        n=void_input_keyboard_events(events,256,0);
        for(int i=0;i<n;i++)assert(events[i].data==0);
    }
    assert(!touch_cursor_context);
    void_input_touch(1,0,1,0,17);void_input_cursor_visible(1);
    assert((touch_cursor_context&3)==3&&!touch_held&&!touch_stick_x);
    uint64_t context=touch_cursor_context;void_input_cursor_visible(1);assert(context==touch_cursor_context);
    assert(touch_submit(0,0,0,0,1,context));assert(!void_input_touch_pad(0,pad));
    assert(touch_submit(0,0,0,0,0,context));assert(!void_input_touch_pad(0,pad));
    void_input_cursor_visible(0);assert((touch_cursor_context&3)==2&&(touch_cursor_context>>2)==2);
    assert(!touch_submit(1,0,1,0,17,context));
    assert(!touch_held&&!touch_stick_x);
    assert(touch_submit(0,0,0,0,0,touch_cursor_context));
    void_input_touch(0,0,0,0,17);
    void_input_key('E',1);void_input_key('E',0);
    void_input_mouse_button(0,1);void_input_mouse_button(0,0);
    void_input_touch_reset();
    void_input_keyboard_snapshot(keys);void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    assert(keys[void_vk_to_dik('E')]&&buttons[0]);
    void_input_keyboard_snapshot(keys);void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    assert(!keys[void_vk_to_dik('E')]&&!buttons[0]);
    /* A real publisher context reaches this same production observation path;
     * neutral polling retains the connection without another device arrival. */
    published=1;publication[0]=1;publication[1]=1;publication[2]=0;publication[3]=0x1234;
    touch_observe_game();assert(touch_game_active&&(touch_cursor_context&3u)==2u&&!touch_connected);
    assert(!void_input_touch_pc_mouse()); /* Merely publishing gameplay is not a touch contact. */
    int old_pending=pending_count,old_keys=key_ring.count,old_mouse=mouse_ring.count;
    assert(void_input_touch_pad(0,pad));
    for(unsigned i=4;i<16;++i)assert(!pad[i]);
    assert(!touch_connected&&pending_count==old_pending&&key_ring.count==old_keys&&mouse_ring.count==old_mouse);
    for(unsigned slot=1;slot<4;++slot) {
        memset(pad,0xA5,sizeof pad);assert(!void_input_touch_pad(slot,pad));
        for(unsigned i=0;i<16;++i)assert(pad[i]==0xA5);
    }
    assert(touch_submit(1,1,0,0,0,touch_cursor_context)&&void_input_touch_pad(0,pad));
    int gate_pending=pending_count,gate_mouse=mouse_ring.count;uint32_t gate_packet=touch_packet;
    assert(void_input_touch_pc_mouse()&&void_input_touch_pc_mouse());
    assert(pending_count==gate_pending&&mouse_ring.count==gate_mouse&&touch_packet==gate_packet);
    memcpy(&x,pad+8,2);memcpy(&y,pad+10,2);assert(x==y&&x>25000&&x<25100);
    void_input_key('E',1);void_input_mouse_button(0,1);void_input_touch_reset();
    assert(!void_input_touch_pc_mouse()); /* Focus/cancel removes only our exception. */
    assert(void_input_touch_pad(0,pad));for(unsigned i=4;i<16;++i)assert(!pad[i]);
    assert(void_input_key_state('E')<0&&void_input_key_state(1)<0);
    publication[0]=2;++publication[1];touch_observe_game();
    assert(!void_input_touch_pc_mouse());
    memset(pad,0xA5,sizeof pad);assert(!void_input_touch_pad(0,pad));
    for(unsigned i=0;i<16;++i)assert(pad[i]==0xA5);
    assert(void_input_key_state('E')<0&&void_input_key_state(1)<0);
    publication[0]=0;++publication[1];touch_observe_game();assert(!void_input_touch_pad(0,pad));
    publication[0]=1;++publication[1];publication[2]=2;touch_observe_game();
    assert(!touch_connected&&void_input_touch_pad(0,pad));for(unsigned i=4;i<16;++i)assert(!pad[i]);
    void_input_key('E',0);void_input_mouse_button(0,0);
    int saved_dispatch=touch_dispatch;
    int32_t before_x=mouse_dx,before_y=mouse_dy,before_touch_x=touch_mouse_dx,before_touch_y=touch_mouse_dy;
    uint32_t mouse_xy[2]={640,540};
    ui_feedback_motion_locked(1200,655,mouse_xy);
    assert(mouse_dx==before_x+1&&mouse_dy==before_y+1);
    assert(touch_mouse_dx==before_touch_x+1&&touch_mouse_dy==before_touch_y+1);
    assert(touch_dispatch==saved_dispatch);
    mouse_xy[0]=1200;mouse_xy[1]=780;
    ui_feedback_motion_locked(0,0,mouse_xy);
    assert(mouse_dx==before_x&&mouse_dy==before_y);
    assert(touch_mouse_dx==before_touch_x&&touch_mouse_dy==before_touch_y);
    before_x=mouse_dx;before_y=mouse_dy;
    mouse_xy[0]=1199;mouse_xy[1]=654;
    ui_feedback_motion_locked(1200,655,mouse_xy);
    assert(mouse_dx==before_x&&mouse_dy==before_y);
    published=-1;touch_game_active=1;touch_cursor_context=0x103;touch_blocked=0;
    ui_logical_x=1200;ui_logical_y=780;ui_logical_context=touch_cursor_context;
    ui_waiting=1;ui_wait_x=1200;ui_wait_y=780;ui_wait_epoch=touch_epoch;
    ui_wait_context=touch_cursor_context;ui_wait_started=void_monotonic_ns();ui_ack_valid=0;
    pending_count=1;pending[0]=(pending_t){0x200,0,(780u<<16)|1200u,(int)touch_epoch,touch_cursor_context};
    fixture_mouse_xy[0]=640;fixture_mouse_xy[1]=540;void_input_pump();
    assert(ui_waiting&&pending_count>0&&!touch_blocked);
    uint64_t first_wait=ui_wait_started;
    ticks+=3000;fixture_mouse_xy[0]+=2;fixture_mouse_xy[1]+=2;void_input_pump();
    assert(ui_wait_started>first_wait);
    ticks+=3000;void_input_pump();assert(touch_blocked);
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    gamepad_present=1;published=1;publication[0]=1;++publication[1];touch_observe_game();
    assert(void_input_touch_pad(0,pad));
    gamepad_present=0;publication[0]=2;++publication[1];touch_observe_game();
    memset(pad,0xA5,sizeof pad);assert(!void_input_touch_pad(0,pad));
    uint64_t menu_context=touch_cursor_context;
    assert(touch_submit(0,0,0,0,1,menu_context));
    memset(pad,0xA5,sizeof pad);assert(!void_input_touch_pad(0,pad));
    for(unsigned i=0;i<sizeof pad;++i)assert(pad[i]==0xA5);
    published=1;publication[0]=1;++publication[1];touch_observe_game();
    uint64_t hybrid_context=touch_cursor_context;
    assert(touch_submit(.5f,0,12,-7,1,hybrid_context)&&void_input_touch_pc_mouse());
    assert(void_input_touch_pad(0,pad));memcpy(&x,pad+8,2);assert(x>19000);
    assert(touch_submit(0,0,0,0,0,hybrid_context)&&void_input_touch_pc_mouse());
    assert(void_input_touch_pad(0,pad));for(unsigned i=4;i<16;++i)assert(!pad[i]);
    assert(!touch_submit(0,0,5,0,0,hybrid_context-4)&&!void_input_touch_pc_mouse());
    assert(touch_submit(.5f,0,0,0,0,touch_cursor_context)&&void_input_touch_pc_mouse());
    touch_blocked=1;assert(!void_input_touch_pc_mouse());void_input_touch_reset();
    assert(!void_input_touch_pc_mouse());
    assert(touch_submit(.5f,0,0,0,16,touch_cursor_context));
    void_input_key('E',1);
    controls_enabled=0;touch_observe_game();
    assert(!touch_held&&!touch_connected&&!touch_stick_x&&!void_input_touch_pc_mouse());
    assert(void_input_key_state('E')<0);
    assert(!void_input_touch_pad(0,pad)&&!touch_submit(.5f,0,0,0,16,touch_cursor_context));
    assert(pad_set(1,0x1000,90,91,12345,-12345,23456,-23456));
    assert(void_input_touch_pad(0,pad));memcpy(&x,pad+8,2);memcpy(&y,pad+10,2);
    assert(x==12345&&y==-12345&&pad[6]==90&&pad[7]==91);
    assert(pad_set(0,0,0,0,0,0,0,0));
    publication[0]=2;++publication[1];touch_observe_game();
    assert(touch_submit(0,0,0,0,1,touch_cursor_context));
    assert(!void_input_touch_pad(0,pad));
    controls_enabled=1;publication[0]=1;++publication[1];touch_observe_game();
    assert(void_input_touch_pad(0,pad));void_input_key('E',0);
    static const uint8_t bindings[28] = {
        0,0,32,'C','E',0,0,27,0x77,0x78,'Q','L','M',
        0x70,0x71,0x72,0x73,0x74,0x75,'B','T','Z',0xA2,
        'R','F','V','H',0xA0
    };
    for(unsigned bit=2;bit<28;++bit) if(bit!=5 && bit!=6) {
        assert(touch_submit(0,0,0,0,1u<<bit,touch_cursor_context));
        assert(void_input_key_state(bindings[bit])<0);
        assert(touch_submit(0,0,0,0,0,touch_cursor_context));
        assert(void_input_key_state(bindings[bit])>=0);
    }
    puts("PASS BioShock bindings: F8/F9, F1-F6, Q/B, Z, M, T, H, Ctrl walking, Shift weapon selection");
    puts("PASS PC menu mode: virtual pad disconnects with UseController=0; gameplay reconnects; absent-player behavior unchanged");
    puts("PASS real production input: persistent analog/XInput16, fractional look, short/rapid edges, source ownership, repeated cancel/neutral release ledger, stale event removal, wheel reset, invalid-packet fail-neutral");
    puts("PASS eager neutral gameplay XInput0 before contact/after reset/vehicleepoch; UI/loading disconnect, physicalowners andslots1..3 preserved, getter leavesqueues untouched; legacyunknown unchanged");
    puts("PASS hybrid mouse gate: classified touch gameplay only, neutral retains movement pad, query consumes nothing, cancel/stale/blocked/menu/unknown restore original controller behavior");
    puts("PASS disabled gameplay touch releases virtual ownership, keeps physical keyboard/gamepad values and menu touch, and reconnects neutral on enable");
    return 0;
}
