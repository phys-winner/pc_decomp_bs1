/* Production reflection -> context generation -> JNI pointer -> native mouse
 * delivery. Original game InputKey dispatch is the explicit capture boundary. */
#include <stdint.h>
/* Normally these weak optional imports precede definitions in separate TUs.
 * Preserve that declaration order in this combined production fixture too. */
extern int void_touch_game_details(uint32_t out[2]) __attribute__((weak));
extern int void_bioshock_touch_enabled(void) __attribute__((weak));
extern int void_touch_weapon_request(uint32_t request) __attribute__((weak));
extern int void_touch_steer(float out[3], uint32_t *flags) __attribute__((weak));
extern int void_touch_game_read(uint32_t out[4]) __attribute__((weak));
extern int void_touch_gamepad_present(void) __attribute__((weak));
extern int void_touch_ui_mouse(uint32_t out[2]) __attribute__((weak));
extern int void_touch_ui_position(int x, int y) __attribute__((weak));
extern int void_touch_ui_button_state(void) __attribute__((weak));
extern int void_touch_ui_control(void) __attribute__((weak));
extern int void_touch_ui_scroll(int steps) __attribute__((weak));
extern int void_machine_is_owned(void) __attribute__((weak));
#define touch_epoch photo_scene_epoch
#define main original_movie_fixture_main
#include "test_bioshock_touch.c"
#undef main
#undef touch_epoch
#include "../android/host/input.c"

enum { PHOTO_OFFSET=2452, PHOTO_MASK=0x20000000u };
static unsigned photo_posts,photo_downs,photo_ups;
static uint64_t photo_clock;
uint32_t void_tick_ms(void) { return (uint32_t)++photo_clock; }
uint64_t void_monotonic_ns(void) { return ++photo_clock*1000000; }
uint32_t void_window_main(void) { return 1; }
void void_post_message(uint32_t window,uint32_t message,uint32_t value,uint32_t point)
{
    assert(window==1);(void)value;(void)point;++photo_posts;
    photo_downs+=message==0x201;photo_ups+=message==0x202;
}
static float pointer_remainder_x,pointer_remainder_y;
int void_android_display_size(int *width,int *height) { *width=2780;*height=1264;return 1; }
#ifdef _WIN32
#define JNIEXPORT
#define JNICALL
#define JNI_TRUE 1
#define JNI_FALSE 0
typedef void JNIEnv;
typedef void *jclass;
typedef float jfloat;
typedef int jboolean;
#endif
#include "photo_native_pointer_fixture.h"

static uint64_t observed(void)
{
    uint8_t keys[256];void_input_keyboard_snapshot(keys);return touch_cursor_context;
}
static void photo_metadata(uint32_t parent,uint32_t field,uint32_t offset,uint32_t mask)
{
    put(field+108,offset);put(field+148,mask);
    put(PAWN+44,class("DerivedPhotoPlayer",parent));
}
static void photo_rejects_bad_metadata(uint32_t parent,uint32_t field,uint32_t bool_kind)
{
    uint32_t out[4];
    const uint32_t bad[][2]={{PHOTO_OFFSET,0},{PHOTO_OFFSET,3},{44,PHOTO_MASK},
        {65536,PHOTO_MASK},{PHOTO_OFFSET+1,PHOTO_MASK},{UINT32_MAX,PHOTO_MASK}};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;++i){
        photo_metadata(parent,field,bad[i][0],bad[i][1]);read_context(out);assert(out[0]==1);
    }
    put(field+44,touch_class(ENGINE));photo_metadata(parent,field,PHOTO_OFFSET,PHOTO_MASK);
    read_context(out);assert(out[0]==1);put(field+44,bool_kind);
    photo_metadata(parent,field,PHOTO_OFFSET,PHOTO_MASK);read_context(out);assert(out[0]==3);
    /* A matching field whose mask metadata is outside readable memory must
     * fail closed, even though its name, type and offset are readable. */
    uint32_t truncated=TEST_SIZE-128;
    memcpy(GPTR(truncated),GPTR(field),128);put(parent+84,truncated);
    photo_metadata(parent,field,PHOTO_OFFSET,PHOTO_MASK);read_context(out);assert(out[0]==1);
    put(parent+84,field);
    photo_metadata(parent,field,PHOTO_OFFSET,PHOTO_MASK);
    /* The authoritative property may not be readable on a truncated object. */
    uint32_t short_pawn=TEST_SIZE-1024;
    memcpy(GPTR(short_pawn),GPTR(PAWN),1024);put(PLAYER+1300,short_pawn);
    read_context(out);assert(out[0]==1);put(PLAYER+1300,PAWN);
    /* The matching field name on an unrelated Pawn is not a ShockPlayer UI. */
    uint32_t unrelated=class("OtherPawn",MEM32(parent+56));
    uint32_t other=property(unrelated,"IsDisplayingPhoto",bool_kind,PHOTO_OFFSET,0);put(other+148,PHOTO_MASK);
    put(PAWN+44,unrelated);read_context(out);assert(out[0]==1);
    photo_metadata(parent,field,PHOTO_OFFSET,PHOTO_MASK);
}

