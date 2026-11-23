"""Exercise BioShock's production original-UI gesture classifier."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests import test_touch_controls as touch_checks

ROOT = Path(__file__).resolve().parents[1]


class BioshockUiTouchTest(unittest.TestCase):
    def test_consecutive_contacts_survive_delayed_native_probe(self):
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        controls = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        ui_down = touch_checks.TouchControlsTest._method(controls, "private void uiDown")
        program = r'''
package org.bioshock.port;
class TouchInput {static final int FIRE=1;}
class GameActivity {static boolean nativePointer(float x,float y,boolean p){return true;}
    static boolean nativeTouch(float x,float y,float dx,float dy,int held,long context){return true;}}
public final class UiQueuedContactCheck {
    static final class Capture implements UiTouchInput.Sink {
        int delay=100,waiting,kind=UiTouchInput.TAP,begins,downs,ups,resets,ends;float x,y;boolean held,failNext;
        java.util.ArrayList<String> edges=new java.util.ArrayList<>();
        public boolean begin(float x,float y){if(failNext)return false;++begins;waiting=delay;return true;}
        public int control(){return waiting-->0?0:kind;}
        public boolean pointer(float x,float y){this.x=x;this.y=y;return true;}
        public boolean button(boolean down){assert held!=down;held=down;if(down)++downs;else ++ups;
            edges.add((down?"D":"U")+":"+(int)x+":"+(int)y);return true;}
        public boolean scroll(int value){assert !held;if(value==0)++ends;return true;}
        public void reset(){held=false;++resets;}
    }
    final Capture capture=new Capture();final UiTouchInput uiInput=new UiTouchInput(capture);
    int menuId=-1;boolean authoritative=true;long cursorContext=3;Runnable uiPoll;
    void hideKeyboard(){}void syncContext(){}int getWidth(){return 2780;}int getHeight(){return 1264;}
    void removeCallbacks(Runnable r){}void postOnAnimation(Runnable r){}
''' + ui_down + r'''
    void tap(int id,int x,int y){uiDown(id,x,y);assert menuId==id;uiInput.end(x,y);menuId=-1;}
    static void drain(UiTouchInput input){for(int i=0;i<4000&&input.active();++i)input.poll();assert !input.active();}
    public static void main(String[] args){
        UiQueuedContactCheck view=new UiQueuedContactCheck();
        // These ten independent contacts all arrive before the first original hover ACK.
        for(int i=0;i<10;++i)view.tap(i,1262+i*11,346+i*3);
        assert view.capture.begins==1&&view.capture.downs==0&&view.uiInput.active();
        for(int i=0;i<40;++i)view.uiInput.poll();assert view.capture.downs==0;
        drain(view.uiInput);assert view.capture.begins==10&&view.capture.downs==10&&view.capture.ups==10&&view.capture.ends==10;
        for(int i=0;i<10;++i){String xy=":"+(1262+i*11)+":"+(346+i*3);
            assert view.capture.edges.get(i*2).equals("D"+xy)&&view.capture.edges.get(i*2+1).equals("U"+xy);}

        Capture c=new Capture();UiTouchInput input=new UiTouchInput(c);input.configure(8,24);
        assert input.begin(10,20);assert !input.begin(30,40); // A simultaneous second finger never becomes a second mouse.
        input.end(10,20);assert input.begin(100,100);input.move(100,140);input.move(100,100);input.end(100,100);
        drain(input);assert c.downs==1&&c.ups==1&&c.ends==2; // A queued out-and-back swipe never turns into a click.

        c=new Capture();c.kind=UiTouchInput.TAP|UiTouchInput.DRAG|UiTouchInput.FREE_DRAG;input=new UiTouchInput(c);
        assert input.begin(10,20);input.end(10,20);
        assert input.begin(1262,346);input.move(1262,491);input.move(1443,491);input.end(1443,491);
        drain(input);assert c.edges.equals(java.util.Arrays.asList("D:10:20","U:10:20","D:1262:346","U:1443:491"));
        assert !c.held&&c.downs==2&&c.ups==2&&c.ends==2;

        c=new Capture();input=new UiTouchInput(c);
        for(int i=0;i<17;++i){assert input.begin(i,0);input.end(i,0);}
        assert !input.begin(18,0);drain(input);assert c.downs==17&&c.ups==17; // Bounded queue preserves accepted contacts.
        c=new Capture();input=new UiTouchInput(c);
        for(int i=0;i<5;++i){assert input.begin(i,0);input.end(i,0);}
        input.cancel();input.cancel();assert c.resets==1&&!input.active()&&!c.held;
        for(int i=0;i<200;++i)input.poll();assert c.downs==0;
        assert input.begin(1,1);input.end(1,1);c.kind=-1;c.waiting=0;input.poll();assert c.resets==2&&!input.active();
        c=new Capture();c.delay=0;input=new UiTouchInput(c);assert input.begin(1,1);input.end(1,1);
        assert input.begin(2,2);c.failNext=true;input.end(2,2);assert !input.active()&&!c.held&&c.resets==1;
        System.out.println("PASS production UI contact FIFO: ten taps before delayed ACK arrive exactly once/in order at original coordinates; actual GameControls accepts them; held second finger rejected, queued vertical/diagonal drag and out-and-back swipe preserved, bounded capacity, context/reset/failure neutral");
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-ui-queued-") as temporary:
            source = Path(temporary) / "UiQueuedContactCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary,
                            ROOT / "android/app/src/org/bioshock/port/UiTouchInput.java", source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary,
                            "org.bioshock.port.UiQueuedContactCheck"], check=True)

    def test_release_tap_swipe_slider_and_delayed_ack(self):
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        program = r'''
package org.bioshock.port;
public final class UiTouchCheck {
    static final class Capture implements UiTouchInput.Sink {
        int kind,downs,ups,resets,ends,hovers,optionTaps;
        boolean held;
        final java.util.ArrayList<String> events=new java.util.ArrayList<>();
        public boolean begin(float x,float y){events.add("begin");return true;}
        public int control(){return kind;}
        public boolean pointer(float x,float y){events.add("move");return true;}
        public boolean button(boolean down){
            assert down!=held;held=down;if(down)++downs;else ++ups;
            events.add(down?"down":"up");return true;
        }
        public boolean scroll(int request){
            assert !held&&(request==0||request==UiTouchInput.HOVER||request==UiTouchInput.OPTION_TAP);
            if(request==0){++ends;events.add("end");}
            else if(request==UiTouchInput.HOVER){++hovers;events.add("hover");}
            else {++optionTaps;events.add("option");}
            return true;
        }
        public void reset(){held=false;++resets;events.add("reset");}
    }
    static void counts(Capture c,int downs,int ups,int ends){
        assert c.downs==downs&&c.ups==ups&&c.ends==ends:
            c.events+" counts "+c.downs+"/"+c.ups+"/"+c.ends;
    }
    public static void main(String[] args){
        Capture c=new Capture();c.kind=UiTouchInput.TAP;
        UiTouchInput t=new UiTouchInput(c);t.configure(8,24);
        assert t.begin(100,100);
        counts(c,0,0,0); // Hover/classification alone must not click.
        t.move(102,101);t.poll();counts(c,0,0,0); // A long held finger still has not clicked.
        t.end(102,101);counts(c,1,0,0); // Android UP starts one polled click.
        assert t.active()&&c.held;
        t.poll();counts(c,1,1,1);assert !t.active()&&!c.held;

        assert t.begin(100,100);t.move(140,100);t.move(180,140);t.end(180,140);
        counts(c,1,1,2);assert !t.active(); // A swipe only points; no press or activation.

        c.kind=0;assert t.begin(50,60);t.end(50,60);counts(c,1,1,2);
        c.kind=UiTouchInput.TAP;t.poll();counts(c,2,1,2);assert t.active();
        t.poll();counts(c,2,2,3);assert !t.active(); // Delayed hover ACK retains the released tap.

        c.kind=UiTouchInput.ROWS|UiTouchInput.TAP;
        assert t.begin(50,60)&&c.hovers==1;t.move(50,120);t.end(50,140);
        counts(c,2,2,4);assert c.optionTaps==0; // List drag does not select or activate.

        c.kind=UiTouchInput.DRAG|UiTouchInput.TAP;
        assert t.begin(100,100);t.move(140,104);counts(c,3,2,4);assert c.held;
        t.end(145,104);counts(c,3,3,5);assert !t.active(); // Slider drag still starts before UP.

        c.kind=UiTouchInput.DRAG|UiTouchInput.TAP|UiTouchInput.FREE_DRAG;
        assert t.begin(100,100);t.move(100,140);counts(c,4,3,5);assert c.held;
        t.end(105,180);counts(c,4,4,6);assert !t.active(); // Pipe tiles, maps and slots drag on both axes.

        c.kind=UiTouchInput.DRAG|UiTouchInput.TAP;
        assert t.begin(100,100);t.move(100,140);t.end(100,180);
        counts(c,4,4,7);assert !t.active(); // A vertical swipe on an ordinary horizontal slider still only points.

        c.kind=UiTouchInput.TAP;assert t.begin(1,1);t.cancel();
        counts(c,4,4,7);assert c.resets==1&&!t.active();
        assert t.begin(1,1);t.end(1,1);t.cancel();
        assert c.resets==2&&!c.held&&!t.active();
        System.out.println("PASS BioShock original UI: release tap, one DOWN per poll, swipe without click, delayed hover ACK, slider drag and cancel");
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-ui-touch-") as temporary:
            source = Path(temporary) / "UiTouchCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary,
                            ROOT / "android/app/src/org/bioshock/port/UiTouchInput.java", source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary,
                            "org.bioshock.port.UiTouchCheck"], check=True)


if __name__ == "__main__":
    unittest.main()
