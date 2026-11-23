"""Production Android event dispatch, including the outer context guard."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests import test_touch_controls as touch_checks

ROOT = Path(__file__).resolve().parents[1]


class PhotoGradeDispatchTest(unittest.TestCase):
    def test_actual_outer_dispatch_and_old_guard_counterfactual(self):
        source = (ROOT / 'android/app/src/org/bioshock/port/GameControls.java').read_text(encoding='utf-8')
        names = ('@Override public boolean onTouchEvent', 'private void pointerDown',
                 'private void pointerMove', 'private void pointerUp', 'private void uiDown',
                 'private int utilitySlot', 'private int action', 'private boolean visible',
                 'private boolean baseVisible', 'private boolean handsAllow', 'private int displayMode(',
                 'private boolean editable(', 'private boolean gameplayAllows(',
                 'private int hit', 'private boolean inside', 'private void visualState',
                 'private void syncContext', 'void release()', 'private void releaseInput')
        methods = '\n'.join(touch_checks.TouchControlsTest._method(source, name) for name in names)
        program = r'''
package org.bioshock.port;
class MotionEvent {
    static final int ACTION_DOWN=0,ACTION_UP=1,ACTION_MOVE=2,ACTION_CANCEL=3,ACTION_POINTER_DOWN=5,ACTION_POINTER_UP=6;
    final int action,id;final float x,y;
    MotionEvent(int a,int p,float x,float y){action=a;id=p;this.x=x;this.y=y;}
    int getActionMasked(){return action;}int getActionIndex(){return 0;}int getPointerId(int i){return id;}
    float getX(int i){return x;}float getY(int i){return y;}long getEventTime(){return 100;}
    int getPointerCount(){return 1;}int getHistorySize(){return 0;}
    float getHistoricalX(int i,int n){throw new AssertionError();}float getHistoricalY(int i,int n){throw new AssertionError();}
    long getHistoricalEventTime(int n){throw new AssertionError();}
}
class HapticFeedbackConstants {static final int VIRTUAL_KEY=1;}
class ViewConfiguration {static ViewConfiguration get(Object context){return new ViewConfiguration();}int getScaledTouchSlop(){return 24;}}
class FixtureView {public boolean onTouchEvent(MotionEvent e){return false;}}
class GameActivity {
    // Explicit native boundary only. Exact mode3/loading acceptance and real
    // pointer/DI/WM delivery are covered by test_photo_grade_context.c.
    static long context=5;static boolean photo=true,staleAfterPoint;
    static int pointers,attempts,accepted,downs,ups,resets,held,flashBegins;
    static long nativeTouchContext(){return context;}static int nativeTouchVehicle(){return 0;}
    static boolean nativePointer(float x,float y,boolean relative){
        ++pointers;if(staleAfterPoint){context+=4;staleAfterPoint=false;}return x>=0&&x<1&&y>=0&&y<1&&!relative;
    }
    static boolean nativeTouch(float x,float y,float dx,float dy,int mask,long expected){
        ++attempts;
        if(expected!=context||((context&3)==1&&!photo)||((context&3)!=2&&(x!=0||y!=0||dx!=0||dy!=0||(mask&~1)!=0))){nativeTouchReset();return false;}
        ++accepted;if(held!=mask){if(mask==1)++downs;else if(held==1)++ups;}held=mask;return true;
    }
    static void nativeTouchReset(){++resets;held=0;}
    static boolean nativeTouchStick(float x,float y,long context){return true;}
    static boolean nativeTouchWeapon(int a,int b,long context){throw new AssertionError();}
    static boolean nativeUiBegin(long expected){++flashBegins;return expected==context&&(context&3)==3;}
    static int nativeUiControl(long expected){return expected==context?UiTouchInput.TAP:-1;}
    static boolean nativeUiScroll(int rows,long expected){return expected==context;}
}
public final class PhotoDispatchCheck extends FixtureView {
    static final int MORE=-1,MODE=-2,SETTINGS=-3,KEYBOARD=-4,WEAPON=-5,SEATS=-6,PIE_ACTIONS=1,PIE_ABILITIES=2,HINT_CROUCH=1;
    static final int CONTEXT_USE=1<<4,CONTEXT_HACK=1<<5,CONTEXT_RESCUE=1<<6,CONTEXT_HARVEST=1<<7,
        CONTEXT_REROLL=1<<8,CONTEXT_CAN_AIM=1<<11,CONTEXT_CINEMATIC=1<<12,CONTEXT_MOVEMENT_ONLY=1<<14,PICKER_ACTIVE=1<<21;
    long cursorContext=-1,sensorTime;boolean authoritative,playing,editing,layoutPending,layoutDirty,pausedByControl,gyroActive,weaponPicker;
    int profile,profileOverride,menuId=-1,editId=-1,pinchId=-1,toolId=-1,toolPressed=-1,pickerRow;
    int weaponId=-1,scoresId=-1,swallowed=-1,scrubId=-1,pieId=-1,pieSelected=-1;
    boolean pieOpen;int[] utility={-1,-1,-1},utilityIds=new int[3];
    // Health-gauge control and hand state (no gauge outside gameplay, no hand bits from the game here).
    static final int HANDS_HOLDING=1<<16,HANDS_WEAPON=1<<17,HANDS_PLASMID=1<<18,HANDS_MELEE=1<<19,HANDS_VALID=1<<20;
    static final long VITALS_HOLD_MS=320,CENTER_HOLD_MS=400;static final int HINT_VITALS=1024;
    boolean touchEnabled=true,showUse=true,centerHeld;
    int vehicleFlags,centerId=-1,ammoId=-1;
    float centerX,centerY,ammoX,ammoY,weaponLastX,weaponLastY;
    long centerTime,centerContext,ammoTime;
    final Runnable centerHold=()->{},ammoHold=()->{};
    boolean ammoHit(float x,float y){return false;}
    void weaponStick(float x,float y){throw new AssertionError();}
    final int[] customDisplay=new int[TouchLayout.COUNT];
    PhotoDispatchCheck(){java.util.Arrays.fill(customDisplay,-1);}
    boolean showMore,vitalsControl=true;int vitalsId=-1,weaponSlots,hintsSeen;float vitalsX,vitalsY;long vitalsDownTime;
    final Runnable vitalsHold=()->{};Object getContext(){return this;}void markHint(int kind){hintsSeen|=kind;}
    boolean vitalsHit(float x,float y){return false;}
    void requestUnbufferedDispatch(MotionEvent e){}
    final int[] visualBefore=new int[5],visualAfter=new int[5];
    final boolean[] shown=new boolean[TouchLayout.COUNT];
    final float[] controlX=new float[TouchLayout.COUNT],controlY=new float[TouchLayout.COUNT],controlR=new float[TouchLayout.COUNT];
    float dp=1;final Runnable uiPoll=()->{},weaponHold=()->{},hintCheck=()->{};
    final TouchInput input=new TouchInput(new TouchInput.Sink(){
        public void submit(float x,float y,float dx,float dy,int held){GameActivity.nativeTouch(x,y,dx,dy,held,cursorContext);}
        public boolean stick(float x,float y){return true;}public void reset(){GameActivity.nativeTouchReset();}
    });
    final UiTouchInput uiInput=new UiTouchInput(new UiTouchInput.Sink(){
        public boolean begin(float x,float y){return pointer(x,y)&&GameActivity.nativeUiBegin(cursorContext);}
        public int control(){return GameActivity.nativeUiControl(cursorContext);}
        public boolean pointer(float x,float y){return GameActivity.nativePointer(x/getWidth(),y/getHeight(),false);}
        public boolean button(boolean down){return GameActivity.nativeTouch(0,0,0,0,down?1:0,cursorContext);}
        public boolean scroll(int value){return GameActivity.nativeUiScroll(value,cursorContext);}
        public void reset(){GameActivity.nativeTouchReset();}
    });
    int getWidth(){return 2780;}int getHeight(){return 1264;}
    void invalidate(){}void hideKeyboard(){}void removeCallbacks(Runnable r){}void postOnAnimation(Runnable r){}
    void postDelayed(Runnable r,long delay){}void stopGyro(){}void updateGyro(){}void readDetails(){}
    void finishEditing(boolean save){throw new AssertionError();}void editLayout(){throw new AssertionError();}boolean editTouch(MotionEvent e){throw new AssertionError();}
    void buildLayout(){for(int i=0;i<shown.length;++i){shown[i]=visible(i);controlX[i]=2400;controlY[i]=1080;controlR[i]=50;}layoutDirty=false;}
    boolean performClick(){return true;}boolean pieDown(int id,float x,float y){throw new AssertionError();}
    void pieMove(float x,float y){throw new AssertionError();}void pieUp(float x,float y){throw new AssertionError();}
    void openPie(int id,float x,float y,int kind,int source){throw new AssertionError();}
    void weaponDown(int id,float x,float y,long time){throw new AssertionError();}void weaponMove(float x,float y){throw new AssertionError();}
    void weaponUp(float x,float y,long t){throw new AssertionError();}void scoresDown(int id,float x,float y,long t){throw new AssertionError();}
    void scoresUp(float x,float y,long t){throw new AssertionError();}void scrubMove(float x){throw new AssertionError();}
    void scrubUp(float x,float y,long time){throw new AssertionError();}boolean scrubStart(int id,float x,boolean b){throw new AssertionError();}
    boolean weaponBarHit(float x,float y){return false;}boolean movementZone(float x,float y){return false;}
    void activate(int control){throw new AssertionError();}void haptic(int type){throw new AssertionError();}
    void offer(int bit,String en,String ru){throw new AssertionError();}
''' + methods + r'''
    void event(int action,int id,float x,float y){assert onTouchEvent(new MotionEvent(action,id,x,y));}
    void hidden(){for(int i=0;i<TouchLayout.COUNT;++i)assert !visible(i):i;}
    static void set(long context,boolean photo){GameActivity.context=context;GameActivity.photo=photo;}
    public static void main(String[] args){
        PhotoDispatchCheck view=new PhotoDispatchCheck();
        // All overlay controls, including keyboard, stay hidden. Tapping at a
        // former gameplay-button position still reaches the original PC click.
        view.event(0,4,2400,1080);view.hidden();
        assert GameActivity.downs==1&&GameActivity.held==1&&view.menuId==4;
        view.event(2,4,1390,632);assert GameActivity.held==1;
        view.event(1,4,1390,632);assert GameActivity.ups==1&&GameActivity.held==0&&view.menuId==-1&&GameActivity.flashBegins==0;
        for(float[] point:new float[][]{{1,1},{1390,632},{2779,1263}}){
            int down=GameActivity.downs,up=GameActivity.ups;view.event(0,8,point[0],point[1]);view.event(1,8,point[0],point[1]);
            assert GameActivity.downs==down+1&&GameActivity.ups==up+1;
        }
        // Same low bits, actual loading: native rejects; no menu owner/click.
        set(9,false);int down=GameActivity.downs,accepted=GameActivity.accepted;
        view.event(0,4,1390,632);view.event(1,4,1390,632);view.hidden();
        assert view.menuId==-1&&GameActivity.downs==down&&GameActivity.accepted==accepted&&GameActivity.held==0;
        // Cancel, an orphan UP and a stale transition cannot retain a press.
        set(13,true);view.event(0,5,1390,632);assert GameActivity.held==1;
        view.event(3,5,1390,632);assert GameActivity.held==0&&view.menuId==-1;
        down=GameActivity.downs;view.event(1,5,1390,632);assert GameActivity.downs==down&&GameActivity.held==0;
        set(17,true);GameActivity.staleAfterPoint=true;down=GameActivity.downs;
        view.event(0,7,1390,632);view.event(1,7,1390,632);assert view.menuId==-1&&GameActivity.held==0&&GameActivity.downs==down;
        // Native FinishedPhotoDisplay may clear the context on DOWN, before
        // Android UP. That UP must not become a Fire in resumed gameplay.
        set(25,true);view.event(0,8,1390,632);assert GameActivity.held==1;
        down=GameActivity.downs;set(30,false);view.event(1,8,1390,632);
        assert view.playing&&view.menuId==-1&&GameActivity.held==0&&GameActivity.downs==down;
        set(33,true);view.event(0,9,1390,632);set(37,false);view.event(1,9,1390,632);
        assert !view.playing&&view.menuId==-1&&GameActivity.held==0;view.hidden();
        // Ordinary flags3 still enters the production UiTouchInput/Flash path.
        set(39,false);int before=GameActivity.flashBegins;view.event(0,10,1390,632);view.event(1,10,1390,632);view.uiInput.poll();
        assert GameActivity.flashBegins==before+1&&!view.uiInput.active()&&GameActivity.held==0;
        System.out.println("PASS actual onTouchEvent->pointerDown/uiDown/pointerUp: photo flags1 click/move/up anywhere, hidden controls+keyboard, native loading rejection, cancel/orphan/stale and Photo-DOWN->gameplay-UP reset; original flags3 Flash route retained");
    }
}
'''
        java = Path(os.environ.get('JAVA_HOME', 'C:/Portable/jdk-24.0.1')) / 'bin'
        out = ROOT / 'work/photo-grade-292'
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='photo-dispatch-') as temporary:
            folder = Path(temporary)
            touch = touch_checks.touch_input_source(folder)
            fixture = folder / 'PhotoDispatchCheck.java'
            command = [java / 'javac.exe', '--release', '8', '-encoding', 'UTF-8', '-d', folder,
                       touch, ROOT / 'android/app/src/org/bioshock/port/TouchLayout.java',
                       ROOT / 'android/app/src/org/bioshock/port/UiTouchInput.java', fixture]
            fixture.write_text(program, encoding='utf-8')
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            run = [java / 'java.exe', '-ea', '-cp', folder, 'org.bioshock.port.PhotoDispatchCheck']
            result = subprocess.run(run, capture_output=True, text=True)
            (out / 'dispatch-host.log').write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end='')
            anchor = 'if(action==MotionEvent.ACTION_CANCEL) { releaseInput();return true; }'
            self.assertEqual(program.count(anchor), 1)
            old = program.replace(anchor, anchor + '\n        if((cursorContext&3)==1)return true;')
            fixture.write_text(old, encoding='utf-8')
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run(run, capture_output=True, text=True)
            (out / 'old-guard-counterfactual.log').write_text(result.stdout + result.stderr)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('AssertionError', result.stderr)
            first_down = next(i for i, line in enumerate(old.splitlines(), 1)
                              if 'assert GameActivity.downs==1&&GameActivity.held==1' in line)
            self.assertIn(f'PhotoDispatchCheck.java:{first_down}', result.stderr)


if __name__ == '__main__':
    unittest.main()
