"""Production editor caption geometry against the actual exported original font."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest

from tests import test_touch_controls as touch_checks

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/probes/editor-caption-bounds"
FONT_SHA256 = "2116a3b9ee7cea27a439fcd2e53cc61b9a0195fd207debe68041cafbadf52752"


class EditorCaptionBoundsTest(unittest.TestCase):
    def test_production_labels_with_original_font_and_viewport_edges(self):
        source = (ROOT / "android/app/src/org/bioshock/port/GameControls.java").read_text(encoding="utf-8")
        methods = "\n".join(touch_checks.TouchControlsTest._method(source, name) for name in (
            "private static float clamp", "private String text", "private String label",
            "private String accessibilityLabel", "private void drawEditorLabel"))
        font = ROOT / "build/native-settings/native-editor-font.ttf"
        self.assertEqual(hashlib.sha256(font.read_bytes()).hexdigest(), FONT_SHA256)
        program = r'''
package org.bioshock.port;
import java.awt.Font;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.font.FontRenderContext;
import java.awt.font.GlyphVector;
import java.awt.geom.Rectangle2D;
import java.awt.image.BufferedImage;
import java.io.File;
import javax.imageio.ImageIO;
class Rect {
    int left,top,right,bottom;
    int width(){return right-left;}int height(){return bottom-top;}
}
class Paint {
    enum Style{FILL}enum Align{LEFT,CENTER}
    static class FontMetrics{float ascent,descent;}
    static final FontRenderContext FRC=new FontRenderContext(null,true,true);
    Font original,font;float size;Align align;
    Paint(Font f){original=f;font=f;}
    void setStyle(Style s){}void setColor(int c){}void setTextAlign(Align a){align=a;}
    void setTextSize(float value){size=value;font=original.deriveFont(value);}float getTextSize(){return size;}
    float measureText(String value){return (float)font.getStringBounds(value,FRC).getWidth();}
    void getTextBounds(String value,int first,int end,Rect out){
        assert align==Align.LEFT;
        Rectangle2D b=font.createGlyphVector(FRC,value.substring(first,end)).getVisualBounds();
        out.left=(int)Math.floor(b.getMinX());out.top=(int)Math.floor(b.getMinY());
        out.right=(int)Math.ceil(b.getMaxX());out.bottom=(int)Math.ceil(b.getMaxY());
    }
    FontMetrics getFontMetrics(){
        java.awt.font.LineMetrics f=font.getLineMetrics("Ag",FRC);FontMetrics m=new FontMetrics();
        m.ascent=-f.getAscent();m.descent=f.getDescent();return m;
    }
}
class Canvas {
    int draws;String text;double left,top,right,bottom;Graphics2D graphics;
    void drawText(String value,float x,float baseline,Paint p){
        assert p.align==Paint.Align.CENTER;float origin=x-p.measureText(value)*.5f;
        Rectangle2D b=p.font.createGlyphVector(Paint.FRC,value).getVisualBounds();
        left=origin+b.getMinX();right=origin+b.getMaxX();top=baseline+b.getMinY();bottom=baseline+b.getMaxY();
        ++draws;text=value;
        if(graphics!=null){graphics.setFont(p.font);graphics.setColor(java.awt.Color.WHITE);graphics.drawString(value,origin,baseline);}
    }
}
class PortPreferences{String language="RUS";String getString(String key,String fallback){return language;}}
public class EditorCaptionBoundsCheck {
    final Paint paint;final Rect safe=new Rect(),editorTextBounds=new Rect();
    Font editorFont;float dp;int w,h;boolean playing=true;PortPreferences prefs=new PortPreferences();
    EditorCaptionBoundsCheck(Font f){editorFont=f;paint=new Paint(f);}
    int getWidth(){return w;}int getHeight(){return h;}PortPreferences preferences(){return prefs;}
    /* PRODUCTION METHODS */
    void verify(Canvas c){
        assert c.left>=safe.left+4*dp-.01&&c.top>=safe.top+4*dp-.01:c.text+" starts outside";
        assert c.right<=w-safe.right-4*dp+.01&&c.bottom<=h-safe.bottom-4*dp+.01:c.text+" ends outside";
    }
    public static void main(String[] args)throws Exception{
        Font original=Font.createFont(Font.TRUETYPE_FONT,new File(args[0]));
        Font actual=original.deriveFont(100f);
        GlyphVector yo=actual.createGlyphVector(Paint.FRC,"ё"),e=actual.createGlyphVector(Paint.FRC,"е");
        assert actual.canDisplay('ё')&&actual.canDisplay('е');
        assert yo.getVisualBounds().getWidth()>yo.getGlyphMetrics(0).getAdvance()*1.5;
        assert e.getVisualBounds().getWidth()<e.getGlyphMetrics(0).getAdvance()*1.1;
        EditorCaptionBoundsCheck view=new EditorCaptionBoundsCheck(original);Canvas canvas=new Canvas();
        int[] controls={TouchLayout.FIRE,TouchLayout.ALT,TouchLayout.JUMP,TouchLayout.DUCK,TouchLayout.USE,TouchLayout.WEAPON,TouchLayout.MORE,TouchLayout.PAUSE};
        int cases=0;
        for(String language:new String[]{"RUS","INT"})for(int[] size:new int[][]{{2780,1264},{1920,1080},{1280,720},{800,480}})
        for(float density:new float[]{1,1.5f,2,3.4375f})for(float scale:new float[]{.65f,1,1.6f})for(boolean leftHanded:new boolean[]{false,true}){
            view.prefs.language=language;view.w=size[0];view.h=size[1];view.dp=density;
            view.safe.left=size[0]==2780?140:0;view.safe.top=3;view.safe.right=5;view.safe.bottom=7;
            float l=view.safe.left,t=view.safe.top,r=view.w-view.safe.right,b=view.h-view.safe.bottom;
            for(int control:controls){
                String semantic=view.label(control);
                float diameter=TouchLayout.size(control,density,scale),radius=diameter*.5f;
                float[] at=TouchLayout.anchor(control,l,t,r,b,density,scale,leftHanded);
                for(float[] center:new float[][]{at,{l+radius,t+radius},{r-radius,b-radius},{l+radius,b-radius},{r-radius,t+radius}}){
                    view.drawEditorLabel(canvas,semantic,center[0],center[1]+radius+11*density,11*density,Math.max(diameter+24*density,80*density),0);
                    view.verify(canvas);++cases;
                    assert semantic.equals(view.label(control));
                    if(language.equals("RUS")&&control==TouchLayout.MORE){assert semantic.equals("Ещё")&&canvas.text.equals("Еще");assert view.accessibilityLabel(control).equals("Ещё действия");}
                }
            }
        }
        view.w=2780;view.h=1264;view.dp=3.4375f;view.safe.left=140;view.safe.top=view.safe.right=view.safe.bottom=0;view.prefs.language="RUS";
        BufferedImage preview=new BufferedImage(view.w,view.h,BufferedImage.TYPE_INT_RGB);Graphics2D g=preview.createGraphics();
        g.setColor(new java.awt.Color(12,16,19));g.fillRect(0,0,view.w,view.h);
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING,RenderingHints.VALUE_TEXT_ANTIALIAS_ON);canvas.graphics=g;
        for(int control:controls){
            float radius=TouchLayout.size(control,view.dp,1)*.5f;float[] at=TouchLayout.anchor(control,140,0,view.w,view.h,view.dp,1,false);
            g.setColor(new java.awt.Color(24,29,35));g.fillOval(Math.round(at[0]-radius),Math.round(at[1]-radius),Math.round(radius*2),Math.round(radius*2));
            g.setColor(new java.awt.Color(70,78,82));g.drawOval(Math.round(at[0]-radius),Math.round(at[1]-radius),Math.round(radius*2),Math.round(radius*2));
        }
        for(int control:controls){
            float radius=TouchLayout.size(control,view.dp,1)*.5f;float[] at=TouchLayout.anchor(control,140,0,view.w,view.h,view.dp,1,false);
            view.drawEditorLabel(canvas,view.label(control),at[0],at[1]+radius+11*view.dp,11*view.dp,Math.max(radius*2+24*view.dp,80*view.dp),0);view.verify(canvas);
        }
        ImageIO.write(preview,"png",new File(args[1]));g.dispose();canvas.graphics=null;
        /* Missing original font keeps Unicode semantics in the system-font fallback. */
        view.editorFont=null;view.drawEditorLabel(canvas,"Ещё",100,100,30,200,0);assert canvas.text.equals("Ещё");
        int before=canvas.draws;view.drawEditorLabel(canvas,"",0,0,30,200,0);view.drawEditorLabel(canvas,"x",0,0,30,0,0);assert canvas.draws==before;
        System.out.println("PASS "+cases+" production editor caption bounds: real pinned native font, RUS/INT, viewport/density/scale/handedness/default/custom edges, unchanged semantic/accessibility labels; original malformed yo workaround and no-font fallback");
    }
}
'''.replace("/* PRODUCTION METHODS */", methods)
        OUT.mkdir(parents=True, exist_ok=True)
        path = OUT / "EditorCaptionBoundsCheck.java"
        path.write_text(program, encoding="utf-8")
        layout = OUT / "TouchLayout.java"
        layout.write_bytes((ROOT / "android/app/src/org/bioshock/port/TouchLayout.java").read_bytes())
        java = Path(os.environ.get("JAVA_HOME", "C:/Portable/jdk-24.0.1")) / "bin"
        commands = [[str(java / "javac.exe"), "--release", "8", "-encoding", "UTF-8", "-d", str(OUT), str(path), str(layout)],
                    [str(java / "java.exe"), "-ea", "-Djava.awt.headless=true", "-cp", str(OUT), "org.bioshock.port.EditorCaptionBoundsCheck", str(font), str(OUT / "native-caption-preview.png")]]
        logs = []
        for command in commands:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=60)
            logs.append((result.stdout + result.stderr).decode(errors="replace"))
            self.assertEqual(result.returncode, 0, logs[-1])
        (OUT / "host.log").write_text("\n".join(logs), encoding="utf-8")
        pins = ("android/app/src/org/bioshock/port/GameControls.java", "tests/test_editor_caption_bounds.py", "tests/test_native_editor_style.py")
        (OUT / "receipt.json").write_text(json.dumps({"status": "PASS_PRODUCTION_EDITOR_FONT_AND_BOUNDS", "cases": 7680,
            "scope": "Production Java methods with actual original TTF via Java2D metrics; host geometry/raster preview, not Android rendering equivalence",
            "font_sha256": FONT_SHA256, "device_executed": False,
            "source_sha256": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in pins},
            "commands": commands}, indent=2) + "\n")
        print(logs[-1].strip())


if __name__ == "__main__":
    unittest.main()
