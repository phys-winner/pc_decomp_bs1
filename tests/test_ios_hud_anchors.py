"""Original iOS HUD anchors and the production ammunition touch rectangle."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests import test_touch_controls

ROOT = Path(__file__).resolve().parents[1]


class IosHudAnchorsTest(unittest.TestCase):
    def test_ammunition_moves_without_accumulating_or_changing_pc_layout(self):
        program = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const root={PortSaved:1,PortStageWidth:1616,PortHudScale:1.5,PortHudClear:230,PortHudMode:1,
 centerBox:{},rightBox:{},HandsIcons:{},PhotoDisplayBox:{},ammoScrollBar:{},CurrentAbilityName:{},
 PortX_ammoScrollBar:8,PortY_ammoScrollBar:650,PortXS_ammoScrollBar:100,PortYS_ammoScrollBar:100,
 PortX_CurrentAbilityName:20,PortY_CurrentAbilityName:680,PortXS_CurrentAbilityName:100,PortYS_CurrentAbilityName:100};
root._root=root;vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),root);
root.PortTouchPromptMode=0;root.PortLayout();
const before=[root.ammoScrollBar._x,root.ammoScrollBar._y,root.CurrentAbilityName._x,root.CurrentAbilityName._y];
root.PortTouchPromptMode=4;root.PortLayout();
const after=[root.ammoScrollBar._x,root.ammoScrollBar._y,root.CurrentAbilityName._x,root.CurrentAbilityName._y];
for(let i=0;i<4;i++)assert.equal(after[i]-before[i],i%2?-(720-140*1.5-8):1616-260*1.5);
assert.equal(root.ammoScrollBarDefaultY,root.ammoScrollBar._y);
assert.equal(root.CurrentAbilityNameDefaultY,root.CurrentAbilityName._y);
root.PortLayout();assert.deepEqual([root.ammoScrollBar._x,root.ammoScrollBar._y,root.CurrentAbilityName._x,root.CurrentAbilityName._y],after);
root.PortTouchPromptMode=1;root.PortLayout();
assert.deepEqual([root.ammoScrollBar._x,root.ammoScrollBar._y,root.CurrentAbilityName._x,root.CurrentAbilityName._y],before);
console.log('PASS original HUD: top-right ammo/ability label in iOS, no repeated shift, original PC anchors restored');
'''
        subprocess.run([shutil.which("node"), "-e", program, str(ROOT / "android/flash/hud_layout.as")], check=True)

    def test_ios_ammo_change_keeps_the_original_compact_panel(self):
        program = r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert'),events=[];
const r={PortOriginalAmmoSelectionMenu:d=>events.push(d),AmmoButtonReleased:()=>events.push('release'),ResetAmmoMenu:()=>events.push('compact')};r._root=r;
vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),r);
r.PortTouchPromptMode=4;r.AmmoSelectionMenu('Down');assert.deepEqual(events,['Down','release','compact']);
events.length=0;r.PortTouchPromptMode=1;r.AmmoSelectionMenu('Up');assert.deepEqual(events,['Up']);
console.log('PASS iOS ammunition uses original switch and compact display; PC expanded menu unchanged');
"""
        subprocess.run([shutil.which("node"), "-e", program, str(ROOT / "android/flash/hud_touch.as")], check=True)

    def test_ios_hides_only_duplicate_ammunition_counts(self):
        program = r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const r={ammoScrollBar:{SelectedAmmoIcon:{count:{_visible:true}}}};r._root=r;
for(let i=0;i<3;i++)r.ammoScrollBar['ammoMenu'+i]={count:{_visible:true},contents:{_visible:true}};
vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),r);
r.PortTouchPromptMode=4;r.PortAmmoCounts();
for(let i=0;i<3;i++){assert.equal(r.ammoScrollBar['ammoMenu'+i].count._visible,false);assert.equal(r.ammoScrollBar['ammoMenu'+i].contents._visible,true);}
assert.equal(r.ammoScrollBar.SelectedAmmoIcon.count._visible,true);
r.PortTouchPromptMode=1;r.PortAmmoCounts();
for(let i=0;i<3;i++)assert.equal(r.ammoScrollBar['ammoMenu'+i].count._visible,true);
console.log('PASS iOS hides duplicate menu counts, keeps ammo icons and main counter; PC restored');
"""
        subprocess.run([shutil.which("node"), "-e", program, str(ROOT / "android/flash/hud_touch.as")], check=True)

    def test_production_ammo_hit_tracks_the_relocated_panel(self):
        controls = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(test_touch_controls.TouchControlsTest._method(controls, name)
                            for name in ("private float ammoHudTop(", "private boolean ammoHit("))
        program = r'''
class IosHudHitCheck {
 static final int HANDS_HOLDING=1<<16,HANDS_MELEE=1<<19;
 boolean playing=true,editing,ios;int weaponSlots=HANDS_HOLDING;float uiScale=1.5f;
 int getWidth(){return 2424;} int getHeight(){return 1080;} boolean iosScheme(){return ios;}
 /* METHODS */
 public static void main(String[]args){
  IosHudHitCheck c=new IosHudHitCheck();
  assert c.ammoHit(250,950) && !c.ammoHit(2200,150);
  c.ios=true;assert c.ammoHit(2200,150) && !c.ammoHit(250,950);
  assert !c.ammoHit(1800,150) && !c.ammoHit(2200,400);
  c.weaponSlots|=HANDS_MELEE;assert !c.ammoHit(2200,150);
  c.weaponSlots=0;assert !c.ammoHit(2200,150);
  System.out.println("PASS production ammunition hit: iOS top-right, PC bottom-left, unavailable/melee gates");
 }
}
'''.replace("/* METHODS */", methods)
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "IosHudHitCheck.java"
            fixture.write_text(program)
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary, fixture], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary, "IosHudHitCheck"], check=True)
