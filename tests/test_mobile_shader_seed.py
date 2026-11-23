"""Execute production Java mobile-PCS seeding while preserving engine cache progress."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/org/bioshock/port"


class MobileShaderSeedTest(unittest.TestCase):
    def test_production_seed_disabled_existing_case_and_failure_paths(self):
        program = r'''
package org.bioshock.port;
import java.io.*;
import java.nio.file.*;
import java.util.*;
public class MobileShaderSeedCheck {
    static File cache(File profile){return new File(profile,"ShaderCache-mobile-v3/ShaderCache.pcs");}
    static void noPartial(File profile){
        assert !cache(profile).exists():"Partial cache published";
        File folder=cache(profile).getParentFile();
        if(folder.isDirectory())for(File file:folder.listFiles())assert !file.getName().endsWith(".tmp"):"Seed temporary leaked";
    }
    public static void main(String[] args)throws Exception{
        File root=new File(args[0]);byte[] seed=new byte[50001];for(int i=0;i<seed.length;++i)seed[i]=(byte)(i*31+7);
        File off=new File(root,"off");assert off.mkdir();
        assert !NativeProfile.seedMobileShaders(off,false,()->{throw new AssertionError("OFF opened seed asset");});
        assert off.list().length==0:"OFF created shader cache paths";
        File fresh=new File(root,"fresh");assert fresh.mkdir();
        File legacy=new File(fresh,"ShaderCache-mobile-v2/ShaderCache.pcs");assert legacy.getParentFile().mkdir();
        byte[] legacyBytes={1,2,3,4};Files.write(legacy.toPath(),legacyBytes);
        File legacyProgram=new File(legacy.getParentFile(),"old-program.bin");Files.write(legacyProgram.toPath(),legacyBytes);
        assert NativeProfile.seedMobileShaders(fresh,true,()->new ByteArrayInputStream(seed));
        assert Arrays.equals(seed,Files.readAllBytes(cache(fresh).toPath())):"Seed bytes changed";
        assert cache(fresh).getParentFile().list().length==1;
        assert Arrays.equals(legacyBytes,Files.readAllBytes(legacy.toPath()))&&Arrays.equals(legacyBytes,Files.readAllBytes(legacyProgram.toPath()));
        assert legacy.getParentFile().list().length==2:"Previous mobile revision was altered";
        byte[] warm={9,8,7,6,5};Files.write(cache(fresh).toPath(),warm);
        assert !NativeProfile.seedMobileShaders(fresh,false,()->{throw new AssertionError("OFF after ON opened seed asset");});
        assert Arrays.equals(warm,Files.readAllBytes(cache(fresh).toPath()))&&Arrays.equals(legacyBytes,Files.readAllBytes(legacy.toPath()));
        assert !NativeProfile.seedMobileShaders(fresh,true,()->{throw new AssertionError("Warm cache opened seed asset");});
        assert Arrays.equals(warm,Files.readAllBytes(cache(fresh).toPath())):"Warm engine progress overwritten";
        File mixed=new File(root,"mixed");File mixedFolder=new File(mixed,"sHaDeRcAcHe-MoBiLe-v3");assert mixedFolder.mkdirs();
        File mixedFile=new File(mixedFolder,"sHaDeRcAcHe.PcS");Files.write(mixedFile.toPath(),warm);
        assert !NativeProfile.seedMobileShaders(mixed,true,()->{throw new AssertionError("Mixed existing file opened seed");});
        assert Arrays.equals(warm,Files.readAllBytes(mixedFile.toPath()))&&mixedFolder.list().length==1;
        Files.delete(mixedFile.toPath());
        assert NativeProfile.seedMobileShaders(mixed,true,()->new ByteArrayInputStream(seed));
        assert Arrays.equals(seed,Files.readAllBytes(new File(mixedFolder,"ShaderCache.pcs").toPath()));
        File missing=new File(root,"missing");assert missing.mkdir();
        try{NativeProfile.seedMobileShaders(missing,true,()->{throw new IOException("Missing seed asset");});throw new AssertionError("Missing seed accepted");}
        catch(IOException expected){noPartial(missing);}
        File broken=new File(root,"broken");assert broken.mkdir();
        try{NativeProfile.seedMobileShaders(broken,true,()->new InputStream(){
            int calls;@Override public int read()throws IOException{throw new IOException("Unexpected scalar read");}
            @Override public int read(byte[] bytes,int start,int length)throws IOException{
                if(calls++>0)throw new IOException("Interrupted seed input");
                Arrays.fill(bytes,start,start+length,(byte)3);return length;
            }
        });throw new AssertionError("Interrupted seed accepted");}catch(IOException expected){noPartial(broken);}
        assert NativeProfile.seedMobileShaders(broken,true,()->new ByteArrayInputStream(seed)):"Failed seed could not be retried";
        File blocked=new File(root,"blocked");assert cache(blocked).mkdirs();
        try{NativeProfile.seedMobileShaders(blocked,true,()->{throw new AssertionError("Directory target opened seed");});throw new AssertionError("Directory cache accepted");}
        catch(IOException expected){assert cache(blocked).isDirectory();}
        File racing=new File(root,"racing");assert racing.mkdir();
        assert !NativeProfile.seedMobileShaders(racing,true,()->{
            Files.write(cache(racing).toPath(),warm,StandardOpenOption.CREATE_NEW);return new ByteArrayInputStream(seed);
        }):"Seed replaced a cache created while asset opened";
        assert Arrays.equals(warm,Files.readAllBytes(cache(racing).toPath()))&&cache(racing).getParentFile().list().length==1;
        // The generated production seed can also be passed without interpreting its binary format.
        if(args.length>1){
            File real=new File(root,"real-seed");assert real.mkdir();
            assert NativeProfile.seedMobileShaders(real,true,()->new FileInputStream(args[1]));
            assert Arrays.equals(Files.readAllBytes(Paths.get(args[1])),Files.readAllBytes(cache(real).toPath()));
        }
        System.out.println("PASS production mobile v3 PCS seed: OFF never reads, v2 untouched, streaming bytes exact, warm and mixed-case progress preserved, missing/interrupted assets leave no partial file, nonreplace race and retry");
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-mobile-seed-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe = folder / "MobileShaderSeedCheck.java"
            probe.write_text(program, encoding="utf-8")
            java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
            actual = ROOT / "build/mobile-shaders/mobile-shader-cache.pcs"
            for command in ([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(folder), str(probe), str(JAVA / "NativeProfile.java")],
                            [str(java / "java.exe"), "-ea", "-cp", str(folder), "org.bioshock.port.MobileShaderSeedCheck", str(folder), *([str(actual)] if actual.is_file() else [])]):
                result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        startup = function((JAVA / "GameActivity.java").read_text(encoding="utf-8"), "private void startIfReady(")
        self.assertLess(startup.index("NativeProfile.seedMobileShaders("), startup.index("SessionLog.begin("))
        self.assertLess(startup.index("NativeProfile.seedMobileShaders("), startup.index("nativeStart("))
        # Since 19.11 the Direct3D 10 renderer compiles the game's own shaders.spk at build time: the
        # Direct3D 9 mobile shader cache is never seeded, whatever the old mobileShaders preference says.
        self.assertIn("NativeProfile.seedMobileShaders(userdata, false, null);", startup)
        self.assertNotIn('getBoolean("mobileShaders"', startup)
        self.assertNotIn('"mobile-shader-cache.pcs"', startup)


if __name__ == "__main__":
    unittest.main()
