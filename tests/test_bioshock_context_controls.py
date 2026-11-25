"""Execute the shipped HUD action/prompt logic against the original HUDMessage."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
import pefile
from tests import test_touch_controls as touch_checks

ROOT = Path(__file__).resolve().parents[1]


class ContextControlsTest(unittest.TestCase):
    def test_playstation_prompt_detection_handles_hid_bridge_vendor_ids(self):
        pad = (ROOT / "android/app/src/org/bioshock/port/GamepadInput.java").read_text(encoding="utf-8")
        method = touch_checks.TouchControlsTest._method(pad, "static boolean isPlayStation(")
        source = r'''
class InputDevice {
    final int vendor; final String name;
    InputDevice(int vendor, String name) { this.vendor=vendor; this.name=name; }
    int getVendorId() { return vendor; } String getName() { return name; }
}
class PlayStationPromptCheck {
    /* METHOD */
    public static void main(String[] args) {
        assert isPlayStation(new InputDevice(0x054c, "Wireless Controller"));
        assert isPlayStation(new InputDevice(0x1d6b, "Sony DualSense (USB Test)"));
        assert isPlayStation(new InputDevice(0x1d6b, "PlayStation 5 Controller"));
        assert !isPlayStation(new InputDevice(0x045e, "Xbox Wireless Controller"));
        assert !isPlayStation(new InputDevice(0, "Generic Gamepad"));
        assert !isPlayStation(null);
        System.out.println("PASS PlayStation prompt detection: Sony VID and generic-VID HID names; Xbox/generic excluded");
    }
}
'''.replace("/* METHOD */", method)
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        with tempfile.TemporaryDirectory(prefix="bioshock-pad-prompts-") as temporary:
            path = Path(temporary) / "PlayStationPromptCheck.java"
            path.write_text(source, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "PlayStationPromptCheck"], check=True)

    def test_original_selector_checks_use_predicate_before_selecting_focus(self):
        raw = Path("C:/Games/BioShock/Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        base = pe.OPTIONAL_HEADER.ImageBase
        code = pe.get_data(0x10CDB703 - base, 0x26)
        self.assertEqual(code[:3], bytes.fromhex("8bcee8"))
        self.assertEqual(0x10CDB70A + int.from_bytes(code[3:7], "little", signed=True), 0x10CDAD00)
        self.assertEqual(code[7:11], bytes.fromhex("85c0741b"))  # false CanBeUsedNow jumps past the focus assignment.
        self.assertEqual(pe.get_data(0x10CDB723 - base, 6), bytes.fromhex("89b750090000"))

    def test_external_input_detection_and_dialog_lifecycle(self):
        activity = (ROOT / "android/app/src/org/bioshock/port/GameActivity.java").read_text(encoding="utf-8")
        pad = (ROOT / "android/app/src/org/bioshock/port/GamepadInput.java").read_text(encoding="utf-8")
        methods = "\n".join(touch_checks.TouchControlsTest._method(activity, name) for name in (
            "private void inputDevicesChanged()", "private void offerTouchVisibility()", "static boolean shouldOfferTouch("))
        detection = "\n".join(touch_checks.TouchControlsTest._method(pad, name) for name in (
            "static boolean isGamepad(", "static boolean externalInputAttached()"))
        source = r'''
import java.util.*;
class MotionEvent {static final int AXIS_X=0,AXIS_Y=1;}
class InputDevice {
    static final int SOURCE_GAMEPAD=0x401,SOURCE_JOYSTICK=0x1000010,SOURCE_MOUSE=0x2002,SOURCE_TOUCHPAD=0x100008,KEYBOARD_TYPE_ALPHABETIC=2;
    static final LinkedHashMap<Integer,InputDevice> all=new LinkedHashMap<>();
    final String name;final int sources,keyboard;final boolean virtual,axes;
    InputDevice(String n,int s,int k,boolean v,boolean a){name=n;sources=s;keyboard=k;virtual=v;axes=a;}
    static int[] getDeviceIds(){int[]ids=new int[all.size()];int i=0;for(int id:all.keySet())ids[i++]=id;return ids;}
    static InputDevice getDevice(int id){return all.get(id);}
    boolean isVirtual(){return virtual;}int getSources(){return sources;}int getKeyboardType(){return keyboard;}
    boolean supportsSource(int source){return (sources&source)==source;}
    String getName(){return name;}Object getMotionRange(int axis,int source){return axes?this:null;}
}
class GamepadInput { /* DETECTION */ }
class Preferences {boolean enabled=true;boolean getBoolean(String k,boolean d){return enabled;}void put(String k,boolean v){assert k.equals("touchEnabled");enabled=v;}}
class Controls {boolean ready=true;int releases,reloads;boolean canOfferInputPrompt(){return ready;}void release(){++releases;}void reloadSettings(){++reloads;}}
class AlertDialog {
    interface Click{void click(Object d,int which);}interface Dismiss{void dismiss(Object d);}
    Click positive,negative;Dismiss dismissed;boolean shown;
    void show(){shown=true;}void dismiss(){if(shown){shown=false;if(dismissed!=null)dismissed.dismiss(this);}}
    void setOnDismissListener(Dismiss d){dismissed=d;}
    static class Builder {
        final AlertDialog dialog=new AlertDialog();Builder(Object activity){}
        Builder setTitle(String t){return this;}Builder setMessage(String t){return this;}
        Builder setNegativeButton(String t,Click f){dialog.negative=f;return this;}
        Builder setPositiveButton(String t,Click f){dialog.positive=f;return this;}AlertDialog create(){return dialog;}
    }
}
public class InputDialogCheck {
    boolean started=true,activityResumed=true,surfaceReady=true,focused=true,externalInputAttached,inputPrompted,paused;
    int status=Integer.MAX_VALUE,pauses,saves;AlertDialog inputDialog,pauseDialog;final Controls controls=new Controls();final Preferences portPreferences=new Preferences();
    boolean hasWindowFocus(){return focused;}int nativeStatus(){return status;}void nativePause(boolean p){paused=p;++pauses;}
    String text(String a,String b){return a;}void savePortSettings(Object x){++saves;}void immersive(){}
    /* METHODS */
    static void devices(InputDevice... devices){InputDevice.all.clear();for(int i=0;i<devices.length;i++)InputDevice.all.put(i+1,devices[i]);}
    static void device(String n,int s,int k,boolean v,boolean axes){devices(new InputDevice(n,s,k,v,axes));}
    public static void main(String[]args){
        for(String n:new String[]{"fingerprint","uinput-fpc-key","uinput-goodix-fingerprint"}){
            device(n,InputDevice.SOURCE_GAMEPAD,2,false,true);assert !GamepadInput.externalInputAttached();
        }
        device("Virtual keyboard",0,2,true,false);assert !GamepadInput.externalInputAttached();
        device("sensor",InputDevice.SOURCE_JOYSTICK,0,false,false);assert !GamepadInput.externalInputAttached();
        device("USB keyboard",0,2,false,false);assert !GamepadInput.externalInputAttached();
        device("USB mouse",InputDevice.SOURCE_MOUSE,0,false,false);assert !GamepadInput.externalInputAttached();
        devices(new InputDevice("USB keyboard",0,2,false,false),new InputDevice("USB mouse",InputDevice.SOURCE_MOUSE,0,false,false));
        assert GamepadInput.externalInputAttached();
        devices(new InputDevice("USB mouse",InputDevice.SOURCE_MOUSE,0,false,false),new InputDevice("USB keyboard",0,2,false,false));
        assert GamepadInput.externalInputAttached();
        devices(new InputDevice("Virtual keyboard",0,2,true,false),new InputDevice("USB mouse",InputDevice.SOURCE_MOUSE,0,false,false));
        assert !GamepadInput.externalInputAttached();
        device("pad",InputDevice.SOURCE_JOYSTICK,0,false,true);assert GamepadInput.externalInputAttached();
        device("pad",InputDevice.SOURCE_GAMEPAD,0,false,false);assert GamepadInput.externalInputAttached();
        for(boolean ext:new boolean[]{false,true})for(boolean enabled:new boolean[]{false,true})
            assert shouldOfferTouch(ext,enabled)==(ext==enabled):"mismatch prompt state ext="+ext+" enabled="+enabled;
        InputDialogCheck a=new InputDialogCheck();a.externalInputAttached=true;a.controls.ready=false;a.offerTouchVisibility();assert !a.inputPrompted&&a.inputDialog==null;
        a.controls.ready=true;a.focused=false;a.offerTouchVisibility();assert !a.inputPrompted;
        a.focused=true;a.offerTouchVisibility();assert a.paused&&a.inputDialog.shown&&a.controls.releases==1;
        AlertDialog first=a.inputDialog;int pausedOnce=a.pauses; a.offerTouchVisibility();
        assert a.inputDialog==first&&a.controls.releases==1&&a.pauses==pausedOnce;
        InputDevice.all.clear();a.inputDevicesChanged();
        assert !a.externalInputAttached&&a.inputDialog==null&&!a.inputPrompted&&!a.paused;
        a.offerTouchVisibility();assert a.inputDialog==null&&a.controls.releases==1;
        a.portPreferences.enabled=false;a.offerTouchVisibility();assert a.inputDialog!=null&&a.paused;
        AlertDialog enabling=a.inputDialog;enabling.positive.click(enabling,1);enabling.dismiss();
        assert a.portPreferences.enabled&&a.controls.reloads==1&&a.saves==1&&!a.paused;
        device("USB keyboard",0,2,false,false);a.inputDevicesChanged();assert a.inputDialog==null&&!a.externalInputAttached;
        devices(new InputDevice("USB keyboard",0,2,false,false),new InputDevice("USB mouse",InputDevice.SOURCE_MOUSE,0,false,false));
        a.inputDevicesChanged();assert a.inputDialog!=null&&a.externalInputAttached;
        AlertDialog disabling=a.inputDialog;disabling.positive.click(disabling,1);disabling.dismiss();assert !a.portPreferences.enabled&&a.controls.reloads==2;
        a.offerTouchVisibility();assert a.inputDialog==null&&!a.inputPrompted;
        InputDevice.all.clear();a.inputDevicesChanged();assert a.inputDialog!=null&&!a.externalInputAttached;
        a.activityResumed=false;a.inputDialog.dismiss();assert a.paused;
        System.out.println("PASS actual external input filtering and dialog: keyboard/mouse pair, independent inputs, pad detection, mismatch transitions, idempotent offer, accept/decline, background dismissal stays paused");
    }
}
'''.replace("/* DETECTION */", detection).replace("/* METHODS */", methods)
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        with tempfile.TemporaryDirectory(prefix="bioshock-input-dialog-") as temporary:
            path = Path(temporary) / "InputDialogCheck.java"
            path.write_text(source, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", temporary, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "InputDialogCheck"], check=True)

    def test_original_hud_messages_and_reversible_touch_prompts(self):
        node = shutil.which("node")
        self.assertIsNotNone(node)
        script = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
let searches=0;
// Original 10C04DA0 searches receiver + byteOffset; 10C1A240 returns the UTF-8 codepoint index.
function nativeIndexOf(receiver,needle,offset){
    if(++searches>100)throw Error('original UTF-8 indexOf search did not terminate');
    const text=Buffer.from(String(receiver),'utf8'),find=Buffer.from(String(needle),'utf8');
    const found=text.indexOf(find,Math.max(0,Number(offset)||0));
    return found<0?-1:Array.from(text.subarray(0,found).toString('utf8')).length;
}
const events=[];const root={nativeIndexOf,trace(){},fscommand(...a){events.push(a);}};
function clip(){return {_currentframe:1,gotoAndPlay(n){this._currentframe=n;}};}
root.HandsIcons={_visible:true};
root.centerBox={ReticleHolder:{},GathererSaveFocus:clip(),GathererKillFocus:clip(),ReRoll:clip(),focusArea:clip()};
root.centerBox.GathererSaveFocus.GathererSaveText={};root.centerBox.GathererKillFocus.GathererKillText={};
root.centerBox.ReRoll.Contents={};root.centerBox.focusArea.actionLabel={};root.centerBox.focusArea.hackFocus=clip();
root.centerBox.focusArea.hackFocus.Contents={};
for(const m of ['ClearAAction','SetReticleUnderlay','ClearReticleUnderlay','ShowAppropriateAAction','ShowHackUnderlay','ClearHackUnderlay'])root[m]=()=>{};
root._root=root;vm.createContext(root);
vm.runInContext('String.prototype.split=String.prototype.lastIndexOf=String.prototype.charAt=function(){throw Error("unsupported String method");};',root);
vm.runInContext('String.prototype.indexOf=function(needle,offset){return nativeIndexOf(String(this),needle,offset);};',root);
assert.equal(nativeIndexOf('ИСПОЛЬЗОВАТЬ (E)',' (',0),12);
assert.equal(nativeIndexOf('ИСПОЛЬЗОВАТЬ (E)',' (',14),12);
vm.runInContext(fs.readFileSync(process.argv[1],'utf8'),root);
let original=fs.readFileSync(process.argv[2],'utf8').replace(/\r\n/g,'\n');
const header='function HUDMessage(MsgType, Msg)\n{';
original=original.replace(header,header+'\n   _root.PortRememberContext(MsgType,Msg);\n   Msg = _root.PortPrompt(MsgType,Msg);');
assert(original.includes('_root.PortRememberContext(MsgType,Msg);'));
vm.runInContext(original,root);
root.PortTouchPrompts('1','rus');assert.equal(events.at(-1)[1],'0');
const types=['UseFocus','HackFocus','GathererSaveFocus','GathererKillFocus','ReRollFocus'];
for(const type of types)assert.strictEqual(root['PortOriginal_'+type],'');
const nativeUndefined={toString(){return 'Undefined';}};
assert.notEqual(nativeUndefined,undefined);assert.equal(String(nativeUndefined),'Undefined');
for(const type of types)root['PortOriginal_'+type]=nativeUndefined;
root.PortTouchPrompts('1','int');assert.equal(root.PortContextFlags,0);assert.equal(events.at(-1)[1],'0');
for(const bogus of [nativeUndefined,undefined,null,'Undefined','undefined','Null','null'])for(const type of types){
    root.HUDMessage(type,bogus);assert.equal(root.PortContextFlags,0);assert.equal(root.PortPrompt(type,bogus),'');
    assert.strictEqual(root['PortOriginal_'+type],'');
}
root.PortTouchPrompts('1','rus');assert.equal(events.at(-1)[1],'0');
root.HUDMessage('UseFocus','ИСПОЛЬЗОВАТЬ (E)');
assert.equal(root.PortContextFlags,1);assert.equal(root.centerBox.focusArea.actionLabel.actionText,'ИСПОЛЬЗОВАТЬ (тап по центру)');
root.HUDMessage('HackFocus','ВЗЛОМАТЬ (V)');assert.equal(root.PortContextFlags,3);
assert.equal(events.at(-1)[1],'3');
assert.equal(root.centerBox.focusArea.hackFocus.Contents.HUDHack,'ВЗЛОМАТЬ (удерживайте центр)');
root.HUDMessage('GathererSaveFocus','СПАСТИ (L)');root.HUDMessage('GathererKillFocus','СОБРАТЬ (H)');
root.HUDMessage('ReRollFocus','ОБЫСКАТЬ ЕЩЕ (R)');assert.equal(root.PortContextFlags,31);
assert(root.centerBox.GathererSaveFocus.GathererSaveText.HUDSave.includes('кнопка Спасти'));
assert(root.centerBox.GathererKillFocus.GathererKillText.HUDMurder.includes('кнопка Собрать'));
assert(root.centerBox.ReRoll.Contents.HUDSearchAgain.includes('кнопка Обыскать'));
root.PortTouchPrompts('0','rus');assert.equal(root.PortContextFlags,31);
assert.equal(root.centerBox.focusArea.actionLabel.actionText,'ИСПОЛЬЗОВАТЬ (E)');
assert.equal(root.centerBox.focusArea.hackFocus.Contents.HUDHack,'ВЗЛОМАТЬ (V)');
root.PortTouchPrompts('1','int');assert.equal(root.centerBox.focusArea.actionLabel.actionText,'ИСПОЛЬЗОВАТЬ (tap screen center)');
assert.equal(root.PortPrompt('UseFocus','USE (SPECIAL) (E)'),'USE (SPECIAL) (tap screen center)');
assert.equal(root.PortPrompt('UseFocus','ИСПОЛЬЗОВАТЬ (ОСОБОЕ) (E)'),'ИСПОЛЬЗОВАТЬ (ОСОБОЕ) (tap screen center)');
assert.equal(root.PortPrompt('Training','Press E'),'Press E');
root.PortTouchPrompts('4','rus');assert.equal(root.PortContextFlags,31);assert.equal(root.HandsIcons._visible,false);
assert.equal(root.centerBox.focusArea.actionLabel.actionText,'ИСПОЛЬЗОВАТЬ');
assert.equal(root.PortPrompt('UseFocus','USE (SPECIAL) (E)'),'USE (SPECIAL)');
root.PortTouchPrompts('2','int');
assert.equal(root.centerBox.focusArea.actionLabel.actionText,'ИСПОЛЬЗОВАТЬ ' + String.fromCharCode(0xe063));
assert.equal(root.PortPrompt('UseFocus','PICK UP (E)'), 'PICK UP ' + String.fromCharCode(0xe063));
assert.equal(root.centerBox.focusArea.hackFocus.Contents.HUDHack,'ВЗЛОМАТЬ ' + String.fromCharCode(0xe064));
assert.equal(root.centerBox.ReRoll.Contents.HUDSearchAgain,'ОБЫСКАТЬ ЕЩЕ (RT)');
assert.equal(root.centerBox.GathererSaveFocus.GathererSaveText.HUDSave,'СПАСТИ ' + String.fromCharCode(0xe065));
root.PortTouchPrompts('3','rus');
assert.equal(root.centerBox.focusArea.actionLabel.actionText,'ИСПОЛЬЗОВАТЬ ' + String.fromCharCode(0xe060));
assert.equal(root.PortPrompt('UseFocus','ANSWER'), 'ANSWER ' + String.fromCharCode(0xe060));
assert.equal(root.centerBox.focusArea.hackFocus.Contents.HUDHack,'ВЗЛОМАТЬ ' + String.fromCharCode(0xe061));
assert.equal(root.centerBox.ReRoll.Contents.HUDSearchAgain,'ОБЫСКАТЬ ЕЩЕ (R2)');
assert.equal(root.centerBox.GathererKillFocus.GathererKillText.HUDMurder,'СОБРАТЬ ' + String.fromCharCode(0xe061));
assert.equal(root.centerBox.GathererSaveFocus.GathererSaveText.HUDSave,'СПАСТИ ' + String.fromCharCode(0xe062));
for(const type of ['UseFocus','HackFocus','GathererSaveFocus','GathererKillFocus','ReRollFocus'])root.HUDMessage(type,'');
assert.equal(root.PortContextFlags,0);assert.equal(events.at(-1)[1],'0');
console.log('PASS original HUD action availability, empty focus clears, all five contexts, Russian/English center gestures, key suffix removal and exact original-prompt restoration');
'''
        original = ROOT / "work/ha22-20261110/HUDPC-export/scripts/frame_1/DoAction_15.as"
        subprocess.run([node, "-e", script, str(ROOT / "android/flash/hud_touch.as"), str(original)],
                       cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
