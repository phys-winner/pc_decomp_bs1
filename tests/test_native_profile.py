"""Execute the production first-profile INI initializer and its pinned asset staging."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/org/bioshock/port"


class NativeProfileTest(unittest.TestCase):
    def test_original_graphics_matcher_recognizes_stock_low_seed(self):
        inputs = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(inputs["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        # Engine.U PCOptionsData.LowQuality: texture 2, actor 0, global lighting bit 5, other effects off.
        for texture, actor, global_light, expected in ((2, 0, True, 0), (1, 0, True, 3),
                                                       (2, 2, True, 3), (2, 0, False, 3)):
            machine = Uc(UC_ARCH_X86, UC_MODE_32)
            machine.mem_map(0x10A76000, 0x2000)
            machine.mem_write(0x10A76000, pe.get_data(0x10A76000 - pe.OPTIONAL_HEADER.ImageBase, 0x2000))
            machine.mem_map(0x2000000, 0x3000)
            machine.mem_map(0x300000, 0x10000)
            movie, options = 0x2000000, 0x2001000
            machine.mem_write(movie + 148, struct.pack("<I", options))
            machine.mem_write(options + 56, struct.pack("<BB2xI", 2, 0, 32))
            machine.mem_write(options + 64, struct.pack("<BB2xI", 3, 1, 31))
            machine.mem_write(options + 72, struct.pack("<BB2xI", 3, 2, 31))
            machine.mem_write(options + 88, struct.pack("<BB2xI", texture, actor, 256 if global_light else 0))
            machine.reg_write(UC_X86_REG_ECX, movie)
            machine.reg_write(UC_X86_REG_ESP, 0x308000)
            machine.emu_start(0x10A77B80, 0x10A76880, count=500)
            self.assertEqual(machine.reg_read(UC_X86_REG_EIP), 0x10A76880)
            self.assertEqual(machine.mem_read(options + 80, 1)[0], expected)

    def test_original_bootstrap_marker_preserves_seed_and_crash_recovery(self):
        self._assert_original_bootstrap()

    def _assert_original_bootstrap(self, complete=None, user=b"Original user settings"):
        inputs = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(inputs["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        base = pe.OPTIONAL_HEADER.ImageBase
        if complete is None:
            complete = (ROOT / "build/localization/Builds/Release/Default.ini").read_bytes()
            complete = complete.replace(b"FullscreenViewportX=2048", b"FullscreenViewportX=2780")
            complete = complete.replace(b"FullscreenViewportY=1152", b"FullscreenViewportY=1264")
        for markers, reset in (({}, True), ({"donotdelete.ini": b""}, False),
                               ({"donotdelete.ini": b"", "running.ini": b""}, True)):
            with self.subTest(markers=list(markers)):
                files = {"bioshock.ini": complete, "user.ini": user, **markers}
                before = files.copy()
                uc = Uc(UC_ARCH_X86, UC_MODE_32)
                uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
                uc.mem_write(base, pe.get_memory_mapped_image())
                uc.mem_map(0x100000, 0x10000)
                uc.mem_map(0x200000, 0x4000)
                uc.mem_map(0x50000000, 0x5000)
                size, delete, create, destroy = 0x50001000, 0x50002000, 0x50003000, 0x50004000
                uc.mem_write(0x11129750, struct.pack("<I", 0x200000))
                uc.mem_write(0x200000, struct.pack("<I", 0x201000))
                for slot, boundary in ((0x0C, size), (0x10, delete), (0x08, create)):
                    uc.mem_write(0x201000 + slot, struct.pack("<I", boundary))
                uc.mem_write(0x202000, struct.pack("<I", 0x203000))
                uc.mem_write(0x203000, struct.pack("<I", destroy))
                uc.reg_write(UC_X86_REG_EBP, 0x108000)
                uc.reg_write(UC_X86_REG_ESP, 0x106000)
                calls = []

                def original_bootstrap_boundary(machine, address, length, unused):
                    if address not in (size, delete, create, destroy):
                        return
                    sp = machine.reg_read(UC_X86_REG_ESP)
                    resume = struct.unpack("<I", machine.mem_read(sp, 4))[0]
                    if address == destroy:
                        calls.append(("close", "donotdelete.ini"))
                        result, arguments = 0, 1
                    else:
                        pointer = struct.unpack("<I", machine.mem_read(sp + 4, 4))[0]
                        units = []
                        while True:
                            unit = bytes(machine.mem_read(pointer + 2 * len(units), 2))
                            if unit == b"\0\0":
                                break
                            units.append(unit)
                            self.assertLess(len(units), 260)
                        name = b"".join(units).decode("utf-16-le").lower()
                        self.assertIn(name, ("running.ini", "donotdelete.ini", "bioshock.ini", "user.ini"))
                        if address == size:
                            calls.append(("size", name))
                            result, arguments = len(files[name]) if name in files else 0xFFFFFFFF, 1
                        elif address == delete:
                            calls.append(("delete", name))
                            result, arguments = int(name in files), 3
                            files.pop(name, None)
                        else:
                            calls.append(("create", name))
                            files[name] = b""
                            result, arguments = 0x202000, 3
                    machine.reg_write(UC_X86_REG_EAX, result)
                    machine.reg_write(UC_X86_REG_ESP, sp + 4 + 4 * arguments)
                    machine.reg_write(UC_X86_REG_EIP, resume)

                uc.hook_add(UC_HOOK_CODE, original_bootstrap_boundary)
                # Only filesystem interface boundaries are supplied. Every marker
                # decision, delete call and empty-writer lifetime is original x86.
                uc.emu_start(0x1092D109, 0x1092D20F, count=500)
                self.assertEqual(uc.reg_read(UC_X86_REG_EIP), 0x1092D20F)
                if reset:
                    self.assertEqual([name for action, name in calls if action == "delete"],
                                     ["running.ini", "bioshock.ini", "user.ini"])
                    self.assertNotIn("bioshock.ini", files)
                    self.assertEqual(files["donotdelete.ini"], b"")
                    self.assertIn(("close", "donotdelete.ini"), calls)
                else:
                    self.assertEqual(files, before)
                    self.assertFalse(any(action in ("delete", "create", "close") for action, name in calls))

    def test_production_initializer_encodings_and_existing_profiles(self):
        program = r'''
package org.bioshock.port;
import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.*;
import java.nio.file.*;
import java.util.*;
public class NativeProfileCheck {
    static byte[] encode(String text,Charset encoding,byte[] bom)throws Exception{
        ByteArrayOutputStream out=new ByteArrayOutputStream();out.write(bom);out.write(text.getBytes(encoding));return out.toByteArray();
    }
    static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    static File config(File profile){return new File(profile,"AppData/Roaming/Bioshock/Bioshock.ini");}
    static File marker(File profile){return new File(config(profile).getParentFile(),"DoNotDelete.ini");}
    static void absent(File profile)throws Exception{
        check(!config(profile).exists(),"Failed preparation must leave no partial INI");
        File folder=config(profile).getParentFile();
        if(folder.exists())for(File child:folder.listFiles())check(!child.getName().endsWith(".tmp"),"Temporary file leaked");
    }
    public static void main(String[] args)throws Exception{
        File root=new File(args[0]);
        File boundary=new File(root,"escaped-profile"),outside=new File(root,"outside-AppData");
        try{NativeProfile.initialize(boundary,2780,1264,()->{throw new AssertionError("Escaped template read");});throw new AssertionError("Profile boundary accepted");}
        catch(IOException expectedFailure){check(!new File(outside,"Roaming").exists(),"Created directories outside the selected profile");}
        String text="; полный исходный шаблон\r\n[Startup]\r\nPersistentPackages=Core\r\n"
            +"[WinDrv.WindowsClient]\r\nWindowedViewportX=2048\r\nWindowedViewportY=1152\r\n"
            +" FullscreenViewportX = 2048  ; native width\r\nfullscreenviewporty=1152\r\nStartupFullscreen=False\r\nKeep=unchanged\r\n"
            +" uSeCoNtroller = False  ; legacy setting\r\n"
            +"[WinDrv.WindowsClientEditor]\r\nFullscreenViewportX=640\r\nFullscreenViewportY=480\r\nStartupFullscreen=False\r\nUseController=True\r\n"
            +"[Engine.Engine]\r\nLanguage=rus\r\n";
        String expected=text.replace(" FullscreenViewportX = 2048  ;", " FullscreenViewportX = 2780  ;")
            .replace("fullscreenviewporty=1152", "fullscreenviewporty=1264")
            .replace("StartupFullscreen=False\r\nKeep", "StartupFullscreen=True\r\nKeep");
        Charset[] encodings={StandardCharsets.UTF_8,StandardCharsets.UTF_8,StandardCharsets.UTF_16LE,StandardCharsets.UTF_16BE};
        byte[][] boms={{},{(byte)0xef,(byte)0xbb,(byte)0xbf},{(byte)0xff,(byte)0xfe},{(byte)0xfe,(byte)0xff}};
        for(int i=0;i<encodings.length;++i){
            File profile=new File(root,"encoding-"+i);check(profile.mkdir(),"Fresh profile");
            final byte[] original=encode(text,encodings[i],boms[i]);
            check(NativeProfile.initialize(profile,2780,1264,()->new ByteArrayInputStream(original)),"Fresh INI was not created");
            check(Arrays.equals(Files.readAllBytes(config(profile).toPath()),encode(expected,encodings[i],boms[i])),"Encoding/BOM/other settings changed");
            check(marker(profile).isFile()&&marker(profile).length()==0,"Native empty bootstrap marker missing");
            File stale=new File(config(profile).getParentFile(),"rUnNiNg.InI");Files.write(stale.toPath(),new byte[0]);
            Files.write(config(profile).toPath(),encode(expected.replace("uSeCoNtroller = False", "uSeCoNtroller = True"),encodings[i],boms[i]));
            check(!NativeProfile.initialize(profile,2780,1264,()->{throw new IOException("Existing profile opened template");}),"Existing INI was reseeded");
            check(!stale.exists(),"Process-death marker survived existing-profile relaunch");
            check(Arrays.equals(Files.readAllBytes(config(profile).toPath()),encode(expected,encodings[i],boms[i])),"Relaunch must clear the legacy controller flag at the same resolution");
            check(!NativeProfile.initialize(profile,1920,1080,()->{throw new IOException("Existing profile template read");}),"Existing INI reseeded on aspect change");
            String resized=expected.replace(" FullscreenViewportX = 2780  ;", " FullscreenViewportX = 2247  ;");
            check(Arrays.equals(Files.readAllBytes(config(profile).toPath()),encode(resized,encodings[i],boms[i])),"Existing aspect change affected quality, height, comments or encoding");
            check(!NativeProfile.initialize(profile,1920,1080,()->{throw new IOException("Existing profile template read");}),"Existing INI reseeded on repeat");
            check(Arrays.equals(Files.readAllBytes(config(profile).toPath()),encode(resized,encodings[i],boms[i])),"Aspect migration not idempotent");
        }
        String savedHigh="[WinDrv.WindowsClient]\r\nFullscreenViewportX=1920\r\nFullscreenViewportY=1080\r\nStartupFullscreen=True\r\nWindowedViewportX=1920\r\n"
            +"[Engine.RenderConfig]\r\nHighDetailShaders=True\r\nShadows=True\r\nTextureDetail=High\r\nForceGlobalLighting=False\r\nHorizontalFOVLock=True\r\n"
            +"[D3DDrv.D3DRenderDevice]\r\nHighDetailActors=True\r\nSuperHighDetailActors=True\r\n";
        File high=new File(root,"saved-high");check(config(high).getParentFile().mkdirs(),"High saved profile");
        Files.write(config(high).toPath(),savedHigh.getBytes(StandardCharsets.UTF_8));
        check(!NativeProfile.initialize(high,2424,1080,()->{throw new IOException("Existing template read");}),"Existing high profile reseeded");
        check(marker(high).isFile()&&marker(high).length()==0,"Unmarked existing profile remains vulnerable to original wipe");
        check(Arrays.equals(Files.readAllBytes(config(high).toPath()),savedHigh.replace("FullscreenViewportX=1920", "FullscreenViewportX=2424").getBytes(StandardCharsets.UTF_8)),"Saved1920 fix must change only fullscreen width before engine startup");
        String savedLowRes=savedHigh.replace("FullscreenViewportX=1920", "FullscreenViewportX=800").replace("FullscreenViewportY=1080", "FullscreenViewportY=600");
        Files.write(config(high).toPath(),savedLowRes.getBytes(StandardCharsets.UTF_8));
        NativeProfile.initialize(high,2424,1080,()->{throw new IOException("Existing template read");});
        check(Arrays.equals(Files.readAllBytes(config(high).toPath()),savedLowRes.replace("FullscreenViewportX=800", "FullscreenViewportX=1347").getBytes(StandardCharsets.UTF_8)),"Lower selected height was lost");
        File mixed=new File(root,"mixed");File mixedFolder=new File(mixed,"aPpDaTa/rOaMiNg/bIoShOcK");check(mixedFolder.mkdirs(),"Mixed profile");
        File existing=new File(mixedFolder,"bIoShOcK.InI");byte[] saved={(byte)0xff,0,1,2,3};Files.write(existing.toPath(),saved);
        Method child=NativeProfile.class.getDeclaredMethod("child",File.class,String.class);child.setAccessible(true);
        check(((File)child.invoke(null,mixed,"AppData")).getName().equals("aPpDaTa"),"Directory spelling was not resolved from disk");
        check(((File)child.invoke(null,mixedFolder,"Bioshock.ini")).getName().equals("bIoShOcK.InI"),"INI spelling was not resolved from disk");
        check(!NativeProfile.initialize(mixed,2780,1264,()->{throw new IOException("Must not read asset");}),"Mixed existing profile reseeded");
        check(Arrays.equals(saved,Files.readAllBytes(existing.toPath()))&&mixedFolder.list().length==2&&Arrays.equals(mixed.list(),new String[]{"aPpDaTa"})&&new File(mixedFolder,"DoNotDelete.ini").isFile(),"Existing bytes or mixed-case paths changed");
        File mixedFresh=new File(root,"mixed-fresh");File reused=new File(mixedFresh,"APPDATA/roaming/BIOSHOCK");check(reused.mkdirs(),"Mixed fresh profile");
        File oldMarker=new File(reused,"doNOTdelete.INI");byte[] oldMarkerBytes={7,8,9};Files.write(oldMarker.toPath(),oldMarkerBytes);
        check(NativeProfile.initialize(mixedFresh,2780,1264,()->new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8))),"Could not reuse mixed directories");
        check(Arrays.equals(Files.readAllBytes(new File(reused,"Bioshock.ini").toPath()),expected.getBytes(StandardCharsets.UTF_8)),"Mixed directory fresh result");
        check(Arrays.equals(Files.readAllBytes(oldMarker.toPath()),oldMarkerBytes)&&reused.list().length==2,"Existing mixed-case marker changed");
        for(int i=0;i<3;++i){
            File profile=new File(root,"bad-"+i);check(profile.mkdir(),"Failure profile");final int failure=i;
            try{
                NativeProfile.initialize(profile,2780,1264,()->{
                    if(failure==0)throw new IOException("Missing pinned template");
                    if(failure==1)return new ByteArrayInputStream("[WinDrv.WindowsClient]\nFullscreenViewportX=640\n".getBytes(StandardCharsets.UTF_8));
                    return new ByteArrayInputStream(new byte[]{(byte)0xc3,0x28});
                });
                throw new AssertionError("Invalid/missing template accepted");
            }catch(IOException expectedFailure){absent(profile);check(!marker(profile).exists(),"Invalid template published a bootstrap marker");}
        }
        File blocked=new File(root,"blocked");check(blocked.mkdir(),"Blocked profile");Files.write(new File(blocked,"AppData").toPath(),new byte[]{7});
        try{NativeProfile.initialize(blocked,2780,1264,()->new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)));throw new AssertionError("Non-directory accepted");}catch(IOException expectedFailure){}
        File badMarker=new File(root,"bad-marker");check(marker(badMarker).mkdirs(),"Blocked marker fixture");
        try{NativeProfile.initialize(badMarker,2780,1264,()->new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)));throw new AssertionError("Marker failure accepted");}
        catch(IOException expectedFailure){absent(badMarker);check(marker(badMarker).isDirectory(),"Existing invalid marker was changed");}
        // Unknown/nonempty runtime markers block startup without changing player files.
        File oldProfile=new File(root,"old-profile");check(config(oldProfile).getParentFile().mkdirs(),"Existing profile fixture");
        Files.write(config(oldProfile).toPath(),saved);File crash=new File(config(oldProfile).getParentFile(),"Running.ini");Files.write(crash.toPath(),new byte[]{6});
        try{NativeProfile.initialize(oldProfile,2780,1264,()->{throw new IOException("Existing profile asset read");});throw new AssertionError("Unknown runtime marker accepted");}catch(IOException expectedFailure){}
        check(!marker(oldProfile).exists()&&Arrays.equals(saved,Files.readAllBytes(config(oldProfile).toPath()))&&Arrays.equals(Files.readAllBytes(crash.toPath()),new byte[]{6}),"Unknown runtime marker/player settings changed");
        File markerDirectory=new File(root,"runtime-directory"),runtimeDirectory=new File(config(markerDirectory).getParentFile(),"Running.ini");check(runtimeDirectory.mkdirs(),"Runtime directory fixture");
        Files.write(new File(runtimeDirectory,"keep").toPath(),new byte[]{9});
        try{NativeProfile.initialize(markerDirectory,2780,1264,()->{throw new AssertionError("Unknown marker read template");});throw new AssertionError("Runtime directory accepted");}catch(IOException expectedFailure){}
        check(Arrays.equals(Files.readAllBytes(new File(runtimeDirectory,"keep").toPath()),new byte[]{9})&&!config(markerDirectory).exists(),"Unknown marker directory changed");
        File staleFresh=new File(root,"fresh-stale"),staleFreshFolder=config(staleFresh).getParentFile();check(staleFreshFolder.mkdirs(),"Fresh stale profile");
        File freshRunning=new File(staleFreshFolder,"rUnNiNg.InI"),freshUser=new File(staleFreshFolder,"uSeR.InI");Files.write(freshRunning.toPath(),new byte[0]);Files.write(freshUser.toPath(),saved);
        check(NativeProfile.initialize(staleFresh,2780,1264,()->new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8))),"Fresh stale profile not seeded");
        check(!freshRunning.exists()&&Arrays.equals(saved,Files.readAllBytes(freshUser.toPath())),"Fresh stale cleanup removed user INI or left Running");
        // Execute the real pinned complete Default.ini, not only a synthetic template.
        byte[] complete=Files.readAllBytes(Paths.get(args[1]));File real=new File(root,"complete");check(real.mkdir(),"Complete profile");
        check(NativeProfile.initialize(real,2780,1264,()->new ByteArrayInputStream(complete)),"Complete pinned template rejected");
        String completeText=new String(complete,StandardCharsets.UTF_8);
        String completeExpected=completeText.replace("FullscreenViewportX=2048", "FullscreenViewportX=2780")
            .replace("FullscreenViewportY=1152", "FullscreenViewportY=1264");
        for(String key:new String[]{"HighDetailShaders","Shadows","RealTimeReflection","UseRippleSystem",
            "UseHighDetailSoftParticles","UseSpecCubeMap","UseDistortion"}){
            check(completeExpected.contains(key+"=True"),"Missing low preset key "+key);
            completeExpected=completeExpected.replace(key+"=True",key+"=False");
        }
        String newline=completeExpected.contains("\r\n")?"\r\n":"\n";
        if(!completeExpected.endsWith("\n")&&!completeExpected.endsWith("\r"))completeExpected+=newline;
        completeExpected+="UseHighDetailPostProcEffects=False"+newline+"TextureDetail=Medium"+newline+"ForceGlobalLighting=True"+newline+"HorizontalFOVLock=False"+newline;
        completeExpected=completeExpected.replace("[D3DDrv.D3DRenderDevice]"+newline+"HighDetailActors=True"+newline+"SuperHighDetailActors=True",
            "[D3DDrv.D3DRenderDevice]"+newline+"HighDetailActors=False"+newline+"SuperHighDetailActors=False");
        check(Arrays.equals(Files.readAllBytes(config(real).toPath()),completeExpected.getBytes(StandardCharsets.UTF_8)),"Full template changed beyond fullscreen and low graphics keys");
        // D4 Hor+: a new profile starts with the patch 1.1 Horizontal FOV Lock off (the original default, on, is Vert- at 20:9).
        check(completeExpected.split("HorizontalFOVLock=",-1).length==2&&completeExpected.contains("HorizontalFOVLock=False"),"New profile must start Hor+ (HorizontalFOVLock=False once)");
        check(completeExpected.contains("HardwareOcclusion=True")&&completeExpected.contains("PostProcessing=True"),"Required pipeline or occlusion disabled");
        check(marker(real).isFile()&&marker(real).length()==0&&completeExpected.contains("FirstRun=0"),"Fresh bootstrap marker or original FirstRun was changed");
        File user=new File(config(real).getParentFile(),"User.ini"),save=new File(real,"SaveGames/slot.bsg"),preferences=new File(real,"bioshock-port.properties");
        check(save.getParentFile().mkdir(),"Save fixture");Files.write(user.toPath(),saved);Files.write(save.toPath(),new byte[]{3,4,5});Files.write(preferences.toPath(),new byte[]{8,7,6});
        byte[] lowBefore=Files.readAllBytes(config(real).toPath());
        for(int relaunch=0;relaunch<3;++relaunch){
            File running=new File(config(real).getParentFile(),"rUnNiNg.InI");Files.write(running.toPath(),new byte[0]);
            check(!NativeProfile.initialize(real,2780,1264,()->{throw new AssertionError("Relaunch reseeded template");}),"Process-death relaunch reseeded player INI");
            check(!running.exists()&&Arrays.equals(lowBefore,Files.readAllBytes(config(real).toPath()))&&Arrays.equals(saved,Files.readAllBytes(user.toPath()))&&Arrays.equals(new byte[]{3,4,5},Files.readAllBytes(save.toPath()))&&Arrays.equals(new byte[]{8,7,6},Files.readAllBytes(preferences.toPath())),"Process-death relaunch changed Low/player data");
        }
        // Missing settings belong inside RenderConfig, with existing comments and the next section preserved.
        String middle=text.replace("[Engine.Engine]", "[Engine.RenderConfig]\r\nTextureDetail=High ; original comment\r\nUseHighDetailPostProcEffects=True\r\n[Engine.Engine]");
        File middleProfile=new File(root,"middle-render");check(middleProfile.mkdir(),"Middle render fixture");
        check(NativeProfile.initialize(middleProfile,2780,1264,()->new ByteArrayInputStream(middle.getBytes(StandardCharsets.UTF_8))),"Middle render seed");
        String middleSaved=new String(Files.readAllBytes(config(middleProfile).toPath()),StandardCharsets.UTF_8);
        check(middleSaved.contains("TextureDetail=Medium ; original comment\r\nUseHighDetailPostProcEffects=False"),"Existing low values/comments");
        check(middleSaved.contains("ForceGlobalLighting=True\r\nHorizontalFOVLock=False\r\n[Engine.Engine]\r\nLanguage=rus"),"Missing low keys leaked into the next section");

        System.out.println("PASS production profile/relaunch: exact Low/fullscreen/Hor+, UTF-8/UTF-16 BOMs, case preservation, existing High unchanged except aspect, empty stale Running removed for fresh/existing, User/saves/preferences unchanged, unknown markers and escaped paths rejected");
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-native-profile-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe = folder / "NativeProfileCheck.java"
            probe.write_text(program, encoding="utf-8")
            boundary = folder / "escaped-profile"
            outside = folder / "outside-AppData"
            boundary.mkdir()
            outside.mkdir()
            if os.name == "nt":
                linked = subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(boundary / "AppData"), str(outside)],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(linked.returncode, 0, linked.stdout + linked.stderr)
            else:
                (boundary / "AppData").symlink_to(outside, target_is_directory=True)
            java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
            for command in ([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(folder), str(probe), str(JAVA / "NativeProfile.java")],
                            [str(java / "java.exe"), "-ea", "-cp", str(folder), "org.bioshock.port.NativeProfileCheck", str(folder), str(ROOT / "build/localization/Builds/Release/Default.ini")]):
                result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Feed the real Java-prepared Low/profile bytes into the immutable
            # original bootstrap: stale Running wipes before, normalized markers preserve after.
            prepared = folder / "complete/AppData/Roaming/Bioshock"
            self.assertFalse((prepared / "Running.ini").exists())
            self.assertEqual((prepared / "DoNotDelete.ini").read_bytes(), b"")
            self._assert_original_bootstrap((prepared / "Bioshock.ini").read_bytes(), (prepared / "User.ini").read_bytes())

    def test_pinned_asset_and_startup_order(self):
        source = (ROOT / "tools/build_apk.py").read_text(encoding="utf-8")
        pin = "da5b52de385974392d025cff06252357f8c150b85098682235bdf1c0b14ffac0"
        namespace = {"ROOT": ROOT, "hashlib": hashlib, "NATIVE_PROFILE_DEFAULT_SHA256": pin}
        exec(function(source, "def stage_native_profile_default("), namespace)
        with tempfile.TemporaryDirectory(prefix="bioshock-profile-asset-", dir=ROOT / "build") as temporary:
            asset = Path(temporary) / "native-profile-default.ini"
            receipt = namespace["stage_native_profile_default"](asset)
            self.assertEqual(asset.read_bytes(), (ROOT / receipt["source"]).read_bytes())
            self.assertEqual(receipt["sha256"], pin)
            self.assertEqual(receipt["bytes"], asset.stat().st_size)
            self.assertEqual(json.loads((ROOT / "build/localization/manifest.json").read_text())["default_ini_sha256"], pin)
            namespace["NATIVE_PROFILE_DEFAULT_SHA256"] = "0" * 64
            asset.unlink()
            with self.assertRaisesRegex(ValueError, "Pinned localized"):
                namespace["stage_native_profile_default"](asset)
            self.assertFalse(asset.exists())
        activity = (JAVA / "GameActivity.java").read_text(encoding="utf-8")
        startup = function(activity, "private void startIfReady(")
        self.assertLess(startup.index("NativeProfile.initialize("), startup.index("SessionLog.begin("))
        self.assertLess(startup.index("NativeProfile.initialize("), startup.index("nativeStart("))
        self.assertIn('stage_native_profile_default(assets / "native-profile-default.ini")', source)


if __name__ == "__main__":
    unittest.main()
