"""iPhone and iPad touch layouts: wide-screen anchors, scale, and hit targets."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from tests import test_touch_controls

ROOT = Path(__file__).resolve().parents[1]


class IosTouchLayoutTest(unittest.TestCase):
    def test_place_and_hit(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = ROOT / "android/app/src/org/bioshock/port/IosTouchLayout.java"
        program = r'''
package org.bioshock.port;
public class IosLayoutCheck {
    static int index(int pad, int role, int action) {
        for (int i = 0; i < IosTouchLayout.count(pad); ++i) {
            IosTouchLayout.Spec spec = IosTouchLayout.spec(pad, i);
            if (spec.role == role && spec.action == action) return i;
        }
        throw new AssertionError(role + ":" + action);
    }
    static float width(float[] rects, int i) { return rects[i * 4 + 2] - rects[i * 4]; }
    static void sprite(int scheme, int role, int action, String normal, String pressed) {
        IosTouchLayout.Spec spec = IosTouchLayout.spec(scheme, index(scheme, role, action));
        if (!normal.equals(spec.sprite(false)) || !pressed.equals(spec.sprite(true)))
            throw new AssertionError("Original iOS sprite state mismatch: " + action);
    }
    static void inside(float[] rects, float right, float bottom) {
        for (int i = 0; i < rects.length; i += 4) {
            if (!(rects[i] >= 0 && rects[i + 1] >= 0 && rects[i + 2] <= right + .001f && rects[i + 3] <= bottom + .001f
                && rects[i + 2] > rects[i] && rects[i + 3] > rects[i + 1]))
                throw new AssertionError(i + " " + rects[i] + "," + rects[i + 1] + " " + rects[i + 2] + "," + rects[i + 3]);
        }
    }
    public static void main(String[] args) {
        int phone = IosTouchLayout.PHONE, pad = IosTouchLayout.PAD;
        for (int scheme : new int[]{phone, pad}) {
            sprite(scheme, IosTouchLayout.PULSE, IosTouchLayout.PAUSE, "pause", "pause_down");
            sprite(scheme, IosTouchLayout.PULSE, IosTouchLayout.MAP, "map", "map_down");
            sprite(scheme, IosTouchLayout.PULSE, IosTouchLayout.NEXT, "next", "next_down");
            sprite(scheme, IosTouchLayout.PULSE, IosTouchLayout.RELOAD, "reload", "reload_down");
            sprite(scheme, IosTouchLayout.PULSE, IosTouchLayout.RESCUE, "gather", "gather_down");
            sprite(scheme, IosTouchLayout.HOLD, IosTouchLayout.DUCK, "crouch", "crouch");
            sprite(scheme, IosTouchLayout.PULSE, IosTouchLayout.USE, "use", "use_down");
            int use = index(scheme, IosTouchLayout.PULSE, IosTouchLayout.USE);
            index(scheme, IosTouchLayout.HOLD, IosTouchLayout.JUMP);
            sprite(scheme, IosTouchLayout.PULSE, IosTouchLayout.LOG, "diary", "diary_down");
            index(scheme, IosTouchLayout.PULSE, IosTouchLayout.AIM);
            assert IosTouchLayout.MAP == (1 << 12) : "Map must use M, never F9 quick load";
            boolean tablet = scheme == pad;
            float[] rects = IosTouchLayout.place(scheme, 0, 0, tablet ? 1024 : 568, tablet ? 768 : 320,
                    1, 0, tablet ? 768 : 320);
            int k = 4 * use;
            assert rects[k+2]-rects[k] >= (tablet?97:58);
            assert rects[k+3]-rects[k+1] >= (tablet?96:58);
            assert IosTouchLayout.hit(scheme, rects, null, rects[k] + 4, rects[k + 1] + 4, 0) != use;
            assert IosTouchLayout.hit(scheme, rects, null, (rects[k] + rects[k + 2]) / 2, (rects[k + 1] + rects[k + 3]) / 2, 0) == use;
        }
        int pause = index(phone, IosTouchLayout.PULSE, IosTouchLayout.PAUSE);
        int fire = index(phone, IosTouchLayout.HOLD, IosTouchLayout.FIRE);
        int stick = index(phone, IosTouchLayout.MOVE, 0);
        int reload = index(phone, IosTouchLayout.PULSE, IosTouchLayout.RELOAD);
        int decoration = -1;
        for (int i = 0; i < IosTouchLayout.count(phone); ++i)
            if (IosTouchLayout.spec(phone, i).role == IosTouchLayout.DRAW) decoration = i;
        if (decoration < 0) throw new AssertionError("Decorative bounds");
        float[] fitted = IosTouchLayout.place(phone, 0, 0, 2340, 1080, 1, 0, 1080);
        float[] small = IosTouchLayout.place(phone, 0, 0, 2340, 1080, 0.5f, 0, 1080);
        float[] large = IosTouchLayout.place(phone, 0, 0, 2340, 1080, 2, 0, 1080);
        inside(fitted, 2340, 1080); inside(small, 2340, 1080); inside(large, 2340, 1080);
        if (!(fitted[stick * 4] < 200)) throw new AssertionError("stick not on the left");
        if (!(fitted[reload * 4 + 2] > 2000)) throw new AssertionError("reload not on the right");
        if (!(fitted[pause * 4] < 220)) throw new AssertionError("pause not in the left corner");
        if (!(width(small, fire) < width(fitted, fire) && width(large, fire) > width(fitted, fire)))
            throw new AssertionError("scale does not change button size");
        float[] tablet = IosTouchLayout.place(pad, 0, 0, 2340, 1080, 1, 0, 1080);
        inside(tablet, 2340, 1080);
        for (int scheme : new int[]{phone, pad}) for (int width : new int[]{1400, 2424})
            for (float zoom : new float[]{.5f, 1f, 2f}) {
            float[] controls = IosTouchLayout.place(scheme, 25, 15, width, 1040, zoom, 220, 760);
            float[] beforePickup=IosTouchLayout.place(scheme,25,15,width,1040,zoom,15,1040);
            for(int i=0;i<16;++i)assert controls[i]==beforePickup[i]:"HUD/weapon moved a left control";
            assert Math.abs(controls[12]-(25+IosTouchLayout.spec(scheme,3).x*Math.min((width-25)/(scheme==pad?1024f:568f),1025f/(scheme==pad?576f:320f))))<.001f;
            assert controls[13]>controls[9]:"Pause must stay below Map";
            for (int i = 0; i < 12; ++i) {
                if (IosTouchLayout.spec(scheme, i).role == IosTouchLayout.DRAW) continue;
                float x = (controls[i * 4] + controls[i * 4 + 2]) * .5f;
                float y = (controls[i * 4 + 1] + controls[i * 4 + 3]) * .5f;
                if (IosTouchLayout.hit(scheme, controls, null, x, y, 24) != i)
                    throw new AssertionError("Unreachable control " + i + " in scheme " + scheme + " at " + zoom);
            }
        }
        float[] referencePad=IosTouchLayout.place(pad,0,0,1200,675,1,0,675);
        assert Math.abs((referencePad[24]+referencePad[26])*.5f-1115.625f)<.01f;
        assert Math.abs((referencePad[25]+referencePad[27])*.5f-387.890625f)<.01f;
        assert Math.abs((referencePad[32]+referencePad[34])*.5f-1013.671875f)<.01f;
        assert Math.abs((referencePad[33]+referencePad[35])*.5f-582.421875f)<.01f;
        float[] referencePhone=IosTouchLayout.place(phone,0,0,738,416,1,0,416);
        assert Math.abs((referencePhone[24]+referencePhone[26])*.5f-645.1f)<2;
        assert Math.abs((referencePhone[25]+referencePhone[27])*.5f-142.5f)<2;
        int padCrouch = index(pad, IosTouchLayout.HOLD, IosTouchLayout.DUCK);
        // The original right cluster is 133x175 on phone and 200x346 on iPad.
        // Jump/aim may use free space, but must never reduce these original sizes.
        for (int scheme : new int[]{phone, pad}) for (int screenW : new int[]{568, 1024, 1400, 1920, 2424})
            for (int screenH : new int[]{320, 720, 1080})
            for (float zoom : new float[]{.5f, .8f, 1, 1.3f, 1.5f, 1.75f, 2}) {
            boolean isPad = scheme == pad;
            float safeW = screenW - 25, safeH = screenH - 15;
            float fit = Math.min(safeW / (isPad ? 1024 : 568), safeH / (isPad ? 576 : 320));
            float expectedSize = Math.min(fit * zoom,
                Math.min(safeW * .48f / (isPad ? 200 : 133), safeH / (isPad ? 346 : 175)));
            float[] controls = IosTouchLayout.place(scheme, 25, 15, screenW, screenH, zoom, 15, screenH);
            inside(controls, screenW, screenH);
            float[] hud = IosTouchLayout.place(scheme,25,15,screenW,screenH,zoom,15+safeH*.23f,15+safeH*.72f);
            for(int k=0;k<4;++k)if(controls[k]!=hud[k])throw new AssertionError("HUD or weapon moved/resized stick");
            if(controls[3] < screenH-safeH*.12f)throw new AssertionError("Stick left its lower anchor");
            if(screenH-controls[43] < safeH*.13f)throw new AssertionError("Jump is too close to bottom edge");
            for (int i = 4; i < 10; ++i) {
                IosTouchLayout.Spec spec = IosTouchLayout.spec(scheme, i);
                if (Math.abs(width(controls, i) - spec.w * expectedSize) > .001f ||
                    Math.abs(controls[i*4+3] - controls[i*4+1] - spec.h * expectedSize) > .001f)
                    throw new AssertionError("Original iOS control shrank: " + scheme + ":" + i + " at " + zoom);
            }
            for (int i = 10; i < IosTouchLayout.count(scheme); ++i) {
                int a = i * 4;
                float x = (controls[a] + controls[a+2]) * .5f, y = (controls[a+1] + controls[a+3]) * .5f;
                if (IosTouchLayout.hit(scheme, controls, null, x, y, 24) != i)
                    throw new AssertionError("Added action unreachable: " + scheme + ":" + i);
                for (int j = 0; j < IosTouchLayout.count(scheme); ++j) {
                    if (i == j) continue;
                    int b = j * 4;
                    if (Math.min(controls[a+2], controls[b+2]) - Math.max(controls[a], controls[b]) > .001f &&
                        Math.min(controls[a+3], controls[b+3]) - Math.max(controls[a+1], controls[b+1]) > .001f)
                        throw new AssertionError("Added action overlaps " + scheme + ":" + i + "/" + j + " at " + zoom);
                }
            }
        }
        for(int scheme:new int[]{phone,pad}){
            float zoom=IosTouchLayout.scaleForDensity(scheme,2424,1080,1,126);
            float[] c=IosTouchLayout.place(scheme,0,0,2424,1080,zoom,250,760);
            assert width(c,2)>=126 && width(c,3)>=126 && width(c,8)>=126 && width(c,10)>=125.99f && width(c,14)>=126;
            assert width(c,15)*(IosTouchLayout.useButtonFraction(scheme,2)-IosTouchLayout.useButtonFraction(scheme,0))>=125.99f;
            assert c[51]-c[49]>=125.99f && c[55]-c[53]>=125.99f;
        }
        int phoneCrouch = index(phone, IosTouchLayout.HOLD, IosTouchLayout.DUCK);
        if (!(width(tablet, padCrouch) < width(fitted, phoneCrouch)))
            throw new AssertionError("iPad crouch should be smaller on a phone");
        float px = (fitted[pause * 4] + fitted[pause * 4 + 2]) * 0.5f;
        float py = (fitted[pause * 4 + 1] + fitted[pause * 4 + 3]) * 0.5f;
        if (IosTouchLayout.hit(phone, fitted, null, px, py, 0) != pause) throw new AssertionError("pause hit");
        float dx = (fitted[decoration * 4] + fitted[decoration * 4 + 2]) * 0.5f;
        float dy = (fitted[decoration * 4 + 1] + fitted[decoration * 4 + 3]) * 0.5f;
        if (IosTouchLayout.hit(phone, fitted, null, dx, dy, 0) != -1) throw new AssertionError("Decorative bounds own no input");
        float[] tight = IosTouchLayout.place(phone, 10, 20, 110, 90, 2, 20, 90);
        inside(tight, 110, 90);
        for (int i = 0; i < tight.length; i += 4)
            if (tight[i] < 10 || tight[i + 1] < 20) throw new AssertionError("left the safe rect");
        System.out.println("PASS iOS touch layout: 210 legacy-size cases, separate Jump/Aim without overlap, edge anchors, scale, hit targets");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / "org/bioshock/port").mkdir(parents=True)
            check = folder / "org/bioshock/port/IosLayoutCheck.java"
            check.write_text(program, encoding="utf-8")
            layout = folder / "org/bioshock/port/IosTouchLayout.java"
            layout.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
            subprocess.run([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8",
                            "-d", temporary, str(layout), str(check)], check=True)
            subprocess.run([str(java / "java.exe"), "-ea", "-cp", temporary, "org.bioshock.port.IosLayoutCheck"], check=True)

    def test_sprite_press_and_release(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        java_root = ROOT / "android/app/src/org/bioshock/port"
        controls = (java_root / "GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(test_touch_controls.TouchControlsTest._method(controls, name) for name in (
            "private void clearIosPointers(", "private boolean iosPressed(",
            "private int iosSlot(", "private int iosFree(",
            "private boolean iosVisible(", "private boolean[] iosAllow(",
            "private void iosDown(", "private void iosUp(", "private int iosSelectorHit(", "private void iosPlace(", "float hudControlClearStage(",
            "private static String[] iosEquipmentSprites(", "private String iosArtwork("))
        program = r'''
package org.bioshock.port;
class GameActivity {
    static int phase,slot;static long choices;
    static boolean nativeTouchWeapon(int p,int s,long context){phase=p;slot=s;return true;}
    static long nativeTouchContext(){return 2;}
    static long nativeTouchIosChoices(){return choices;}
}
class HapticFeedbackConstants { static final int VIRTUAL_KEY = 1; }
class TouchLayout { static final int FIRE=0, ALT=1, WEAPON=2; static final float VITALS_LEFT=40;static final float[] VITALS_STAGE={36,16,312,92}; }
class TouchInput {
    static final int MOVEMENT=1, BUTTON=2, LOOK=3, USE=16, FIRE=1, ALT=2, RELOAD=1<<23, NEXT=64;
    int pulsed,held;
    void pulse(int action) { pulsed=action; }
    boolean down(int id,float x,float y,int bits,int role,long time) { held=bits;return true; }
    boolean owns(int id) { return held!=0; }
    void up(int id,float x,float y,long time) { held=0; }
}
public class IosSpriteCheck {
    static final int CONTEXT_CINEMATIC=4096, CONTEXT_REROLL=1<<8, IOS_WEAPON_SHIFT=23,
        IOS_ABILITY_SHIFT=16, IOS_ACTIVE_PLASMID=1<<28, IOS_CROUCHED=1<<29;
    static final String[] IOS_WEAPON_SPRITES=iosEquipmentSprites("weapon_",16), IOS_ABILITY_SPRITES=iosEquipmentSprites("ability_",32);
    final int[] iosIds=new int[8],iosHits=new int[8];
    final TouchInput input=new TouchInput();
    final boolean[] iosSwitchOnRelease=new boolean[8];
    int iosNextId=-1,iosSelectorRow,iosSelectorItems,iosSelectorSelected=-1;
    boolean iosNextHeld,iosSelectorOpen;
    float[] iosSelectorRects=new float[12];
    long cursorContext=2;
    Runnable iosNextHold=/* HOLD */;
    boolean iosScheme(){return true;}
    int weaponSlots,vehicleFlags,pauses;
    int weaponBegins,weaponEnds;
    int vitalsId=-1,ammoId=-1,pieId=-1,weaponId=-1,swallowed=-1;
    boolean pieOpen,playing=true,editing,vitalsControl=true,showUse=true;
    float vitalsX,vitalsY,ammoX,ammoY,weaponX,weaponY,weaponLastX,weaponLastY;
    long ammoTime;
    static final long VITALS_HOLD_MS=320;
    final Runnable ammoHold=()->{};
    final Runnable vitalsHold=()->{};
    static class Safe {float left,top,right,bottom;}
    Safe safe=new Safe(),cutoutSafe=new Safe();
    float iosUiScale=1;
    int getWidth(){return 2424;} int getHeight(){return 1080;}
    float hudClearStage(){return 120;}
    float dp=2;
    float[] iosRects=IosTouchLayout.place(iosPad(),0,0,2424,1080,1,0,1080);
    int iosPad() { return IosTouchLayout.PHONE; }
    boolean handsAllow(int role) { return true; }
    void iosPause() { pauses++; }
    void haptic(int type) {}
    void weaponDown(int id,float x,float y,long time) { weaponBegins++; }
    void weaponMove(float x,float y) {}
    void weaponUp(float x,float y,long time) { weaponEnds++; }
    boolean pieDown(int id,float x,float y) { return false; }
    boolean vitalsHit(float x,float y) { return false; }
    boolean ammoHit(float x,float y) { return false; }
    void removeCallbacks(Runnable r) {}
    void postDelayed(Runnable r,long delay) {}
    void invalidate() {}
    void pointerMove(int id,float x,float y,long time) {}
    void pointerUp(int id,float x,float y,long time) {}
    /* METHODS */
    void press(int i,int id) { iosDown(id,(iosRects[i*4]+iosRects[i*4+2])/2,(iosRects[i*4+1]+iosRects[i*4+3])/2,1); }
    void release(int i,int id) { iosUp(id,(iosRects[i*4]+iosRects[i*4+2])/2,(iosRects[i*4+1]+iosRects[i*4+3])/2,10); }
    public static void main(String[] args) {
        IosSpriteCheck ui=new IosSpriteCheck(); ui.clearIosPointers();
        ui.iosPlace();float[] emptyHands=ui.iosRects.clone();
        float clear=ui.hudControlClearStage();
        float gaugeLeft=(36*1.5f+Math.max(0,clear-40*1.5f))*1080/720f;
        assert gaugeLeft>=Math.max(ui.iosRects[10],ui.iosRects[14])+6*ui.dp-.001f:"Gauges cover Map/Pause";
        for(int hands:new int[]{0,1<<16,(1<<16)|(1<<17),(1<<16)|(1<<18)})for(int flags:new int[]{0,1<<13,IOS_ACTIVE_PLASMID|(1<<13)}){
            ui.weaponSlots=hands;ui.vehicleFlags=flags;ui.iosPlace();
            assert java.util.Arrays.equals(emptyHands,ui.iosRects):"Pickup/HUD changed control geometry";
        }
        for(int bar:new int[]{0,24,72,126,0}) {
            ui.safe.left=bar;ui.safe.top=bar;ui.safe.right=bar;ui.safe.bottom=bar;
            ui.iosPlace();assert java.util.Arrays.equals(emptyHands,ui.iosRects):"Transient bars resized iOS controls";
        }
        ui.cutoutSafe.left=182;ui.iosPlace();assert ui.iosRects[0]>=182:"Camera cutout must remain protected";
        ui.cutoutSafe.left=0;ui.iosPlace();
        ui.weaponSlots=ui.vehicleFlags=0;
        ui.press(10,15); assert ui.input.held==IosTouchLayout.JUMP;
        ui.release(10,15); assert ui.input.held==0;
        ui.press(11,15); assert ui.input.pulsed==IosTouchLayout.AIM;
        ui.release(11,15);
        ui.press(9,15); assert ui.input.pulsed==IosTouchLayout.LOG;
        ui.release(9,15);ui.input.pulsed=0;
        ui.press(3,11); assert ui.iosPressed(3) && ui.pauses==0;
        ui.iosUp(11,1200,500,10); assert ui.pauses==0 && !ui.iosPressed(3);
        ui.press(3,11); ui.release(3,11); assert ui.pauses==1 && !ui.iosPressed(3);
        ui.press(2,12); assert ui.iosPressed(2) && ui.input.pulsed==0;
        ui.release(2,12); assert ui.input.pulsed==IosTouchLayout.MAP && !ui.iosPressed(2);
        ui.press(6,13); assert ui.iosPressed(6) && ui.weaponBegins==0 && ui.input.pulsed==IosTouchLayout.MAP;
        ui.release(6,13); assert ui.weaponEnds==0 && ui.input.pulsed==TouchInput.RELOAD;
        ui.clearIosPointers(); assert !ui.iosPressed(6);
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),8),false)==null;
        ui.weaponSlots=2<<IOS_WEAPON_SHIFT; ui.vehicleFlags=22<<IOS_ABILITY_SHIFT|IOS_ACTIVE_PLASMID;
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),7),true).equals("ability_22");
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),8),false).equals("weapon_2");
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),6),true).equals("plasmid_reload_down");
        ui.vehicleFlags|=IOS_CROUCHED;
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),1),false).equals("stand");
        ui.input.pulsed=0;ui.vehicleFlags=IOS_ACTIVE_PLASMID;
        ui.press(7,1);assert ui.input.held==TouchInput.FIRE;ui.release(7,1);assert ui.input.held==0 && ui.input.pulsed==0;
        ui.press(8,1);assert ui.input.held==0 && ui.input.pulsed==0;ui.release(8,1);assert ui.input.pulsed==TouchInput.ALT;
        ui.vehicleFlags=0;ui.input.pulsed=0;
        ui.press(7,1);assert ui.input.held==0;ui.iosUp(1,1200,500,10);assert ui.input.pulsed==0;
        ui.press(8,1);assert ui.input.held==TouchInput.FIRE;ui.release(8,1);
        ui.press(5,1);assert ui.input.pulsed==0;ui.release(5,1);assert ui.input.pulsed==TouchInput.NEXT;
        ui.input.pulsed=0;GameActivity.choices=2|(3<<5)|(4<<10);
        ui.press(5,1);ui.iosNextHold.run();assert ui.iosSelectorOpen && ui.iosSelectorRow==0;
        assert !ui.iosVisible(ui.iosPad(),10) && !ui.iosVisible(ui.iosPad(),14);
        float[] select=ui.iosSelectorRects;
        ui.iosUp(1,(select[4]+select[6])/2,(select[5]+select[7])/2,200);
        assert GameActivity.phase==8 && GameActivity.slot==2 && ui.input.pulsed==0 && !ui.iosSelectorOpen;
        GameActivity.choices=(long)(5|(8<<5)|(11<<10))<<32;ui.vehicleFlags=IOS_ACTIVE_PLASMID;
        ui.press(5,1);ui.iosNextHold.run();assert ui.iosSelectorRow==1;
        select=ui.iosSelectorRects;
        ui.iosUp(1,(select[8]+select[10])/2,(select[9]+select[11])/2,200);
        assert GameActivity.phase==9 && GameActivity.slot==10;
        ui.press(5,1);ui.iosNextHold.run();ui.clearIosPointers();assert !ui.iosSelectorOpen && ui.iosNextId==-1;
        ui.press(6,1);ui.release(6,1);assert GameActivity.phase==10;
        int use=IosTouchLayout.count(ui.iosPad())-1;
        ui.input.pulsed=0;
        assert !ui.iosVisible(ui.iosPad(),use);
        ui.press(use,14); ui.release(use,14); assert ui.input.pulsed==0;
        ui.vehicleFlags=IosTouchLayout.GATE_USE;
        assert ui.iosVisible(ui.iosPad(),use);
        ui.press(use,14); assert ui.iosPressed(use) && ui.input.pulsed==0;
        ui.iosUp(14,1200,500,10); assert !ui.iosPressed(use) && ui.input.pulsed==0;
        ui.press(use,14); ui.release(use,14); assert ui.input.pulsed==TouchInput.USE;
        ui.vehicleFlags=CONTEXT_REROLL;
        ui.press(use,14); ui.release(use,14); assert ui.input.pulsed==TouchInput.RELOAD;
        ui.showUse=false; assert !ui.iosVisible(ui.iosPad(),use);
    }
}
'''.replace("/* METHODS */", methods)
        hold = controls[controls.index("private final Runnable iosNextHold=()") + len("private final Runnable iosNextHold="):]
        hold = hold[:hold.index("\n    };") + len("\n    }")]
        program = program.replace("/* HOLD */", hold)
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "IosSpriteCheck.java"
            fixture.write_text(program, encoding="utf-8")
            subprocess.run([str(java / "javac.exe"), "--release", "8", "-d", temporary,
                            str(java_root / "IosTouchLayout.java"), str(fixture)], check=True)
            subprocess.run([str(java / "java.exe"), "-ea", "-cp", temporary,
                            "org.bioshock.port.IosSpriteCheck"], check=True)


if __name__ == "__main__":
    unittest.main()
