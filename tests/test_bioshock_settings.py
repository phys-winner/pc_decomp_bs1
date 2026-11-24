"""Execute the production original-Flash settings bridge and its AS2 navigation."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tools.patch_native_settings import PORT_NAVIGATION, responsive_release

ROOT = Path(__file__).resolve().parents[1]


class BioshockSettingsTest(unittest.TestCase):
    def test_original_release_commits_once_and_keeps_disable_guards(self):
        self.assertEqual(responsive_release('on(release){ _root.guiVal = "TRUE"; }'),
                         'on(release){ _root.UIHighlightRight(); }')
        original = ('on(release){ if(!_root.PCUseControllerVisible){return undefined;} '
                    '_root.guiVal = "FALSE"; _root.AcceptOrSelect(); }')
        self.assertEqual(responsive_release(original), original.replace('_root.guiVal = "FALSE";', '_root.UIHighlightRight();'))
        slider = 'on(release){ _root.guiVal = "NULL"; _root.sliderIdx = -1; }'
        self.assertEqual(responsive_release(slider), slider.replace('_root.guiVal', '_root.HandleGuiUpdate();\n   _root.guiVal', 1))
        self.assertIn('_root.HandleGuiUpdate();', responsive_release(slider.replace('on(release)', 'on(rollOut)')))
        self.assertEqual(responsive_release('on(release){_root.CancelOrExit();}'), 'on(release){_root.CancelOrExit();}')

    def test_original_fs_callback_and_poll(self):
        env, linker = environment("x64")
        host = (ROOT / "android/host/host.h").read_text(encoding="utf-8")
        runtime = (ROOT / "android/runtime/recomp_types.h").read_text(encoding="utf-8")
        regs = host[host.index("typedef struct void_regs {"):host.index("} void_regs;") + len("} void_regs;")]
        end = runtime.index("} xmm_t;") + len("} xmm_t;")
        xmm = runtime[runtime.rindex("typedef union {", 0, end):end]
        slots = re.search(r"#define RECOMP_X87_SLOTS \d+", runtime).group()
        with tempfile.TemporaryDirectory(prefix="bioshock-settings-") as tmp:
            out = Path(tmp)
            (out / "bioshock_settings_fixture.h").write_text(xmm + "\n" + regs + "\n" + slots + r'''
#define MEM8(address) (*(uint8_t *)ADDR(address))
typedef void (*void_fn)(void);
extern double g_st_win[RECOMP_X87_SLOTS];
extern int64_t g_st_i64_win[RECOMP_X87_SLOTS];
extern int g_fp_sp;
void void_regs_save(void_regs *out);
void void_regs_load(const void_regs *in);
uint32_t void_gstrdup(const char *s);
void void_gfree(uint32_t address);
uint32_t void_call_guest(uint32_t address,int nargs,const uint32_t *args,int pop);
void_fn void_dispatch_find(uint32_t address);
void void_dispatch_add(uint32_t address,void_fn function);
_Noreturn void void_fatal(const char *format,...);
''', encoding="utf-8")
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS", "/DRECOMP_X87_WINDOW",
                            "/clang:--target=x86_64-pc-windows-msvc", "/I" + str(out), "/c",
                            str(ROOT / "tests/test_bioshock_settings.c"), "/Fo" + str(out / "test.obj")],
                           cwd=ROOT, env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(out / "test.obj"), "/OUT:" + str(out / "test.exe")], env=env, check=True)
            subprocess.run([str(out / "test.exe")], env=env, check=True, timeout=30)
        subprocess.run([str(CLANG.with_name("clang.exe")), "--target=aarch64-linux-android29", "-O2", "-std=gnu11",
                        "-Wall", "-Wextra", "-Werror", "-fno-strict-aliasing", "-fwrapv", "-DRECOMP_X87_WINDOW",
                        "-DUT3_ANDROID_APP", "-Iandroid/runtime", "-Iandroid/host", "-Igenerated", "-fsyntax-only",
                        "android/host/bioshock_settings.c"], cwd=ROOT, check=True)

    def test_production_actionscript_navigation(self):
        node = shutil.which("node")
        self.assertIsNotNone(node)
        script = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source="String.prototype.split=function(){throw new Error('unsupported original String.split');};\n"+fs.readFileSync(process.argv[1],'utf8');
const calls=[];
function panel(){const p={};for(let i=0;i<12;i++)p['Row'+i]={Focus:{gotoAndStop(){}},Value:{},Left:{},Right:{},Fov:{Thumb:{}}};
    p.Back={Focus:{gotoAndStop(){}}};p.ColumnLeft={};p.ColumnRight={};return p;}
const root={PortRussian:true,PortOriginalNames:['OriginalOption'],OriginalOption:{},PortEntry:{},
    _currentframe:7,SubOptionsScreen1:7,SubOptionsScreen2:8,PortSectionPage:1,PortOriginalSelections:14,maxSelections:14,
    ScrollBar0:{},PortPanel:panel(),ResetList(){this.selectorPos=0;},PortOriginal_AcceptOrSelect(){calls.push(['originalAccept']);},
    PortOriginal_CancelOrExit(){calls.push(['originalBack']);},PortOriginal_UIHighlightLeft(){},
    PortOriginal_UIHighlightUp(){calls.push(['originalUp']);},PortOriginal_UIHighlightDown(){calls.push(['originalDown']);},
    PortOriginal_UIHighlightRight(){},PortOriginal_OnEnterFrame(){calls.push(['originalFrame']);},
    fscommand(...args){calls.push(args);}};
root._root=root;
vm.runInNewContext(source,root);
vm.runInNewContext(process.argv[2],root);
root._currentframe=7;root.PortPage=0;
root.PortOpenAdditional();assert(root.PortFromAdditional&&root.PortPage===3);
assert(root.PortPanel.Status.includes('Выберите раздел'));
root.SwitchToSubOptionsScreen2=()=>calls.push(['switchControls']);
root.PortOpenSection(2);assert.equal(root.PortRequested,2);assert.deepEqual(calls.pop(),['switchControls']);
root.PortRequested=0;root.PortFromAdditional=false;root.PortShow(0);
assert.equal(root.PortPanel._x,-16384);assert(root.PortPanel._x+1091<0); // Hidden buttons cannot overlap any original control.
// Graphics: its own page, no paging between sections.
assert.equal(root.PortPage,0);root.AcceptOrSelect();assert.deepEqual(calls.pop(),['originalAccept']);
assert.equal(root.PortEntry.Caption,'Дополнительно...');
root.UIHighlightUp();assert.deepEqual(calls.pop(),['originalUp']);
root.UIHighlightDown();assert.deepEqual(calls.pop(),['originalDown']);
root.PortShow(1);assert.equal(root.OriginalOption._visible,false);assert.equal(root.PortPanel._visible,true);
assert.equal(root.PortPanel._x,0); // Restore authored panel/button positions when opened.
assert.deepEqual(calls.pop(),['PortSettings','refresh']);
assert.equal(root.selectorPos,-1);
assert.equal(root.PortPanel.Row0.Focus._visible,false);
root.PortFocus(0,0);assert.equal(root.PortPanel.Row0.Focus._visible,true);
root.PortFocus(0,1);root.PortBlur(0);assert.equal(root.PortPanel.Row0.Focus._visible,true);
root.PortBlur(1);assert.equal(root.PortPanel.Row0.Focus._visible,false);
root.PortFocus(11,33);assert.equal(root.selectorPos,-1);
assert.equal(root.PortPanel.Row11._x,-16384);
let idleCalls=calls.length;root.AcceptOrSelect();root.UIHighlightLeft();root.UIHighlightRight();assert.equal(calls.length,idleCalls);
assert.deepEqual(Array.from(root.PortKeys),['fpsLimit','hdr','uiScale','playerFov','renderScale','upscaler','anisotropy','weaponFov']);
assert.equal(root.maxSelections,9);root.UIHighlightUp();assert.equal(root.selectorPos,8);
root.UIHighlightDown();assert.equal(root.selectorPos,0);root.UIHighlightDown();assert.equal(root.selectorPos,1);
root.UIHighlightUp();assert.equal(root.selectorPos,0);
assert.equal(root.PortPanel.Row0._x,410);assert.equal(root.PortPanel.Row4._y,312);
assert.equal(root.PortPanel.Row3._y,480);assert.equal(root.PortPanel.Row7._y,480);assert.equal(root.PortPanel.Row7._visible,true);
assert.equal(root.PortPanel.ColumnLeft._visible,true);
root.PortSet('renderer','dxvk-turnip');root.PortSet('fpsChoices','0,60,90,120');root.PortSet('fpsLimit','0.0');root.PortSet('uiScale','1.5');
assert.equal(root.PortPanel.Row0.Value.Caption,'Ограничение FPS: Частота дисплея');
assert.equal(root.PortPanel.Row2.Value.Caption,'Масштаб интерфейса: 150%');
root.PortSet('hdr','1');assert.equal(root.PortPanel.Row1.Value.Caption,'HDR-рендеринг: Вкл.');
root.PortAdjust(1,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','hdr=0']);
assert(root.PortStatus.includes('Без HDR'));root.PortRender();assert(root.PortPanel.Status.includes('дымки'));
root.PortSet('hdr','0');assert.equal(root.PortCaption('hdr'),'HDR-рендеринг: Выкл.');
root.PortAdjust(1,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','hdr=1']);assert.equal(root.PortStatus,'');
root.PortSet('hdr','1');
assert.equal(root.PortPanel.Row0.Left.Caption,'>');assert.equal(root.PortPanel.Row0.Right.Caption,'>');
assert.equal(root.PortPanel.Header,'ДОПОЛНИТЕЛЬНЫЕ НАСТРОЙКИ ГРАФИКИ');assert(root.PortPanel.Status.includes('перезапуска'));
assert.equal(root.PortPanel.Back.Caption,'Назад');
root.PortEntry.Caption='';root.PortPanel.Header='';root.PortPanel.Status='';root.PortPanel.Back.Caption='';
root.PortPanel.Row0.Value.Caption='';root.PortPanel.Row0.Left.Caption='';root.PortPanel.Row0.Right.Caption='';
root.OnEnterFrame();assert.equal(root.PortEntry.Caption,'Дополнительно...');
assert.equal(root.PortPanel.Header,'ДОПОЛНИТЕЛЬНЫЕ НАСТРОЙКИ ГРАФИКИ');assert.equal(root.PortPanel.Back.Caption,'Назад');
assert.equal(root.PortPanel.Row0.Value.Caption,'Ограничение FPS: Частота дисплея');
assert(root.PortCaption('fpsLimit').includes('Частота дисплея'));
assert(!Array.from(root.PortKeys).includes('renderer'));
root.PortAdjust(0,-1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','fpsLimit=120']);root.PortSet('fpsLimit','120.00001');assert.equal(root.PortCaption('fpsLimit'),'Ограничение FPS: 120');root.PortSet('fpsLimit','0.0');
const exactRates=['0','29.97003','37.5','45.0','59.94006','90.0','120.00001'];
root.PortSet('fpsChoices',exactRates.join(','));
root.PortSet('fpsLimit','29.97003');assert.equal(root.PortCaption('fpsLimit'),'Ограничение FPS: 30');
root.PortSet('fpsLimit','37.5');assert.equal(root.PortCaption('fpsLimit'),'Ограничение FPS: 37.5');
assert.deepEqual(Array.from(root.PortChoices('fpsLimit')),exactRates);
for(let i=0;i<exactRates.length;i++){
    root.PortSet('fpsLimit',exactRates[i]);root.PortAdjust(0,1);
    assert.deepEqual(calls.slice(-2)[0],['PortSettings','fpsLimit='+exactRates[(i+1)%exactRates.length]]);
    root.PortAdjust(0,-1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','fpsLimit='+exactRates[(i+exactRates.length-1)%exactRates.length]]);
}
root.PortSet('fpsChoices','0');root.PortSet('fpsLimit','0.0');root.PortAdjust(0,1);
assert.deepEqual(calls.slice(-2)[0],['PortSettings','fpsLimit=0']);
delete root.PortValues.fpsChoices;assert.deepEqual(Array.from(root.PortChoices('fpsLimit')),['0']);
root.PortAdjust(2,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','uiScale=1']);
root.PortAdjust(2,-1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','uiScale=1.4']);
root.selectorPos=8;root.AcceptOrSelect();assert.deepEqual(calls.pop(),['TriggerEffectEvent','Event=UIFrontEndBack, Tag=']);
assert.equal(root.PortPage,0);assert.equal(root.maxSelections,14);
assert(root.OriginalOption._visible && !root.PortPanel._visible && root.PortEntry._visible);
root.OnEnterFrame();assert.deepEqual(calls.pop(),['originalFrame']);
root.CancelOrExit();assert.deepEqual(calls.pop(),['originalBack']);
// Input: the touch controls are the Input section's own page, in two columns.
Object.assign(root,{_currentframe:8,PortSectionPage:2,PortOriginalSelections:4,maxSelections:4,PortEntry:{},PortPanel:panel(),
    PortOriginalNames:['SingleTab0'],SingleTab0:{}});
vm.runInNewContext(source,root);
assert.equal(root.PortEntry.Caption,'Сенсорное управление...');assert.equal(root.PortPage,0);
root.PortShow(2);assert.equal(root.SingleTab0._visible,false);
assert.deepEqual(Array.from(root.PortKeys),['touchSensitivity','touchAcceleration','touchInvertY','touchGyro','touchGyroSensitivity','touchVitals',
    'touchScale','touchOpacity','touchLeftHanded','touchMore','editLayout','resetLayout']);
assert.equal(root.maxSelections,13);root.UIHighlightUp();assert.equal(root.selectorPos,12);
for(let i=1;i<=13;i++){root.UIHighlightDown();assert.equal(root.selectorPos,(12+i)%13);}
assert.equal(root.PortPanel.Row0._x,410);assert.equal(root.PortPanel.Row5._y,592);
assert.equal(root.PortPanel.Row6._x,870);assert.equal(root.PortPanel.Row6._y,312);assert.equal(root.PortPanel.Row11._y,592);
assert.equal(root.PortPanel.Row11._visible,true);assert.equal(root.PortPanel.ColumnLeft._visible,true);
assert.equal(root.PortPanel.ColumnLeft.Caption,'ОБЗОР');assert.equal(root.PortPanel.ColumnRight.Caption,'КНОПКИ');
assert.equal(root.PortPanel.Header,'СЕНСОРНОЕ УПРАВЛЕНИЕ');
root._currentframe=7;root.UIHighlightUp();assert.deepEqual(calls.pop(),['originalUp']);
root.UIHighlightDown();assert.deepEqual(calls.pop(),['originalDown']);root._currentframe=8;
root.PortSet('touchSensitivity','0.3');root.PortAdjust(0,-1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','touchSensitivity=0.22']);
root.PortSet('touchGyro','1');assert(root.PortCaption('touchGyro').includes('При огне'));
for(const value of [undefined,0,'0','0.0']){
    root.PortSet('touchGyro',value);assert.equal(root.PortCaption('touchGyro'),'Гироскоп: Выкл.');
}
for(const value of [2,'2','2.0']){
    root.PortSet('touchGyro',value);assert.equal(root.PortCaption('touchGyro'),'Гироскоп: Всегда');
}
root.PortSet('touchScale','1.2');assert.equal(root.PortPanel.Row6.Value.Caption,'Размер: 120%');
root.PortSet('touchVitals','1');assert.equal(root.PortPanel.Row5.Value.Caption,'Шкала здоровья: Вкл.');
root.PortSet('touchMore','0');assert.equal(root.PortPanel.Row9.Value.Caption,'Кнопка Еще: Выкл.');
root.PortAdjust(9,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','touchMore=1']);
root.PortAdjust(5,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','touchVitals=0']);
root.bInMainMenu=true;
let n=calls.length;root.PortAdjust(10,1);assert.equal(calls.length,n);assert(root.PortStatus.includes('Войдите'));
root.bInMainMenu=false;root.PortAdjust(10,1);assert.deepEqual(calls.slice(-2),[['PortSettings','editLayout=1'],['ResumeSelected']]);
root.PortAdjust(11,1);assert.deepEqual(calls.pop(),['PortSettings','resetLayout=1']);
root.CancelOrExit();assert.equal(root.PortPage,0);assert.equal(root.maxSelections,4);
root.PortEntry.Caption='';root.OnEnterFrame();assert.equal(root.PortEntry.Caption,'Сенсорное управление...');
assert.deepEqual(calls.pop(),['originalFrame']);
// The original-menu request (gear) opens the requested section on its frame.
root.PortRequested=2;vm.runInNewContext(source,root);assert.equal(root.PortPage,2);assert.equal(root.PortRequested,0);
// The visible Additional catalog reaches both sections and returns without losing its parent.
const routes=[];root.PortOpenSection=page=>routes.push(page);root.SwitchToPCOptionsScreen=()=>routes.push(0);
root.PortRequested=3;root.PortFromAdditional=true;vm.runInNewContext(source,root);
assert.equal(root.PortPage,3);assert.equal(root.PortPanel.Header,'ДОПОЛНИТЕЛЬНЫЕ НАСТРОЙКИ ПОРТА');
assert.deepEqual(Array.from(root.PortKeys),['graphicsSettings','controlSettings','inputSettings','gameSettings']);
assert.equal(root.PortCaption('gameSettings'),'Игра и исправления...');
root.PortAdjust(0,1);root.PortAdjust(1,1);root.PortAdjust(2,1);root.PortAdjust(3,1);assert.deepEqual(routes,[1,2,4,5]);
// Game and fixes: tweaks keep the original until chosen, the fixes default on (Java), exact values.
root.PortShow(5);assert.deepEqual(Array.from(root.PortKeys),['physicsRate','cutsceneFps','fixScrounger','mouseRaw']);
assert.equal(root.PortPanel.Header,'ИГРА И ИСПРАВЛЕНИЯ');assert(root.PortPanel.Status.includes('60 Гц'));
assert.equal(root.PortPanel.Row0._x,640);assert.equal(root.PortPanel.Row3._y,510);assert.equal(root.PortPanel.Row4._visible,false);
root.PortSet('physicsRate','30');assert.equal(root.PortPanel.Row0.Value.Caption,'Физика: 30 Гц (оригинал)');
root.PortAdjust(0,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','physicsRate=60']);
root.PortSet('physicsRate','60');assert.equal(root.PortCaption('physicsRate'),'Физика: 60 Гц');
root.PortAdjust(0,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','physicsRate=30']);
root.PortSet('cutsceneFps','1');assert.equal(root.PortPanel.Row1.Value.Caption,'Ролики до 60 FPS: Вкл.');
root.PortAdjust(1,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','cutsceneFps=0']);
root.PortSet('fixScrounger','1');assert.equal(root.PortCaption('fixScrounger'),'Защита предметов: Вкл.');
root.PortAdjust(2,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','fixScrounger=0']);assert(root.PortStatus.includes('Лэнгфорда'));
root.PortSet('mouseRaw','0');assert.equal(root.PortCaption('mouseRaw'),'Мышь без сглаживания: Выкл.');
root.PortAdjust(3,-1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','mouseRaw=1']);
root.PortRussian=false;assert.equal(root.PortCaption('physicsRate'),'Physics: 60 Hz');root.PortRussian=true;
root.PortShow(4);assert.deepEqual(Array.from(root.PortKeys),['touchScheme','touchPrompts','touchUse','iosUiScale','haptic','hapticStrength','touchAimAssist']);
root.PortSet('touchUse','1');root.PortAdjust(2,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','touchUse=0']);
root.PortSet('touchScheme','1');assert.equal(root.PortCaption('touchScheme'),'Наэкранное управление: Порт');
root.PortSet('touchScheme','2');assert.equal(root.PortCaption('touchScheme'),'Наэкранное управление: iPhone');
root.PortSet('touchScheme','3');assert.equal(root.PortCaption('touchScheme'),'Наэкранное управление: iPad');
root.PortSet('touchScheme','0');assert.equal(root.PortCaption('touchScheme'),'Наэкранное управление: Выкл.');
root.PortValues.touchScheme=undefined;assert.equal(root.PortCaption('touchScheme'),'Наэкранное управление: ...');
root.PortSet('iosUiScale','1');assert.equal(root.PortCaption('iosUiScale'),'Масштаб интерфейса iOS: 100%');
root.PortSet('haptic','1');assert.equal(root.PortCaption('haptic'),'Вибрация: Вкл.');
root.PortSet('hapticStrength','0.5');assert.equal(root.PortCaption('hapticStrength'),'Сила вибрации: 50%');
root.PortSet('touchAimAssist','0');assert.equal(root.PortCaption('touchAimAssist'),'Помощь в прицеливании: Выкл.');
assert.equal(root.PortCaption('touchVitals')==='Шкала здоровья: ...'||root.PortCaption('touchVitals').indexOf('Шкала здоровья')===0,true);
root.PortRussian=false;
assert.equal(root.PortCaption('iosUiScale'),'iOS interface scale: 100%');
assert.equal(root.PortCaption('haptic'),'Vibration: On');
assert.equal(root.PortCaption('touchAimAssist'),'Touch aim assist: Off');
root.PortRussian=true;
root.PortShow(1);assert.deepEqual(Array.from(root.PortChoices('renderScale')),['50','60','70','80','90','100']);
root.PortSet('renderScale','50');root.PortAdjust(4,-1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','renderScale=100']);
root.PortSet('upscaler','0');root.PortAdjust(5,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','upscaler=1']);
assert.deepEqual(Array.from(root.PortChoices('upscaler')),['0','1']);
root.PortSet('upscaler','2');assert.equal(root.PortCaption('upscaler'),'Масштабирование: FSR 1');
root.PortAdjust(5,1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','upscaler=0']);
root.PortSet('upscaler','0');
assert.deepEqual(Array.from(root.PortChoices('anisotropy')),['0','4','8','16']);
root.PortSet('anisotropy','0');assert.equal(root.PortPanel.Row6.Value.Caption,'Анизотропия: Игра');assert.equal(root.PortCaption('anisotropy').indexOf('...'),-1);
root.PortAdjust(6,-1);assert.deepEqual(calls.slice(-2)[0],['PortSettings','anisotropy=16']);
root.PortSet('anisotropy','16');assert.equal(root.PortCaption('anisotropy'),'Анизотропия: 16x');
assert(root.PortPanel.Status.includes('Анизотропия применяется после перезапуска'));
root.PortSet('playerFov','75');root.PortSet('weaponFov','60');
assert.equal(root.PortPanel.Row3.Fov.Caption,'Обзор игрока: 75 град.');
assert.equal(root.PortPanel.Row7.Fov.Caption,'Обзор оружия: 60 град.');
const beforeDrag=calls.length;root._xmouse=410;root.PortFovPress(3);root._xmouse=536;root.OnEnterFrame();
assert.equal(root.PortPanel.Row3.Fov.Thumb._x,126);assert.equal(calls.length,beforeDrag);
root.PortFovRelease();assert.deepEqual(calls.pop(),['PortSettings','playerFov=120']);assert.equal(root.PortValues.weaponFov,'60');
root._xmouse=700;root.PortFovPress(7);root.PortFovRelease();assert.deepEqual(calls.pop(),['PortSettings','weaponFov=40']);
root.PortAdjust(7,1);assert.deepEqual(calls.pop(),['PortSettings','weaponFov=41']);
root.PortShow(2);root.PortSet('touchAcceleration','1');root.PortAdjust(1,-1);
assert.deepEqual(calls.slice(-2)[0],['PortSettings','touchAcceleration=0']);
root.CancelOrExit();assert.equal(routes.pop(),3);root.PortShow(3);root.CancelOrExit();
assert.equal(routes.pop(),0);assert.equal(root.PortFromAdditional,false);
console.log('PASS production AS2 settings: sections Graphics/Input, original callbacks/scope, clip text bindings, one/two-column layout, Up/Down wrap, exact values, FPS 0.0, interface scale, gyro, edit-layout guard/resume');
'''
        subprocess.run([node, "-e", script, str(ROOT / "android/flash/port_settings.as"), PORT_NAVIGATION], cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
