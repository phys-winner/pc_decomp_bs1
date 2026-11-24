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
            if (!(rects[i] >= 0 && rects[i + 1] >= 0 && rects[i + 2] <= right && rects[i + 3] <= bottom
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
        if (!(fitted[pause * 4] < 80)) throw new AssertionError("pause not in the left corner");
        if (!(width(small, fire) < width(fitted, fire) && width(large, fire) > width(fitted, fire)))
            throw new AssertionError("scale does not change button size");
        float[] tablet = IosTouchLayout.place(pad, 0, 0, 2340, 1080, 1, 0, 1080);
        inside(tablet, 2340, 1080);
        for (int scheme : new int[]{phone, pad}) for (int width : new int[]{1400, 2424})
            for (float zoom : new float[]{.5f, 1f, 2f}) {
            float[] controls = IosTouchLayout.place(scheme, 25, 15, width, 1040, zoom, 220, 760);
            for (int i = 0; i < 4; ++i)
                if (controls[i * 4 + 1] < 220 || controls[i * 4 + 3] > 760.001f)
                    throw new AssertionError("Left control overlaps the main HUD at scale " + zoom);
            for (int i = 0; i < 10; ++i) {
                if (IosTouchLayout.spec(scheme, i).role == IosTouchLayout.DRAW) continue;
                float x = (controls[i * 4] + controls[i * 4 + 2]) * .5f;
                float y = (controls[i * 4 + 1] + controls[i * 4 + 3]) * .5f;
                if (IosTouchLayout.hit(scheme, controls, null, x, y, 24) != i)
                    throw new AssertionError("Unreachable control " + i + " in scheme " + scheme + " at " + zoom);
            }
        }
        int padCrouch = index(pad, IosTouchLayout.HOLD, IosTouchLayout.DUCK);
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
        System.out.println("PASS iOS touch layout: edge anchors, scale, pause hit, decorative bounds");
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
            "private void iosDown(", "private void iosUp(",
            "private static String[] iosEquipmentSprites(", "private String iosArtwork("))
        program = r'''
package org.bioshock.port;
class HapticFeedbackConstants { static final int VIRTUAL_KEY = 1; }
class TouchInput {
    static final int MOVEMENT=1, BUTTON=2, LOOK=3;
    int pulsed;
    void pulse(int action) { pulsed=action; }
    boolean down(int id,float x,float y,int bits,int role,long time) { return true; }
    boolean owns(int id) { return false; }
    void up(int id,float x,float y,long time) {}
}
public class IosSpriteCheck {
    static final int CONTEXT_CINEMATIC=4096, IOS_WEAPON_SHIFT=23, IOS_ABILITY_SHIFT=16, IOS_ACTIVE_PLASMID=1<<28, IOS_CROUCHED=1<<29;
    static final String[] IOS_WEAPON_SPRITES=iosEquipmentSprites("weapon_",16), IOS_ABILITY_SPRITES=iosEquipmentSprites("ability_",32);
    final int[] iosIds=new int[8],iosHits=new int[8];
    final TouchInput input=new TouchInput();
    int weaponSlots,vehicleFlags,pauses;
    float dp=2;
    float[] iosRects=IosTouchLayout.place(iosPad(),0,0,2424,1080,1,0,1080);
    int iosPad() { return IosTouchLayout.PHONE; }
    boolean[] iosAllow(int pad) { return null; }
    void iosPause() { pauses++; }
    void haptic(int type) {}
    /* METHODS */
    void press(int i,int id) { iosDown(id,(iosRects[i*4]+iosRects[i*4+2])/2,(iosRects[i*4+1]+iosRects[i*4+3])/2,1); }
    void release(int i,int id) { iosUp(id,(iosRects[i*4]+iosRects[i*4+2])/2,(iosRects[i*4+1]+iosRects[i*4+3])/2,10); }
    public static void main(String[] args) {
        IosSpriteCheck ui=new IosSpriteCheck(); ui.clearIosPointers();
        ui.press(3,11); assert ui.iosPressed(3) && ui.pauses==0;
        ui.iosUp(11,1200,500,10); assert ui.pauses==0 && !ui.iosPressed(3);
        ui.press(3,11); ui.release(3,11); assert ui.pauses==1 && !ui.iosPressed(3);
        ui.press(2,12); assert ui.iosPressed(2) && ui.input.pulsed==0;
        ui.release(2,12); assert ui.input.pulsed==IosTouchLayout.MAP && !ui.iosPressed(2);
        ui.press(6,13); assert ui.iosPressed(6) && ui.input.pulsed==IosTouchLayout.RELOAD;
        ui.clearIosPointers(); assert !ui.iosPressed(6);
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),8),false)==null;
        ui.weaponSlots=2<<IOS_WEAPON_SHIFT; ui.vehicleFlags=22<<IOS_ABILITY_SHIFT|IOS_ACTIVE_PLASMID;
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),7),true).equals("ability_22");
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),8),false).equals("weapon_2");
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),6),true).equals("plasmid_reload_down");
        ui.vehicleFlags|=IOS_CROUCHED;
        assert ui.iosArtwork(IosTouchLayout.spec(ui.iosPad(),1),false).equals("stand");
    }
}
'''.replace("/* METHODS */", methods)
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "IosSpriteCheck.java"
            fixture.write_text(program, encoding="utf-8")
            subprocess.run([str(java / "javac.exe"), "--release", "8", "-d", temporary,
                            str(java_root / "IosTouchLayout.java"), str(fixture)], check=True)
            subprocess.run([str(java / "java.exe"), "-ea", "-cp", temporary,
                            "org.bioshock.port.IosSpriteCheck"], check=True)


if __name__ == "__main__":
    unittest.main()
