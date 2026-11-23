package org.bioshock.port;

import java.util.ArrayDeque;

/**
 * One original-UI pointer. A finger moving over a menu only points at it, as a
 * mouse would: the original highlight follows, and in lists the row under the
 * finger is selected without being activated. A tap clicks, or changes the
 * option row under it. A sideways drag that starts on a slider adjusts it.
 */
final class UiTouchInput {
    /** Native probe flags: a tap clicks, list rows follow the finger, a drag adjusts, a tap changes the option. */
    static final int TAP = 1, ROWS = 2, DRAG = 4, OPTION = 8, FREE_DRAG = 16;
    /**
     * List requests beside -8..8 rows: from 64 until the gesture retires the
     * game highlights the row under the moving cursor (held beyond the list,
     * it steps); 65 taps the option row under the cursor.
     */
    static final int HOVER = 64, OPTION_TAP = 65;
    interface Sink {
        boolean begin(float x,float y);
        int control(); // 0 awaiting original hover, positive probe flags, negative retired.
        boolean pointer(float x,float y);
        boolean button(boolean down);
        boolean scroll(int request); // rows or a list request; 0 retires the gesture.
        void reset();
    }
    private final Sink sink;
    private static final int MAX_PENDING=16;
    private static final class Gesture {
        final float startX,startY;
        float x,y,crossX,crossY;
        boolean ended,moved,dragChecked,dragging,pressed;
        Gesture(float x,float y){startX=this.x=x;startY=this.y=y;}
    }
    private final ArrayDeque<Gesture> pending=new ArrayDeque<>();
    private Gesture current,contact;
    private boolean classified;
    private int flags;
    private float slop=8;

    UiTouchInput(Sink sink) { this.sink=sink; }
    void configure(float slop,float row) {
        if(Float.isFinite(slop)&&slop>0)this.slop=slop;
    }
    boolean active() { return current!=null; }
    boolean waiting() { return active()&&!classified; }
    /** True while a gesture still needs another native or UI step. */
    boolean polling() { return active(); }
    boolean begin(float x,float y) {
        if(contact!=null||pending.size()>=MAX_PENDING||!Float.isFinite(x)||!Float.isFinite(y))return false;
        Gesture next=new Gesture(x,y);
        if(current!=null) {
            // Android already released the earlier finger; its native hover
            // acknowledgement may still be pending on a slow Flash frame.
            pending.addLast(next);contact=next;return true;
        }
        if(!sink.begin(x,y))return false;
        current=contact=next;classified=false;flags=0;
        poll();return active();
    }
    void move(float x,float y) {
        if(contact==null||!Float.isFinite(x)||!Float.isFinite(y))return;
        point(contact,x,y);
        advance();
    }
    void end(float x,float y) {
        if(contact==null)return;
        if(Float.isFinite(x)&&Float.isFinite(y))point(contact,x,y);
        contact.ended=true;contact=null;advance();
    }
    private void point(Gesture gesture,float x,float y) {
        gesture.x=x;gesture.y=y;
        if(!gesture.moved&&Math.hypot(x-gesture.startX,y-gesture.startY)>slop) {
            gesture.moved=true;gesture.crossX=x;gesture.crossY=y;
        }
    }
    void poll() {
        if(!active())return;
        if(!classified) {
            int kind=sink.control();
            if(kind<0){cancel();return;}
            if(kind==0)return;
            flags=kind;classified=true;
            // Rows follow the finger from now on, in the game's own Tick.
            if((flags&ROWS)!=0&&!sink.scroll(HOVER)){cancel();return;}
        }
        advance();
    }
    private void advance() {
        if(!classified||!active())return;
        Gesture gesture=current;
        if(gesture.moved&&!gesture.dragChecked) {
            gesture.dragChecked=true;
            float dx=gesture.crossX-gesture.startX,dy=gesture.crossY-gesture.startY;
            // Maps, pipe tiles and gene-bank slots also drag vertically.
            if((flags&DRAG)!=0&&((flags&FREE_DRAG)!=0||Math.abs(dx)>Math.abs(dy))) {
                if(!sink.pointer(gesture.startX,gesture.startY)||!sink.button(true)){cancel();return;}
                gesture.dragging=true;
            }
        }
        if(!sink.pointer(gesture.x,gesture.y)){cancel();return;}
        if(!gesture.moved&&(flags&TAP)!=0&&(flags&OPTION)==0) {
            if(!gesture.ended)return;
            if(!gesture.pressed) {
                if(!sink.button(true)){cancel();return;}
                gesture.pressed=true;
                return;
            }
            if(!sink.button(false)){cancel();return;}
        } else if(!gesture.ended)return;
        if(!gesture.ended)return;
        if(gesture.dragging) {
            if(!sink.button(false)){cancel();return;}
        } else if(!gesture.moved&&(flags&OPTION)!=0) {
            if(!sink.scroll(OPTION_TAP)){cancel();return;}
        }
        if(!sink.scroll(0)){cancel();return;}
        // Native queued taps may still await UI acknowledgement after Android UP.
        current=pending.pollFirst();classified=false;flags=0;
        if(current!=null&&!sink.begin(current.startX,current.startY))cancel();
    }
    void cancel() {
        if(active())sink.reset();
        current=contact=null;pending.clear();classified=false;flags=0;
    }
}
