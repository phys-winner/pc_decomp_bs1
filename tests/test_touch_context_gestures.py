"""Run real gameplay pointer routing together with the production TouchInput."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests import test_touch_controls as existing

ROOT = Path(__file__).resolve().parents[1]


class TouchContextGesturesTest(unittest.TestCase):
    def test_center_actions_ammo_swipe_and_cancel(self):
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        method = existing.TouchControlsTest._method
        actual = "\n".join(method(source, signature) for signature in (
            "private void pointerDown(", "private void pointerMove(", "private void pointerUp(",
            "private int utilitySlot(", "private void releaseInput("))
        callbacks = "\n".join(method(source, "private final Runnable " + name) + ";" for name in ("centerHold", "ammoHold"))
        program = r'''
package org.bioshock.port;
import java.util.ArrayList;
class ViewConfiguration {
    static ViewConfiguration get(Object context){return new ViewConfiguration();}
    int getScaledTouchSlop(){return 8;}
}
class HapticFeedbackConstants {static final int VIRTUAL_KEY=1,LONG_PRESS=2;}
class TouchLayout {static final int FIRE=0,FIRE2=1,USE=2,PAUSE=3;}
public class ContextGestureCheck {
    static final int MORE=-1,SEATS=-6,WEAPON=-5,PIE_ACTIONS=0,PIE_ABILITIES=1;
    static final int CONTEXT_USE=1<<4,CONTEXT_HACK=1<<5,CONTEXT_RESCUE=1<<6,CONTEXT_HARVEST=1<<7,
        CONTEXT_CINEMATIC=1<<12,PICKER_ACTIVE=1<<21;
    static final int HINT_CROUCH=16,HINT_VITALS=1024;
    static final long VITALS_HOLD_MS=320,CENTER_HOLD_MS=400;
    static class GameActivity {
        static long context=0x100000002L;
        static long nativeTouchContext(){return context;}
        static boolean nativeTouchWeapon(int a,int b,long c){return true;}
        static boolean nativePointer(float x,float y,boolean down){return true;}
        static boolean nativeTouch(float a,float b,float c,float d,int held,long context){return true;}
    }
    static class Capture implements TouchInput.Sink {
        int held,use,hack,ammo,resets;float mx,my,look;
        public void submit(float x,float y,float dx,float dy,int mask){
            int pressed=mask&~held;
            if((pressed&TouchInput.USE)!=0)++use;
            if((pressed&TouchInput.HACK)!=0)++hack;
            if((pressed&TouchInput.AMMO_DOWN)!=0)++ammo;
            held=mask;mx=x;my=y;look+=Math.abs(dx)+Math.abs(dy);
        }
        public boolean stick(float x,float y){return true;}
        public void reset(){held=0;mx=my=0;++resets;}
    }
    static class UiInput {
        boolean active(){return false;}void move(float x,float y){}void end(float x,float y){}void cancel(){}
    }
    final Capture capture=new Capture();final TouchInput input=new TouchInput(capture);
    final UiInput uiInput=new UiInput();final ArrayList<Runnable> pending=new ArrayList<>();
    final int[] utility={-1,-1,-1},utilityIds=new int[3];
    final Runnable uiPoll=()->{},weaponHold=()->{},vitalsHold=()->{};
    boolean playing=true,touchEnabled=true,editing,pieOpen,authoritative=true,centerHeld,weaponPicker,gyroActive,vitalsControl=true;
    int profile,vehicleFlags=CONTEXT_USE|CONTEXT_HACK,centerId=-1,ammoId=-1,vitalsId=-1,
        swallowed=-1,pieId=-1,pieSelected=-1,scrubId=-1,weaponId=-1,scoresId=-1,menuId=-1,
        editId=-1,pinchId=-1,toolId=-1,toolPressed=-1,weaponSlots;
    float dp=1,ammoX,ammoY,centerX,centerY,vitalsX,vitalsY;
    long cursorContext=GameActivity.context,centerContext,centerTime,ammoTime,sensorTime;
    Object getContext(){return this;}int getWidth(){return 1000;}int getHeight(){return 600;}
    void postDelayed(Runnable r,long delay){assert delay>0;pending.add(r);}
    void removeCallbacks(Runnable r){pending.removeIf(value->value==r);}
    void haptic(int type){}void invalidate(){}void stopGyro(){}void performClick(){}
    int hit(float x,float y){return x==50&&y==50?TouchLayout.PAUSE:y==500?(x<400?TouchLayout.FIRE2:TouchLayout.FIRE):x==770&&y==400?TouchLayout.USE:-1;}
    int action(int id){return id==TouchLayout.PAUSE?TouchInput.PAUSE:id==TouchLayout.USE?TouchInput.USE:TouchInput.FIRE;}
    boolean pieDown(int id,float x,float y){return false;}
    void openPie(int id,float x,float y,int kind,int control){}void pieMove(float x,float y){}void pieUp(float x,float y){}
    void weaponDown(int id,float x,float y,long time){}void weaponMove(float x,float y){}void weaponUp(float x,float y,long time){}
    void scoresDown(int id,float x,float y,long time){}void scoresUp(float x,float y,long time){}
    boolean scrubStart(int id,float x,boolean b){return false;}void scrubMove(float x){}void scrubUp(float x,float y,long time){}
    boolean vitalsHit(float x,float y){return false;}boolean weaponBarHit(float x,float y){return false;}
    boolean ammoHit(float x,float y){return x>=800&&y<=100;}
    boolean movementZone(float x,float y){return x<400;}
    void offer(int bit,String en,String ru){}void markHint(int bit){}
    void uiDown(int id,float x,float y){}boolean inside(int control,float x,float y){return hit(x,y)==control;}
    int pauses;
    void activate(int control){assert control==TouchLayout.PAUSE;++pauses;}void syncContext(){}
    /* CALLBACKS */
    /* PRODUCTION */
    void runPending(){ArrayList<Runnable> ready=new ArrayList<>(pending);pending.clear();for(Runnable r:ready)r.run();}
    void clean(){releaseInput();capture.use=capture.hack=capture.ammo=0;capture.look=0;playing=touchEnabled=true;editing=false;GameActivity.context=cursorContext=0x100000002L;vehicleFlags=CONTEXT_USE|CONTEXT_HACK;}
    void neutral(){assert !input.hasContacts()&&!input.moving()&&input.held()==0&&capture.held==0&&capture.mx==0&&capture.my==0;}
    public static void main(String[]args){
        ContextGestureCheck g=new ContextGestureCheck();g.input.acceleration(false);
        // A center tap is a single Use; the delayed hold must have been cancelled.
        g.pointerDown(1,500,300,0);g.pointerUp(1,502,301,120);g.runPending();
        assert g.capture.use==1&&g.capture.hack==0&&g.capture.ammo==0;g.neutral();g.clean();
        // Holding sends Hack once; release cannot add Use.
        g.pointerDown(2,500,300,1000);g.runPending();g.pointerUp(2,500,300,1450);
        assert g.capture.hack==1&&g.capture.use==0&&g.capture.ammo==0;g.neutral();g.clean();
        // A look swipe permanently cancels the action even if it ends at its origin.
        g.pointerDown(3,500,300,2000);g.pointerMove(3,540,310,2050);g.pointerMove(3,500,300,2100);
        g.runPending();g.pointerUp(3,500,300,2200);
        assert g.capture.look>0&&g.capture.use==0&&g.capture.hack==0;g.neutral();g.clean();
        // A context transition already synchronized by Java cancels both paths.
        g.pointerDown(4,500,300,3000);++g.cursorContext;g.runPending();g.pointerUp(4,500,300,3100);
        assert g.capture.use==0&&g.capture.hack==0;g.neutral();g.clean();
        // Native context may change between the 100 ms Java context polls.
        g.pointerDown(5,500,300,4000);++GameActivity.context;g.runPending();g.pointerUp(5,500,300,4120);
        assert g.capture.use==0&&g.capture.hack==0:"stale native context dispatched action";g.neutral();g.clean();
        // CANCEL/focus loss removes a still-pending long press and neutralizes all input.
        g.pointerDown(6,500,300,5000);Runnable stale=g.centerHold;g.releaseInput();stale.run();g.pointerUp(6,500,300,5100);
        assert g.capture.use==0&&g.capture.hack==0;g.neutral();g.clean();
        // Ammo gestures begin at the right HUD: crossing slop must still own movement.
        g.pointerDown(7,850,50,6000);assert g.ammoId==7&&!g.input.hasContacts();
        g.pointerMove(7,900,70,6050);
        assert g.ammoId==-1&&g.input.moving()&&g.input.anchorX()==850&&g.input.anchorY()==50;
        assert g.input.moveX()>0&&g.input.moveY()<0&&g.capture.look==0;
        g.runPending();g.pointerMove(7,910,80,6090);g.pointerUp(7,910,80,6120);
        assert g.capture.ammo==0&&g.capture.use==0&&g.capture.hack==0;g.neutral();g.clean();
        // An unchanged ammo hold keeps the existing next-ammo action, only once.
        g.pointerDown(8,850,50,7000);g.runPending();g.pointerUp(8,850,50,7450);g.runPending();
        assert g.capture.ammo==1&&g.capture.use==0&&g.capture.hack==0;g.neutral();g.clean();
        // No target means a center touch is just a camera contact.
        g.vehicleFlags=0;g.pointerDown(9,500,300,8000);g.runPending();g.pointerUp(9,500,300,8100);
        assert g.capture.use==0&&g.capture.hack==0;g.neutral();
        g.clean();g.vehicleFlags=CONTEXT_USE|CONTEXT_HACK|CONTEXT_RESCUE|CONTEXT_HARVEST;
        g.pointerDown(10,500,300,9000);g.runPending();g.pointerUp(10,500,300,9500);
        assert g.capture.use==0&&g.capture.hack==0;g.neutral();
        g.clean();g.pointerDown(11,500,300,10000);g.vehicleFlags|=CONTEXT_RESCUE;g.runPending();g.pointerUp(11,500,300,10100);
        assert g.capture.use==0&&g.capture.hack==0;g.neutral();
        g.clean();g.pointerDown(12,300,500,11000);g.pointerDown(13,900,500,11000);
        g.pointerMove(12,350,500,11040);g.pointerMove(13,950,500,11040);
        assert g.capture.held==TouchInput.FIRE&&g.capture.mx>0&&g.capture.look>0;
        g.pointerUp(13,950,500,11070);assert g.capture.held==TouchInput.FIRE;
        g.pointerUp(12,350,500,11070);g.neutral();
        g.pointerDown(14,100,300,12000);g.pointerDown(15,300,500,12000);
        float before=g.capture.look;g.pointerMove(15,380,500,12040);
        assert g.capture.held==TouchInput.FIRE&&g.input.anchorX()==100&&g.capture.look==before;
        g.pointerUp(15,380,500,12070);g.pointerUp(14,100,300,12070);g.neutral();
        g.clean();g.vehicleFlags=0;g.pointerDown(16,770,400,13000);g.pointerUp(16,770,400,13060);
        assert g.capture.use==1;g.neutral();
        g.clean();g.vehicleFlags|=CONTEXT_CINEMATIC;
        g.pointerDown(20,900,500,14000);g.pointerMove(20,950,550,14040);g.pointerUp(20,950,550,14070);
        g.pointerDown(21,500,300,14100);g.runPending();g.pointerUp(21,500,300,14550);
        assert g.capture.use==0&&g.capture.hack==0&&g.capture.look==0;g.neutral();
        g.pointerDown(22,50,50,14600);g.pointerUp(22,50,50,14660);assert g.pauses==1;g.neutral();
        g.vehicleFlags&=~CONTEXT_CINEMATIC;
        g.pointerDown(23,900,500,15000);assert g.capture.held==TouchInput.FIRE;
        g.pointerUp(23,900,500,15060);g.neutral();
        System.out.println("PASS production pointer routing: center action and cancellation, HUD ammo movement, simultaneous dual Fire, independent movement and look");
    }
}
'''.replace("/* CALLBACKS */", callbacks).replace("/* PRODUCTION */", actual)
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        with tempfile.TemporaryDirectory(prefix="touch-context-test-") as temporary:
            path = Path(temporary) / "ContextGestureCheck.java"
            path.write_text(program, encoding="utf-8")
            for command in ([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary,
                             existing.touch_input_source(temporary), path],
                            [java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.ContextGestureCheck"]):
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