int main(void)
{
    assert(original_movie_fixture_main()==0);owned=1;
    put(PLAYER+1300,PAWN);put(GUI+280,MOVIES);put(GUI+284,1);put(MOVIES,HUD);name(HUD,"HUD");
    uint32_t pawn_base=touch_class(PAWN),photo_class=class("ShockPlayer",pawn_base);
    uint32_t bool_kind=class("BoolProperty",0);
    next_property=0x600000; /* Large property arena must not overlap fixture names. */
    uint32_t field=property(photo_class,"IsDisplayingPhoto",bool_kind,PHOTO_OFFSET,0);
    /* Real UE classes may own more than256 fields. The desired bool is
     * deliberately behind300 other fields in its own declaring class. */
    for(unsigned i=0;i<300;++i)
        property(photo_class,"OtherPhotoField",bool_kind,PHOTO_OFFSET+4,0);
    assert(touch_typed_field(photo_class,"IsDisplayingPhoto","BoolProperty")==field);
    put(field+148,PHOTO_MASK);put(field+132,1); /* Other game's ABI is a decoy. */
    put(PAWN+44,class("DerivedPhotoPlayer",photo_class));put(PAWN+PHOTO_OFFSET,0x80000001u);
    void_input_set_screen_size(2780,1264);
    uint64_t gameplay=observed();assert((gameplay&3u)==2 && MEM32(CLIENT+212)==1);
    uint8_t pad[16];assert(void_input_touch_pad(0,pad));
    assert(touch_submit(.5f,.3f,3,4,0,gameplay));

    put(PAWN+PHOTO_OFFSET,0x80000001u|PHOTO_MASK);
    uint64_t grade=observed();assert(grade!=gameplay && (grade&3u)==1);
    assert(touch_game_snapshot[0]==3 && !void_touch_gamepad_present() && !MEM32(CLIENT+212));
    assert(!touch_connected && !touch_held && !touch_stick_x && !touch_stick_y && !touch_mouse_dx && !touch_mouse_dy);
    assert(!void_input_touch_pad(0,pad));assert(observed()==grade);
    assert(MEM32(PAWN+PHOTO_OFFSET)==(0x80000001u|PHOTO_MASK));
    assert(!ui_begin(grade) && !ui_button_waiting && !ui_waiting);

    const float invalid[][4]={{.1f,0,0,0},{0,.1f,0,0},{0,0,1,0},{0,0,0,1}};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;++i)
        assert(!touch_submit(invalid[i][0],invalid[i][1],invalid[i][2],invalid[i][3],0,grade));
    assert(!touch_submit(0,0,0,0,2,grade));assert(!touch_submit(0,0,0,0,1u<<7,grade));
    assert(!Java_org_bioshock_port_GameActivity_nativePointer(NULL,NULL,-.01f,.5f,0));
    assert(!Java_org_bioshock_port_GameActivity_nativePointer(NULL,NULL,1,.5f,0));
    const float targets[][2]={{.001f,.001f},{.5f,.5f},{.999f,.999f}};
    for(unsigned i=0;i<sizeof targets/sizeof *targets;++i){
        photo_posts=photo_downs=photo_ups=0;
        assert(Java_org_bioshock_port_GameActivity_nativePointer(NULL,NULL,targets[i][0],targets[i][1],0));
        assert(touch_submit(0,0,0,0,1,grade));
        int32_t dx,dy,dz;uint8_t buttons[8];void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(buttons[0]&0x80);
        assert(touch_submit(0,0,0,0,0,grade));void_input_pump();
        assert(photo_downs==1 && photo_ups==1 && photo_posts>=3 && !ui_button_waiting && !ui_waiting);
        void_input_mouse_snapshot(&dx,&dy,&dz,buttons);assert(!buttons[0] && !touch_held);
    }
    /* Touch never releases a physical mouse which remains held. */
    void_input_mouse_button(0,1);assert(touch_submit(0,0,0,0,1,grade));assert(touch_submit(0,0,0,0,0,grade));
    assert(physical_buttons[0] && mouse_buttons[0]);void_input_mouse_button(0,0);
    void_input_pump();photo_posts=photo_downs=photo_ups=0;
    assert(touch_submit(0,0,0,0,1,grade));
    /* FinishedPhotoDisplay clears this exact packed bit; observing it must
     * retire old grade input before gameplay resumes, without another Fire. */
    put(PAWN+PHOTO_OFFSET,0x80000001u);
    gameplay=observed();assert(gameplay!=grade && (gameplay&3u)==2 && MEM32(CLIENT+212)==1);
    assert(!touch_held && !touch_buttons[0] && !touch_mouse_latched[0]);void_input_pump();assert(!photo_downs);
    assert(!touch_submit(0,0,0,0,1,grade));assert(touch_submit(.2f,.1f,1,2,0,gameplay));
    assert(void_input_touch_pad(0,pad));

    put(PAWN+PHOTO_OFFSET,PHOTO_MASK);photo_rejects_bad_metadata(photo_class,field,bool_kind);
    /* A real Flash menu takes priority over the photo HUD when present. */
    put(GUI+284,2);put(MOVIES+4,PAUSE);name(PAUSE,"ComboLock");put(PAUSE+232,1);put(PAUSE+228,RANGES);
    uint32_t out[4];read_context(out);assert(out[0]==2);
    /* Preserve a user/physical controller option that this adapter did not set. */
    put(CLIENT+212,1);put(GUI+284,1);read_context(out);assert(out[0]==3 && MEM32(CLIENT+212)==1);
    put(PAWN+PHOTO_OFFSET,0);read_context(out);assert(out[0]==1 && MEM32(CLIENT+212)==1);
    put(PAWN+PHOTO_OFFSET,PHOTO_MASK);read_context(out);assert(out[0]==3 && MEM32(CLIENT+212)==1);

    /* Same low flags with unknown/loading snapshot must remain blocked. */
    grade=observed();assert((grade&3u)==1);
    touch_game_snapshot[0]=4;touch_game_active=1;
    assert(!touch_submit(0,0,0,0,1,touch_cursor_context));
    touch_game_snapshot[0]=3;touch_game_active=0;
    assert(!touch_submit(0,0,0,0,1,touch_cursor_context));
    puts("PASS production photo grade: typed inherited ShockPlayer BoolProperty after300 own-class fields/mask94, adjacent-bit and invalid metadata guards; actual classifier -> generation/reset -> nativePointer -> original PC LMB delivery without Flash ACK; resume, physical ownership/controller preservation; mode3-only neutral input and ordinary flags1 rejection");
    return 0;
}
