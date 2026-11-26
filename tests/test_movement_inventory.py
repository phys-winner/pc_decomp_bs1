"""Exercise the configurable stick, Classic bar and movement settings page."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests.test_touch_controls import touch_input_source
from tools.patch_native_settings import PORT_NAVIGATION

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/org/bioshock/port"


class MovementInventoryTest(unittest.TestCase):
    def test_stick_and_bar(self):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        source = r"""
package org.bioshock.port;
import java.util.Collections;
public class MovementInventoryCheck {
    static class Sink implements TouchInput.Sink {
        float look;
        public void submit(float x,float y,float dx,float dy,int held){look+=dx;}
        public boolean stick(float x,float y){return true;}
        public void reset(){}
    }
    static void near(float a,float b){assert Math.abs(a-b)<.0001f:a+" != "+b;}
    public static void main(String[] args){
        Sink sink=new Sink();
        TouchInput input=new TouchInput(sink);
        input.configure(100,1,1,false);
        for(int percent:new int[]{0,5,8,12,16,20,25,30,40}){
            assert PortPreferences.validStickDeadZone(percent);
            input.deadZone(percent);
            input.down(1,0,0,0,true);
            input.move(1,percent,0);near(input.moveX(),0);
            input.move(1,percent+.1f,0);assert input.moveX()>0;
            input.move(1,(100+percent)/2f,0);near(input.moveX(),.5f);
            input.move(1,100,-100);near(input.moveX(),(float)Math.sqrt(.5));near(input.moveY(),input.moveX());
            input.up(1,100,-100);
        }
        assert !PortPreferences.validStickDeadZone(13);
        input.deadZone(0);
        input.down(1,160,100,0,true);near(input.anchorX(),160);near(input.moveX(),0);
        input.move(1,170,100);near(input.moveX(),.1f);near(input.anchorX(),160);
        input.up(1,170,100);
        input.movementStick(true,100,100,50,false);
        input.down(1,125,100,0,true);near(input.anchorX(),100);near(input.moveX(),.5f);
        input.up(1,125,100);
        input.down(1,175,100,0,true);assert input.moving();input.up(1,175,100);
        input.down(1,176,100,0,true);assert !input.moving();input.move(1,186,100);near(sink.look,10);
        input.up(1,186,100);
        input.movementStick(true,100,100,50,true);
        input.down(1,160,100,0,false);assert input.moving();input.up(1,160,100);

        near(TouchLayout.stickAlpha(0,true,.65f),0);
        near(TouchLayout.stickAlpha(1,false,.65f),0);
        near(TouchLayout.stickAlpha(1,true,.35f),.45f);
        near(TouchLayout.stickAlpha(2,false,.65f),.65f*.22f);
        near(TouchLayout.stickAlpha(2,true,.8f),.8f);
        assert TouchLayout.weaponSwipeStep(71,0,1)==0 && TouchLayout.weaponSwipeStep(72,0,1)==1;
        assert TouchLayout.weaponSwipeStep(-144,0,2)==-1 && TouchLayout.weaponSwipeStep(72,72,1)==0;
        assert input.allowWeaponStep(0) && !input.allowWeaponStep(149_999_999);
        assert input.allowWeaponStep(150_000_000) && !input.allowWeaponStep(150_000_000);

        float[] crouch={10,145,140,300};
        float[] stick=TouchLayout.movementStick(10,0,510,300,60,1,false,
            Collections.singletonList(crouch),null);
        assert stick!=null && !TouchLayout.overlaps(stick,crouch,4) && stick[2]<=210 && stick[3]<=300;
        float[] mirror=TouchLayout.movementStick(10,0,510,300,60,1,true,
            Collections.singletonList(new float[]{380,145,510,300}),null);
        near(stick[0]+mirror[2],520);near(stick[3],mirror[3]);
        float[] block={220,235,300,285};
        float[] tab=TouchLayout.inventoryTab(10,0,510,285,48,1,Collections.singletonList(block),null);
        near((tab[0]+tab[2])/2,260);near(tab[2]-tab[0],48);
        assert tab[3]<=277 && !TouchLayout.overlaps(tab,block,8);
        InventoryBar bar=new InventoryBar();
        bar.layout(tab,18,502,48);
        int[] items={2,1,4,8,1,44,88, 1,11, 4,44, 8,88};
        assert bar.update(items);
        assert bar.count(0)==2 && bar.currentImage(0)==4 && bar.current(0,1) && bar.activeRow()==1;
        assert bar.down(1,260,tab[1]+10,0);bar.up(1,260,tab[1]+10,0);
        assert bar.open && bar.owns(1)==false;
        near(bar.progress(75),.5f);near(bar.rowTop(0,150),tab[1]-96);
        near(bar.rowTop(1,150),tab[1]-48);
        assert !bar.down(2,40,200,150) && !bar.open;
        assert bar.down(3,260,tab[1]+10,300);bar.up(3,260,tab[1]+10,300);
        float x=bar.cellLeft(1,0)+24,y=bar.rowTop(1,450)+24;
        assert bar.down(4,x,y,450);
        InventoryBar.Selection item=bar.up(4,x,y,450);
        assert item.phase==12 && item.image==8 && item.itemClass==88 && !bar.open;
        assert bar.down(5,260,tab[1]+10,600);bar.up(5,260,tab[1]+10,600);
        assert bar.down(6,260,tab[1]+10,750);bar.up(6,260,tab[1]+10,750);
        assert !bar.open;
        int[] weapons={2,0,4,0,0,44,0, 1,11, 4,44};
        bar.update(weapons);
        assert bar.down(7,260,tab[1]+10,900);bar.up(7,260,tab[1]+10,900);
        near(bar.rowTop(0,1050),tab[1]-48);
        x=bar.cellLeft(0,0)+24;y=bar.rowTop(0,1050)+24;
        assert bar.down(8,x,y,1050);
        bar.update(new int[]{1,0,4,0,0,44,0, 4,44});
        assert bar.up(8,x,y,1060)==null;
        bar.cancel();assert !bar.open && !bar.owns(8);
        System.out.println("PASS dead-zone choices, fixed and floating centres, visibility, swipe threshold and rate, bar geometry and selection");
    }
}
"""
        with tempfile.TemporaryDirectory(prefix="bioshock-movement-") as directory:
            check = Path(directory) / "MovementInventoryCheck.java"
            check.write_text(source, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", directory,
                            touch_input_source(directory), JAVA / "TouchLayout.java", JAVA / "PortPreferences.java",
                            JAVA / "InventoryBar.java", check], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", directory,
                            "org.bioshock.port.MovementInventoryCheck"], check=True)

    def test_movement_settings_page(self):
        script = r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const calls=[],focus=()=>({gotoAndStop(){}}),button=()=>({Focus:focus()});
const panel={ColumnLeft:{},ColumnRight:{},Back:button(),Movement:button()};
for(let i=0;i<12;i++)panel['Row'+i]={Focus:focus(),Value:{},Left:{},Right:{},Fov:{Thumb:{}}};
const root={PortPanel:panel,PortEntry:button(),PortOriginalNames:[],PortSectionPage:2,
  PortOriginalSelections:5,SubOptionsScreen1:7,SubOptionsScreen2:8,_currentframe:8,ScrollBar0:{},
  fscommand(...args){calls.push(args);},ResetList(){}};
root._root=root;
vm.runInNewContext(process.argv[1]+fs.readFileSync(process.argv[2],'utf8'),root);
root.PortShow(2);assert(panel.Movement._visible);
root.UIHighlightUp();root.UIHighlightRight();assert(panel.Movement.Focus._visible);
root.AcceptOrSelect();assert.equal(root.PortPage,6);assert.equal(root.maxSelections,4);
assert(!panel.Movement._visible);assert.deepEqual(calls.pop(),['PortSettings','refreshMovement']);
assert.deepEqual(Array.from(root.PortKeys),['touchStickDeadZone','touchStickVisibility','touchStaticStick']);
assert.deepEqual(Array.from(root.PortChoices('touchStickDeadZone')),['0','5','8','12','16','20','25','30','40']);
root.PortSet('touchStickDeadZone','0');assert.equal(panel.Row0.Value.Caption,'Dead zone: 0%');
root.PortSet('touchStickVisibility','1');assert(panel.Row1.Value.Caption.endsWith('While touching'));
root.PortRussian=true;root.PortRender();assert.equal(panel.Header,'СТИК ДВИЖЕНИЯ');
root.PortAdjust(1,1);assert.deepEqual(calls.at(-2),['PortSettings','touchStickVisibility=2']);
root.CancelOrExit();assert.equal(root.PortPage,2);
root._currentframe=7;root.PortShow(3);root.PortOpenMovement();assert.equal(root.PortPage,6);
root.CancelOrExit();assert.equal(root.PortPage,3);
console.log('PASS Movement stick settings navigation, choices and EN/RU text');
"""
        subprocess.run([shutil.which("node"), "-e", script, PORT_NAVIGATION,
                        ROOT / "android/flash/port_settings.as"], check=True, timeout=10)

    def test_native_classic_inventory(self):
        env, linker = environment("x64")
        host = (ROOT / "android/host/host.h").read_text()
        runtime = (ROOT / "android/runtime/recomp_types.h").read_text()
        regs = host[host.index("typedef struct void_regs {"):host.index("} void_regs;") + len("} void_regs;")]
        end = runtime.index("} xmm_t;") + len("} xmm_t;")
        xmm = runtime[runtime.rindex("typedef union {", 0, end):end]
        slots = re.search(r"#define RECOMP_X87_SLOTS \d+", runtime).group()
        with tempfile.TemporaryDirectory(prefix="bioshock-classic-inventory-") as temporary:
            out = Path(temporary)
            (out / "bioshock_guest_state_fixture.h").write_text(xmm + "\n" + regs + "\n" + slots + r"""
#define MEM8(address) (*(uint8_t *)ADDR(address))
extern double g_st_win[RECOMP_X87_SLOTS];
extern int64_t g_st_i64_win[RECOMP_X87_SLOTS];
extern int g_fp_sp;
void void_regs_save(void_regs *out);
void void_regs_load(const void_regs *in);
uint32_t void_galloc(uint32_t bytes);
uint32_t void_gstrdup(const char *text);
void void_gfree(uint32_t address);
uint32_t void_call_guest_this(uint32_t address,uint32_t self,int nargs,const uint32_t *args);
""")
            subprocess.run([CLANG, "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                            "/DRECOMP_X87_WINDOW", "/clang:--target=x86_64-pc-windows-msvc", "/I" + str(out),
                            "/c", ROOT / "tests/test_touch_inventory.c", "/Fo" + str(out / "test.obj")],
                           cwd=ROOT, env=env, check=True)
            subprocess.run([linker, "/nologo", out / "test.obj", "/OUT:" + str(out / "test.exe")],
                           env=env, check=True)
            subprocess.run([out / "test.exe"], env=env, check=True, timeout=30)
