"""Exercise the production pointer state machine without Android or JNI."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def touch_input_source(directory):
    source = (ROOT / "android/app/src/org/bioshock/port/TouchInput.java").read_text(encoding="utf-8")
    source = source.replace("import android.os.Handler;\n", "").replace("import android.os.Looper;\n", "")
    source = source.replace("new Handler(Looper.getMainLooper())", "new Handler()")
    source = source.replace("final class TouchInput {", """final class TouchInput {
    static final class Handler {
        private static final java.util.ArrayList<Runnable> pending = new java.util.ArrayList<>();
        boolean postDelayed(Runnable runnable, long delay) { pending.add(runnable); return true; }
        static void releaseAll() {
            java.util.ArrayList<Runnable> ready = new java.util.ArrayList<>(pending);
            pending.clear();
            for (Runnable runnable : ready) runnable.run();
        }
    }
""")
    path = Path(directory) / "TouchInput.java"
    path.write_text(source, encoding="utf-8")
    return path


class TouchControlsTest(unittest.TestCase):
    def test_reference_camera_angular_response(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        self.assertIn("cameraCounts(sensitivity)/dp", source)
        methods = "\n".join(self._method(source, name) for name in
                            ("public void onSensorChanged(", "static float cameraCounts(", "static float gyroDelta("))
        program = r'''
package org.bioshock.port;
public class CameraReferenceCheck {
    static final class Surface {static final int ROTATION_0 = 0, ROTATION_90 = 1, ROTATION_180 = 2, ROTATION_270 = 3;}
    static final class Display {int rotation; int getRotation(){return rotation;}}
    static final class Sensor {static final int TYPE_GYROSCOPE = 4; int getType(){return TYPE_GYROSCOPE;}}
    static final class SensorEvent {final Sensor sensor = new Sensor(); float[] values = {1,2,0}; long timestamp;}
    final Display display = new Display();
    Display getDisplay(){return display;}
    final class Input {void relativeLook(float x, float y){dx=x;dy=y;}}
    final Input input = new Input();
    final boolean gyroActive = true;
    final float gyroSensitivity = 1;
    long sensorTime;
    int sensorRotation;
    float dx,dy;
    /* PRODUCTION */
    public static void main(String[] args) {
        float counts=cameraCounts(.22f)/2.625f*100;
        double bioshock=counts*62.49375;
        double ut3=(.22*10/2.625*100)*24.9975;
        assert Math.abs(bioshock-ut3)<.001:bioshock+" != "+ut3;
        assert Math.abs(cameraCounts(1)*62.49375-10*24.9975)<.001;
        float[] yaw = {2,-1,-2,1}, pitch = {1,2,-1,-2};
        for (int rotation=0; rotation<4; ++rotation) {
            CameraReferenceCheck check = new CameraReferenceCheck();
            check.display.rotation = rotation;
            SensorEvent event = new SensorEvent();
            event.timestamp = 1_000_000_000;
            check.onSensorChanged(event);
            for (int sign : new int[] {1,-1}) {
                event.values = new float[] {sign,2*sign,0};
                event.timestamp += 10_000_000;
                check.onSensorChanged(event);
                assert Math.abs(check.dx-cameraCounts(gyroDelta(sign*yaw[rotation],.01)))<.00001f : "yaw rotation " + rotation;
                assert Math.abs(check.dy-cameraCounts(gyroDelta(sign*pitch[rotation],.01)))<.00001f : "pitch rotation " + rotation;
            }
        }
        System.out.println("PASS camera calibration and horizontal gyro direction at all four display rotations");
    }
}
'''.replace("/* PRODUCTION */", methods)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "CameraReferenceCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.CameraReferenceCheck"], check=True)

    @staticmethod
    def _method(source, signature):
        begin = source.index(signature)
        end = source.index("{", begin) + 1
        depth = 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[begin:end]

    def test_gameplay_hints_hide_in_native_ui_and_editor(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(self._method(source, name) for name in (
            "@Override protected void onDraw", "private void drawHint", "private String toolLabel", "private void drawTools",
            "private void offer(", "private void markHint", "private void offerHint", "void editLayout()"))
        program = r'''
class Typeface{}
class Paint{enum Style{FILL}Typeface font=new Typeface();Typeface getTypeface(){return font;}void setTypeface(Typeface f){font=f;}void setStyle(Style s){}void setColor(int c){}void setTextSize(float size){}float measureText(String s){return s.length()*4;}}
class Canvas{int hints;void drawColor(int c){}void drawRoundRect(float a,float b,float c,float d,float e,float f,Paint p){++hints;}}
class SystemClock{static long now=1000;static long uptimeMillis(){return now;}}
class TouchLayout{static final int COUNT=1,FIRE=0,WEAPON=1;}
class Base{protected void onDraw(Canvas c){}}
class PortPreferences{int bits;void put(String key,int value){assert key.equals("touchHints");bits=value;}}
public class GameplayHintCheck extends Base{
    static final int HINT_LOOK=1,HINT_WEAPON=128,HINT_ACTIONS=256,HINT_VITALS=1024,HINT_AMMO=2048,TOOL_DONE=0,TOOL_RESET=1,TOOL_BUTTONS=2;
    static final int CONTEXT_CINEMATIC=1<<12,CONTEXT_VITALS=1<<13,HANDS_PLASMID=1<<18,HANDS_HOLDING=1<<16;
    boolean vitalsControl=true,touchEnabled=true,touchPrompts=true;void drawVitals(Canvas c){}
    int vehicleFlags,weaponSlots;final Runnable hintCheck=()->{};
    void postDelayed(Runnable job,long delay){assert job==hintCheck&&delay>0;}
    boolean handsAllow(int control){return control==TouchLayout.WEAPON;}
    static final long HINT_MS=5500;
    boolean layoutDirty,playing=true,editing,pieOpen,authoritative=true,layoutPending;
    final Paint paint=new Paint();Typeface editorFont;final boolean[]shown={false};
    final float[][]tools={{10,10,100,40},{110,10,200,40},{210,10,300,40},{310,10,400,40}};float dp=1;int toolPressed=-1,editControl=-1;
    int hintsSeen,pies,saves,invalidations,releases;long hintUntil,resetDeadline,posted;String hintText;
    final PortPreferences prefs=new PortPreferences();final java.util.ArrayList<String>drawn=new java.util.ArrayList<>(),announced=new java.util.ArrayList<>();
    PortPreferences preferences(){return prefs;}GameplayHintCheck activity(){return this;}void savePortSettings(Object x){++saves;}
    String text(String english,String russian){return english;}int getWidth(){return 1000;}int getHeight(){return 600;}float width(){return 1000;}
    void invalidate(){++invalidations;}void postInvalidateDelayed(long delay){posted=delay;}void announceForAccessibility(String s){announced.add(s);}
    void release(){++releases;}void buildLayout(){layoutDirty=false;}void loadEditorStyle(){}
    void drawStick(Canvas c){}void drawControl(Canvas c,int id){}void drawEditorCaption(Canvas c,int id){}void drawPie(Canvas c){++pies;}
    boolean iosScheme(){return false;}void drawIos(Canvas c){}
    void drawText(Canvas c,String s,float x,float y,float size,float max,int color){drawn.add(s);}
    void drawPill(Canvas c,float[]r,String s,boolean pressed){drawn.add(s);}
    /* PRODUCTION METHODS */
    void clear(){drawn.clear();}
    public static void main(String[]args){
        GameplayHintCheck v=new GameplayHintCheck();v.offerHint();String first=v.hintText;long firstDeadline=v.hintUntil;
        assert first!=null&&v.hintsSeen==HINT_LOOK&&v.prefs.bits==HINT_LOOK&&v.saves==1&&v.posted==5550&&firstDeadline==6500;
        Canvas game=new Canvas();v.onDraw(game);assert game.hints==1&&v.drawn.contains(first);
        // Native menus and minigames both select non-playing touch contexts.
        v.playing=false;SystemClock.now=1200;v.clear();Canvas menu=new Canvas();v.onDraw(menu);v.offerHint();
        assert menu.hints==0&&v.drawn.isEmpty()&&v.hintText.equals(first)&&v.hintUntil==firstDeadline&&v.saves==1;
        // The original deadline continues to expire while a native UI is open.
        SystemClock.now=6500;v.onDraw(menu);assert v.hintText==null&&menu.hints==0&&v.hintsSeen==HINT_LOOK;
        v.playing=true;SystemClock.now=7000;v.offerHint();String next=v.hintText;long nextDeadline=v.hintUntil;
        assert next!=null&&next.contains("Reload")&&v.hintsSeen==(HINT_LOOK|HINT_WEAPON)&&v.saves==2;
        v.clear();Canvas resumed=new Canvas();v.onDraw(resumed);assert resumed.hints==1&&v.drawn.contains(next);
        v.pieOpen=true;v.clear();Canvas pie=new Canvas();v.onDraw(pie);assert pie.hints==0&&v.pies==1&&!v.drawn.contains(next);v.pieOpen=false;
        v.editLayout();assert v.editing&&v.releases==1&&v.announced.contains("Drag a control. Tap Done to save.");
        v.clear();Canvas editor=new Canvas();v.onDraw(editor);
        assert editor.hints==0&&!v.drawn.contains(next)&&v.drawn.containsAll(java.util.Arrays.asList("Done","Reset","Buttons","Cancel",
            "Drag to move. Pinch to resize. Tap Buttons for visibility."));
        assert v.hintUntil==nextDeadline&&v.saves==2;
        v.editing=false;SystemClock.now=8000;v.clear();Canvas again=new Canvas();v.onDraw(again);assert again.hints==1&&v.drawn.contains(next)&&v.hintUntil==nextDeadline;
        SystemClock.now=nextDeadline;v.clear();Canvas expired=new Canvas();v.onDraw(expired);assert expired.hints==0&&v.hintText==null;
        v.offerHint();assert v.hintText==null; // Prologue, with hidden vitals and no plasmid.
        v.vehicleFlags=CONTEXT_VITALS;v.offerHint();assert v.hintText.contains("Health gauge");
        SystemClock.now=v.hintUntil;v.offerHint();assert (v.hintsSeen&HINT_ACTIONS)==0;
        v.weaponSlots=HANDS_PLASMID;v.offerHint();assert v.hintText.contains("refills EVE");
        System.out.println("PASS production hint visibility: gameplay first-use/expiry preserved, native UI/minigame hidden, pie/editor overlay isolated, editor instruction and accessibility announcement retained, hint resumes without extending deadline");
    }
}
'''.replace("/* PRODUCTION METHODS */", methods)
        out = ROOT / "build/probes/gameplay-hint-visibility"
        out.mkdir(parents=True, exist_ok=True)
        path = out / "GameplayHintCheck.java"
        path.write_text(program, encoding="utf-8")
        subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", out, path], check=True)
        result = subprocess.run([java / "java.exe", "-ea", "-cp", out, "GameplayHintCheck"], capture_output=True, text=True, check=True)
        (out / "host.log").write_text(result.stdout, encoding="utf-8")
        print(result.stdout.strip())

    def test_gyro_mode_tracks_fire_and_context(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        program = r'''
package org.bioshock.port;
class SensorManager {
    static final int SENSOR_DELAY_GAME=1;
    boolean registerListener(Object listener,Object sensor,int delay){return true;}
}
public class GyroContextCheck {
    boolean focused=true,shown=true,playing=true,touchEnabled=true,editing,pieOpen,gyroActive,gyroListening;
    int gyroMode=1,vehicleFlags;static final int CONTEXT_AIMING=1<<10,CONTEXT_CINEMATIC=1<<12,CONTEXT_FIRING=1<<15;long sensorTime;final Object gyro=new Object();final SensorManager sensors=new SensorManager();
    final TouchInput input=new TouchInput(new TouchInput.Sink(){
        public void submit(float a,float b,float c,float d,int held){}public boolean stick(float x,float y){return true;}public void reset(){}
    });
    boolean isShown(){return shown;}void stopGyro(){gyroListening=false;}
''' + self._method(source, "private void updateGyro") + self._method(source, "static boolean gyroWanted") + r'''
    public static void main(String[] args){
        GyroContextCheck v=new GyroContextCheck();v.updateGyro();assert !v.gyroActive;
        v.input.down(1,0,0,TouchInput.ALT,TouchInput.PASSIVE,0);v.updateGyro();assert !v.gyroActive;
        v.input.cancel();v.input.down(1,0,0,TouchInput.FIRE,TouchInput.PASSIVE,0);v.updateGyro();assert v.gyroActive&&v.gyroListening;
        v.input.cancel();v.vehicleFlags=CONTEXT_FIRING;v.updateGyro();assert v.gyroActive;v.vehicleFlags=0;
        v.input.cancel();v.updateGyro();assert !v.gyroActive&&!v.gyroListening;
        v.gyroMode=2;v.updateGyro();assert v.gyroActive;
        v.vehicleFlags=CONTEXT_CINEMATIC;v.updateGyro();assert !v.gyroActive&&!v.gyroListening;v.vehicleFlags=0;
        v.touchEnabled=false;v.updateGyro();assert v.gyroActive;v.touchEnabled=true;
        v.editing=true;v.updateGyro();assert !v.gyroActive;v.editing=false;
        v.pieOpen=true;v.updateGyro();assert !v.gyroActive;v.pieOpen=false;
        v.playing=false;v.updateGyro();assert !v.gyroActive;v.playing=true;
        v.focused=false;v.updateGyro();assert !v.gyroActive;v.focused=true;
        v.shown=false;v.updateGyro();assert !v.gyroActive;v.shown=true;
        v.gyroMode=3;v.updateGyro();assert !v.gyroActive;
        v.vehicleFlags=CONTEXT_AIMING;v.updateGyro();assert v.gyroActive;
        v.gyroMode=4;v.vehicleFlags=0;v.updateGyro();assert !v.gyroActive;
        v.input.down(2,0,0,TouchInput.FIRE,TouchInput.PASSIVE,0);v.updateGyro();assert v.gyroActive;
        v.input.cancel();v.vehicleFlags=CONTEXT_AIMING;v.updateGyro();assert v.gyroActive;
        v.gyroMode=0;v.updateGyro();assert !v.gyroActive;
        System.out.println("PASS production gyro: while-firing follows FIRE, weapon/plasmid switch is neutral, always/off and menu/editor/focus/visibility gates");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "GyroContextCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary,
                            touch_input_source(temporary), path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.GyroContextCheck"], check=True)

    def test_bioshock_reload_swipe_picker_and_action_map(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(self._method(source, name) for name in ("private void weaponDown", "private void startPicker",
            "private void weaponMove", "private void weaponStick", "private void weaponUp", "private void cancelWeapon",
            "private int[] pieItems(int kind)", "private boolean inRadial(", "private boolean availableInRadial(",
            "private void pieSelect(", "private int action(", "private int displayMode(",
            "private boolean editable(", "private boolean gameplayAllows("))
        program = r'''
package org.bioshock.port;
import java.util.Arrays;
class ViewConfiguration {
    static ViewConfiguration get(Object context){return new ViewConfiguration();}
    int getScaledTouchSlop(){return 8;}
}
class HapticFeedbackConstants {static final int VIRTUAL_KEY=1,LONG_PRESS=2;}
public class BioShockGestures {
    static final int PIE_ACTIONS=0,PIE_ABILITIES=1,PIE_TOOLS=2,PIE_CONTROL=-100,SEATS=-6,MORE_MAIN=-7,TOOLS=-9,
        SETTINGS=-3,MORE=-1,MODE=-2,KEYBOARD=-4,WEAPON=-5,CONTEXT_CINEMATIC=1<<12,CONTEXT_MOVEMENT_ONLY=1<<14;
    static final long PICK_HOLD_MS=300;
    static class Capture implements TouchInput.Sink {
        int held,pressed;
        public void submit(float x,float y,float dx,float dy,int mask){pressed|=mask&~held;held=mask;}
        public boolean stick(float x,float y){return true;}
        public void reset(){held=0;}
    }
    final Capture capture=new Capture();final TouchInput input=new TouchInput(capture);
    static final int PICKER_ACTIVE=1<<21;
    static class GameActivity {static int phase,slot;static boolean nativeTouchWeapon(int p,int s,long c){phase=p;slot=s;return true;}}
    int weaponId=-1,weaponSwipe,pickerRow=-1,pickerStep,weaponSlots;long cursorContext=3;int ticks;void weaponTick(){++ticks;}
    int vehicleFlags,pieSource;
    float pieX,pieY;
    boolean playing=true,touchEnabled=true,showUse=true,showMore;
    final int[] customDisplay=new int[TouchLayout.COUNT];
    BioShockGestures(){Arrays.fill(customDisplay,-1);}
    boolean handsAllow(int control){return true;}
    Runnable settingsAction;
    void activate(int control){assert control==TouchLayout.PAUSE;input.pulse(TouchInput.PAUSE);}
    void openPie(int id,float x,float y,int kind,int source){}
    void release(){}
    boolean weaponPicker,weaponSwiped,pending;float dp=1,weaponX,weaponY,weaponLastX,weaponLastY;
    final Runnable weaponHold=new Runnable(){public void run(){startPicker();}};
    Object getContext(){return this;}void invalidate(){}void haptic(int type){}
    void postDelayed(Runnable action,long delay){assert delay==PICK_HOLD_MS;pending=true;}
    void removeCallbacks(Runnable action){pending=false;}
''' + methods + r'''
    void hold(){if(pending){pending=false;weaponHold.run();}}
    void clean(){input.cancel();capture.pressed=0;}
    static void pause(){try{Thread.sleep(170);}catch(InterruptedException e){throw new RuntimeException(e);}}
    static boolean contains(int[] items,int target){for(int item:items)if(item==target)return true;return false;}
    public static void main(String[] args){
        BioShockGestures g=new BioShockGestures();
        assert TouchInput.RELOAD==(1<<23)&&TouchInput.HEAL==(1<<24)&&TouchInput.HACK==(1<<25);
        assert TouchInput.RESCUE==(1<<11)&&TouchInput.HELP==(1<<12)&&TouchInput.LOG==(1<<20)&&TouchInput.WALK==(1<<22);
        g.weaponDown(1,100,100,0);g.weaponUp(101,101,50);
        assert g.capture.pressed==TouchInput.RELOAD&&g.capture.held==0&&!g.pending;g.clean();
        g.weaponDown(2,100,100,0);g.weaponMove(160,100);assert !g.pending&&g.capture.pressed==0;g.weaponMove(180,100);g.hold();g.weaponUp(180,100,500);
        assert g.capture.pressed==TouchInput.NEXT&&g.capture.held==0;g.clean();
        pause();g.weaponDown(3,100,100,0);g.weaponMove(20,100);g.weaponUp(20,100,500);
        assert g.capture.pressed==TouchInput.PREVIOUS&&g.capture.held==0;g.clean();
        g.weaponDown(4,100,100,0);g.weaponMove(100,160);g.hold();g.weaponUp(100,160,500);
        assert g.capture.pressed==0&&!g.pending;g.clean();
        g.weaponDown(5,100,100,0);g.hold();assert g.weaponPicker&&g.capture.held==TouchInput.PICKER;
        g.weaponUp(100,100,300);assert g.capture.pressed==TouchInput.PICKER&&g.capture.held==0;g.clean();
        g.weaponDown(6,100,100,0);g.hold();g.cancelWeapon();assert g.weaponId==-1&&!g.weaponPicker&&g.capture.held==0;
        g.weaponSlots=PICKER_ACTIVE;g.weaponDown(7,100,100,0);g.hold();
        g.weaponMove(100,60);assert GameActivity.phase==4&&GameActivity.slot==15;
        g.weaponMove(196,60);assert GameActivity.phase==4&&GameActivity.slot==17;
        g.weaponMove(148,150);assert GameActivity.phase==5&&GameActivity.slot==16;
        g.weaponUp(148,150,800);assert GameActivity.phase==6;g.clean();
        g.weaponDown(8,100,100,0);g.hold();g.weaponUp(100,100,400);assert GameActivity.phase==7;g.clean();
        pause();g.weaponDown(9,100,100,0);int before=g.ticks;g.weaponMove(400,100);assert g.ticks-before==1;
        pause();g.weaponMove(320,100);assert g.ticks-before==2;g.weaponMove(250,100);assert g.ticks-before==2;g.weaponUp(250,100,900);g.clean();
        pause();g.weaponDown(10,100,100,0);before=g.ticks;g.weaponMove(180,100);pause();g.weaponMove(100,100);g.weaponUp(100,100,900);
        assert g.ticks-before==2;
        assert (g.capture.pressed&TouchInput.RELOAD)==0;g.clean();
        int[] primary=g.pieItems(PIE_ACTIONS),abilities=g.pieItems(PIE_ABILITIES),tools=g.pieItems(PIE_TOOLS);
        assert primary.length==8&&primary[0]==TouchInput.HEAL&&primary[1]==TouchInput.HACK;
        assert contains(primary,TouchInput.RESCUE)&&contains(primary,TouchInput.HELP)&&contains(primary,TouchInput.BEST)&&contains(primary,TouchInput.TAUNT);
        for(int i=0;i<6;++i)assert contains(abilities,TouchInput.WEAPON1<<i);
        assert contains(abilities,TouchInput.AMMO_UP)&&contains(abilities,TouchInput.AMMO_DOWN);
        assert contains(tools,TouchInput.SCORES)&&contains(tools,TouchInput.MAP)&&contains(tools,TouchInput.LOG)&&contains(tools,SETTINGS);
        for(int[] items:new int[][]{primary,abilities,tools})for(int item:items)assert item!=0;
        g.cursorContext=2;
        g.customDisplay[TouchLayout.HACK]=TouchLayout.ON_SCREEN;
        g.customDisplay[TouchLayout.WEAPON]=TouchLayout.IN_RADIAL;
        g.customDisplay[TouchLayout.FIRE2]=TouchLayout.IN_RADIAL|TouchLayout.ON_SCREEN;
        g.customDisplay[TouchLayout.DUCK]=TouchLayout.IN_RADIAL;
        int[] edited=g.pieItems(PIE_ACTIONS);
        assert !contains(edited,TouchInput.HACK)&&contains(edited,PIE_CONTROL-TouchLayout.DUCK);
        assert contains(edited,PIE_CONTROL-TouchLayout.FIRE2)&&contains(edited,PIE_CONTROL-TouchLayout.WEAPON);
        g.pieSelect(PIE_CONTROL-TouchLayout.FIRE2);
        assert g.capture.pressed==TouchInput.FIRE&&g.capture.held==0;g.clean();
        g.pieSelect(PIE_CONTROL-TouchLayout.WEAPON);
        assert g.capture.pressed==TouchInput.RELOAD&&g.capture.held==0;g.clean();
        g.vehicleFlags=CONTEXT_CINEMATIC;
        assert !contains(g.pieItems(PIE_ACTIONS),PIE_CONTROL-TouchLayout.DUCK);
        g.pieSelect(PIE_CONTROL-TouchLayout.DUCK);
        assert g.capture.pressed==0;
        System.out.println("PASS production BioShock tap reload/EVE, slow left/right swipes, cancelled vertical swipe, held original picker, neutral cancel; exact native action bits; immediate heal/hack and complete action menus");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "BioShockGestures.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary,
                            touch_input_source(temporary),
                            ROOT / "android/app/src/org/bioshock/port/TouchLayout.java", path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.BioShockGestures"], check=True)

    def test_pie_captions_fit_each_sector_without_squashing(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(self._method(source, name) for name in ("private void drawText", "private String pieLabel",
            "private int[] pieItems(int kind)", "private float pieRadius", "private float pieCaptionWidth",
            "private float pieTitleWidth", "private float pieRing"))
        program = r'''
package org.bioshock.port;
import java.util.Arrays;
class Paint {
    enum Align {CENTER} enum Style {FILL}
    static class FontMetrics {float ascent,descent;}
    float size;void setStyle(Style style){}void setColor(int color){}
    void setTextAlign(Align align){assert align==Align.CENTER;}
    void setTextSize(float value){assert Float.isFinite(value)&&value>0;size=value;}
    float measureText(String label){
        // Wider than Roboto's averages, so the production fit is checked conservatively.
        float width=0;for(int i=0;i<label.length();++i)width+=label.charAt(i)>127?.66f:.62f;
        return size*width;
    }
    FontMetrics getFontMetrics(){FontMetrics f=new FontMetrics();f.ascent=-.8f*size;f.descent=.25f*size;return f;}
}
class Canvas {
    int draws;float x,y,width,top,bottom,size;
    void drawText(String label,float a,float b,Paint p){
        ++draws;x=a;y=b;width=p.measureText(label);Paint.FontMetrics m=p.getFontMetrics();
        top=b+m.ascent;bottom=b+m.descent;size=p.size;
    }
}
public class PieFitCheck {
    static final int SETTINGS=-3,SEATS=-6,MORE_MAIN=-7,TOOLS=-9,PIE_ACTIONS=0,PIE_ABILITIES=1,PIE_TOOLS=2,ABILITY_COUNT=6,PIE_CONTROL=-100;
    final Paint paint=new Paint();float dp=1,layoutScale=1,w=800,h=360,pieR;int profile;boolean russian;
    float width(){return w;}float height(){return h;}String text(String en,String ru){return russian?ru:en;}
    boolean inRadial(int control){return TouchLayout.defaultRadial(control);}
    boolean availableInRadial(int control){return false;}
    String label(int control){throw new AssertionError();}
''' + methods + r'''
    public static void main(String[] args){
        PieFitCheck view=new PieFitCheck();Canvas c=new Canvas();int cases=0;
        for(float density:new float[]{1,2.75f,3,3.5f})for(float[] screen:new float[][]{{2400,1080},{1920,1080},{1280,720},{2560,1600},{3200,1440}})
        for(float scale:new float[]{.65f,1,1.6f})for(boolean russian:new boolean[]{false,true})
        for(int kind:new int[]{PIE_ACTIONS,PIE_ABILITIES,PIE_TOOLS})for(int profile=0;profile<5;++profile){
            if(screen[1]/density<320)continue; // No Android device is shorter than 320dp in landscape.
            view.dp=density;view.w=screen[0];view.h=screen[1];view.layoutScale=scale;view.russian=russian;view.profile=profile;
            int[] items=view.pieItems(kind);int n=items.length;
            view.pieR=view.pieRadius(n);
            assert n>=4&&n<=9;
            float r=view.pieR,inner=r*.36f,ring=view.pieRing(),caption=view.pieCaptionWidth(n);
            assert r<=Math.min(view.w,view.h)*.45f+.001f&&caption>0;
            for(int i=0;i<n;++i){
                String label=view.pieLabel(items[i],false);assert !label.isEmpty();
                double a=Math.PI*2*i/n;
                float ix=(float)Math.sin(a)*ring,iy=-(float)Math.cos(a)*ring;
                view.drawText(c,label,ix,iy+14*density,11*density,caption,0);
                assert c.size<=11*density+.001f&&c.size>=9*density-.001f&&c.width<=caption+.01f:label+" "+c.size/density;
                assert Math.abs((c.top+c.bottom)*.5f-(iy+14*density))<.001f;
                // Every corner of the caption box stays inside its own sector of the ring.
                for(float x:new float[]{ix-c.width*.5f,ix+c.width*.5f})for(float y:new float[]{c.top,c.bottom}){
                    double angle=Math.atan2(x,-y),offset=Math.abs(Math.IEEEremainder(angle-a,Math.PI*2));
                    assert offset<Math.PI/n:label+" leaves its sector";
                    assert Math.hypot(x,y)<r&&Math.hypot(x,y)>inner:label;
                }
                // The full name stays readable in its pill beside the pie.
                String full=view.pieLabel(items[i],true);
                view.drawText(c,full,0,0,14*density,view.pieTitleWidth(),0);
                assert c.size>=11.5f*density&&c.width<=view.pieTitleWidth()+.01f:full;
                ++cases;
            }
        }
        int draws=c.draws;view.drawText(c,"",0,0,10,10,0);view.drawText(c,"x",0,0,10,0,0);assert c.draws==draws;
        System.out.println("PASS "+cases+" production BioShock pie captions: EN/RU, actions/plasmids/tools, 65..160 percent, phone/tablet densities; captions fit their sector and ring");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "PieFitCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary,
                            touch_input_source(temporary),
                            ROOT / "android/app/src/org/bioshock/port/TouchLayout.java", path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.PieFitCheck"], check=True)

    def test_closed_ime_retires_served_connection_before_hiding(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        def method(signature):
            begin = source.index(signature)
            end = source.index("{", begin) + 1
            depth = 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[begin:end]
        program = r'''
class Context {
    static final String INPUT_METHOD_SERVICE="input_method";
    InputMethodManager manager;
    Object getSystemService(String name){assert name.equals(INPUT_METHOD_SERVICE);return manager;}
}
class InputMethodManager {
    int restarts,hides;ImeRetireCheck served;
    void restartInput(ImeRetireCheck view){
        assert !view.imeRequested&&!view.imeWasVisible&&!view.onCheckIsTextEditor();
        ++restarts;served=view;view.hideKeyboard(); // Re-entry must be neutral after editor retirement.
    }
    void hideSoftInputFromWindow(Object token,int flags){assert restarts==hides+1;++hides;}
}
public class ImeRetireCheck {
    boolean imeRequested,imeWasVisible,authoritative=true;long cursorContext=7,imeSession=10;
    final Context context=new Context();Context getContext(){return context;}Object getWindowToken(){return this;}
''' + method("void hideKeyboard") + method("@Override public boolean onCheckIsTextEditor").replace("@Override ", "") + r'''
    public static void main(String[] args){
        ImeRetireCheck view=new ImeRetireCheck();InputMethodManager imm=new InputMethodManager();view.context.manager=imm;
        view.imeRequested=true;assert view.onCheckIsTextEditor();view.hideKeyboard();
        assert view.imeSession==11&&imm.restarts==1&&imm.hides==1&&!view.onCheckIsTextEditor();
        view.hideKeyboard();assert view.imeSession==11&&imm.restarts==1&&imm.hides==1;
        view.imeWasVisible=true;view.hideKeyboard();assert view.imeSession==12&&imm.restarts==2&&imm.hides==2;
        view.imeRequested=true;view.context.manager=null;view.hideKeyboard();assert view.imeSession==13&&!view.onCheckIsTextEditor();
        System.out.println("PASS production hideKeyboard: editor false/session retired before IMM restart then hide; recursive and repeated close neutral; null IMM safe");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ImeRetireCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "ImeRetireCheck"], check=True)

    def test_ui_visibility_and_ime_sessions(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(self._method(source, name) for name in ("private int action", "private boolean visible",
            "private boolean baseVisible", "private boolean handsAllow", "private int displayMode(",
            "private boolean editable(", "private boolean gameplayAllows(",
            "private boolean textUi", "private boolean ready", "@Override public boolean performEditorAction",
            "private void applyPendingSettings"))
        program = r'''
package org.bioshock.port;
class GameActivity { static long current=7;static long nativeTouchContext(){return current;} }
class EditorBase { public boolean performEditorAction(int action){return false;} }
public class ImeGuardCheck extends EditorBase {
    static final int CONTEXT_USE=1<<4,CONTEXT_HACK=1<<5,CONTEXT_RESCUE=1<<6,CONTEXT_HARVEST=1<<7,
        CONTEXT_REROLL=1<<8,CONTEXT_VALID=1<<9,CONTEXT_AIMING=1<<10,CONTEXT_CAN_AIM=1<<11,CONTEXT_CINEMATIC=1<<12,CONTEXT_MOVEMENT_ONLY=1<<14;
    boolean touchEnabled=true,showUse=true;
    static final int MORE=-1,MODE=-2,SETTINGS=-3,KEYBOARD=-4,WEAPON=-5,SEATS=-6,MORE_MAIN=-7,QUICKSAVE=-8,VEHICLE_SEAT_JUMP=4;
    boolean editing,authoritative=true,playing,imeRequested=true,showMore;int profile,vehicleFlags,weaponSlots;
    static final int HANDS_HOLDING=1<<16,HANDS_WEAPON=1<<17,HANDS_PLASMID=1<<18,HANDS_MELEE=1<<19,HANDS_VALID=1<<20;
    long cursorContext=7,context=7,imeSession=2,session=1;
    int hidden,commits;
    boolean settingsPending;int menuId=-1,reloads;
    final int[] customDisplay=new int[TouchLayout.COUNT];
    ImeGuardCheck(){java.util.Arrays.fill(customDisplay,-1);}
    final TouchInput input=new TouchInput(new TouchInput.Sink(){
        public void submit(float a,float b,float c,float d,int held){}
        public boolean stick(float x,float y){return true;}
        public void reset(){}
    });
    void reloadSettings(){++reloads;settingsPending=false;}
    final TouchText text=new TouchText(new TouchText.Sink(){
        public boolean commit(String value){++commits;return true;}public boolean delete(int count){return true;}
    });
    void hideKeyboard(){++hidden;imeRequested=false;++imeSession;}
''' + methods + r'''
    boolean only(int... controls){
        for(int c=0;c<TouchLayout.COUNT;++c){
            boolean expected=false;for(int x:controls)expected|=x==c;
            if(visible(c)!=expected)return false;
        }
        return true;
    }
    public static void main(String[] args) {
        ImeGuardCheck view=new ImeGuardCheck();
        // Original UI: no port controls at all; the game asks for no text, so there is no keyboard button.
        assert view.only();
        view.cursorContext=5;assert view.only();
        view.cursorContext=6;view.playing=true;view.vehicleFlags=CONTEXT_USE;
        int[] foot={TouchLayout.FIRE,TouchLayout.ALT,TouchLayout.JUMP,TouchLayout.DUCK,TouchLayout.USE,
            TouchLayout.WEAPON,TouchLayout.PAUSE};
        assert view.only(foot)&&view.action(TouchLayout.WEAPON)==WEAPON&&view.action(TouchLayout.USE)==TouchInput.USE;
        view.vehicleFlags|=CONTEXT_CINEMATIC;
        assert view.only(TouchLayout.PAUSE);
        view.vehicleFlags=CONTEXT_MOVEMENT_ONLY;
        assert view.visible(TouchLayout.PAUSE)&&!view.visible(TouchLayout.USE);
        view.vehicleFlags=0;view.weaponSlots=HANDS_VALID;assert view.visible(TouchLayout.USE);
        view.weaponSlots=0;view.showUse=false;assert !view.visible(TouchLayout.USE);view.showUse=true;
        view.customDisplay[TouchLayout.USE]=TouchLayout.IN_RADIAL;assert !view.visible(TouchLayout.USE);
        view.customDisplay[TouchLayout.USE]=-1;
        view.vehicleFlags=CONTEXT_USE|CONTEXT_RESCUE|CONTEXT_HARVEST;
        assert view.visible(TouchLayout.RESCUE)&&view.visible(TouchLayout.HARVEST)&&view.visible(TouchLayout.USE);
        view.vehicleFlags=CONTEXT_CAN_AIM;assert view.visible(TouchLayout.AIM);
        view.touchEnabled=false;assert view.only();view.touchEnabled=true;view.vehicleFlags=CONTEXT_USE;
        // More opens from the health gauge; its own button appears only when the player enables it.
        view.showMore=true;
        assert view.only(TouchLayout.FIRE,TouchLayout.ALT,TouchLayout.JUMP,TouchLayout.DUCK,TouchLayout.USE,
            TouchLayout.WEAPON,TouchLayout.MORE,TouchLayout.PAUSE);
        view.showMore=false;
        // Old persisted UT3 vehicle profiles cannot hide or remap BioShock actions.
        for(int p=0;p<5;++p){view.profile=p;assert view.only(foot)&&view.action(TouchLayout.WEAPON)==WEAPON;}
        assert view.action(TouchLayout.FIRE2)==TouchInput.FIRE&&view.action(TouchLayout.BOARD)==0&&view.action(TouchLayout.SCORES)==0;
        view.customDisplay[TouchLayout.FIRE2]=TouchLayout.IN_RADIAL;
        assert !view.visible(TouchLayout.FIRE2);
        view.customDisplay[TouchLayout.FIRE2]=TouchLayout.ON_SCREEN;
        assert view.visible(TouchLayout.FIRE2);
        view.weaponSlots=HANDS_VALID;assert !view.visible(TouchLayout.FIRE2);
        view.weaponSlots=0;view.customDisplay[TouchLayout.FIRE2]=-1;
        view.vehicleFlags=0;
        // The layout editor moves the same controls except Pause, which keeps its corner.
        view.editing=true;assert view.only(TouchLayout.FIRE,TouchLayout.ALT,TouchLayout.JUMP,TouchLayout.DUCK,
            TouchLayout.USE,TouchLayout.WEAPON,TouchLayout.AIM,TouchLayout.RESCUE,TouchLayout.HARVEST,TouchLayout.REROLL,TouchLayout.HACK,
            TouchLayout.MORE,TouchLayout.FIRE2);view.editing=false;view.profile=0;
        // Without a script publisher the manual PLAY/MENU fallback remains.
        view.authoritative=false;view.cursorContext=0;view.playing=false;
        assert view.only(TouchLayout.SETTINGS,TouchLayout.MODE);
        view.playing=true;
        assert view.visible(TouchLayout.MODE)&&view.visible(TouchLayout.SETTINGS)&&view.visible(TouchLayout.FIRE)&&!view.visible(TouchLayout.KEYBOARD);
        view.cursorContext=5;assert view.only();
        view.authoritative=true;view.cursorContext=7;view.playing=false;
        assert !view.ready()&&view.imeRequested&&view.hidden==0;
        assert !view.performEditorAction(6)&&view.imeRequested&&view.hidden==0&&view.commits==0;
        view.session=2;assert view.ready()&&view.text.compose("A");
        assert view.performEditorAction(6)&&!view.imeRequested&&view.hidden==1&&view.commits==1;
        view.imeRequested=true;view.imeSession=10;view.session=9;
        assert !view.performEditorAction(6)&&view.imeRequested&&view.hidden==1;
        view.session=10;GameActivity.current=11;
        assert !view.ready()&&!view.imeRequested&&view.hidden==2;
        view.settingsPending=true;view.applyPendingSettings();
        assert view.reloads==0&&view.settingsPending;
        view.cursorContext=6;view.menuId=1;view.applyPendingSettings();assert view.reloads==0;
        view.menuId=-1;view.editing=true;view.applyPendingSettings();assert view.reloads==0;
        view.editing=false;view.input.down(3,0,0,0,TouchInput.LOOK,-1);view.applyPendingSettings();assert view.reloads==0;
        view.input.cancel();view.imeRequested=true;view.applyPendingSettings();assert view.reloads==0;
        view.imeRequested=false;view.applyPendingSettings();assert view.reloads==1&&!view.settingsPending;
        System.out.println("PASS production visibility: original UI without port controls, duplicate gear hidden, loading none, seven BioShock controls (More on request) independent of legacy profiles, editor set without the fixed Pause and legacy PLAY fallback; stale IME session/Done and deferred settings reload");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ImeGuardCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary,
                            ROOT / "android/app/src/org/bioshock/port/TouchText.java",
                            touch_input_source(temporary),
                            ROOT / "android/app/src/org/bioshock/port/TouchLayout.java", path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.ImeGuardCheck"], check=True)

    def test_activity_back_edge_ownership(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        def method(file, signature):
            text = (ROOT / file).read_text(encoding="utf-8")
            begin = text.index(signature)
            end = text.index("{", begin) + 1
            depth = 1
            while depth:
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            return text[begin:end]
        dispatch = method("android/app/src/org/bioshock/port/GameActivity.java", "@Override public boolean dispatchKeyEvent")
        dispatch = dispatch.replace('        if (key == KeyEvent.KEYCODE_ENTER) android.util.Log.i("BioShock", "[input] dispatch Enter action=" + event.getAction());\n', '')
        back = method("android/app/src/org/bioshock/port/GameActivity.java", "private void back()")
        preime = method("android/app/src/org/bioshock/port/GameControls.java", "@Override public boolean onKeyPreIme")
        program = r'''
class KeyEvent {
    static final int ACTION_DOWN=0,ACTION_UP=1,KEYCODE_BACK=4,KEYCODE_ESCAPE=111,KEYCODE_A=29,KEYCODE_Z=54,
        KEYCODE_BUTTON_START=108,
        KEYCODE_0=7,KEYCODE_9=16,KEYCODE_F1=131,KEYCODE_F12=142,KEYCODE_SPACE=62,KEYCODE_ENTER=66,
        KEYCODE_GRAVE=68,KEYCODE_MINUS=69,KEYCODE_PERIOD=56,KEYCODE_DEL=67,KEYCODE_TAB=61,
        KEYCODE_SHIFT_LEFT=59,KEYCODE_SHIFT_RIGHT=60,KEYCODE_CTRL_LEFT=113,KEYCODE_CTRL_RIGHT=114,
        KEYCODE_DPAD_LEFT=21,KEYCODE_DPAD_UP=19,KEYCODE_DPAD_RIGHT=22,KEYCODE_DPAD_DOWN=20;
    final int action,key;final long downTime;boolean canceled;
    KeyEvent(int action,int key,long downTime){this.action=action;this.key=key;this.downTime=downTime;}
    int getAction(){return action;}int getKeyCode(){return key;}long getDownTime(){return downTime;}boolean isCanceled(){return canceled;}
}
class ActivityBase { public boolean dispatchKeyEvent(KeyEvent event){return false;}public void onBackPressed(){}void finish(){} }
class Launch { String distribution(){return "bioshock";}static void openLauncher(Object activity,String distribution){} }
class GamepadInput { boolean key(KeyEvent event){throw new AssertionError("no gamepad attached");} }
class PreImeBase { public boolean onKeyPreIme(int code,KeyEvent event){return false;} }
class Controls extends PreImeBase {
    boolean overlay,imeRequested,promptReady=true;int closes,backCalls,hidden;
    void hideKeyboard(){imeRequested=false;++hidden;}
    boolean canOfferInputPrompt(){return promptReady;}
    boolean handleBack(){++backCalls;if(imeRequested){hideKeyboard();return true;}if(overlay){overlay=false;++closes;return true;}return false;}
''' + preime + r'''
}
public class ActivityBackCheck extends ActivityBase {
    Launch launch;GamepadInput gamepad;
    boolean started=true,pauseAvailable,pauseChoicesOpened;int pauseChoices,pauseGames;
    Controls controls=new Controls();long backDownTime=-1;boolean backConsumed;
    int pauseShortcutKey;
    final java.util.ArrayList<Integer> nativeEdges=new java.util.ArrayList<>();
    void nativeKey(int vk,boolean down){nativeEdges.add(down?vk:-vk);}
    boolean canOpenPauseChoices(){return pauseAvailable;}
    boolean openPauseChoices(){++pauseChoices;return pauseChoicesOpened;}
    void pauseGame(){++pauseGames;}
''' + back + dispatch + r'''
    boolean deliver(int action,int key,long time) {
        KeyEvent event=new KeyEvent(action,key,time);
        return controls.onKeyPreIme(key,event)||dispatchKeyEvent(event);
    }
    public static void main(String[] args) {
        ActivityBackCheck view=new ActivityBackCheck();view.controls.overlay=true;
        assert view.deliver(0,4,100)&&view.controls.closes==1&&view.nativeEdges.isEmpty();
        assert view.deliver(0,4,100)&&view.controls.closes==1&&view.controls.backCalls==1;
        assert view.deliver(1,4,100)&&view.nativeEdges.isEmpty();
        assert view.deliver(0,4,200)&&view.deliver(1,4,200);
        assert view.nativeEdges.equals(java.util.Arrays.asList(27,-27));
        view.nativeEdges.clear();view.controls.imeRequested=true;
        assert view.deliver(0,4,300)&&view.controls.imeRequested;
        assert view.deliver(1,4,300)&&!view.controls.imeRequested&&view.controls.hidden==1&&view.nativeEdges.isEmpty();
        view.controls.overlay=true;
        assert view.deliver(0,4,400)&&view.deliver(1,4,400)&&view.controls.closes==2&&view.nativeEdges.isEmpty();
        assert view.dispatchKeyEvent(new KeyEvent(0,111,500))&&view.dispatchKeyEvent(new KeyEvent(1,111,500));
        assert view.nativeEdges.equals(java.util.Arrays.asList(27,-27));
        view.nativeEdges.clear();assert view.dispatchKeyEvent(new KeyEvent(1,4,600))&&view.nativeEdges.isEmpty();
        view.pauseAvailable=true;
        assert view.deliver(0,108,700)&&view.deliver(0,108,700)&&view.deliver(1,108,700);
        assert view.pauseChoices==1&&view.pauseGames==1&&view.nativeEdges.isEmpty();
        System.out.println("PASS production Activity/View routing: overlay Back owns repeats/UP, Escape pair preserved, IME consumes both edges, orphan UP neutral, physical Start opens pause once on release");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "ActivityBackCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary, source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "ActivityBackCheck"], check=True)

    def test_keyboard_back_consumes_both_edges(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        begin = source.index("@Override public boolean onKeyPreIme")
        end = source.index("{", begin) + 1
        depth = 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        program = r'''
class KeyEvent {
    static final int KEYCODE_BACK=4,ACTION_DOWN=0,ACTION_UP=1;
    final int action;KeyEvent(int action){this.action=action;}int getAction(){return action;}
}
class PreImeBase { public boolean onKeyPreIme(int code,KeyEvent event){return false;} }
public class PreImeCheck extends PreImeBase {
    boolean imeRequested=true;int hidden;
    void hideKeyboard(){imeRequested=false;++hidden;}
''' + source[begin:end] + r'''
    public static void main(String[] args) {
        PreImeCheck view=new PreImeCheck();
        assert view.onKeyPreIme(4,new KeyEvent(0))&&view.imeRequested&&view.hidden==0;
        assert view.onKeyPreIme(4,new KeyEvent(1))&&!view.imeRequested&&view.hidden==1;
        assert !view.onKeyPreIme(4,new KeyEvent(0));
        view.imeRequested=true;assert !view.onKeyPreIme(99,new KeyEvent(0))&&view.imeRequested;
        System.out.println("PASS production IME Back consumes DOWN/UP, hides once, and cannot fall through to the original game menu");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "PreImeCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary, source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "PreImeCheck"], check=True)

    def test_text_composition(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        program = r'''
package org.bioshock.port;
public class TextCheck {
    static class Sink implements TouchText.Sink {
        String committed="";int deleted,calls;boolean accept=true;
        public boolean commit(String text){++calls;if(!accept)return false;committed+=text;return true;}
        public boolean delete(int count){if(!accept)return false;deleted+=count;return true;}
    }
    public static void main(String[] args) {
        Sink sink=new Sink();TouchText text=new TouchText(sink);
        assert text.compose("\u0416\uD83D\uDE00")&&sink.calls==0;
        assert text.backspace()&&text.before(256).toString().equals("\u0416")&&sink.deleted==0;
        assert text.commit("\u0416")&&sink.calls==1&&text.finish()&&sink.calls==1;
        assert text.compose("ab")&&text.compose("abc")&&text.finish()&&sink.committed.equals("\u0416abc");
        assert text.compose("xy")&&text.delete(4,0)&&sink.deleted==4&&text.before(256).toString().equals("xy");
        assert text.compose("xy");sink.accept=false;assert !text.delete(3,0)&&text.before(256).toString().equals("xy");
        assert !text.commit("z")&&text.before(256).toString().equals("xy");sink.accept=true;
        assert text.finish();
        Sink other=new Sink();TouchText codepoints=new TouchText(other);
        assert codepoints.deleteCodePoints(1,0)&&other.deleted==1; // New IME session can delete an existing game field.
        assert !codepoints.deleteCodePoints(2,0)&&other.deleted==1;
        assert codepoints.commit("A\uD83D\uDE00B")&&!codepoints.deleteCodePoints(2,0)&&other.deleted==1;
        assert codepoints.deleteCodePoints(1,0)&&other.deleted==2;
        assert !codepoints.deleteCodePoints(2,0)&&other.deleted==2;
        assert codepoints.deleteCodePoints(1,0)&&other.deleted==3; // Delegate one original key, never infer surrogate units.
        assert !codepoints.deleteCodePoints(2,0);
        assert codepoints.backspace()&&other.deleted==4;
        assert codepoints.commit("XYZ")&&codepoints.compose("pq")&&codepoints.deleteCodePoints(2,0)&&other.deleted==6;
        assert codepoints.before(256).toString().equals("pq")&&codepoints.backspace()&&other.deleted==6;
        assert codepoints.delete(1,0)&&other.deleted==7&&codepoints.before(256).toString().equals("p");
        assert !codepoints.deleteCodePoints(2,0)&&!codepoints.deleteCodePoints(0,1);
        assert !text.delete(-1,0)&&!text.delete(257,0)&&!text.delete(1,1);
        for(int i=0;i<160;i++)if(i<32||i>=127)assert !TouchText.valid(Character.toString((char)i));
        assert !TouchText.valid("\uD800")&&!TouchText.valid("\uDC00")&&!TouchText.valid("\uD800x");
        assert !TouchText.valid("\u2028")&&!TouchText.valid("\u2029")&&!TouchText.valid(null);
        assert TouchText.valid("\u0416\uD83D\uDE00");
        char[] max=new char[257];java.util.Arrays.fill(max,'x');assert !TouchText.valid(new String(max));
        assert TouchText.valid(new String(max,0,256));
        System.out.println("PASS IME local composition/backspace, outside-composition deletion, one original Backspace for a new/unknown session, bounded BMP multi-delete, no surrogate-count guesses, rejected-sink atomicity, UTF16/control/size bounds");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "TextCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary,
                            ROOT / "android/app/src/org/bioshock/port/TouchText.java", source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.TextCheck"], check=True)

    def test_layout_geometry_and_sensor_bounds(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(self._method(source, name) for name in ("private void buildLayout", "private int action",
            "private boolean visible", "private boolean baseVisible", "private boolean handsAllow", "private int displayMode(",
            "private boolean editable(", "private boolean gameplayAllows(", "private float buttonSize(", "float hudClearStage",
            "private float width", "private float height", "static float gyroDelta", "private static float clamp"))
        program = r'''
package org.bioshock.port;
import java.util.ArrayList;
public class GeometryCheck {
    static final int CONTEXT_USE=1<<4,CONTEXT_HACK=1<<5,CONTEXT_RESCUE=1<<6,CONTEXT_HARVEST=1<<7,
        CONTEXT_REROLL=1<<8,CONTEXT_VALID=1<<9,CONTEXT_AIMING=1<<10,CONTEXT_CAN_AIM=1<<11,CONTEXT_CINEMATIC=1<<12,CONTEXT_MOVEMENT_ONLY=1<<14;
    boolean touchEnabled=true,showUse=true;
    static final int MORE=-1,MODE=-2,SETTINGS=-3,KEYBOARD=-4,WEAPON=-5,SEATS=-6,MORE_MAIN=-7,QUICKSAVE=-8,VEHICLE_SEAT_JUMP=4,HUD_MAP=1<<15;
    int vehicleFlags,weaponSlots=HUD_MAP; // The minimap shown: the strictest HUD.
    // Hand bits from the game (bioshock_touch.c); without HANDS_VALID every combat button stays available.
    static final int HANDS_HOLDING=1<<16,HANDS_WEAPON=1<<17,HANDS_PLASMID=1<<18,HANDS_MELEE=1<<19,HANDS_VALID=1<<20;
    boolean showMore;
    static class Insets {int left,top,right,bottom;} final Insets safe=new Insets();
    int w=2400,h=1080;int getWidth(){return w;}int getHeight(){return h;}
    final float[] controlX=new float[TouchLayout.COUNT],controlY=new float[TouchLayout.COUNT],controlR=new float[TouchLayout.COUNT],
        customX=new float[TouchLayout.COUNT],customY=new float[TouchLayout.COUNT];
    final boolean[] shown=new boolean[TouchLayout.COUNT];
    final int[] customDisplay=new int[TouchLayout.COUNT],customSize=new int[TouchLayout.COUNT];
    GeometryCheck(){java.util.Arrays.fill(customDisplay,-1);}
    final ArrayList<float[]> occupied=new ArrayList<>(),reserved=new ArrayList<>();
    float dp=3,scale=1,layoutScale=1,uiScale=1;
    boolean playing=true,editing,leftHanded,authoritative=true,layoutDirty;int profile;long cursorContext=6;
    void buildTools(){}
''' + methods + r'''
    int crowded;
    void check(String name){
        java.util.Arrays.fill(customX,Float.NaN);java.util.Arrays.fill(customY,Float.NaN);
        buildLayout();
        float left=safe.left,top=safe.top,right=w-safe.right,bottom=h-safe.bottom;
        for(int c=0;c<TouchLayout.COUNT;++c){
            assert shown[c]==visible(c):name+" control "+c+" not placed";
            if(!shown[c])continue;
            float x=controlX[c],y=controlY[c],r=controlR[c];
            if(c==TouchLayout.PAUSE){
                // The player asked for Pause in the screen corner itself, ignoring the cutout inset.
                float[] corner=TouchLayout.pauseCorner(dp);
                assert Math.abs(x-(corner[0]+corner[2])/2)<.01f&&Math.abs(y-(corner[1]+corner[3])/2)<.01f:name+" Pause left its corner";
                assert x-r>=-.01f&&y-r>=-.01f&&x+r<=w+.01f&&y+r<=h+.01f:name+" Pause outside the screen";
            } else
            assert x-r>=left-.01f&&x+r<=right+.01f&&y-r>=top-.01f&&y+r<=bottom+.01f:name+" control "+c+" outside";
            assert 2*r>=TouchLayout.MIN_DP*dp-.01f:name+" control "+c+" too small";
            for(int d=c+1;d<TouchLayout.COUNT;++d)if(shown[d])
                assert Math.hypot(x-controlX[d],y-controlY[d])>=r+controlR[d]+4*dp-.01f:name+" controls "+c+"/"+d+" touch";
            for(float[] hud:reserved)
                assert !TouchLayout.overlaps(new float[]{x-r,y-r,x+r,y+r},hud,4*dp-.01f):name+" control "+c+" covers the HUD";
        }
        // The combat cluster moves as one rigid group by a bounded shift and stays on its thumb's side.
        float shiftX=Float.NaN,shiftY=Float.NaN;boolean rigid=true;
        for(int c:TouchLayout.CLUSTER)if(shown[c]){
            float[] at=TouchLayout.anchor(c,left,top,right,bottom,dp,layoutScale,leftHanded);
            float ox=controlX[c]-at[0],oy=controlY[c]-at[1];
            if(Float.isNaN(shiftX)){shiftX=ox;shiftY=oy;}
            else rigid&=Math.abs(ox-shiftX)<.01f&&Math.abs(oy-shiftY)<.01f;
            assert leftHanded?controlX[c]<w*.5f:controlX[c]>w*.5f:name+" control "+c+" on the wrong side";
        }
        if(Float.isNaN(shiftX)) {
            // No cluster outside gameplay.
        } else if(rigid) {
            assert Math.abs(shiftX)<=TouchLayout.MAX_SHIFT_X*(right-left)+.5f&&Math.abs(shiftY)<=TouchLayout.MAX_SHIFT_Y*(bottom-top)+.5f:name;
        } else {
            ++crowded;
            // Only an enlarged control set under an enlarged HUD may give up the shape.
            assert scale>1&&uiScale>1.25f:name+" cluster lost its shape";
        }
    }
    public static void main(String[] args) {
        GeometryCheck g=new GeometryCheck();int cases=0;
        float[][] screens={{2400,1080,3},{2400,1080,2.75f},{1920,1080,3},{1280,720,2},{2560,1600,2},{3200,1440,3.5f},
            {2340,1080,2.625f,80},{1600,720,2},{2778,1284,3}};
        for(float[] s:screens)for(int profile=0;profile<5;++profile)for(float scale:new float[]{.65f,1,1.3f,1.6f})
        for(float ui:new float[]{1,1.25f,1.5f})for(boolean left:new boolean[]{false,true}){
            g.w=(int)s[0];g.h=(int)s[1];g.dp=s[2];g.safe.left=s.length>3?(int)s[3]:0;
            g.profile=profile;g.scale=scale;g.uiScale=ui;g.leftHanded=left;g.playing=true;g.cursorContext=6;
            g.check(s[0]+"x"+s[1]+"@"+s[2]+" profile "+profile+" scale "+scale+" ui "+ui+(left?" left":""));++cases;
        }
        // The reference phone keeps Fire exactly at its designed place.
        g.w=2400;g.h=1080;g.dp=3;g.safe.left=0;g.profile=0;g.scale=1;g.uiScale=1;g.leftHanded=false;g.check("reference");
        assert Math.abs(g.controlX[TouchLayout.FIRE]-(2400-140*3))<.01f&&Math.abs(g.controlY[TouchLayout.FIRE]-(1080-142*3))<.01f;
        // Fire keeps its 92dp target; duplicate fire, ammo and quicksave buttons are hidden.
        assert Math.abs(g.controlR[TouchLayout.FIRE]-46*3)<.01f&&g.controlR[TouchLayout.DUCK]>=31*3-.01f;
        assert !g.shown[TouchLayout.FIRE2]&&!g.shown[TouchLayout.BOARD]&&!g.shown[TouchLayout.SCORES];
        // The original weapon bar (UTHUD defaults), five cards with one selected, at 100 and 150 percent HUD.
        float[] bar=TouchLayout.weaponBar(0,0,2400,1080,1,5);
        assert Math.abs(bar[4]-75f/1280*2400)<.01f&&Math.abs(bar[2]-bar[0]-5.5f*bar[4])<.01f;
        assert Math.abs(bar[3]-(1080-16f/1280*2400))<.01f&&Math.abs((bar[0]+bar[2])*.5f-(1200+70f/1280*2400))<.01f;
        float[] big=TouchLayout.weaponBar(0,0,2400,1080,1.5f,5);
        assert Math.abs(big[4]-1.5f*bar[4])<.01f&&Math.abs((big[0]+big[2])*.5f-(1200+1.5f*70f/1280*2400))<.01f;
        assert TouchLayout.weaponBar(0,0,2400,1080,1,1)==null&&TouchLayout.weaponBar(0,0,0,1080,1,5)==null;
        int[] slots=TouchLayout.weaponSlots(1|2|16|32|64|(5<<10)|(1<<14));
        assert slots.length==5&&slots[0]==0&&slots[2]==4&&slots[4]==6;
        // A swipe starts on the selected card and moves one card per step, clamped to the ends.
        assert TouchLayout.weaponIndex(3,0,100,5)==3&&TouchLayout.weaponIndex(3,149,100,5)==4&&TouchLayout.weaponIndex(3,-151,100,5)==1;
        assert TouchLayout.weaponIndex(3,900,100,5)==4&&TouchLayout.weaponIndex(3,-900,100,5)==0&&TouchLayout.weaponIndex(3,Float.NaN,100,5)==3;
        // Only BioShock's top health/EVE and ammunition panels reserve space.
        g.weaponSlots=0;g.uiScale=1.37f;g.check("BioShock HUD");assert g.reserved.size()==2;
        assert Math.abs(g.controlX[TouchLayout.FIRE]-(2400-140*3))<.01f&&Math.abs(g.controlY[TouchLayout.FIRE]-(1080-142*3))<.01f;
        g.weaponSlots=HUD_MAP;g.uiScale=1.37f;g.check("legacy flags ignored");assert g.reserved.size()==2;
        assert Math.abs(g.controlX[TouchLayout.FIRE]-(2400-140*3))<.01f&&Math.abs(g.controlY[TouchLayout.FIRE]-(1080-142*3))<.01f;
        g.uiScale=1;
        // A player's placement is kept as dropped and defaults move away from it.
        java.util.Arrays.fill(g.customX,Float.NaN);java.util.Arrays.fill(g.customY,Float.NaN);
        g.customX[TouchLayout.FIRE]=.5f;g.customY[TouchLayout.FIRE]=.5f;g.buildLayout();
        assert Math.abs(g.controlX[TouchLayout.FIRE]-1200)<.01f&&Math.abs(g.controlY[TouchLayout.FIRE]-540)<.01f;
        g.leftHanded=true;g.buildLayout();assert Math.abs(g.controlX[TouchLayout.FIRE]-1200)<.01f;
        g.customX[TouchLayout.FIRE]=.8f;g.buildLayout();assert Math.abs(g.controlX[TouchLayout.FIRE]-.2f*2400)<.01f;
        g.customDisplay[TouchLayout.FIRE2]=TouchLayout.ON_SCREEN;
        g.customSize[TouchLayout.FIRE2]=106;
        g.buildLayout();assert g.shown[TouchLayout.FIRE2]&&g.controlR[TouchLayout.FIRE2]==53*g.dp;
        // Original UI: no port controls (the keyboard button is gone), nothing reserved for a hidden HUD.
        g.leftHanded=false;g.playing=false;g.cursorContext=7;g.check("menu");
        assert g.reserved.isEmpty();
        for(int c=0;c<TouchLayout.COUNT;++c)assert !g.shown[c]:"menu shows control "+c;
        // Pie directions and absolute hits: twelve o'clock first, clockwise.
        assert TouchLayout.sector(0,-50,8,24)==0&&TouchLayout.sector(50,0,8,24)==2&&TouchLayout.sector(0,50,8,24)==4;
        assert TouchLayout.sector(-50,0,8,24)==6&&TouchLayout.sector(-36,-36,8,24)==7&&TouchLayout.sector(10,-10,8,24)==-1;
        assert TouchLayout.sector(Float.NaN,0,8,0)==-1&&TouchLayout.sector(1,1,0,0)==-1;
        for(int n=4;n<=7;++n)for(int i=0;i<n;++i){
            double a=TouchLayout.sectorAngle(i,n);
            float x=(float)Math.sin(a)*80,y=-(float)Math.cos(a)*80;
            assert TouchLayout.sector(x,y,n,24)==i&&TouchLayout.pieHit(300+x,200+y,300,200,40,118,n)==i;
        }
        assert TouchLayout.pieHit(300,200,300,200,40,118,7)==TouchLayout.CENTER&&TouchLayout.pieHit(300,330,300,200,40,118,7)==-1;
        assert TouchLayout.fitScale(1.6f,360)<1.6f&&TouchLayout.fitScale(1.6f,800)==1.6f&&TouchLayout.fitScale(.1f,360)==.65f;
        assert gyroDelta(0,.01)==0;
        assert gyroDelta(Float.NaN,.01)==0;
        assert gyroDelta(Float.POSITIVE_INFINITY,.01)==0;
        assert gyroDelta(1,0)==0&&gyroDelta(1,-1)==0&&gyroDelta(1,.101)==0;
        float one=gyroDelta(1,.02),two=gyroDelta(1,.01)*2;
        assert Math.abs(one-two)<.00001;
        assert gyroDelta(1000,.1)==25&&gyroDelta(-1000,.1)==-25;
        assert gyroDelta(.001f,.01)==0;
        System.out.println("crowded layouts placed per control: "+g.crowded);
        System.out.println("PASS "+cases+" production layouts: 9 phones/tablets/cutout, 5 profiles, 65..160 percent controls, 100..150 percent HUD, both hands; inside safe area, 44dp targets, no overlap, original HUD clear, rigid thumb cluster; custom/mirror/menu placement; pie sectors; gyro bounds");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "GeometryCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary,
                            touch_input_source(temporary),
                            ROOT / "android/app/src/org/bioshock/port/TouchLayout.java", path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.GeometryCheck"], check=True)

    def test_fractional_display_rates_and_default(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = (ROOT / "android/app/src/org/bioshock/port/GameActivity.java").read_text(encoding="utf-8")
        methods = "\n".join(self._method(source, name) for name in
            ("private static Display.Mode physicalMode", "private static boolean compatibleMode", "private float[] supportedFpsRates",
             "private float selectedFps", "private float fpsLimit", "static float resolveFps(", "private static Display.Mode displayModeForFps"))
        methods += "\n" + "\n".join(line for line in source.splitlines()
            if line.strip().startswith(("static final float DEFAULT_FPS", "static final float RATE_TOLERANCE")))
        program = r'''
import java.util.ArrayList;
import java.util.Collections;
import java.util.TreeMap;
class Display {
    float active=144;
    int id=1,width=2400,height=1080;
    static final class Mode {
        final int id,width,height;final float rate;
        Mode(int id,int width,int height,float rate){this.id=id;this.width=width;this.height=height;this.rate=rate;}
        int getModeId(){return id;}
        int getPhysicalWidth(){return width;}int getPhysicalHeight(){return height;}float getRefreshRate(){return rate;}
    }
    Mode[] modes;
    Mode getMode(){return new Mode(id,width,height,active);}
    Mode[] getSupportedModes(){return modes;}
    float getRefreshRate(){return active;}
}
class PortPreferences {
    final java.util.HashMap<String,Float> values=new java.util.HashMap<>();
    float getFloat(String name,float fallback){return values.getOrDefault(name,fallback);}
}
public class FpsCheck {
    Display display=new Display();final PortPreferences portPreferences=new PortPreferences();
    Display gameDisplay(){return display;}
''' + methods + r'''
    public static void main(String[] args) {
        FpsCheck check=new FpsCheck();
        check.display.modes=new Display.Mode[]{new Display.Mode(1,2400,1080,144),new Display.Mode(2,2400,1080,59.94f),
            new Display.Mode(3,2400,1080,30),new Display.Mode(4,2400,1080,59.94f),new Display.Mode(5,1280,720,240),
            new Display.Mode(6,2400,1080,120),new Display.Mode(7,2400,1080,90),new Display.Mode(8,2400,1080,75),
            new Display.Mode(9,2400,1080,Float.NaN)};
        float[] rates=check.supportedFpsRates();
        assert java.util.Arrays.equals(rates,new float[]{15,30,37.5f,45,59.94f,72,75,90,120,144});
        assert displayModeForFps(check.display,30).getModeId()==3:"native 30 wins over the half of 59.94";
        assert displayModeForFps(check.display,59.94f).getModeId()==2:"native 60 label wins over the half of 120";
        assert displayModeForFps(check.display,45).getModeId()==7;
        assert displayModeForFps(check.display,37.5f).getModeId()==8;
        assert displayModeForFps(check.display,72).getModeId()==1;
        assert displayModeForFps(check.display,0).getModeId()==1;
        check.display.id=4;
        assert displayModeForFps(check.display,59.94f).getModeId()==4:"keep the current mode on a tie";
        check.display.id=1;
        assert check.selectedFps()==59.94f&&check.fpsLimit()==59.94f:"default is 60 (the nearest mode), not the top mode";
        check.display.active=59.94f;assert check.fpsLimit()==59.94f;
        check.portPreferences.values.put("fpsLimit",59.94f);
        assert check.selectedFps()==59.94f&&check.fpsLimit()==59.94f;
        check.portPreferences.values.put("fpsLimit",0f);
        check.display.active=144;assert check.fpsLimit()==144;
        check.display.active=59.94f;assert check.fpsLimit()==59.94f;
        check.portPreferences.values.put("fpsLimit",60f);
        assert check.selectedFps()==59.94f:"a saved 60 must not become the top mode";
        check.portPreferences.values.put("fpsLimit",100f);
        assert check.selectedFps()==90:"an unavailable rate falls back below it";
        check.portPreferences.values.put("fpsLimit",45f);
        check.display.active=90;assert check.fpsLimit()==45;
        check.display.active=Float.NaN;assert check.fpsLimit()==0;
        check.display.active=144;
        check.display.width=1600;check.display.height=720;
        assert java.util.Arrays.equals(rates,check.supportedFpsRates())&&check.fpsLimit()==45;
        check.display.modes=new Display.Mode[]{new Display.Mode(1,2400,1080,40)};
        assert java.util.Arrays.equals(check.supportedFpsRates(),new float[]{20,40});
        assert displayModeForFps(check.display,20).getRefreshRate()==40;
        check.display.modes=new Display.Mode[]{new Display.Mode(5,1280,720,60)};
        assert check.supportedFpsRates().length==0&&check.fpsLimit()==0;
        check.display=null;assert check.fpsLimit()==0;
        System.out.println("PASS full and half FPS caps, duplicate and active mode preference, fractional rates, current resolution and saved caps");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "FpsCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "FpsCheck"], check=True)

    def test_pointer_ownership_edges_and_sampling(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        program = r'''
package org.bioshock.port;
import java.util.ArrayList;
final class TouchCheck {
    static final class Capture implements TouchInput.Sink {
        float x,y,dx,dy,totalX,totalY,sx,sy; int held,resets,sticks; boolean acceptStick=true;
        ArrayList<Integer> masks = new ArrayList<>();
        public void submit(float a,float b,float c,float d,int mask) {
            x=a;y=b;dx=c;dy=d;held=mask;totalX+=c;totalY+=d;masks.add(mask);
            assert Float.isFinite(a)&&Float.isFinite(b)&&Float.isFinite(c)&&Float.isFinite(d);
        }
        public boolean stick(float a,float b) {sx=a;sy=b;++sticks;return acceptStick;}
        public void reset() {x=y=dx=dy=0;held=0;resets++;}
    }
    static void close(float a,float b) {assert Math.abs(a-b)<.0001f:a+" != "+b;}
    public static void main(String[] args) {
        Capture c = new Capture(); TouchInput t = new TouchInput(c);
        assert TouchInput.COMMAND==(1<<12)&&TouchInput.BEST==(1<<21);
        t.configure(100,2,1,false);
        assert t.down(17,100,200,0,true);
        t.move(17,108,200);close(c.x,0);close(c.y,0);
        t.move(17,200,100);close((float)Math.hypot(c.x,c.y),1);
        assert t.down(4,900,400,TouchInput.FIRE,false);
        t.move(4,920,410);close(c.dx,40);close(c.dy,20);assert c.held==TouchInput.FIRE;
        assert t.down(30,900,400,TouchInput.FIRE,false);
        t.up(4,925,410);close(c.totalX,50);assert c.held==TouchInput.FIRE;
        float before=c.totalX;t.move(30,1000,400);close(c.totalX,before+200);
        t.up(30,1000,400);assert c.held==0;
        t.up(17,200,100);close(c.x,0);close(c.y,0);
        t.down(5,0,0,TouchInput.FIRE,false);t.down(6,0,0,TouchInput.JUMP,false);
        t.cancel();assert c.resets==1&&c.held==0&&!t.moving();
        before=c.totalX;t.up(5,999,999);t.up(6,999,999);close(c.totalX,before);
        t.down(100,0,0,0,false);t.move(100,1,2);close(c.dx,2);close(c.dy,4);
        t.down(101,0,0,TouchInput.ALT,false);t.configure(100,2,.5f,true);
        t.move(100,2,4);close(c.dx,1);close(c.dy,-2);
        t.relativeLook(4,6);close(c.dx,2);close(c.dy,-3);
        before=c.totalX;t.move(100,Float.NaN,99);t.relativeLook(Float.POSITIVE_INFINITY,0);close(c.totalX,before);
        assert !t.down(100,0,0,0,false);assert !t.down(-1,0,0,0,false);
        t.cancel();for(int i=0;i<10;i++)assert t.down(i,0,0,0,false);
        assert !t.down(11,0,0,TouchInput.FIRE,false);t.cancel();
        int count=c.masks.size();t.pulse(TouchInput.USE);t.pulse(TouchInput.USE);
        assert c.masks.size()==count+4;
        assert c.masks.get(count)==TouchInput.USE&&c.masks.get(count+1)==0;
        assert c.masks.get(count+2)==TouchInput.USE&&c.masks.get(count+3)==0;
        count=c.masks.size();t.pulseDelayed(TouchInput.COMMAND,120);
        assert c.held==TouchInput.COMMAND&&c.masks.size()==count+1;
        t.pulseDelayed(TouchInput.COMMAND,120);assert c.masks.size()==count+1;
        t.down(40,0,0,TouchInput.FIRE,false);
        assert c.held==(TouchInput.COMMAND|TouchInput.FIRE);
        t.up(40,0,0);assert c.held==TouchInput.COMMAND;
        TouchInput.Handler.releaseAll();assert c.held==0;
        t.pulseDelayed(TouchInput.COMMAND,120);t.pulseDelayed(TouchInput.USE,120);
        assert c.held==(TouchInput.COMMAND|TouchInput.USE);
        TouchInput.Handler.releaseAll();assert c.held==0;
        t.pulseDelayed(TouchInput.COMMAND,120);t.down(41,0,0,TouchInput.FIRE,false);
        t.cancel();count=c.masks.size();TouchInput.Handler.releaseAll();
        assert c.masks.size()==count&&c.held==0;
        t.configure(Float.NaN,Float.NaN,Float.NaN,false);
        t.down(2,0,0,0,true);t.move(2,60,0);close(c.x,1);t.cancel();
        t.down(3,0,0,TouchInput.FIRE,false);count=c.masks.size();t.pulse(TouchInput.FIRE);
        assert c.masks.size()==count; t.cancel();
        t.toggle(TouchInput.SCORES);assert c.held==TouchInput.SCORES&&t.latched(TouchInput.SCORES);
        t.down(4,0,0,TouchInput.FIRE,false);assert c.held==(TouchInput.SCORES|TouchInput.FIRE);
        t.up(4,0,0);assert c.held==TouchInput.SCORES;t.cancel();assert c.held==0&&!t.latched(TouchInput.SCORES);
        int duck=TouchInput.DUCK, crouchButton=TouchInput.BUTTON;
        t.down(5,0,0,duck,crouchButton,100);assert c.held==duck;
        t.up(5,0,0,175);assert c.held==0;
        t.down(5,0,0,duck,crouchButton,275);assert c.held==duck;
        t.up(5,0,0,350);assert c.held==0&&!t.latched(duck);
        t.down(5,0,0,duck,crouchButton,500);assert c.held==duck;
        t.up(5,0,0,575);assert c.held==0;
        t.down(5,0,0,duck,crouchButton,675);assert c.held==duck;
        t.up(5,0,0,750);assert c.held==0;
        t.down(5,0,0,duck,crouchButton,1000);t.move(5,20,0,1040);t.up(5,0,0,1075);
        t.down(5,0,0,duck,crouchButton,1100);assert c.held==duck;
        t.up(5,0,0,1175);assert c.held==0;
        t.cancel();t.down(5,0,0,duck,crouchButton,1250);t.up(5,0,0,1325);assert c.held==0;
        t.down(5,0,0,duck,crouchButton,1400);t.up(5,0,0,1750);assert c.held==0;
        // A second finger in the stick zone turns the camera instead of taking the stick.
        t.configure(100,2,1,false);
        assert t.down(1,100,500,0,TouchInput.MOVEMENT,-1)&&t.moving()&&!t.looking();
        assert t.down(2,120,520,0,TouchInput.MOVEMENT,-1)&&t.looking();
        t.move(2,125,520);close(c.dx,10);close(c.x,0);t.cancel();
        // A passive contact (weapon or scores) neither looks nor moves.
        assert t.down(3,500,500,0,TouchInput.PASSIVE,-1)&&!t.looking()&&!t.moving();
        t.move(3,600,500);close(c.dx,0);
        // Weapon picker: shoulder held first, then a normalized direction only while held.
        assert t.hold(3,TouchInput.PICKER)&&c.held==TouchInput.PICKER;
        assert t.stick(0,1)&&c.sx==0&&c.sy==1;
        assert t.stick(3,4)&&Math.abs(c.sx-.6f)<1e-6f&&Math.abs(c.sy-.8f)<1e-6f;
        int sticks=c.sticks;assert t.stick(.6f,.8f)&&c.sticks==sticks;
        c.acceptStick=false;assert !t.stick(-1,0);c.acceptStick=true;
        t.up(3,600,500);assert c.held==0&&t.stickX()==0&&t.stickY()==0;
        sticks=c.sticks;assert t.stick(1,0)&&c.sticks==sticks&&t.stickX()==0;
        assert t.down(3,0,0,0,TouchInput.PASSIVE,-1)&&t.hold(3,TouchInput.PICKER);
        assert t.stick(1,0)&&t.hold(3,0)&&c.held==0&&t.stickX()==0;t.cancel();
        assert !t.down(9,0,0,1<<28,TouchInput.BUTTON,-1)&&!t.hold(9,TouchInput.FIRE);
        // Look acceleration follows finger speed in dp/ms, never the sample count.
        TouchInput a=new TouchInput(c);a.configure(100,1,1,false);a.density(1);
        a.down(1,0,0,0,TouchInput.LOOK,0);c.totalX=0;
        for(int i=1;i<=50;++i)a.move(1,i*.2f,0,i);
        close(c.totalX,10);
        a.cancel();a.down(1,0,0,0,TouchInput.LOOK,0);c.totalX=0;
        for(int i=1;i<=50;++i)a.move(1,i*4,0,i);
        assert c.totalX>360&&c.totalX<=200*TouchInput.ACCEL_MAX+.01f:c.totalX;
        a.cancel();a.down(1,0,0,0,TouchInput.LOOK,0);c.totalX=0;
        for(int i=1;i<=50;++i){a.move(1,i*2-1,0,i);a.move(1,i*2,0,i);}
        float shared=c.totalX;a.cancel();a.down(1,0,0,0,TouchInput.LOOK,0);c.totalX=0;
        for(int i=1;i<=50;++i)a.move(1,i*2,0,i);
        assert Math.abs(shared-c.totalX)<c.totalX*.02f:shared+" vs "+c.totalX;
        a.cancel();a.acceleration(false);a.down(1,0,0,0,TouchInput.LOOK,0);c.totalX=0;
        for(int i=1;i<=50;++i)a.move(1,i*4,0,i);
        close(c.totalX,200);
        a.acceleration(true);a.cancel();a.down(1,0,0,0,TouchInput.LOOK,100);c.totalX=0;
        a.move(1,50,0,50);a.move(1,60,0,-1);close(c.totalX,60);
        assert TouchInput.accelerationGain(0)==1&&TouchInput.accelerationGain(10)==TouchInput.ACCEL_MAX;
        assert TouchInput.accelerationGain(Float.NaN)==1;
        t.cancel();t.configure(100,2,1,false);c.totalX=0;
        t.down(1,0,0,TouchInput.FIRE,false);t.down(2,0,0,0,false);
        t.move(2,10,0);close(c.totalX,20); // A stationary trigger cannot steal look.
        t.move(1,5,0);close(c.totalX,30);t.up(1,5,0);t.move(2,15,0);close(c.totalX,40);
        t.cancel();assert !t.looking();
        for(int button:new int[]{TouchInput.JUMP,TouchInput.DUCK}){
            t.down(1,0,0,button,false);t.move(1,4,0);close(c.dx,8);t.cancel();
        }
        System.out.println("PASS stable IDs, radial dead zone/diagonal normalization, independent move/fire/look, every aiming contact including Jump/Duck, duplicate holders, final-UP sample, cancel/repeated-UP, ALT sensitivity/invert, relative sensor deltas, invalid packets, 10-pointer bound, fast pulse edges, finite configuration");
        System.out.println("PASS second stick finger looks, passive holds, weapon picker shoulder/direction/neutral release, 28-bit actions, speed-based look acceleration independent of sample batching");
    }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "TouchCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary,
                            touch_input_source(temporary), source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.TouchCheck"], check=True)



if __name__ == "__main__":
    unittest.main()
