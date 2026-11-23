"""Original editor art, production BC3 pixels and editor-only Java paint state."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

from tests.host_environment import environment
from tests import test_touch_controls as touch_checks
from tools import patch_native_settings as native
from tools.inspect_flash_actions import tags

ROOT = Path(__file__).resolve().parents[1]


def original_images():
    data = (ROOT / "build/localization/Content/FlashMovies/pausePC.swf").read_bytes()
    assert hashlib.sha256(data).hexdigest() == native.SOURCE_SHA256
    plain = data[8:] if data[:3] == b"FWS" else zlib.decompress(data[8:])
    header = (5 + (plain[0] >> 3) * 4 + 7) // 8 + 4
    return {struct.unpack_from("<H", body)[0]: body for code, _, _, body, _ in tags(plain[header:], header)
            if code == 512 and struct.unpack_from("<H", body)[0] in (63, 65, 67)}


class NativeEditorStyleTest(unittest.TestCase):
    def test_original_pixels_equal_reference_decoder(self):
        images = original_images()
        self.assertEqual(set(images), {63, 65, 67})
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="native-editor-pixels-") as directory:
            out = Path(directory)
            source = out / "pixels.cpp"
            source.write_text(r'''
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include "''' + str(ROOT / "tests/fixtures/dxt_decode.h").replace("\\", "/") + r'''"
int main(){
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    uint8_t input[8202],bgra[256*32*4];
    for(int record=0;record<3;++record){
        if(fread(input,1,sizeof input,stdin)!=sizeof input)return 2;
        dxt::decode(input+10,bgra,256*4,256,32,0x35545844u);
        for(unsigned i=0;i<sizeof bgra;i+=4){uint8_t b=bgra[i];bgra[i]=bgra[i+2];bgra[i+2]=b;}
        if(fwrite(bgra,1,sizeof bgra,stdout)!=sizeof bgra)return 3;
    }
}
''')
            subprocess.run([str(linker.parent / "cl.exe"), "/nologo", "/O2", "/std:c++17", "/EHs",
                            str(source), "/Fo" + str(out / "pixels.obj")], cwd=ROOT, env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(out / "pixels.obj"), "/OUT:" + str(out / "pixels.exe")],
                           env=env, check=True)
            result = subprocess.run([str(out / "pixels.exe")], input=b"".join(images[i] for i in (63, 65, 67)),
                                    capture_output=True, env=env, check=True)
            for index, ident in enumerate((63, 65, 67)):
                rgba = native.editor_bitmap(images[ident])
                self.assertEqual(rgba, result.stdout[index * len(rgba):(index + 1) * len(rgba)])
                png = native.editor_png(rgba)
                self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
                position, compressed = 8, bytearray()
                while position < len(png):
                    size = struct.unpack_from(">I", png, position)[0]
                    kind, body = png[position + 4:position + 8], png[position + 8:position + 8 + size]
                    self.assertEqual(zlib.crc32(kind + body), struct.unpack_from(">I", png, position + 8 + size)[0])
                    if kind == b"IHDR":self.assertEqual(struct.unpack(">IIBBBBB", body), (256, 32, 8, 6, 0, 0, 0))
                    if kind == b"IDAT":compressed.extend(body)
                    position += size + 12
                rows = zlib.decompress(compressed)
                self.assertEqual(len(rows), 32 * 1025)
                self.assertEqual(bytes(rows[y * 1025] for y in range(32)), bytes(32))
                self.assertEqual(b"".join(rows[y * 1025 + 1:(y + 1) * 1025] for y in range(32)), rgba)
        for bad in (b"", images[63][:-1], images[63] + b"\0", images[63][:6] + b"\2\0\0\0" + images[63][10:]):
            with self.assertRaises(ValueError):native.editor_bitmap(bad)
        with self.assertRaises(ValueError):native.editor_png(bytes(10))

    def test_editor_draw_state_and_language(self):
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(touch_checks.TouchControlsTest._method(source, name) for name in (
            "private String text", "private void buildTools", "@Override protected void onDraw",
            "private void drawPill", "private void drawTools", "private String toolLabel",
            "private void loadEditorStyle", "private int toolHit"))
        program = r'''
import java.io.*;
class Typeface {static Typeface createFromAsset(Assets a,String name){assert name.equals("native-editor-font.ttf");return new Typeface();}}
class Bitmap {int id;Bitmap(int value){id=value;}}
class BitmapFactory {static int next;static Bitmap decodeStream(InputStream in){return new Bitmap(++next);}}
class Assets {int opened,closed;boolean broken;InputStream open(String name)throws IOException{
    assert name.startsWith("native-editor-button-");++opened;if(broken)throw new IOException("fixture missing");
    return new ByteArrayInputStream(new byte[1]){public void close(){++closed;}};
}}
class Context {Assets assets=new Assets();Assets getAssets(){return assets;}}
class Paint {enum Style{FILL}Typeface typeface=new Typeface();void setTypeface(Typeface f){typeface=f;}Typeface getTypeface(){return typeface;}
    void setTextSize(float v){}float measureText(String s){return s.length()*7;}void setColor(int c){}void setStyle(Style s){}
}
class Rect {int left,top;}
class RectF {float l,t,r,b;RectF(float a,float c,float d,float e){l=a;t=c;r=d;b=e;}}
class Canvas {java.util.ArrayList<Integer> skins=new java.util.ArrayList<>();java.util.ArrayList<RectF> rects=new java.util.ArrayList<>();
    void drawColor(int c){}void drawRoundRect(float a,float b,float c,float d,float e,float f,Paint p){}
    void drawBitmap(Bitmap b,Object src,RectF dst,Paint p){skins.add(b.id);rects.add(dst);}
}
class SystemClock {static long uptimeMillis(){return 100;}}
class PortPreferences {String language;String getString(String k,String fallback){assert k.equals("language");return language==null?fallback:language;}}
class TouchLayout {static final int COUNT=8;}
class Base {protected void onDraw(Canvas c){}}
class android {static class util {static class Log {static void w(String a,String b,Exception e){}}}}
public class NativeEditorStyleCheck extends Base {
    static final int TOOL_DONE=0,TOOL_RESET=1,TOOL_BUTTONS=2,TOOL_CANCEL=3,ACCENT=0xFFFFAE42;
    final Paint paint=new Paint();final Rect safe=new Rect();final float[][] tools=new float[4][];
    final Bitmap[] editorButtons=new Bitmap[3];Typeface editorFont;boolean editorStyleLoaded;
    boolean layoutDirty=true,editing=true,playing=true,pieOpen;boolean[] shown={true,true,true,true,true,true,true,true};
    int toolPressed=-1,drawnControls,drawnCaptions;long resetDeadline;float dp=1;Context context=new Context();PortPreferences p;
    java.util.ArrayList<String> labels=new java.util.ArrayList<>();
    PortPreferences preferences(){return p;}Context getContext(){return context;}
    int getWidth(){return 1000;}float width(){return 1000;}
    void buildLayout(){layoutDirty=false;buildTools();}
    void drawStick(Canvas c){}void drawPie(Canvas c){}void drawHint(Canvas c){}void drawVitals(Canvas c){}
    void drawControl(Canvas c,int control){++drawnControls;assert !editing||paint.getTypeface()==editorFont;}
    void drawEditorCaption(Canvas c,int control){++drawnCaptions;assert drawnControls>=8&&paint.getTypeface()==editorFont;}
    void drawText(Canvas c,String s,float x,float y,float size,float max,int color){labels.add(s);assert paint.getTypeface()==editorFont;}
''' + methods + r'''
    public static void main(String[] ignored){
        NativeEditorStyleCheck v=new NativeEditorStyleCheck();
        assert v.text("Done","Готово").equals("Готово");v.p=new PortPreferences();
        assert v.text("Done","Готово").equals("Готово");v.p.language="INT";
        assert v.text("Done","Готово").equals("Done");v.p.language="RUS";
        Canvas c=new Canvas();Typeface original=v.paint.getTypeface();v.onDraw(c);
        assert v.drawnControls==8&&v.drawnCaptions==8&&v.paint.getTypeface()==original&&v.editorFont!=null;
        assert v.context.assets.opened==3&&v.context.assets.closed==3;
        assert c.skins.equals(java.util.Arrays.asList(1,1,1,1)); // Original gold UP skin for every tool.
        assert v.labels.subList(0,4).equals(java.util.Arrays.asList("Готово","Сброс","Кнопки","Отмена"));
        assert v.tools[0][0]==293&&v.tools[0][1]==10&&v.tools[0][2]==389&&v.tools[0][3]==50;
        assert v.tools[3][0]==611&&v.tools[3][2]==707;
        for(int i=0;i<v.tools.length;++i)assert v.toolHit(v.tools[i][0]+48,20)==i;
        assert c.rects.get(0).l==v.tools[0][0]&&c.rects.get(1).r==v.tools[1][2];
        v.toolPressed=TOOL_DONE;c.skins.clear();v.onDraw(c);assert c.skins.get(0)==3;
        v.toolPressed=-1;v.resetDeadline=200;c.skins.clear();v.onDraw(c);assert c.skins.get(1)==2;
        assert v.labels.contains("Сбросить?");v.toolPressed=TOOL_RESET;c.skins.clear();v.onDraw(c);assert c.skins.get(1)==3;
        int captions=v.drawnCaptions;v.editing=false;v.pieOpen=false;v.onDraw(new Canvas());assert v.paint.getTypeface()==original&&v.context.assets.opened==3&&v.drawnCaptions==captions;
        NativeEditorStyleCheck broken=new NativeEditorStyleCheck();broken.context.assets.broken=true;broken.loadEditorStyle();
        assert broken.editorButtons[0]==null&&broken.editorFont==null;broken.loadEditorStyle();assert broken.context.assets.opened==1;
        System.out.println("PASS production editor: original skin for all four tools, layout and hit targets, press and reset states, EN/RU labels, missing-assets fallback");
    }
}
'''
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        with tempfile.TemporaryDirectory(prefix="native-editor-java-") as directory:
            path = Path(directory) / "NativeEditorStyleCheck.java"
            path.write_text(program, encoding="utf-8")
            subprocess.run([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", directory, path], check=True)
            subprocess.run([java / "java.exe", "-ea", "-cp", directory, "NativeEditorStyleCheck"], check=True)


if __name__ == "__main__":
    unittest.main()
