"""Exercise the production settings store, including invalid values and restart."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PortPreferencesTest(unittest.TestCase):
    def test_roundtrip_validation_and_atomic_write(self):
        program = r'''
package org.bioshock.port;
import java.io.*;
public class PreferencesCheck {
    public static void main(String[] args) throws Exception {
        File root=new File(args[0]);
        PortPreferences p=new PortPreferences(root);
        assert p.getInt("missing",60)==60;
        assert p.getFloat("missing",.22f)==.22f;
        p.put("fpsLimit",45); p.put("language","RUS");
        p.put("touchLeftHanded",true); p.put("touchSensitivity",.35f);
        p.put("invalidInt","junk"); p.put("invalidFloat","NaN");
        p.put("infinity","Infinity"); p.put("invalidBoolean","yes");
        p.save();
        PortPreferences loaded=new PortPreferences(root);
        assert loaded.getInt("fpsLimit",60)==45;
        assert loaded.getString("language","").equals("RUS");
        assert loaded.getBoolean("touchLeftHanded",false);
        assert loaded.getFloat("touchSensitivity",0)==.35f;
        assert loaded.getInt("invalidInt",60)==60;
        assert loaded.getFloat("invalidFloat",.22f)==.22f;
        assert loaded.getFloat("infinity",1)==1;
        assert !loaded.getBoolean("invalidBoolean",false);
        assert loaded.getBoolean("invalidBoolean",true);
        loaded.put("language","FRA"); loaded.save();
        assert new PortPreferences(root).getString("language","").equals("FRA");
        File properties=new File(root,"bioshock-port.properties");
        assert properties.isFile();
        assert !new File(properties.getPath()+".tmp").exists();
        loaded.put("touchX0",.4f); loaded.put("lastLayoutRevision",3); loaded.save();
        loaded.put("touchX0",.5f); loaded.save();
        PortPreferences withLayout=new PortPreferences(root);
        assert withLayout.getFloat("touchX0",0)==.5f;
        assert withLayout.getInt("lastLayoutRevision",0)==3;
        java.util.Properties external=new java.util.Properties();
        try(FileInputStream in=new FileInputStream(properties)){external.load(in);}
        external.setProperty("Language","RUS"); external.remove("language");
        external.setProperty("TouchLeftHanded","True"); external.remove("touchlefthanded");
        external.setProperty("preserve","external-value");
        try(FileOutputStream out=new FileOutputStream(properties)){external.store(out,"external edit");}
        assert withLayout.reloadIfChanged();
        assert withLayout.getString("language","").equals("RUS");
        assert withLayout.getBoolean("touchLeftHanded",false);
        assert withLayout.getFloat("touchX0",0)==.5f;
        withLayout.put("fpsLimit",60);withLayout.save();
        assert new PortPreferences(root).getString("preserve","").equals("external-value");
        withLayout.put("touchSensitivity",.7f);
        external.setProperty("touchsensitivity",".1");
        try(FileOutputStream out=new FileOutputStream(properties)){external.store(out,"pending local edits");}
        assert !withLayout.reloadIfChanged();
        assert withLayout.getFloat("touchSensitivity",0)==.7f;
        withLayout.save();
        try {new PortPreferences(null);throw new AssertionError();}catch(IOException expected){}
        System.out.println("PASS: settings defaults, malformed/nonfinite values, save verification, atomic replacement and restart");
    }
}
'''
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        with tempfile.TemporaryDirectory(prefix="bioshock-prefs-") as temporary:
            folder = Path(temporary)
            source = folder / "PreferencesCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", folder,
                            ROOT / "android/app/src/org/bioshock/port/PortPreferences.java", source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", folder,
                            "org.bioshock.port.PreferencesCheck", folder], check=True)

    def test_fractional_fps_roundtrip(self):
        program = r'''
package org.bioshock.port;
import java.io.File;
public class PreferencesFpsCheck {
    public static void main(String[] args) throws Exception {
        File root=new File(args[0]);
        PortPreferences settings=new PortPreferences(root);
        settings.put("fpsLimit",59.94f);
        settings.save();
        assert new PortPreferences(root).getFloat("fpsLimit",0)==59.94f;
        System.out.println("PASS BioShock fractional FPS setting persists without integer rounding");
    }
}
'''
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        with tempfile.TemporaryDirectory(prefix="bioshock-fps-prefs-") as temporary:
            source = Path(temporary) / "PreferencesFpsCheck.java"
            source.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-d", temporary,
                            ROOT / "android/app/src/org/bioshock/port/PortPreferences.java", source], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", temporary,
                            "org.bioshock.port.PreferencesFpsCheck", temporary], check=True)


if __name__ == "__main__":
    unittest.main()
