"""Exercise production event notifications, waits and the guest VM/GL handoff."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]


class EventHandoffTest(unittest.TestCase):
    def test_winapi_wakes_yield_after_unlock_and_preserve_wait_semantics(self):
        source = (ROOT / "android/host/threads.c").read_text(encoding="utf-8")
        methods = "\n".join(function(source, signature) for signature in (
            "int void_machine_is_owned(void)", "static int machine_contended(void)\n{",
            "static void machine_prioritize_audio_locked(void)", "static void serve_next_locked(void)",
            "static void yield_hook(void)", "static int object_ready(", "static void object_consume(",
            "static uint32_t wait_core(", "static uint32_t event_op(", "uint32_t void_event_op(",
            "WINAPI_EXPORT(K32, SetEvent,", "WINAPI_EXPORT(K32, ResetEvent,", "WINAPI_EXPORT(K32, PulseEvent,"))
        program = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
enum {VOID_OBJ_EVENT=1,VOID_OBJ_THREAD,VOID_OBJ_PROCESS,VOID_OBJ_TIMER,VOID_OBJ_SEMAPHORE,VOID_OBJ_MUTEX};
enum {ERROR_INVALID_HANDLE_=6,ERROR_ACCESS_DENIED_=5};
#define INFINITE_ UINT32_MAX
#define WAIT_TIMEOUT_ 258u
#define WAIT_OBJECT_0_ 0u
#define VLOG(...) ((void)0)
#define WINAPI_EXPORT(dll,name,args) static uint32_t api_##name(void)
#define ARG(index) argument
typedef struct {uint32_t words[32];double fp[8];uint64_t shadows[8];} void_regs;
typedef struct {uint32_t id;int depth;void_regs regs;int machine_cond,object_cond;uint32_t start;int first_wait;} void_thread;
typedef struct {int kind,signaled,manual_reset;uint64_t pulse_generation,pulse_claimed;int count;uint32_t owner;int recursion;} void_object;
typedef int (*void_wait_extra)(void *);
static void_thread setter={328,1,{{0},{0},{0}},0,0,0,0},waiter={260,1,{{0},{0},{0}},0,0,0,0},*t_self=&setter;
static void_regs live;
static void_object event={VOID_OBJ_EVENT,0,0,0,0,0,0,0};
static int t_machine_owned=1,mlock,wait_lock,mcond,wait_cond,m_locked,w_locked,lease=328;
static uint32_t machine_holder=328,argument=1,last_error,unreal_wait_count;
static uint32_t unreal_wait_handles[32],unreal_wait_signals[32];
static uint64_t ticket_next=2,ticket_serving,t_lock_wait_ns;
static unsigned handed,event_broadcasts,machine_broadcasts,save_calls,load_calls;
static int access_allowed=1,consume_during_handoff,wait_action,expected_set_state,expected_pulse;
static uint64_t now_ns(void){static uint64_t ticks;return ++ticks;}
static void_thread *void_thread_self(void){return t_self;}
static void void_set_last_error(uint32_t value){last_error=value;}
static void_object *void_handle_get(uint32_t h,int kind){assert(kind==VOID_OBJ_EVENT);return h==1?&event:NULL;}
static int event_handle_allows(uint32_t h,unsigned access){assert(h==1&&access==2);return access_allowed;}
static void suspend_checkpoint(void){}
static void pin_guest_thread(void){assert(machine_holder==t_self->id&&t_machine_owned&&!w_locked);}
static void void_regs_save(void_regs *saved){assert(t_machine_owned&&machine_holder==t_self->id&&!w_locked);*saved=live;++save_calls;}
static void void_regs_load(const void_regs *saved){assert(t_machine_owned&&machine_holder==t_self->id&&lease==(int)t_self->id&&!w_locked);live=*saved;++load_calls;}
void d3d_gl_thread_release(void) __attribute__((weak));
void d3d_gl_thread_acquire(void) __attribute__((weak));
void d3d_gl_thread_release(void){assert(t_machine_owned&&lease==(int)t_self->id&&!w_locked);lease=0;}
void d3d_gl_thread_acquire(void){assert(t_machine_owned&&machine_holder==t_self->id&&!lease&&!w_locked);lease=(int)t_self->id;}
static _Noreturn void void_fatal(const char *message){(void)message;abort();}
static void pthread_mutex_lock(int *lock){
    if(lock==&wait_lock){assert(!w_locked);w_locked=1;}
    else {assert(lock==&mlock&&!m_locked&&!w_locked);m_locked=1;}
}
static void pthread_mutex_unlock(int *lock){
    if(lock==&wait_lock){assert(w_locked);w_locked=0;}
    else {assert(lock==&mlock&&m_locked);m_locked=0;}
}
static void pthread_cond_broadcast(int *condition){
    if(condition==&wait_cond){assert(w_locked);++event_broadcasts;}
    else {assert(condition==&mcond&&m_locked&&!w_locked&&!machine_holder&&!t_machine_owned&&!lease);++machine_broadcasts;}
}
static int pthread_cond_wait(int *condition,int *lock);
static int pthread_cond_timedwait(int *condition,int *lock,const struct timespec *deadline){(void)deadline;return pthread_cond_wait(condition,lock);}
static int clock_gettime(int clock,struct timespec *time){(void)clock;time->tv_sec=0;time->tv_nsec=0;return 0;}
#define CLOCK_REALTIME 0
static void void_blocking_begin(void){
    void_regs_save(&t_self->regs);d3d_gl_thread_release();t_machine_owned=0;machine_holder=0;
}
static void void_blocking_end(void){
    machine_holder=t_self->id;t_machine_owned=1;d3d_gl_thread_acquire();void_regs_load(&t_self->regs);
}
typedef struct object_waiter{struct object_waiter *next;int *cond;void_object *const *objects;int count;int extra;} object_waiter;
static object_waiter *object_waiters;
static int legacy_wake(void){return 1;}
/* This fixture checks the release-before-check order (BIOSHOCK_WAIT_RELEASE_FIRST=1). */
static int wait_release_first(void){return 1;}
/* Machine statistics are a QA diagnostic, off in this fixture. */
static int machine_stats_on(void){return 0;}
static void machine_stat_add(uint64_t hold,uint64_t wait,int acquire,int yield){(void)hold;(void)wait;(void)acquire;(void)yield;abort();}
static uint64_t t_hold_start,machine_foreign_yields;
/* The QA hold sampler (BIOSHOCK_HOLD_SAMPLER=1) is off here too. */
static void hold_mark(int held){(void)held;}
static void machine_wake_turn_locked(void){pthread_cond_broadcast(&mcond);}
static int machine_wait_turn_locked(uint64_t ticket);
/* Machine loans (2026-11-20): no urgent producer and no loan in this fixture. */
#define URGENT_SLICE_NS 2000000ull
static int t_urgent,t_borrowing;
static uint64_t t_held_since;
static void machine_release(void){abort();}
static void machine_acquire(void){abort();}
typedef struct machine_waiter{struct machine_waiter *next;uint64_t ticket;int *cond;} machine_waiter;
static machine_waiter *machine_waiters,*urgent_waiter;
static uint64_t urgent_since,parked_ticket;
static int served_urgent_first;
static void wake_object_waiters_locked(const void_object *o){(void)o;pthread_cond_broadcast(&wait_cond);}
/* PRODUCTION */
static int machine_wait_turn_locked(uint64_t ticket){while(ticket!=ticket_serving)pthread_cond_wait(&mcond,&mlock);return 0;}
static void prepare_owner(void){
    t_self=&setter;t_machine_owned=1;machine_holder=setter.id;lease=(int)setter.id;ticket_next=2;ticket_serving=0;
}
static int pthread_cond_wait(int *condition,int *lock){
    if(condition==&mcond){
        assert(lock==&mlock&&m_locked&&!w_locked&&!machine_holder&&!t_machine_owned&&!lease);
        assert(ticket_next==3&&ticket_serving==1);
        if(expected_set_state)assert(event.signaled);
        if(expected_pulse)assert(!event.signaled&&event.pulse_generation);
        ++handed;memset(&live,0xa5,sizeof live);
        // A runnable consumer can take the real event while the setter is parked.
        if(consume_during_handoff){assert(object_ready(&event,waiter.id));object_consume(&event,waiter.id);}
        ++ticket_serving;
        return 0;
    }
    assert(condition==&wait_cond&&lock==&wait_lock&&w_locked&&!t_machine_owned&&!machine_holder&&!lease&&wait_action);
    w_locked=0;prepare_owner();expected_set_state=wait_action==1;expected_pulse=wait_action==2;
    if(wait_action==1){assert(api_SetEvent()==1);assert(api_ResetEvent()==1);}
    else assert(api_PulseEvent()==1);
    expected_set_state=expected_pulse=0;wait_action=0;
    d3d_gl_thread_release();t_machine_owned=0;machine_holder=0;t_self=&waiter;w_locked=1;
    return 0;
}
int main(void){
    // One overdue producer may pass the queue while normal tickets retain order.
    machine_waiter head={NULL,3,NULL}, second={&head,4,NULL}, audio={&second,5,NULL};
    urgent_waiter=machine_waiters=&audio;
    ticket_serving=3;
    urgent_since=now_ns();
    machine_prioritize_audio_locked();
    assert(head.ticket==3&&second.ticket==4&&audio.ticket==5&&!served_urgent_first);
    urgent_since=now_ns()-URGENT_SLICE_NS;
    machine_prioritize_audio_locked();
    assert(head.ticket==4&&second.ticket==5&&audio.ticket==3&&served_urgent_first);
    ticket_serving=4;audio.ticket=6;
    machine_prioritize_audio_locked();
    assert(head.ticket==4&&second.ticket==5&&audio.ticket==6&&!served_urgent_first);
    machine_waiters=urgent_waiter=NULL;
    ticket_serving=0;
    for(unsigned i=0;i<32;++i)live.words[i]=0x12340000+i;
    for(unsigned i=0;i<8;++i){live.fp[i]=0.25+i;live.shadows[i]=UINT64_C(0x1234567800000000)+i;}
    void_regs before=live;
    // The actual WinAPI notification yields only after its wait_lock is unlocked.
    expected_set_state=1;assert(api_SetEvent()==1);expected_set_state=0;
    assert(handed==1&&save_calls==1&&load_calls==1&&!memcmp(&live,&before,sizeof live));
    assert(machine_holder==setter.id&&t_machine_owned&&lease==(int)setter.id&&!m_locked&&!w_locked);
    // Repeated Set and every Reset retain their old event semantics without a new handoff.
    unsigned count=handed;prepare_owner();assert(api_SetEvent()==1&&handed==count);
    uint64_t generation=event.pulse_generation;assert(api_ResetEvent()==1&&handed==count);
    assert(!event.signaled&&event.pulse_generation==generation+1);
    assert(api_ResetEvent()==1&&handed==count&&event.pulse_generation==generation+1);
    // A real auto-reset consumer can use the signal before the SetEvent API returns.
    prepare_owner();consume_during_handoff=1;expected_set_state=1;
    assert(api_SetEvent()==1);consume_during_handoff=expected_set_state=0;assert(handed==count+1&&!event.signaled);
    // Internal callers remain within their host transaction even with a waiting guest.
    prepare_owner();count=handed;assert(void_event_op(1,1)==1&&void_event_op(1,0)==1&&void_event_op(1,2)==1&&handed==count);
    // No machine owner or no contender: event state changes, and yield is never entered.
    t_self=NULL;t_machine_owned=0;machine_holder=0;lease=0;assert(api_SetEvent()==1&&api_PulseEvent()==1&&handed==count);
    t_self=&setter;t_self->depth=0;assert(api_SetEvent()==1&&handed==count);t_self->depth=1;
    prepare_owner();ticket_next=1;event.signaled=0;assert(api_SetEvent()==1&&api_PulseEvent()==1&&handed==count);
    argument=99;assert(api_SetEvent()==0&&last_error==ERROR_INVALID_HANDLE_&&handed==count);
    argument=1;access_allowed=0;assert(api_PulseEvent()==0&&last_error==ERROR_ACCESS_DENIED_&&handed==count);access_allowed=1;
    // Execute the real wait_core for both reset styles, including Set->Reset before waiter resumes.
    for(unsigned manual=0;manual<=1;++manual)for(unsigned action=1;action<=2;++action){
        memset(&event,0,sizeof event);event.kind=VOID_OBJ_EVENT;event.manual_reset=(int)manual;
        t_self=&waiter;t_machine_owned=1;machine_holder=waiter.id;lease=(int)waiter.id;live=before;
        wait_action=(int)action;void_object *objects[]={&event};count=handed;
        assert(wait_core(1,objects,0,INFINITE_,NULL,NULL)==WAIT_OBJECT_0_);
        assert(handed==count+1&&event.pulse_generation==1&&!event.signaled);
        assert(event.pulse_claimed==(manual?0u:1u));
        assert(wait_core(1,objects,0,0,NULL,NULL)==WAIT_TIMEOUT_);
        assert(machine_holder==waiter.id&&t_machine_owned&&lease==(int)waiter.id&&!memcmp(&live,&before,sizeof live));
    }
    assert(event_broadcasts>machine_broadcasts&&machine_broadcasts==handed);
    puts("PASS production event boundary: owned/contended wake only, wait-lock released before real VM/GL handoff, full register restoration, internal/no-owner guards, exact API results and manual/auto Set-Reset/Pulse wait generations");
}
'''.replace("/* PRODUCTION */", methods)
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-event-handoff-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe, obj, binary = folder / "check.c", folder / "check.obj", folder / "check.exe"
            probe.write_text(program, encoding="utf-8")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
                result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
