"""Exercise production settings callbacks and always-on UI/mobile overlay preparation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_mobile_shaders import function
from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/org/bioshock/port"


class NativePortSettingsTest(unittest.TestCase):
    def test_accepted_saves_finish_after_activity_destroy(self):
        source = (JAVA / "GameActivity.java").read_text(encoding="utf-8")
        methods = function(source, "void savePortSettings(") + "\n" + function(source, "@Override protected void onDestroy(")
        program = r'''
package org.bioshock.port;
import java.io.*;
import java.util.concurrent.*;
class ActivityBase {protected void onDestroy(){}boolean isFinishing(){return true;}}
class HandlerCheck {boolean post(Runnable work){work.run();return true;}void removeCallbacks(Runnable work){}}
class DisplayCheck {void unregisterDisplayListener(Object listener){}}
class FpsOverlay {boolean stopped;void stop(){stopped=true;}}
class Toast {static final int LENGTH_LONG=1;static Toast makeText(Object context,String text,int length){throw new AssertionError(text);}void show(){}}
class GameActivity extends ActivityBase {
    static volatile GameActivity currentActivity;
    final ExecutorService settingsIO=Executors.newSingleThreadExecutor();
    final HandlerCheck handler=new HandlerCheck();
    DisplayCheck displayManager;Object displayListener;Runnable progress;
    FpsOverlay fpsOverlay=new FpsOverlay();
    PortPreferences portPreferences;
    /* METHODS */
}
public class SettingsDestroyCheck {
    public static void main(String[] args)throws Exception{
        GameActivity a=new GameActivity();a.portPreferences=new PortPreferences(new File(args[0]));GameActivity.currentActivity=a;
        CountDownLatch running=new CountDownLatch(1),release=new CountDownLatch(1);
        a.settingsIO.execute(()->{running.countDown();try{release.await();}catch(InterruptedException error){throw new AssertionError("Destroy interrupted accepted settings work",error);}});
        assert running.await(5,TimeUnit.SECONDS);
        a.portPreferences.put("mobileShaders",true);a.portPreferences.put("renderer","dxvk-turnip");
        a.savePortSettings(null);
        a.onDestroy();assert GameActivity.currentActivity==null&&a.settingsIO.isShutdown()&&a.fpsOverlay.stopped;
        release.countDown();assert a.settingsIO.awaitTermination(5,TimeUnit.SECONDS);
        PortPreferences reloaded=new PortPreferences(new File(args[0]));
        assert reloaded.getBoolean("mobileShaders",false)&&reloaded.getString("renderer","").equals("dxvk-turnip");
        System.out.println("PASS actual onDestroy gracefully drains previously accepted preference writes");
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-settings-destroy-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe = folder / "SettingsDestroyCheck.java"
            probe.write_text(program.replace("/* METHODS */", methods), encoding="utf-8")
            java = Path(os.environ["JAVA_HOME"]) / "bin"
            for command in ([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(folder), str(probe), str(JAVA / "PortPreferences.java")],
                            [str(java / "java.exe"), "-ea", "-cp", str(folder), "org.bioshock.port.SettingsDestroyCheck", str(folder)]):
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_java_settings_and_overlay_roundtrip(self):
        activity = (JAVA / "GameActivity.java").read_text(encoding="utf-8")
        methods = "\n".join(function(activity, signature) for signature in (
            "static String portSetting(", "private String portSettingValue(", "private float selectedFps(",
            "private void copyOverlayAsset(", "private String preparePortOverlay(", "static float resolveFps("))
        methods += "\n" + "\n".join(line for line in activity.splitlines()
            if line.strip().startswith(("static final float DEFAULT_FPS", "static final float RATE_TOLERANCE")))
        renderer_mode = function((JAVA / "LauncherHooks.java").read_text(encoding="utf-8"), "static String rendererMode(")
        program = r'''
package org.bioshock.port;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
class QueueHandler {
    final List<Runnable> queued=new ArrayList<>();
    boolean post(Runnable action){queued.add(action);return true;}
    void drain(){while(!queued.isEmpty())queued.remove(0).run();}
}
class ControlCheck {int reloads,editors;void requestSettingsReload(){++reloads;}void requestLayoutEditor(){++editors;}}
class GameControls extends ControlCheck {float hudClearStage(){return 112f;}}
class Configuration {int smallestScreenWidthDp=411;}
class Resources {final Configuration configuration=new Configuration();Configuration getConfiguration(){return configuration;}}
class AssetsCheck {
    byte[] pause={1,2,3},mobile={4,5,6,7},combo={8,9,10},hud={11,12},hudRadial={13,14,15};
    InputStream open(String name)throws IOException{
        if(name.equals("native-settings-pause.swf"))return new ByteArrayInputStream(pause);
        if(name.equals("native-combolock.swf"))return new ByteArrayInputStream(combo);
        if(name.equals("native-plasminow.swf"))return new ByteArrayInputStream(combo);
        if(name.equals("native-loading.swf"))return new ByteArrayInputStream(combo);
        if(name.equals("mobile-shaders.spk"))return new ByteArrayInputStream(mobile);
        if(name.equals("native-hud.swf"))return new ByteArrayInputStream(hud);
        if(name.equals("native-hud-radial.swf"))return new ByteArrayInputStream(hudRadial);
        if(name.equals("native-picker.swf"))return new ByteArrayInputStream(combo);
        if(name.equals("native-map.swf"))return new ByteArrayInputStream(combo);
        throw new IOException("Unknown asset");
    }
}
class LauncherHooks {
/* RENDERER_MODE */
}
class GameActivity {
    static volatile GameActivity currentActivity;
    static String preparedOverlay;
    final QueueHandler handler=new QueueHandler();
    final GameControls controls=new GameControls();
    final Resources resources=new Resources();Resources getResources(){return resources;}
    final ExecutorService settingsIO=Executors.newSingleThreadExecutor();
    final AssetsCheck assets=new AssetsCheck();
    final File files;
    final PortPreferences portPreferences;
    String renderer;
    float[] rates={29.97003f,45f,59.94006f,90f,119.88012f};int saves,frameApplies;
    GameActivity(File folder)throws IOException{files=folder;portPreferences=new PortPreferences(folder);}
    File getFilesDir(){return files;}
    AssetsCheck getAssets(){return assets;}
    String prepare()throws IOException{return preparePortOverlay();}
    float[] supportedFpsRates(){return rates;}
    void applyFrameRate(){++frameApplies;}
    void savePortSettings(Runnable action){++saves;try{portPreferences.save();}catch(IOException error){throw new RuntimeException(error);}if(action!=null)handler.post(action);}
    /* METHODS */
}
public class NativePortCheck {
    public static void main(String[] args)throws Exception{
        assert GameActivity.portSetting("renderer",null)==null;
        GameActivity a=new GameActivity(new File(args[0]));GameActivity.currentActivity=a;
        assert GameActivity.portSetting("renderer",null).equals("gles");
        a.renderer="vulkan-turnip";
        assert GameActivity.portSetting("renderer",null).equals("vulkan-turnip"):"Fresh launcher-selected Turnip must match the native menu";
        assert a.portPreferences.getString("renderer",null)==null:"Reading the current mode must not create a pending choice";
        assert GameActivity.portSetting("renderer","gles").equals("gles");
        assert GameActivity.portSetting("renderer",null).equals("gles")&&a.renderer.equals("vulkan-turnip"):"Pending saved GLES wins without changing the running engine";
        assert new PortPreferences(a.files).getString("renderer",null).equals("gles");
        a.handler.drain();a.controls.reloads=0;
        assert GameActivity.portSetting("mobileShaders",null).equals("0");
        // Phones default to a 150% interface; HDR and the gauge control start on, the separate More button off.
        assert GameActivity.portSetting("uiScale",null).equals("1.5");
        assert GameActivity.portSetting("hdr",null).equals("1")&&GameActivity.portSetting("touchMore",null).equals("0");
        assert GameActivity.portSetting("touchVitals",null).equals("1");
        assert GameActivity.portSetting("touchAcceleration",null).equals("1");
        assert GameActivity.portSetting("touchAcceleration","2")==null;
        assert GameActivity.portSetting("touchAcceleration","0").equals("0");
        a.handler.drain();a.controls.reloads=0;
        assert GameActivity.portSetting("hudClear",null).equals("112.0")&&GameActivity.portSetting("hudClear","3")==null;
        assert GameActivity.portSetting("touchSensitivity",null).equals("0.22");
        assert GameActivity.portSetting("fpsLimit",null).equals(Float.toString(59.94006f)):"D8: no saved choice is 60, not the top mode";
        assert GameActivity.portSetting("fpsChoices",null).equals("0,29.97003,45.0,59.94006,90.0,119.88012");
        // Game and fixes: the tweaks keep the original game, the fixes are on.
        assert GameActivity.portSetting("physicsRate",null).equals("30")&&GameActivity.portSetting("fixScrounger",null).equals("1")&&
            GameActivity.portSetting("cutsceneFps",null).equals("1")&&GameActivity.portSetting("mouseRaw",null).equals("0")&&
            GameActivity.portSetting("anisotropy",null).equals("0");
        assert GameActivity.portSetting("physicsRate","45")==null&&GameActivity.portSetting("physicsRate","x")==null&&
            GameActivity.portSetting("anisotropy","2")==null&&GameActivity.portSetting("fixScrounger","yes")==null&&GameActivity.portSetting("mouseRaw","2")==null;
        assert GameActivity.portSetting("physicsRate","60").equals("60")&&GameActivity.portSetting("physicsRate",null).equals("60");
        assert GameActivity.portSetting("anisotropy","16").equals("16")&&GameActivity.portSetting("anisotropy",null).equals("16");
        assert GameActivity.portSetting("fixScrounger","0").equals("0")&&GameActivity.portSetting("fixScrounger",null).equals("0");
        assert GameActivity.portSetting("cutsceneFps","0").equals("0")&&GameActivity.portSetting("mouseRaw","1").equals("1");
        a.handler.drain();a.controls.reloads=0;
        int saved=a.saves;
        for(String[] entry:new String[][]{{"renderer","vulkan-turnip"},{"fpsLimit","59.94006"},{"mobileShaders","1"},{"touchScale","1.6"},{"touchOpacity","0.2"},{"touchSensitivity","0.04"},{"touchGyroSensitivity","5"},{"touchAltSensitivity","3"},{"touchLeftHanded","1"},{"touchInvertY","1"},{"touchGyro","2"},{"touchGyro","3"},{"touchGyro","4"},{"touchEnabled","0"},{"touchUse","0"},{"touchPrompts","0"},{"renderScale","50"},{"renderScale","100"},{"upscaler","1"},{"upscaler","0"}}){
            String actual=GameActivity.portSetting(entry[0],entry[1]);
            assert actual!=null && (actual.equals(entry[1]) || Float.parseFloat(actual)==Float.parseFloat(entry[1])):Arrays.toString(entry);
        }
        assert a.frameApplies==0:"Guest callback must not wait for or execute Android UI work";
        a.handler.drain();assert a.frameApplies==1 && a.controls.reloads==20;
        assert a.saves==saved+20;
        assert GameActivity.portSetting("fpsLimit","45").equals("45.0");
        assert new PortPreferences(a.files).getFloat("fpsLimit",0)==45;
        assert GameActivity.portSetting("fpsLimit","0").equals("0.0");
        assert GameActivity.portSetting("fpsLimit",null).equals("0.0");
        assert GameActivity.portSetting("playerFov",null).equals("75")&&GameActivity.portSetting("weaponFov",null).equals("60");
        assert GameActivity.portSetting("playerFov","110").equals("110")&&GameActivity.portSetting("weaponFov","68").equals("68");
        assert new PortPreferences(a.files).getInt("playerFov",0)==110&&new PortPreferences(a.files).getInt("weaponFov",0)==68;
        assert GameActivity.portSetting("playerFov","54")==null&&GameActivity.portSetting("weaponFov","101")==null;
        int validSaves=a.saves;
        for(String[] entry:new String[][]{{"renderer","../other"},{"fpsLimit","60"},{"fpsLimit","NaN"},{"touchScale","Infinity"},{"touchScale","0.649"},{"touchOpacity","1.01"},{"touchSensitivity","3.01"},{"touchGyro","5"},{"touchGyro","1.5"},{"mobileShaders","true"},{"touchInvertY","yes"},{"uiScale","1.51"},{"touchVehicleProfile","1"},{"renderScale","49"},{"renderScale","55"},{"renderScale","101"},{"upscaler","2"},{"upscaler","3"},{"unknown","1"},{"fpsChoices","60"},{"editLayout","0"},{"resetLayout",null}}){
            assert GameActivity.portSetting(entry[0],entry[1])==null:Arrays.toString(entry);
        }
        assert a.saves==validSaves:"Invalid input must not persist";
        assert GameActivity.portSetting("ReNdErEr",null).equals("vulkan-turnip");
        assert GameActivity.portSetting("renderer","dxvk").equals("vulkan")&&new PortPreferences(a.files).getString("renderer",null).equals("vulkan"):"A legacy DXVK choice is saved as its Vulkan mode";
        assert GameActivity.portSetting("renderer","dxvk-turnip").equals("vulkan-turnip")&&GameActivity.portSetting("renderer","vulkan-panvk").equals("vulkan-panvk");
        assert GameActivity.portSetting("renderer","dxvk-panvk")==null:"Only known modes are accepted";
        assert GameActivity.portSetting("renderer","vulkan-turnip").equals("vulkan-turnip");
        a.rates=new float[100];for(int i=0;i<a.rates.length;++i)a.rates[i]=30.125f+i;
        assert GameActivity.portSetting("fpsChoices",null).length()>128:"Read responses are not limited to write length";
        a.portPreferences.put("unrelated","keep");
        String[] layoutKeys={"touchX","touchY","touchSize","touchDisplay"};
        for(String name:TouchLayout.NAMES)for(String key:layoutKeys)a.portPreferences.put(key+name,TouchLayout.IN_RADIAL);
        assert GameActivity.portSetting("touchUse",null).equals("0");
        assert GameActivity.portSetting("touchUse","1").equals("1");
        assert a.portPreferences.getInt("touchDisplayuse",-1)==(TouchLayout.ON_SCREEN|TouchLayout.IN_RADIAL);
        assert GameActivity.portSetting("resetLayout","1").equals("1");
        for(String name:TouchLayout.NAMES)for(String key:layoutKeys)assert a.portPreferences.getString(key+name,"x").isEmpty();
        assert a.portPreferences.getString("unrelated","").equals("keep");
        assert GameActivity.portSetting("editLayout","1").equals("1");
        assert a.controls.editors==0;a.handler.drain();assert a.controls.editors==1;
        PortPreferences reloaded=new PortPreferences(a.files);
        assert reloaded.getBoolean("mobileShaders",false) && reloaded.getString("renderer","").equals("vulkan-turnip");
        // Execute the real file-preparation methods: on -> off -> on, same persistent UI overlay.
        String on=a.prepare();File overlay=new File(on);
        File pause=new File(overlay,"Content/FlashMovies/pausePC.swf"),shader=new File(overlay,"Builds/Release/shaders.spk");
        assert Arrays.equals(Files.readAllBytes(pause.toPath()),a.assets.pause);
        File combo=new File(overlay,"Content/FlashMovies/ComboLockPC.swf");
        assert Arrays.equals(Files.readAllBytes(combo.toPath()),a.assets.combo);
        assert Arrays.equals(Files.readAllBytes(new File(overlay,"Content/FlashMovies/ProgressBar.swf").toPath()),a.assets.combo);
        assert Arrays.equals(Files.readAllBytes(new File(overlay,"Content/FlashMovies/HUDPC.swf").toPath()),a.assets.hud);
        assert Arrays.equals(Files.readAllBytes(new File(overlay,"Content/FlashMovies/HUDRadial.swf").toPath()),a.assets.hudRadial);
        assert !shader.exists():"The Direct3D 10 renderer must use the game's own shaders.spk, not the Direct3D 9 mobile overlay";
        File sibling=new File(shader.getParentFile(),"keep.bin");if(!shader.getParentFile().mkdirs())throw new AssertionError("Overlay fixture folder");Files.write(sibling.toPath(),new byte[]{9});
        Files.write(shader.toPath(),a.assets.mobile);
        assert a.prepare().equals(on)&&shader.exists():"Activity recreation reuses the prepared overlay";
        GameActivity.preparedOverlay=null;a.portPreferences.put("mobileShaders",false);String off=a.prepare();
        assert on.equals(off) && pause.isFile() && combo.isFile() && !shader.exists() && sibling.isFile():"A stale mobile shader overlay from an older build is removed, siblings stay";
        a.portPreferences.put("mobileShaders",true);GameActivity.preparedOverlay=null;
        assert a.prepare().equals(on) && !shader.exists() && sibling.isFile():"The old mobileShaders choice no longer installs Direct3D 9 shaders";
        a.settingsIO.shutdownNow();assert GameActivity.portSetting("renderer",null)==null;
        GameActivity.currentActivity=null;assert GameActivity.portSetting("renderer",null)==null;
        System.out.println("PASS production native settings whitelist/defaults (UI 150% on phones, HDR on, gauge control on)/persistence, exact fractional display modes, deferred UI apply/layout commands, legacy DXVK renderer names and the persistent overlay without Direct3D 9 shaders");
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-native-settings-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe = folder / "NativePortCheck.java"
            probe.write_text(program.replace("/* METHODS */", methods).replace("/* RENDERER_MODE */", renderer_mode), encoding="utf-8")
            java = Path(os.environ["JAVA_HOME"]) / "bin"
            for command in ([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(folder), str(probe), str(JAVA / "PortPreferences.java"), str(JAVA / "TouchLayout.java")],
                            [str(java / "java.exe"), "-ea", "-cp", str(folder), "org.bioshock.port.NativePortCheck", str(folder)]):
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_launcher_renderer_follows_selected_profile_and_preserves_migration(self):
        sources = {
            "android/content/Context.java": r'''
package android.content;
import java.io.File;
public class Context {
    public static final int MODE_PRIVATE=0;
    public final File files,external;public String legacyFolder;
    public Context(File root){files=new File(root,"private");external=new File(root,"external");}
    public File getFilesDir(){return files;}public File getExternalFilesDir(String type){return external;}
    public Preferences getSharedPreferences(String name,int mode){assert name.equals("bioshock_paths")&&mode==0;return new Preferences();}
    public class Preferences {public String getString(String key,String fallback){assert key.equals("gameFolder");return legacyFolder!=null?legacyFolder:fallback;}}
}
''',
            "android/os/Build.java": 'package android.os; public class Build {public static final String DEVICE="fixture";}',
            "android/system/ErrnoException.java": "package android.system; public class ErrnoException extends Exception {}",
            "android/system/Os.java": r'''
package android.system;
public class Os {public static String getenv(String key){return null;}public static String readlink(String path)throws ErrnoException{throw new ErrnoException();}}
''',
            "android/util/Log.java": r'''
package android.util;
public class Log {public static int w(String tag,String text){return 0;}public static int i(String tag,String text){return 0;}}
''',
            "org/bioshock/port/launcher/PortHooks.java": r'''
package org.bioshock.port.launcher;
import android.content.Context;
public interface PortHooks {String renderer(Context context);void setRenderer(Context context,String mode);void migrate(Context context);String optionProblem(Context context,String key,String value);java.util.List<String> missing(Context context,String distribution,String part,java.io.File folder);
    String rendererWarning(Context context,String mode,String gpu,String driver);}
''',
            "org/bioshock/port/launcher/Launch.java": r'''
package org.bioshock.port.launcher;
import android.content.Context;import java.io.File;import java.util.*;
public class Launch {
    public static File selected;public static Map<String,String> adopted;
    public static String option(Context context,String key){return "int";}
    public static String language(Context context){return "en";}
    public static File folder(Context context,String distribution,String part){assert distribution.equals("bioshock")&&part.equals("game");return selected;}
    public static void adopt(Context context,Map<String,String> choices){adopted=new HashMap<>(choices);}
}
''',
            "org/bioshock/port/LauncherRendererCheck.java": r'''
package org.bioshock.port;
import android.content.Context;import java.io.*;import org.bioshock.port.launcher.Launch;
public class LauncherRendererCheck {
    public static void main(String[] args)throws Exception{
        File root=new File(args[0]),a=new File(root,"A"),b=new File(root,"B");
        Context context=new Context(root);context.legacyFolder=a.getAbsolutePath();
        PortPreferences legacy=new PortPreferences(new File(a,"userdata"));legacy.put("renderer","dxvk-turnip");legacy.put("keep","A");legacy.save();
        PortPreferences selected=new PortPreferences(new File(b,"userdata"));selected.put("renderer","gles");selected.put("keep","B");selected.save();
        LauncherHooks hooks=new LauncherHooks();Launch.selected=a;assert hooks.renderer(context).equals("vulkan-turnip"):"A legacy DXVK name reads as its Vulkan mode";
        Launch.selected=b;assert hooks.renderer(context).equals("gles"):"Selected B must win while legacy path remains A";
        hooks.setRenderer(context,"dxvk");
        assert new PortPreferences(new File(b,"userdata")).getString("renderer",null).equals("dxvk");
        assert new PortPreferences(new File(a,"userdata")).getString("renderer",null).equals("dxvk-turnip");
        assert new PortPreferences(new File(b,"userdata")).getString("keep",null).equals("B");
        selected=new PortPreferences(new File(b,"userdata"));selected.put("renderer","gles");selected.save();
        assert hooks.renderer(context).equals("gles"):"Each hook call must read the latest in-game file";
        Launch.selected=null;assert hooks.renderer(context).equals("vulkan-turnip");
        hooks.setRenderer(context,"gles");assert hooks.renderer(context).equals("gles");
        legacy=new PortPreferences(new File(a,"userdata"));legacy.put("renderer","dxvk-turnip");legacy.save();
        Launch.selected=new File(context.getFilesDir(),"launcher/mount-bioshock-game");
        assert hooks.renderer(context).equals("vulkan-turnip"):"An unresolved mounted profile keeps the existing fallback";
        Launch.selected=b;hooks.migrate(context);
        assert Launch.adopted.get("part.bioshock.game").equals(a.getAbsolutePath());
        assert Launch.adopted.get("renderer.bioshock").equals("vulkan-turnip"):"Migration imports the legacy renderer as its Vulkan mode";
        assert hooks.renderer(context).equals("gles"):
            "Migration must not rewrite the selected B renderer";
        assert new PortPreferences(new File(a,"userdata")).getString("keep",null).equals("A");
        System.out.println("PASS production LauncherHooks: fresh selected A/B profile reads, B-only renderer writes, external native edits, legacy/mount fallback and unchanged migration source");
    }
}
''',
        }
        java = Path(os.environ["JAVA_HOME"]) / "bin"
        with tempfile.TemporaryDirectory(prefix="bioshock-launcher-renderer-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            paths = []
            for name, content in sources.items():
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
                paths.append(str(path))
            for command in ([str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(folder),
                             str(JAVA / "LauncherHooks.java"), str(JAVA / "GameLanguage.java"), str(JAVA / "PortPreferences.java"), *paths],
                            [str(java / "java.exe"), "-ea", "-cp", str(folder), "org.bioshock.port.LauncherRendererCheck", str(folder)]):
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_jni_callback_cleanup_and_bounds(self):
        source = (ROOT / "android/host/android_main.c").read_text(encoding="utf-8")
        actual = function(source, "static int crypto_exception(") + "\n" + function(source, "int void_android_port_setting(")
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define EOVERFLOW 75
#define ENOSYS 38
#define JNI_OK 0
#define JNI_EDETACHED -2
#define JNI_VERSION_1_6 0x10006
typedef int jint;typedef void *jobject;typedef void *jstring;typedef void *jclass;typedef void *jmethodID;
struct Env;struct VM;typedef const struct Env *JNIEnv;typedef const struct VM *JavaVM;
struct Env {int(*ExceptionCheck)(JNIEnv*);void(*ExceptionClear)(JNIEnv*);jstring(*NewStringUTF)(JNIEnv*,const char*);jobject(*CallStaticObjectMethod)(JNIEnv*,jclass,jmethodID,jstring,jstring);const char*(*GetStringUTFChars)(JNIEnv*,jstring,void*);void(*ReleaseStringUTFChars)(JNIEnv*,jstring,const char*);void(*DeleteLocalRef)(JNIEnv*,jobject);};
struct VM {jint(*GetEnv)(JavaVM*,void**,jint);jint(*AttachCurrentThread)(JavaVM*,JNIEnv**,void*);jint(*DetachCurrentThread)(JavaVM*);};
static int detached,attached_count,detach_count,locals,releases,exception,fail,read_request;
static const char *answer="dxvk-turnip";
static JNIEnv env;
static int has_exception(JNIEnv *e){(void)e;return exception;}
static void clear_exception(JNIEnv *e){(void)e;exception=0;}
static jstring new_string(JNIEnv *e,const char *s){(void)e;if(fail==1){exception=1;return NULL;}++locals;return _strdup(s);}
static jobject call(JNIEnv *e,jclass c,jmethodID m,jstring key,jstring value){(void)e;(void)c;(void)m;assert(key);read_request=value==NULL;if(fail==2){exception=1;return NULL;}if(fail==3)return NULL;++locals;return _strdup(answer);}
static const char *chars(JNIEnv *e,jstring s,void *copy){(void)e;(void)copy;return s;}
static void release(JNIEnv *e,jstring s,const char *text){(void)e;assert(s==(void*)text);++releases;}
static void delete_ref(JNIEnv *e,jobject o){(void)e;--locals;free(o);}
static jint get_env(JavaVM *v,void **out,jint ver){(void)v;assert(ver==JNI_VERSION_1_6);if(detached)return JNI_EDETACHED;*out=&env;return JNI_OK;}
static jint attach(JavaVM *v,JNIEnv **out,void *arg){(void)v;(void)arg;++attached_count;*out=&env;return JNI_OK;}
static jint detach(JavaVM *v){(void)v;++detach_count;return JNI_OK;}
static const struct VM vm_table={get_env,attach,detach};
static JavaVM vm=&vm_table,*crypto_vm=&vm;
static jclass settings_class=(void*)1;static jmethodID settings_callback=(void*)2;
/* ACTUAL */
int main(void){
    static const struct Env table={has_exception,clear_exception,new_string,call,chars,release,delete_ref};env=&table;
    char out[4096];
    assert(!void_android_port_setting("renderer",NULL,out,sizeof out)&&!strcmp(out,"dxvk-turnip")&&read_request&&!locals);
    assert(!void_android_port_setting("renderer","dxvk",out,sizeof out)&&!read_request&&!locals);
    assert(void_android_port_setting("renderer",NULL,out,2)==EOVERFLOW&&!out[0]&&!locals);
    char large[129];memset(large,'x',128);large[128]=0;
    assert(!void_android_port_setting("renderer",large,out,sizeof out));
    char too_large[130];memset(too_large,'x',129);too_large[129]=0;
    assert(void_android_port_setting("renderer",too_large,out,sizeof out)==EINVAL&&!locals);
    assert(void_android_port_setting(large,NULL,out,sizeof out)==EINVAL&&!locals);
    detached=1;assert(!void_android_port_setting("renderer",NULL,out,sizeof out));assert(attached_count==1&&detach_count==1&&!locals);
    for(fail=1;fail<=3;++fail){assert(void_android_port_setting("renderer",NULL,out,sizeof out)!=0);assert(!exception&&!locals);}
    fail=0;static char choices[1024];memset(choices,'9',sizeof choices-1);answer=choices;
    assert(!void_android_port_setting("fpsChoices",NULL,out,sizeof out)&&strlen(out)==1023&&!locals);
    assert(void_android_port_setting("fpsChoices",NULL,out,1023)==EOVERFLOW&&!out[0]&&!locals);
    assert(releases>=6);assert(void_android_port_setting(NULL,NULL,out,sizeof out)==EINVAL);
    puts("PASS production settings JNI: read/write/nulls, output/write bounds, long FPS list, attach/detach, local references and cleared callback exceptions");
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-settings-jni-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe, obj, binary = folder / "probe.c", folder / "probe.obj", folder / "probe.exe"
            probe.write_text(program.replace("/* ACTUAL */", actual), encoding="utf-8")
            env, linker = environment("x64")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS", "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
                result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
