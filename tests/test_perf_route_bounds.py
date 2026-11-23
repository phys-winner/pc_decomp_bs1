"""Check smoke anchors against current production touch targets, without Android."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import perf_route
from tests import test_touch_controls as touch_tests

ROOT = Path(__file__).resolve().parents[1]


class PerfRouteBoundsTest(unittest.TestCase):
    def test_backwards_and_look_avoid_context_controls_and_hud(self):
        controls = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(touch_tests.TouchControlsTest._method(controls, name) for name in
                            ("private boolean movementZone(", "private boolean ammoHit("))
        start = controls.index("if(centerId<0&&")
        center = controls[start + 3:controls.index(") {", start)]
        points = [perf_route.scale_point(p, (2400, 1080)) for p in
                  (perf_route.STICK, perf_route.BACKWARD, perf_route.LOOK_FROM, perf_route.LOOK_TO)]
        program = r'''
package org.bioshock.port;
public class RouteBoundsCheck {
    static final int HANDS_HOLDING=1<<16,HANDS_MELEE=1<<19,CONTEXT_USE=1<<4,CONTEXT_HACK=1<<5,CONTEXT_RESCUE=1<<6,CONTEXT_HARVEST=1<<7;
    static class Safe {float left,top,right,bottom;}
    final Safe safe=new Safe();float dp=2.75f,uiScale=1.5f;boolean leftHanded,playing=true,editing;
    int centerId=-1,vehicleFlags=CONTEXT_USE|CONTEXT_HACK,weaponSlots=HANDS_HOLDING;
    int getWidth(){return 2400;}int getHeight(){return 1080;}
    float width(){return getWidth()-safe.left-safe.right;}float height(){return getHeight()-safe.top-safe.bottom;}
    /* METHODS */
    boolean centerHit(float x,float y){return /* CENTER */;}
    void safePoint(float x,float y){
        assert !ammoHit(x,y)&&!centerHit(x,y):"HUD/center action at "+x+","+y;
        float scale=TouchLayout.fitScale(1,height()/dp);
        for(int c=0;c<TouchLayout.COUNT;++c){
            float[] at=TouchLayout.anchor(c,safe.left,safe.top,getWidth()-safe.right,getHeight()-safe.bottom,dp,scale,false);
            float radius=TouchLayout.size(c,dp,scale)*.5f+4*dp;
            assert Math.hypot(x-at[0],y-at[1])>radius:"Control "+TouchLayout.NAMES[c]+" at "+x+","+y;
        }
    }
    public static void main(String[]args){
        RouteBoundsCheck g=new RouteBoundsCheck();float[] p=new float[8];for(int i=0;i<8;++i)p[i]=Float.parseFloat(args[i]);
        for(float density:new float[]{2.5f,2.75f,3.5f})for(float inset:new float[]{0,80}){
            g.dp=density;g.safe.left=inset;
            assert g.movementZone(p[0],p[1])&&g.movementZone(p[2],p[3]);
            assert !g.movementZone(p[4],p[5])&&!g.movementZone(p[6],p[7]);
            for(int i=0;i<8;i+=2)g.safePoint(p[i],p[i+1]);
        }
        g.dp=2.75f;g.safe.left=0;
        float[] hack=TouchLayout.anchor(TouchLayout.HACK,0,0,2400,1080,g.dp,1,false);
        assert Math.hypot(1424-hack[0],444-hack[1])<=TouchLayout.size(TouchLayout.HACK,g.dp,1)*.5f+4*g.dp:"Old route no longer reproduces Hack overlap";
        TouchInput input=new TouchInput(new TouchInput.Sink(){public void submit(float x,float y,float dx,float dy,int held){assert held==0;}public boolean stick(float x,float y){return true;}public void reset(){}});
        input.density(g.dp);input.down(1,p[0],p[1],0,TouchInput.MOVEMENT,0);input.move(1,p[2],p[3],4000);
        assert input.moveY()<0&&input.moveX()==0:"Route must retreat";input.up(1,p[2],p[3],4001);
        assert !input.moving()&&!input.hasContacts()&&input.held()==0;
        System.out.println("PASS current TouchLayout/center/ammo bounds: retreat and upper-right look avoid action targets; old look hits Hack; release neutral");
    }
}
'''.replace("/* METHODS */", methods).replace("/* CENTER */", center)
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        with tempfile.TemporaryDirectory(prefix="perf-route-") as temporary:
            source = Path(temporary) / "RouteBoundsCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary, source,
                            ROOT / "android/app/src/org/bioshock/port/TouchLayout.java", touch_tests.touch_input_source(temporary)], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "org.bioshock.port.RouteBoundsCheck",
                            *[str(value) for point in points for value in point]], check=True)


if __name__ == "__main__":
    unittest.main()
