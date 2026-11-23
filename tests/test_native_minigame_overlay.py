"""Execute the real overlay copy/preparation methods against temporary files."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
ACTIVITY = ROOT / "android/app/src/org/bioshock/port/GameActivity.java"
COMBO = ROOT / "build/native-minigames/ComboLockPC.swf"
COMBO_SHA256 = "773980e69adc828e4571cadbc1276b5482d5df18f346d78b4e716219d0ca6828"
PLASMINOW = ROOT / "build/native-minigames/PlasmiNowPC.swf"


class NativeMinigameOverlayTest(unittest.TestCase):
    def test_actual_asset_copy_cache_mobile_modes_and_failures(self):
        self.assertTrue(COMBO.is_file(), "Generate the native ComboLock overlay before this filesystem fixture")
        payload = COMBO.read_bytes()
        self.assertGreater(len(payload), 32)
        self.assertIn(payload[:3], (b"FWS", b"CWS"))
        digest = hashlib.sha256(payload).hexdigest()
        self.assertEqual(digest, COMBO_SHA256)
        receipt = COMBO.with_suffix(".json")
        self.assertTrue(receipt.is_file(), "The generated ComboLock payload needs its checked receipt")
        self.assertEqual(json.loads(receipt.read_text(encoding="utf-8"))["sha256"], digest)
        plasmid_payload = PLASMINOW.read_bytes()
        plasmid_digest = hashlib.sha256(plasmid_payload).hexdigest()
        self.assertIn(plasmid_payload[:3], (b"FWS", b"CWS"))
        self.assertEqual(json.loads(PLASMINOW.with_suffix(".json").read_text(encoding="utf-8"))["sha256"], plasmid_digest)
        activity = ACTIVITY.read_text(encoding="utf-8")
        methods = "\n".join(function(activity, signature) for signature in (
            "private void copyOverlayAsset(", "private String preparePortOverlay("))
        assets = r'''
package android.content.res;
import java.io.*;
import java.util.*;
public final class AssetManager {
    public static final String PAUSE="native-settings-pause.swf",COMBO="native-combolock.swf",PLASMID="native-plasminow.swf",MOBILE="mobile-shaders.spk";
    public static final String HUD="native-hud.swf",HUD_RADIAL="native-hud-radial.swf";
    public static byte[] plasmid;
    public final byte[] combo,pause={1,3,5,7},mobile={2,4,6,8,10},hud={21,22,23},hudRadial={31,32};
    public final Map<String,Integer> reads=new HashMap<>(),closed=new HashMap<>();
    public String missing,failingRead;public boolean forbidReads;
    public AssetManager(byte[] payload){combo=payload.clone();}
    public int count(String name){return reads.containsKey(name)?reads.get(name):0;}
    public int total(){int n=0;for(int count:reads.values())n+=count;return n;}
    public InputStream open(String name)throws IOException{
        reads.put(name,count(name)+1);
        if(forbidReads)throw new IOException("Unexpected cached asset read: "+name);
        if(name.equals(missing))throw new FileNotFoundException("Missing packaged asset: "+name);
        final byte[] value;
        if(name.equals(PAUSE))value=pause;
        else if(name.equals(COMBO))value=combo;
        else if(name.equals(PLASMID))value=plasmid;
        else if(name.equals(MOBILE))value=mobile;
        else if(name.equals(HUD))value=hud;
        else if(name.equals(HUD_RADIAL))value=hudRadial;
        else throw new FileNotFoundException("Unexpected asset: "+name);
        final boolean fail=name.equals(failingRead);
        return new FilterInputStream(new ByteArrayInputStream(value)){
            int delivered;
            @Override public int read(byte[] buffer,int offset,int length)throws IOException{
                if(fail&&delivered>=16)throw new IOException("Injected asset read failure: "+name);
                int limit=fail?Math.min(length,16-delivered):length;
                int count=in.read(buffer,offset,limit);if(count>0)delivered+=count;return count;
            }
            @Override public int read()throws IOException{
                if(fail&&delivered>=16)throw new IOException("Injected asset read failure: "+name);
                int value=in.read();if(value>=0)++delivered;return value;
            }
            @Override public void close()throws IOException{
                closed.put(name,closed.containsKey(name)?closed.get(name)+1:1);super.close();
            }
        };
    }
}
'''
        fixture = r'''
package org.bioshock.port;
import android.content.res.AssetManager;
import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
class Preferences {
    boolean mobile;
    boolean getBoolean(String key,boolean fallback){assert key.equals("mobileShaders");return mobile;}
}
class GameActivity {
    static String preparedOverlay;
    final File files;final AssetManager assets;final Preferences portPreferences=new Preferences();
    GameActivity(File directory,byte[] payload){files=directory;assets=new AssetManager(payload);preparedOverlay=null;}
    File getFilesDir(){return files;}AssetManager getAssets(){return assets;}
    String prepare()throws IOException{return preparePortOverlay();}
    /* METHODS */
}
public final class OverlayFilesystemCheck {
    static final byte[] OLD={9,8,7,6},KEEP={11,12,13};
    static Path root(GameActivity a){return a.files.toPath().resolve("native-overlay-v1");}
    static Path combo(GameActivity a){return root(a).resolve("Content/FlashMovies/ComboLockPC.swf");}
    static Path plasmid(GameActivity a){return root(a).resolve("Content/FlashMovies/PlasmiNowPC.swf");}
    static Path pause(GameActivity a){return root(a).resolve("Content/FlashMovies/pausePC.swf");}
    static Path mobile(GameActivity a){return root(a).resolve("Builds/Release/shaders.spk");}
    static Path hud(GameActivity a){return root(a).resolve("Content/FlashMovies/HUDPC.swf");}
    static Path hudRadial(GameActivity a){return root(a).resolve("Content/FlashMovies/HUDRadial.swf");}
    static void put(Path path,byte[] bytes)throws IOException{Files.createDirectories(path.getParent());Files.write(path,bytes);}
    static void content(Path path,byte[] expected)throws IOException{
        assert Files.isRegularFile(path):"Missing overlay target: "+path;
        assert Arrays.equals(Files.readAllBytes(path),expected):"Wrong overlay bytes: "+path;
    }
    static String sha(byte[] bytes)throws Exception{
        StringBuilder text=new StringBuilder();for(byte b:MessageDigest.getInstance("SHA-256").digest(bytes))text.append(String.format("%02x",b&255));return text.toString();
    }
    static void failedPrepare(GameActivity a,Path target,byte[] old)throws Exception{
        try{a.prepare();throw new AssertionError("Preparation swallowed the asset failure");}
        catch(IOException expected){assert expected.getMessage()!=null;}
        assert GameActivity.preparedOverlay==null:"Failed preparation must remain retryable";
        content(target,old);
    }
    public static void main(String[] args)throws Exception{
        byte[] actual=Files.readAllBytes(Paths.get(args[1]));assert sha(actual).equals(args[2]):"Actual generated ComboLock payload pin changed";
        AssetManager.plasmid=Files.readAllBytes(Paths.get(args[3]));assert sha(AssetManager.plasmid).equals(args[4]);
        File parent=new File(args[0]);

        GameActivity off=new GameActivity(new File(parent,"off"),actual);
        put(mobile(off),OLD);Path sibling=mobile(off).resolveSibling("keep.bin");put(sibling,KEEP);
        String prepared=off.prepare();assert prepared.equals(root(off).toFile().getAbsolutePath());
        assert prepared.equals(GameActivity.preparedOverlay);
        content(combo(off),actual);content(plasmid(off),AssetManager.plasmid);content(pause(off),off.assets.pause);content(sibling,KEEP);
        assert !Files.exists(mobile(off)):"Mobile OFF must remove a stale shader override";
        assert off.assets.count(AssetManager.PAUSE)==1&&off.assets.count(AssetManager.COMBO)==1;
        assert off.assets.count(AssetManager.PLASMID)==1;
        content(hud(off),off.assets.hud);content(hudRadial(off),off.assets.hudRadial);
        assert off.assets.count(AssetManager.HUD)==1&&off.assets.count(AssetManager.HUD_RADIAL)==1;
        assert off.assets.count(AssetManager.MOBILE)==0:"Mobile OFF must not open its shader asset";
        assert !Files.exists(combo(off).resolveSibling("ComboLockPC.swf.tmp"));

        int reads=off.assets.total();off.portPreferences.mobile=true;off.assets.forbidReads=true;
        assert off.prepare().equals(prepared)&&off.assets.total()==reads:"Prepared overlay cache must not recopy assets";
        content(combo(off),actual);content(plasmid(off),AssetManager.plasmid);content(pause(off),off.assets.pause);assert !Files.exists(mobile(off));

        GameActivity on=new GameActivity(new File(parent,"on"),actual);on.portPreferences.mobile=true;
        on.prepare();content(combo(on),actual);content(plasmid(on),AssetManager.plasmid);content(pause(on),on.assets.pause);content(mobile(on),on.assets.mobile);
        assert on.assets.count(AssetManager.PAUSE)==1&&on.assets.count(AssetManager.COMBO)==1&&on.assets.count(AssetManager.MOBILE)==1;
        assert on.assets.count(AssetManager.PLASMID)==1;
        reads=on.assets.total();on.portPreferences.mobile=false;on.assets.forbidReads=true;
        on.prepare();assert on.assets.total()==reads;content(mobile(on),on.assets.mobile);

        GameActivity missing=new GameActivity(new File(parent,"missing"),actual);
        put(combo(missing),OLD);missing.assets.missing=AssetManager.COMBO;
        failedPrepare(missing,combo(missing),OLD);assert missing.assets.count(AssetManager.COMBO)==1;
        missing.assets.missing=null;missing.prepare();content(combo(missing),actual);
        assert GameActivity.preparedOverlay!=null&&missing.assets.count(AssetManager.COMBO)==2;

        GameActivity readFailure=new GameActivity(new File(parent,"read-failure"),actual);
        put(combo(readFailure),OLD);readFailure.assets.failingRead=AssetManager.COMBO;
        failedPrepare(readFailure,combo(readFailure),OLD);
        assert readFailure.assets.closed.get(AssetManager.COMBO)==1:"Failed asset input was not closed";
        readFailure.assets.failingRead=null;readFailure.prepare();content(combo(readFailure),actual);
        assert GameActivity.preparedOverlay!=null&&readFailure.assets.count(AssetManager.COMBO)==2;
        assert !Files.exists(combo(readFailure).resolveSibling("ComboLockPC.swf.tmp"));
        for(boolean badRead:new boolean[]{false,true}){
            GameActivity failure=new GameActivity(new File(parent,"plasmid-failure-"+badRead),actual);
            put(plasmid(failure),OLD);
            if(badRead)failure.assets.failingRead=AssetManager.PLASMID;else failure.assets.missing=AssetManager.PLASMID;
            failedPrepare(failure,plasmid(failure),OLD);
            if(badRead)assert failure.assets.closed.get(AssetManager.PLASMID)==1;
            failure.assets.failingRead=failure.assets.missing=null;
            failure.prepare();content(plasmid(failure),AssetManager.plasmid);
            assert failure.assets.count(AssetManager.PLASMID)==2&&GameActivity.preparedOverlay!=null;
        }
        assert sha(Files.readAllBytes(Paths.get(args[1]))).equals(args[2]):"Fixture modified the generated source asset";
        assert sha(Files.readAllBytes(Paths.get(args[3]))).equals(args[4]);
        System.out.println("PASS actual GameActivity overlay methods: real ComboLock and PlasmiNow payload copies, both HUD movies, cached no-read mode, mobile OFF/ON, missing/read failure atomic target preservation and successful retry");
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-minigame-overlay-") as temporary:
            directory = Path(temporary)
            stub = directory / "android/content/res/AssetManager.java"
            stub.parent.mkdir(parents=True)
            stub.write_text(assets, encoding="utf-8")
            check = directory / "OverlayFilesystemCheck.java"
            check.write_text(fixture.replace("/* METHODS */", methods), encoding="utf-8")
            java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
            commands = (
                [str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", temporary, str(stub), str(check)],
                [str(java / "java.exe"), "-ea", "-cp", temporary, "org.bioshock.port.OverlayFilesystemCheck", temporary, str(COMBO), digest, str(PLASMINOW), plasmid_digest],
            )
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS actual GameActivity overlay methods", result.stdout)


if __name__ == "__main__":
    unittest.main()
