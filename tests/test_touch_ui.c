/* Production UI pointer queue, with a controllable native SceneClient acknowledgement. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "touch_host_win.h"
#endif
#include "../android/host/input.c"

enum { VIEWPORT=0x110000, MOVE=0x200, DOWN=0x201, UP=0x202 };
typedef struct { uint32_t message,value,point; } posted_t;
static posted_t posted[2048];
static unsigned posted_count,ui_reads;
static int owned=1,ui_result=1,game_valid=1,cancel_on_move;
static uint32_t scene_x=10,scene_y=20,game_mode=2,game_epoch=1;
static uint64_t ticks;
static int control_kind=1,scroll_busy,scroll_total,scroll_calls,scroll_mailbox,scroll_target,scroll_fail;
static int direct_position,direct_calls,direct_x,direct_y,direct_cancel;
static int processed_button=-1;

int void_machine_is_owned(void) { return owned; }
int void_touch_game_read(uint32_t out[4])
{
    if (!owned) return -1;
    if (!game_valid) return 0;
    out[0]=game_mode;out[1]=game_epoch;out[2]=0;out[3]=VIEWPORT;
    return 1;
}
int void_touch_ui_mouse(uint32_t out[2])
{
    if (!owned) return -1;
    ++ui_reads;
    if (ui_result==1) { out[0]=scene_x;out[1]=scene_y; }
    return ui_result;
}
int void_touch_ui_control(void) { assert(owned);scroll_target=control_kind==2;return control_kind; }
int void_touch_ui_button_state(void)
{
    assert(owned);uint32_t x,y;void_input_cursor(&x,&y); /* Also outside input_lock. */
    return processed_button;
}
int void_touch_ui_position(int x,int y)
{
    if(!direct_position)return 0;
    assert(owned);uint32_t a,b;void_input_cursor(&a,&b); /* Callback must run outside input_lock. */
    ++direct_calls;direct_x=x;direct_y=y;
    if(direct_cancel==1)void_input_touch_reset();
    else if(direct_cancel==2)__atomic_add_fetch(&touch_cursor_context,4,__ATOMIC_RELEASE);
    direct_cancel=0;
    return 1;
}
int void_touch_ui_scroll(int steps)
{
    assert(owned);
    if(steps==INT32_MIN){scroll_target=scroll_mailbox=0;return 1;}
    assert(steps>=-8 && steps<=8);++scroll_calls;
    if(scroll_fail)return -1;
    if(scroll_busy||scroll_mailbox)return 0;
    if(!steps)return 1;
    assert(scroll_target);scroll_total+=steps;scroll_mailbox=steps;return 1;
}
uint32_t void_tick_ms(void) { return (uint32_t)ticks; }
uint64_t void_monotonic_ns(void) { return ++ticks*1000000; }
uint32_t void_window_main(void) { return 1; }
void void_log(const char *format,...) { (void)format; }
void void_post_message(uint32_t window,uint32_t message,uint32_t value,uint32_t point)
{
    assert(window==1 && posted_count<sizeof posted/sizeof *posted);
    posted[posted_count++]=(posted_t){message,value,point};
    if (message==MOVE && cancel_on_move) { cancel_on_move=0;void_input_touch_reset(); }
}
static uint32_t point(unsigned x,unsigned y) { return y<<16|x; }
static void cursor(unsigned x,unsigned y)
{
    uint32_t actual_x,actual_y;void_input_cursor(&actual_x,&actual_y);
    assert(actual_x==x && actual_y==y);
}
static void ack(unsigned x,unsigned y) { scene_x=x;scene_y=y; }
static unsigned messages(unsigned message)
{
    unsigned count=0;
    for (unsigned i=0;i<posted_count;++i) count+=posted[i].message==message;
    return count;
}
static void event(unsigned index,unsigned message,unsigned x,unsigned y)
{
    assert(index<posted_count && posted[index].message==message && posted[index].point==point(x,y));
}
static uint64_t fixture(void)
{
    owned=1;ui_result=1;game_valid=1;cancel_on_move=0;game_mode=2;++game_epoch;ticks=1;
    void_input_touch_reset();
    memset(physical_keys,0,sizeof physical_keys);memset(physical_buttons,0,sizeof physical_buttons);
    memset(vk_down,0,sizeof vk_down);memset(dik_down,0,sizeof dik_down);
    memset(touch_key_ledger,0,sizeof touch_key_ledger);memset(touch_button_ledger,0,sizeof touch_button_ledger);
    memset(mouse_buttons,0,sizeof mouse_buttons);memset(mouse_latched,0,sizeof mouse_latched);
    memset(dik_latched,0,sizeof dik_latched);memset(&key_ring,0,sizeof key_ring);memset(&mouse_ring,0,sizeof mouse_ring);
    pending_count=0;cursor_clip_active=0;void_input_set_screen_size(1280,720);void_input_set_cursor(10,20);
    mouse_dx=mouse_dy=mouse_dz=touch_mouse_dx=touch_mouse_dy=touch_wheel=0;
    uint8_t keys[256];void_input_keyboard_snapshot(keys);
    pending_count=0;posted_count=ui_reads=0;control_kind=1;
    scroll_busy=scroll_total=scroll_calls=scroll_mailbox=scroll_target=scroll_fail=0;ack(10,20);
    direct_position=direct_calls=direct_x=direct_y=direct_cancel=0;processed_button=-1;
    assert(touch_game_active && (touch_cursor_context&3u)==3u && !touch_held);
    return touch_cursor_context;
}
static void quick_tap_and_ownership(void)
{
    uint64_t context=fixture();
    owned=0;void_input_mouse_warp(100,120);cursor(10,20);
    assert(touch_submit(0,0,0,0,1,context));
    assert(touch_submit(0,0,0,0,0,context));
    void_input_pump();cursor(10,20);assert(!posted_count && !ui_reads);
    owned=1;void_input_pump();cursor(100,120);
    assert(scene_x==10 && scene_y==20); /* The input pump cannot write the native acknowledgement. */
    assert(posted_count==1);event(0,MOVE,100,120);
    void_input_pump();assert(posted_count==1);
    ack(100,120);void_input_pump();assert(posted_count==2);event(1,DOWN,100,120);
    void_input_pump();
    assert(posted_count==3);event(1,DOWN,100,120);event(2,UP,100,120);
    assert((posted[1].value&1) && !(posted[2].value&1));
}
static void drag_release_and_order(void)
{
    uint64_t context=fixture();
    void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    void_input_mouse_warp(300,320);assert(touch_submit(0,0,0,0,0,context));
    void_input_pump();cursor(100,120);assert(posted_count==1);event(0,MOVE,100,120);
    void_input_pump();cursor(100,120);assert(posted_count==1);
    ack(100,120);void_input_pump();cursor(100,120);
    assert(posted_count==2);event(1,DOWN,100,120);
    void_input_pump();cursor(300,320);
    assert(posted_count==3);event(1,DOWN,100,120);event(2,MOVE,300,320);
    void_input_pump();assert(posted_count==3 && !messages(UP));
    ack(300,320);void_input_pump();assert(posted_count==4);event(3,UP,300,320);
}
static void adjacent_moves_only(void)
{
    uint64_t context=fixture();
    for (int i=0;i<1000;++i) void_input_mouse_warp(100+i%100,120);
    assert(pending_count==1);cursor(10,20);
    assert(touch_submit(0,0,0,0,1,context));
    void_input_mouse_warp(300,320);void_input_mouse_warp(310,330);
    assert(pending_count==3);
    assert(touch_submit(0,0,0,0,0,context));void_input_mouse_warp(400,420);
    assert(pending_count==5);
    fixture();void_input_mouse_warp(100,120);
    vk_down[0x10]=1;void_input_mouse_warp(110,130);
    assert(pending_count==2); /* Different WM_MOUSE modifier state cannot merge. */
    fixture();void_input_mouse_warp(100,120);
    __atomic_add_fetch(&touch_cursor_context,4,__ATOMIC_RELEASE);void_input_mouse_warp(110,130);
    assert(pending_count==2); /* Different scene generation cannot merge. */
    fixture();void_input_mouse_warp(100,120);
    __atomic_add_fetch(&touch_epoch,1,__ATOMIC_RELEASE);void_input_mouse_warp(110,130);
    assert(pending_count==2); /* Different cancellation generation cannot merge. */
}
static void cancellation_and_invalid_scene(void)
{
    uint64_t context=fixture();
    void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    void_input_touch_reset();void_input_touch_reset();void_input_pump();
    assert(!messages(DOWN) && !touch_held && !touch_buttons[0]);
    context=fixture();void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    ++game_epoch;void_input_pump();assert(!messages(DOWN) && !touch_held && !touch_buttons[0]);
    context=fixture();void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    ui_result=0;void_input_pump();assert(!messages(DOWN) && !touch_held && !touch_buttons[0]);
    context=fixture();ui_result=2;void_input_mouse_warp(100,120);
    assert(touch_submit(0,0,0,0,1,context));assert(touch_submit(0,0,0,0,0,context));
    cancel_on_move=1;void_input_pump();
    assert(!messages(DOWN) && !touch_held && !touch_buttons[0]); /* Cancel after batch extraction drops its old edges. */
    context=fixture();void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    void_input_pump();ticks+=3000;void_input_pump();
    assert(!messages(DOWN) && !touch_held && !touch_buttons[0]);
}
static void overflow_and_physical_ownership(void)
{
    uint64_t context=fixture();
    void_input_mouse_button(1,1);void_input_key('E',1);
    void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    for (int i=0;i<500;++i) void_input_key('A',i&1);
    assert(!touch_submit(0,0,0,0,0,context));
    assert(!touch_held && !touch_buttons[0] && physical_buttons[1] && physical_keys['E']);
    void_input_pump();assert(!messages(DOWN));
    assert(void_input_key_state('E')<0 && void_input_key_state(2)<0);
    void_input_touch_reset();assert(void_input_key_state('E')<0 && void_input_key_state(2)<0);
}
static void console_and_legacy(void)
{
    uint64_t context=fixture();ui_result=2;
    void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    assert(touch_submit(0,0,0,0,0,context));void_input_pump();
    assert(posted_count==3);event(0,MOVE,100,120);event(1,DOWN,100,120);event(2,UP,100,120);
    fixture();game_valid=0;uint8_t keys[256];void_input_keyboard_snapshot(keys);
    touch_game_seen=0;void_input_cursor_visible(1);pending_count=posted_count=0;
    void_input_mouse_warp(100,120);cursor(100,120);
    assert(touch_submit(0,0,0,0,1,touch_cursor_context));
    assert(touch_submit(0,0,0,0,0,touch_cursor_context));void_input_pump();
    assert(posted_count==3);event(0,MOVE,100,120);event(1,DOWN,100,120);event(2,UP,100,120);
    fixture();void_input_mouse_move(3,4);cursor(13,24);
    void_input_mouse_button(0,1);void_input_mouse_button(0,0);void_input_pump();
    assert(posted_count==3);event(0,MOVE,13,24);event(1,DOWN,13,24);event(2,UP,13,24);
}
static void scroll_probe_and_delivery(void)
{
    uint64_t context=fixture();control_kind=2;
    void_input_mouse_warp(100,120);assert(ui_begin(context) && ui_control(context)==0);
    assert(!ui_scroll(1,context));owned=0;void_input_pump();assert(!posted_count && ui_control(context)==0);
    owned=1;void_input_pump();cursor(100,120);assert(ui_control(context)==0 && messages(MOVE)==1);
    assert(!messages(DOWN) && !messages(UP));
    ack(100,120);void_input_pump();assert(ui_control(context)==2 && !pending_count);
    assert(!ui_scroll(9,context) && !ui_scroll(1,context+4));
    assert(ui_scroll(8,context) && ui_scroll(3,context));scroll_busy=1;
    void_input_pump();assert(scroll_calls==1 && scroll_total==0 && ui_scroll_pending==11);
    scroll_busy=0;owned=0;void_input_pump();assert(scroll_calls==1);
    owned=1;void_input_pump();assert(scroll_total==8 && ui_scroll_pending==11);
    void_input_pump();assert(scroll_total==8 && scroll_mailbox==8);
    control_kind=1;scroll_mailbox=0;void_input_pump();assert(scroll_total==11 && ui_scroll_pending==3 && scroll_target);
    assert(ui_scroll(0,context));void_input_mouse_warp(300,320);assert(ui_begin(context));
    void_input_pump();cursor(100,120);assert(ui_control(context)==0 && scroll_target);
    scroll_mailbox=0;void_input_pump();assert(!ui_scroll_pending && !scroll_target && !messages(DOWN) && !messages(UP));
    cursor(300,320);ack(300,320);void_input_pump();assert(ui_control(context)==1);
    context=fixture();control_kind=2;assert(ui_begin(context));void_input_pump();assert(ui_scroll(-8,context));
    void_input_touch_reset();void_input_pump();assert(!scroll_total && !scroll_target && ui_control(context)<0);
    context=fixture();control_kind=2;void_input_mouse_warp(100,120);assert(ui_begin(context));
    void_input_pump();ack(100,120);void_input_pump();assert(ui_scroll(1,context));
    ++game_epoch;void_input_pump();assert(!scroll_total && ui_control(context)<0);
    context=fixture();control_kind=2;assert(ui_begin(context));void_input_pump();assert(ui_control(context)==2);
    for(int i=0;i<8;++i)assert(ui_scroll(8,context));assert(!ui_scroll(1,context));
    void_input_mouse_button(0,1);assert(!ui_scroll(1,context));void_input_pump();assert(!scroll_total && !ui_scroll_pending);
    context=fixture();ui_result=2;control_kind=2;assert(ui_begin(context));void_input_pump();assert(ui_control(context)==1);
    context=fixture();control_kind=-1;assert(ui_begin(context));void_input_pump();assert(ui_control(context)<0 && !messages(DOWN));
    context=fixture();control_kind=2;assert(ui_begin(context));void_input_pump();assert(ui_scroll(1,context));
    void_input_key('E',1);const uint16_t text[]={'a'};assert(text_submit(text,1,context));
    scroll_fail=1;void_input_pump();assert(messages(0x100)==1 && messages(0x102)==1 && physical_keys['E']);
    void_input_pump();assert(messages(0x100)==1 && messages(0x102)==1); /* No loss or duplicate after failure. */
}
static void exact_position_and_consumed_ack(void)
{
    const unsigned points[][2]={{1270,700},{10,20},{640,360},{5,710}};
    for(unsigned i=0;i<sizeof points/sizeof *points;++i){
        uint64_t context=fixture();direct_position=1;
        void_input_mouse_warp(points[i][0],points[i][1]);
        assert(touch_submit(0,0,0,0,1,context)&&touch_submit(0,0,0,0,0,context));
        owned=0;void_input_pump();assert(!direct_calls&&!posted_count);owned=1;
        void_input_pump();assert(direct_calls==1&&direct_x==(int)points[i][0]&&direct_y==(int)points[i][1]);
        int already_there=ui_near(10,points[i][0])&&ui_near(20,points[i][1]);
        if(!already_there){
            assert(messages(MOVE)==1&&!messages(DOWN));
            for(unsigned frame=0;frame<3;++frame){ticks+=200;void_input_pump();assert(!messages(DOWN));}
            assert(!mouse_dx&&!mouse_dy); /* No extra relative gain or repeated unit pulses. */
        }
        ack(points[i][0],points[i][1]);void_input_pump();void_input_pump();
        assert(messages(DOWN)==1&&messages(UP)==1&&!touch_held);
        int calls=direct_calls;void_input_pump();assert(calls==direct_calls); /* Idle never drags the cursor away. */
    }
    for(int kind=1;kind<=2;++kind){
        uint64_t context=fixture();direct_position=1;direct_cancel=kind;
        void_input_mouse_warp(1270,700);assert(touch_submit(0,0,0,0,1,context));
        void_input_pump();assert(!posted_count&&!messages(DOWN));
        void_input_touch_reset();
    }
    puts("PASS exact UI positioning: far jumps, VM-only callback outside input lock, original consumed ACK, delayed low-FPS ACK, one click, no idle drift or duplicate relative motion, callback cancellation and stale context");
}
static void flash_processed_edges(void)
{
    for(unsigned tap=0;tap<4;++tap){
        uint64_t context=fixture();direct_position=1;processed_button=0;
        void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context)&&touch_submit(0,0,0,0,0,context));
        ack(100,120);void_input_pump();void_input_pump();
        assert(messages(DOWN)==1&&!messages(UP)&&ui_button_waiting&&ui_button_expected==1);
        ticks+=500; /* A two-FPS Flash tick may trail many message pumps. */
        for(unsigned pump=0;pump<50;++pump){void_input_pump();assert(messages(DOWN)==1&&!messages(UP));}
        owned=0;void_input_pump();assert(ui_button_waiting&&!messages(UP));owned=1;
        processed_button=1;void_input_pump();
        assert(messages(DOWN)==1&&messages(UP)==1&&ui_button_waiting&&!ui_button_expected);
        void_input_mouse_warp(300,320);
        ticks+=500;
        for(unsigned pump=0;pump<50;++pump){void_input_pump();cursor(100,120);assert(direct_x==100&&direct_y==120);}
        assert(messages(MOVE)==1);processed_button=0;void_input_pump();
        assert(!ui_button_waiting&&messages(MOVE)==2);cursor(300,320);
        ack(300,320);void_input_pump();assert(messages(DOWN)==1&&messages(UP)==1);
    }
    for(unsigned kind=0;kind<4;++kind){
        uint64_t context=fixture();direct_position=1;processed_button=0;
        void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context)&&touch_submit(0,0,0,0,0,context));
        ack(100,120);void_input_pump();void_input_pump();assert(ui_button_waiting&&messages(DOWN)==1);
        if(kind==0){++game_epoch;game_mode=1;} /* The original press opened a different movie. */
        else if(kind==1)void_input_touch_reset(); /* Focus loss/CANCEL flushes the old gesture. */
        else if(kind==2)ticks+=3000; /* Stalled Flash cancels; it never permits the unacknowledged click. */
        else processed_button=-1; /* Lost publisher after DOWN. */
        void_input_pump();assert(!ui_button_waiting&&messages(DOWN)==1&&!touch_held);
        /* Existing release-ledger neutrality remains; no new scene is probed/clicked. */
        assert(ui_probe_status<=0);
    }
    puts("PASS original-Flash edge ACK: 100 pumps per tap cannot retire DOWN/UP or move their target before processed-state ACK; four taps exactly once; VM ownership, press transition, cancel/focus loss, missing publisher and cancel-only timeout");
}
static void di_buttons(unsigned downs,unsigned ups)
{
    void_input_event events[256];unsigned got_down=0,got_up=0;
    int count=void_input_mouse_events(events,256,0);
    for(int i=0;i<count;++i)if(events[i].ofs==12){
        assert(events[i].data==0 || events[i].data==128);
        if(events[i].data)++got_down;else ++got_up;
    }
    assert(got_down==downs && got_up==ups);
}
static void di_state(int down)
{
    int32_t dx,dy,dz;uint8_t buttons[8];
    void_input_mouse_snapshot(&dx,&dy,&dz,buttons);
    assert((buttons[0]!=0)==down && (void_input_key_state(1)<0)==down);
}
static void ordered_directinput_edges(void)
{
    uint64_t context=fixture();direct_position=1;processed_button=0;
    void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,1,context));
    void_input_mouse_warp(100,120);assert(touch_submit(0,0,0,0,0,context));
    /* The full fast tap exists as intent, while neither DI interface may see
     * its UP (or even its DOWN) before the ordered pointer gate opens. */
    assert(!touch_held && !touch_button_ledger[0]);di_buttons(0,0);di_state(0);
    ack(100,120);void_input_pump();
    assert(messages(DOWN)==1&&!messages(UP)&&ui_mouse_buttons[0]);
    assert(mouse_ring.count==1 && mouse_ring.touch[mouse_ring.head]);
    di_buttons(1,0);di_state(1);
    ticks+=500;
    for(unsigned poll=0;poll<50;++poll){void_input_pump();di_buttons(0,0);di_state(1);}
    /* This is the same processed-state seam tested with original x86 Advance:
     * DI can poll repeatedly, but cannot replace DOWN with UP before Advance. */
    processed_button=1;void_input_pump();
    assert(messages(DOWN)==1&&messages(UP)==1&&!ui_mouse_buttons[0]);
    assert(messages(MOVE)==2 && (posted[2].value&1)); /* Held drag MOVE retained intent flags. */
    di_buttons(0,1);di_state(0);
    void_input_mouse_warp(300,320);processed_button=0;void_input_pump();
    for(unsigned poll=0;poll<50;++poll){void_input_pump();di_buttons(0,0);di_state(0);}
    assert(messages(DOWN)==1&&messages(UP)==1&&!ui_button_waiting);

    context=fixture();direct_position=1;processed_button=0;
    assert(touch_submit(0,0,0,0,1,context));void_input_touch_reset();void_input_pump();
    assert(!messages(DOWN)&&!messages(UP)&&!touch_button_ledger[0]);di_buttons(0,0);di_state(0);

    context=fixture();direct_position=1;processed_button=0;
    assert(touch_submit(0,0,0,0,1,context));void_input_pump();di_buttons(1,0);di_state(1);
    void_input_touch_reset();void_input_pump();
    assert(!ui_mouse_buttons[0]&&!ui_button_waiting&&!touch_held);di_buttons(0,1);di_state(0);

    /* Physical DOWN is immediate even while UI intent has not published.
     * Ordered UI UP then retires touch ownership without releasing the mouse. */
    context=fixture();direct_position=1;processed_button=0;
    assert(touch_submit(0,0,0,0,1,context));void_input_mouse_button(0,1);
    di_buttons(1,0);di_state(1);void_input_pump();di_buttons(0,0);
    assert(ui_mouse_buttons[0]&&physical_buttons[0]);
    assert(touch_submit(0,0,0,0,0,context));processed_button=1;void_input_pump();
    assert(!ui_mouse_buttons[0]&&physical_buttons[0]);di_buttons(0,0);di_state(1);
    void_input_mouse_warp(300,320);ticks+=3000;void_input_pump();
    assert(!ui_button_waiting&&physical_buttons[0]);di_buttons(0,0);di_state(1);
    void_input_mouse_button(0,0);di_buttons(0,1);di_state(0);

    /* Physical release cannot clear a published, still-held UI touch. */
    context=fixture();direct_position=1;processed_button=0;
    assert(touch_submit(0,0,0,0,1,context));void_input_pump();di_buttons(1,0);di_state(1);
    void_input_mouse_button(0,1);void_input_mouse_button(0,0);di_buttons(0,0);di_state(1);
    assert(touch_submit(0,0,0,0,0,context));processed_button=1;void_input_pump();di_buttons(0,1);di_state(0);
    puts("PASS ordered DirectInput UI edges: fast submitted tap remains invisible before position ACK; DI history/immediate/VK state hold DOWN through 50 polls until original processed ACK; exactly one UP, held MOVE flags, tagged cancellation ledger, unpublished cancel and both physical ownership handoffs");
}
int main(void)
{
    quick_tap_and_ownership();drag_release_and_order();adjacent_moves_only();
    cancellation_and_invalid_scene();overflow_and_physical_ownership();console_and_legacy();
    scroll_probe_and_delivery();
    exact_position_and_consumed_ack();
    flash_processed_edges();
    ordered_directinput_edges();
    puts("PASS UI scroll intent: ordered anchor probe, no pre-classification DOWN, pending/invalid/console cases, VM-only bounded mailbox batches, busy retry, stale/reset/physical ownership and capacity");
    puts("PASS production UI input: VM ownership, logical cursor, SceneClient ACK, quick taps, ordered drag release, adjacent coalescing, stale/reset/invalid/timeout/overflow neutral, physical ownership, console and legacy paths; no guest VM mapped");
    return 0;
}
