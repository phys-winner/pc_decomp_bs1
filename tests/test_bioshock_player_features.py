"""Language selection, optional Russian files, water volume, and picker commit semantics."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.test_touch_controls import TouchControlsTest

ROOT = Path(__file__).resolve().parents[1]


class PlayerFeaturesTest(unittest.TestCase):
    def test_language_profile_and_optional_files(self):
        program = r'''
package org.bioshock.port;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
public class LanguageCheck {
    public static void main(String[] args) throws Exception {
        File root=new File(args[0]),profile=new File(root,"userdata");profile.mkdirs();
        String ini="[WinDrv.WindowsClient]\nFullscreenViewportX=1280\nFullscreenViewportY=720\nStartupFullscreen=True\n"+
            "[Engine.Engine]\nLanguage=rus\n[Other]\nLanguage=keep\n";
        NativeProfile.initialize(profile,2944,1840,()->new ByteArrayInputStream(ini.getBytes(StandardCharsets.UTF_8)),"int");
        File config=new File(profile,"AppData/Roaming/Bioshock/Bioshock.ini");
        String english=new String(Files.readAllBytes(config.toPath()),StandardCharsets.UTF_8);
        assert english.contains("Language=int\n")&&english.contains("Language=keep\n");
        File user=new File(config.getParentFile(),"User.ini");Files.write(user.toPath(),new byte[]{1,2,3});
        NativeProfile.initialize(profile,2944,1840,()->{throw new AssertionError();},"fra");
        String french=new String(Files.readAllBytes(config.toPath()),StandardCharsets.UTF_8);
        assert french.equals(english.replace("Language=int\n","Language=fra\n"));
        assert java.util.Arrays.equals(Files.readAllBytes(user.toPath()),new byte[]{1,2,3});
        try{NativeProfile.initialize(profile,2944,1840,()->null,"../rus");throw new AssertionError();}catch(IOException expected){}
        assert GameLanguage.missing(null,"int","int").isEmpty();
        assert GameLanguage.missing(root,"rus","rus").size()==26;
        assert GameLanguage.missing(root,"rus","int").size()==1;
        assert GameLanguage.missing(root,"int","rus").size()==25;
        assert GameLanguage.voice("rus", "same").equals("rus");
        assert GameLanguage.voice("rus", "int").equals("int");
        for(String path:GameLanguage.missing(root,"rus","rus")){
            File f=new File(root,path);f.getParentFile().mkdirs();Files.write(f.toPath(),new byte[]{1});
        }
        assert !new File(root,"Content/BinkMovies").exists();
        assert GameLanguage.missing(root,"rus","rus").isEmpty();
        assert GameLanguage.missing(root,"../rus","../rus").size()==1;
        File voice=new File(root,"Content/Sounds_Windows/streams_0_audio.rus_fsb");Files.write(voice.toPath(),new byte[0]);
        assert GameLanguage.missing(root,"rus","rus").size()==1;
        assert GameLanguage.missing(root,"rus","int").isEmpty();
        assert GameLanguage.missing(root,"int","rus").size()==1;
        assert GameLanguage.missing(root,"deu","deu").size()==26;
        System.out.println("PASS actual INI language updates preserve resolution, other sections and User.ini; optional pack presence and partial installs");
    }
}
'''
        self.run_java("LanguageCheck", program, [ROOT / "android/app/src/org/bioshock/port" / f
                      for f in ("NativeProfile.java", "GameLanguage.java")])

    def run_java(self, name, source, extra=()):
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            path = folder / (name + ".java")
            path.write_text(source, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", folder, *extra, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", folder, "org.bioshock.port." + name, temporary], check=True)

    def test_water_volume_at_all_tilts(self):
        source = (ROOT.parent / "launcher/android/src/port/launcher/Ui.java").read_text(encoding="utf-8")
        method = TouchControlsTest._method(source, "static float waterLevel(")
        self.run_java("WaterCheck", "package org.bioshock.port; public class WaterCheck {" + method + r'''
public static void main(String[]args){
    for(float w:new float[]{120,500,1300})for(float h:new float[]{48,80,160})for(int i=-100;i<=100;i++){
        float slope=i/100f,level=waterLevel(w,h,slope);double wet=0;
        for(int x=0;x<10000;x++)wet+=Math.max(0,Math.min(h,h-level-((x+.5)*w/10000-w/2)*slope));
        assert Math.abs(wet/(10000*h)-.8)<.0001:wet/(10000*h);
    }
    System.out.println("PASS water remains 80% full across 1,809 button dimensions/tilts");
}}''')

    def test_ios_selection_uses_original_commands(self):
        node = shutil.which("node")
        script = r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert'),events=[];
const root={fscommand:(...a)=>events.push(a)};root._root=root;
vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),root);
root.PortIosSelect('weapon','Shotgun');root.PortIosSelect('plasmid','IcicleAssaultAbility');
root.PortIosSelect('eve','BioAmmoHypo');root.PortIosSelect('eve','bogus');root.PortIosSelect('bad','Shotgun');
assert.deepEqual(events,[['SelectWeapon','class=Shotgun'],['SelectPlasmid','class=IcicleAssaultAbility'],['UseHypo','class=BioAmmoHypo']]);
console.log('PASS iOS exact weapon/plasmid/EVE commands and invalid category rejection');
"""
        subprocess.run([node, "-e", script, str(ROOT / "android/flash/hud_touch.as")], check=True)

    def test_picker_preview_is_side_effect_free_until_release(self):
        node = shutil.which("node")
        self.assertIsNotNone(node)
        script = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');const events=[];
function bar(prefix,n){const b={slotsFilled:n};for(let i=0;i<n;i++)b[prefix+i]={contents:{attachedMovieName:prefix+i},friendlyName:'Item '+i,gotoAndPlay(){}};return b;}
const root={HandsIcons:{weaponScrollBar:bar('wpnMenu',8),abilityScrollBar:bar('abilityMenu',3)},MaxWeapons:8,MaxAbilities:3,
 SelectedWeapon(c){events.push(['weapon',c]);},SelectedAbility(c){events.push(['plasmid',c]);},LBReleased(){events.push(['close']);}};
root._root=root;root.fscommand=(...a)=>events.push(a);vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),root);
root.PortTouchSelect('0','0');assert.equal(root.PortTouchClass,'wpnMenu3');assert.equal(events.length,0);
root.PortTouchSelect('0','15');assert.equal(root.PortTouchClass,'wpnMenu7');assert.equal(events.length,0);
root.PortTouchSelect('1','-15');assert.equal(root.PortTouchClass,'abilityMenu0');assert.equal(events.length,0);
root.PortTouchCommit();assert.deepEqual(events,[['plasmid','abilityMenu0'],['close']]);
events.length=0;root.HandsIcons.abilityScrollBar.slotsFilled=0;root.PortTouchSelect('1','0');root.PortTouchCommit();assert.deepEqual(events,[['close']]);
console.log('PASS original picker preview, row change, clamped slots, release commit and empty row cancellation');
'''
        subprocess.run([node, "-e", script, str(ROOT / "android/flash/touch_picker.as")], check=True)


if __name__ == "__main__":
    unittest.main()
