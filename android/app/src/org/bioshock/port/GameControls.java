package org.bioshock.port;

import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.text.InputType;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.view.WindowInsets;
import android.view.ViewConfiguration;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.io.IOException;
import java.io.InputStream;

final class GameControls extends View implements SensorEventListener {
    /** Control behaviours beyond TouchInput's held action bits. */
    private static final int MORE=-1, MODE=-2, SETTINGS=-3, KEYBOARD=-4, WEAPON=-5, SEATS=-6, MORE_MAIN=-7, QUICKSAVE=-8, TOOLS=-9;
    private static final int PIE_ACTIONS=0, PIE_ABILITIES=1, PIE_TOOLS=2, ABILITY_COUNT=6;
    private static final int PIE_CONTROL=-100;
    private static final int ACCENT=0xFFFFAE42;
    /** Tap reloads, flick cycles, hold opens BioShock's original weapon/plasmid picker. */
    private static final long PICK_HOLD_MS=300, SCORES_TAP_MS=300, HINT_MS=5500, RESET_CONFIRM_MS=3000;
    private static final int HINT_LOOK=1, HINT_WEAPON=128, HINT_MORE=4, HINT_ACTIONS=256, HINT_VITALS=1024, HINT_AMMO=2048;
    /** The original health/EVE gauge is a control: tap uses a first aid kit, hold opens More. */
    private static final long VITALS_HOLD_MS=320;
    /** Script vehicle flags: Jump deploys, deployed, a gunner seat passes Jump on, look steering. */
    private static final int VEHICLE_DEPLOY=1, VEHICLE_DEPLOYED=2, VEHICLE_SEAT_JUMP=4, VEHICLE_LOOK_STEER=8;
    /** Script weapon bar word: slot bits 0-9, the selected slot in 10-13, shown in 14, HUD minimap in 15. */
    private static final int BAR_SHOWN=1<<14,HUD_MAP=1<<15;
    private static final int PICKER_ACTIVE=1<<21;
    /** BioShock hands (bioshock_touch.c): held item, weapon owned, plasmid owned, Wrench held, valid. */
    private static final int HANDS_HOLDING=1<<16,HANDS_WEAPON=1<<17,HANDS_PLASMID=1<<18,HANDS_MELEE=1<<19,
        HANDS_VALID=1<<20,HANDS_MASK=31<<16;
    private static final int IOS_WEAPON_SHIFT=23,IOS_ABILITY_SHIFT=16,IOS_ACTIVE_PLASMID=1<<28,IOS_CROUCHED=1<<29;
    private static final String[] IOS_WEAPON_SPRITES=iosEquipmentSprites("weapon_",16);
    private static final String[] IOS_ABILITY_SPRITES=iosEquipmentSprites("ability_",32);
    private static final int TOOL_DONE=0, TOOL_RESET=1, TOOL_BUTTONS=2, TOOL_CANCEL=3;
    private static final int CONTEXT_USE=1<<4,CONTEXT_HACK=1<<5,CONTEXT_RESCUE=1<<6,
        CONTEXT_HARVEST=1<<7,CONTEXT_REROLL=1<<8,CONTEXT_VALID=1<<9,CONTEXT_AIMING=1<<10,CONTEXT_CAN_AIM=1<<11,
        CONTEXT_CINEMATIC=1<<12,CONTEXT_VITALS=1<<13,CONTEXT_MOVEMENT_ONLY=1<<14,CONTEXT_FIRING=1<<15;
    private static final long CENTER_HOLD_MS=400;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Rect safe=new Rect(),cutoutSafe=new Rect();
    private final float[] controlX=new float[TouchLayout.COUNT],controlY=new float[TouchLayout.COUNT],
        controlR=new float[TouchLayout.COUNT],customX=new float[TouchLayout.COUNT],customY=new float[TouchLayout.COUNT];
    private final boolean[] shown=new boolean[TouchLayout.COUNT];
    private final int[] customSize=new int[TouchLayout.COUNT],customDisplay=new int[TouchLayout.COUNT];
    private final ArrayList<float[]> occupied=new ArrayList<>(),reserved=new ArrayList<>();
    /** Utility controls under a finger, acting on release over the button, and their pointer ids. */
    private final int[] utility=new int[10],utilityIds=new int[10];
    private final TouchInput input;
    private final UiTouchInput uiInput;
    private final SensorManager sensors;
    private final Sensor gyro;
    private Runnable settingsAction;
    private java.util.function.BooleanSupplier pauseAction;
    private float dp,scale=1,layoutScale=1,opacity=.65f,sensitivity=.22f,gyroSensitivity=1,uiScale=1;
    private boolean playing,editing,leftHanded,invertY,pausedByControl,focused,gyroListening,gyroActive;
    /** Player options: a separate More button (off by default), the health gauge as a control (on). */
    private boolean showMore,vitalsControl=true,touchEnabled=true,showUse=true,touchPrompts=true;
    /** 0 hides the overlay, 1 port layout, 2 iPhone, 3 iPad. touchEnabled false hides buttons and keeps touch input. */
    private int touchScheme=1;
    private float iosUiScale=1f,hapticStrength=1f;
    private boolean hapticOn=true,touchAimAssist;
    private float[] iosRects=new float[0];
    private final int[] iosIds={-1,-1,-1,-1,-1,-1,-1,-1},iosHits={-1,-1,-1,-1,-1,-1,-1,-1};
    private final boolean[] iosSwitchOnRelease=new boolean[8];
    private int iosNextId=-1,iosSelectorRow,iosSelectorItems,iosSelectorSelected=-1;
    private boolean iosNextHeld,iosSelectorOpen;
    private float[] iosSelectorRects=new float[12];
    private final Runnable iosNextHold=()->{
        if(iosNextId<0||!iosScheme()||!this.playing||this.editing||this.cursorContext!=GameActivity.nativeTouchContext())return;
        iosNextHeld=true;
        iosSelectorRow=(this.vehicleFlags&IOS_ACTIVE_PLASMID)!=0?1:0;
        iosSelectorItems=(int)(GameActivity.nativeTouchIosChoices() >>> (32*iosSelectorRow));
        iosSelectorOpen=iosSelectorItems!=0;
        iosSelectorRects=IosTouchLayout.selector(this.iosRects);
        invalidate();
    };
    private final HashMap<String,Bitmap> iosSprites=new HashMap<>();
    private final RectF iosSpriteRect=new RectF();
    private final Paint iosPaint=new Paint(Paint.FILTER_BITMAP_FLAG);
    private boolean rumbleActive;
    private int rumbleAmplitude;
    private long rumbleUntil;
    private int menuId=-1,profile,profileOverride,gyroMode,sensorRotation=-1;
    private boolean authoritative,layoutDirty=true;
    private boolean imeRequested,imeWasVisible;
    private long imeSession;
    private int accessibilityFocus=-1,hovered=-1;
    private long sensorTime,resetDeadline;
    private long cursorContext;
    private boolean layoutPending;
    private boolean settingsPending;
    // Layout editor: the dragged control and the toolbar.
    private int editId=-1,editControl=-1,pinchId=-1,toolId=-1,toolPressed=-1;
    private float editDX,editDY,pinchSpan,pinchSize;
    private final float[][] tools=new float[4][];
    private AlertDialog editOptions;
    private final Bitmap[] editorButtons=new Bitmap[3];
    private final Rect editorTextBounds=new Rect();
    private Typeface editorFont;
    private boolean editorStyleLoaded;
    // Weapon button: tap next, swipe along the weapon bar, hold the original quick pick.
    private int weaponId=-1;
    private boolean weaponPicker;
    private float weaponX,weaponY,weaponLastX,weaponLastY;
    private int weaponSwipe,pickerRow=-1,pickerStep;
    private boolean weaponSwiped;
    private int ammoId=-1;
    private float ammoX,ammoY;
    private long ammoTime;
    private int centerId=-1;
    private float centerX,centerY;
    private long centerTime,centerContext;
    private boolean centerHeld;
    private final Runnable centerHold=new Runnable() {
        @Override public void run() {
            if(centerId<0||!playing||editing||centerContext!=cursorContext||
                cursorContext!=GameActivity.nativeTouchContext()||(vehicleFlags&(CONTEXT_RESCUE|CONTEXT_HARVEST))!=0)return;
            centerHeld=true;
            if((vehicleFlags&CONTEXT_HACK)!=0) {
                input.pulse(TouchInput.HACK);
                haptic(HapticFeedbackConstants.LONG_PRESS);
            }
        }
    };
    private final Runnable ammoHold=new Runnable() {
        @Override public void run() {
            if(ammoId<0||!playing||editing)return;
            input.pulse(TouchInput.AMMO_DOWN);
            swallowed=ammoId;ammoId=-1;
            haptic(HapticFeedbackConstants.LONG_PRESS);
        }
    };
    // Weapon bar swipe: starts on the selected weapon, one card per step, switches on release.
    private int scrubId=-1,scrubStart,scrubIndex,weaponSlots,vehicleFlags;
    private int[] scrubSlots=new int[0];
    private float scrubX,scrubStep;
    private boolean scrubButton;
    private Vibrator vibrator;
    private final Runnable rumblePoll=new Runnable() {
        @Override public void run() {
            if(!hapticOn||!(hapticStrength>0f)){rumbleStop();return;}
            if(focused&&vibrator!=null){
                int motors=GameActivity.nativeRumble();
                int peak=Math.max((motors>>>16)&0xFFFF,motors&0xFFFF);
                if(peak>0&&vibrator.hasVibrator()){
                    int amplitude=Math.max(1,Math.min(255,Math.round(255f*(peak/65535f)*hapticStrength)));
                    long now=SystemClock.uptimeMillis();
                    if(!rumbleActive||amplitude!=rumbleAmplitude||now+200>=rumbleUntil){
                        try{
                            vibrator.vibrate(VibrationEffect.createOneShot(1000,amplitude));
                            rumbleActive=true;rumbleAmplitude=amplitude;rumbleUntil=now+1000;
                        }catch(RuntimeException ignored){}
                    }
                }else if(rumbleActive)rumbleStop();
            }
            postDelayed(this,40);
        }
    };
    // Scores: held shows while held, a tap keeps them shown until the next tap.
    private int scoresId=-1;
    private long scoresTime;
    // Pie: More actions or vehicle seats; drag and release, or tap to keep it open.
    private boolean pieOpen,pieDragged,pieSticky;
    private int pieId=-1,pieKind,pieSelected=-1,pieSource=-1,swallowed=-1;
    private int[] pieItems=new int[0];
    private float pieX,pieY,pieR,pieStartX,pieStartY;
    private String pieHint;
    // One-time hints.
    private int hintsSeen;
    private String hintText;
    private long hintUntil;
    private final Runnable contextPoll=new Runnable() {
        @Override public void run() { syncContext();applyPendingSettings();postDelayed(this,100); }
    };
    private final Runnable uiPoll=new Runnable() {
        @Override public void run() {
            syncContext();uiInput.poll();
            // Also while a finger points at a list: held beyond its edge, the selection keeps stepping.
            if(uiInput.polling())postOnAnimation(this);
        }
    };
    private final Runnable weaponHold=new Runnable() {
        @Override public void run() { if(weaponId>=0&&!weaponPicker)startPicker(); }
    };
    private final Runnable hintCheck=new Runnable() {
        @Override public void run() { offerHint(); }
    };
    // Health/EVE gauge gestures; the original HUD itself is their picture.
    private int vitalsId=-1;
    private float vitalsX,vitalsY;
    private final Runnable vitalsHold=new Runnable() {
        @Override public void run() {
            if(vitalsId<0||!playing||editing)return;
            int id=vitalsId;vitalsId=-1;
            haptic(HapticFeedbackConstants.LONG_PRESS);
            openPie(id,vitalsX,vitalsY,PIE_ACTIONS,-1);
            markHint(HINT_VITALS);
        }
    };

    GameControls(Context context) {
        super(context);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setHapticFeedbackEnabled(true);
        setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        for(int i=0;i<utility.length;++i)utility[i]=-1;
        input=new TouchInput(new TouchInput.Sink() {
            @Override public void submit(float x,float y,float dx,float dy,int held) {
                if(!GameActivity.nativeTouch(x,y,dx,dy,held,cursorContext)) { input.cancel();syncContext(); }
                updateGyro();
            }
            @Override public void gyroLook(float x,float y,float dx,float dy,int held) {
                if(!GameActivity.nativeTouchGyro(x,y,dx,dy,held,cursorContext)) { input.cancel();syncContext(); }
                updateGyro();
            }
            @Override public boolean stick(float x,float y) { return GameActivity.nativeTouchStick(x,y,cursorContext); }
            @Override public void reset() { GameActivity.nativeTouchReset(); }
        });
        uiInput=new UiTouchInput(new UiTouchInput.Sink() {
            @Override public boolean begin(float x,float y) {
                return pointer(x,y)&&GameActivity.nativeUiBegin(cursorContext);
            }
            @Override public int control() { return GameActivity.nativeUiControl(cursorContext); }
            @Override public boolean pointer(float x,float y) {
                return GameActivity.nativePointer(x/getWidth(),y/getHeight(),false);
            }
            @Override public boolean button(boolean down) {
                return GameActivity.nativeTouch(0,0,0,0,down?TouchInput.FIRE:0,cursorContext);
            }
            @Override public boolean scroll(int rows) { return GameActivity.nativeUiScroll(rows,cursorContext); }
            @Override public void reset() { GameActivity.nativeTouchReset(); }
        });
        sensors=(SensorManager)context.getSystemService(Context.SENSOR_SERVICE);
        vibrator=context.getSystemService(Vibrator.class);
        gyro=sensors==null?null:sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        reloadSettings();
    }
    private GameActivity activity() { return (GameActivity)getContext(); }
    private PortPreferences preferences() { return activity().portSettings(); }
    private String text(String english,String russian) {
        PortPreferences p=preferences();
        return p==null||"RUS".equals(p.getString("language","RUS"))?russian:english;
    }
    void setSettingsAction(Runnable action) { settingsAction=action; }
    void setPauseAction(java.util.function.BooleanSupplier action) { pauseAction=action; }
    void reloadSettings() {
        release();settingsPending=false;
        dp=getResources().getDisplayMetrics().density;
        PortPreferences p=preferences();
        if(p!=null) {
            sensitivity=clamp(p.getFloat("touchSensitivity",.22f),.04f,3);
            scale=clamp(p.getFloat("touchScale",1),.65f,1.6f);
            opacity=clamp(p.getFloat("touchOpacity",.65f),.2f,1);
            uiScale=TouchLayout.clampUiScale(p.getFloat("uiScale",TouchLayout.defaultUiScale(getResources().getConfiguration().smallestScreenWidthDp)));
            leftHanded=p.getBoolean("touchLeftHanded",false);
            showMore=p.getBoolean("touchMore",false);
            vitalsControl=p.getBoolean("touchVitals",true);
            boolean enabledPref=p.getBoolean("touchEnabled",true);
            int stored=p.getInt("touchScheme",enabledPref?1:0);
            if(stored<0||stored>3)stored=enabledPref?1:0;
            if(enabledPref&&stored==0)stored=1;
            touchEnabled=enabledPref;
            touchScheme=stored;
            iosUiScale=clamp(p.getFloat("iosUiScale",1f),.5f,2f);
            hapticOn=p.getBoolean("haptic",true);
            hapticStrength=clamp(p.getFloat("hapticStrength",1f),0f,1f);
            touchAimAssist=p.getBoolean("touchAimAssist",false);
            showUse=p.getBoolean("touchUse",true);
            touchPrompts=p.getBoolean("touchPrompts",true);
            invertY=p.getBoolean("touchInvertY",false);
            gyroMode=Math.max(0,Math.min(4,p.getInt("touchGyro",0)));
            profileOverride=Math.max(0,Math.min(4,p.getInt("touchVehicleProfile",0)));
            gyroSensitivity=clamp(p.getFloat("touchGyroSensitivity",1),.1f,5);
            hintsSeen=p.getInt("touchHints",0);
        }
        loadCustomLayout(p);
        GameActivity.nativeTouchOptions(touchEnabled,iosScheme()?4:touchPrompts?1:0);
        GameActivity.nativeTouchAim(touchAimAssist);
        startRumble();
        input.configure(60*scale*dp,cameraCounts(sensitivity)/dp,
            p==null?1:clamp(p.getFloat("touchAltSensitivity",1),.1f,3),invertY);
        input.density(dp);
        input.acceleration(p==null||p.getBoolean("touchAcceleration",true));
        uiInput.configure(ViewConfiguration.get(getContext()).getScaledTouchSlop(),24*dp*uiScale);
        int observed=GameActivity.nativeTouchVehicle();
        authoritative=observed>=0;
        profile=profileOverride>0?profileOverride:Math.max(0,observed);
        readDetails();
        setContentDescription(text("Touch controls. Drag on the right to look; tap screen center to use, hold it to hack.",
            "Сенсорное управление. Ведите пальцем справа для обзора; касание центра — действие, удержание — взлом."));
        layoutDirty=true;updateGyro(); invalidate();
    }
    void requestSettingsReload() { settingsPending=true;applyPendingSettings(); }
    private void applyPendingSettings() {
        // A native UI click can still await mouse-poll acknowledgement after Android UP.
        if(settingsPending&&!editing&&(cursorContext&3)!=3&&menuId<0&&!input.hasContacts()&&!imeRequested)reloadSettings();
    }
    private static float clamp(float value,float minimum,float maximum) {
        return Math.max(minimum,Math.min(maximum,value));
    }
    void release() {
        hideKeyboard();releaseInput();
    }
    private void releaseInput() {
        removeCallbacks(uiPoll);removeCallbacks(weaponHold);removeCallbacks(vitalsHold);removeCallbacks(ammoHold);removeCallbacks(centerHold);vitalsId=ammoId=centerId=-1;centerHeld=false;
        if(weaponPicker&&(weaponSlots&PICKER_ACTIVE)!=0)GameActivity.nativeTouchWeapon(7,15,cursorContext);
        if(uiInput!=null)uiInput.cancel();
        if(input!=null) input.cancel();
        menuId=editId=pinchId=toolId=toolPressed=-1;
        weaponId=scoresId=swallowed=scrubId=-1;weaponPicker=false;
        for(int i=0;i<utility.length;++i)utility[i]=-1;
        pieOpen=false;pieId=-1;pieSelected=-1;
        sensorTime=0; gyroActive=false; stopGyro(); clearIosPointers(); invalidate();
    }
    void hideKeyboard() {
        if(!imeRequested&&!imeWasVisible)return;
        ++imeSession;
        imeRequested=imeWasVisible=false;
        InputMethodManager manager=(InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if(manager!=null) {
            manager.restartInput(this);
            manager.hideSoftInputFromWindow(getWindowToken(),0);
        }
    }
    private boolean textUi(long context) {
        return authoritative&&(context&3)==3&&context==cursorContext&&context==GameActivity.nativeTouchContext();
    }
    private void showKeyboard() {
        syncContext();if(!textUi(cursorContext))return;
        if(imeRequested){hideKeyboard();return;}
        releaseInput();++imeSession;imeRequested=true;
        if(!requestFocus()){imeRequested=false;return;}
        final long context=cursorContext;
        final InputMethodManager manager=(InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if(manager==null){imeRequested=false;return;}
        manager.restartInput(this);
        post(new Runnable(){@Override public void run(){if(imeRequested&&textUi(context))manager.showSoftInput(GameControls.this,InputMethodManager.SHOW_IMPLICIT);}});
    }
    @Override public boolean onCheckIsTextEditor() { return imeRequested&&authoritative&&(cursorContext&3)==3; }
    @Override public InputConnection onCreateInputConnection(EditorInfo info) {
        if(!onCheckIsTextEditor())return null;
        info.inputType=InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        info.imeOptions=EditorInfo.IME_ACTION_DONE|EditorInfo.IME_FLAG_NO_EXTRACT_UI|
            EditorInfo.IME_FLAG_NO_FULLSCREEN|EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING;
        info.initialSelStart=info.initialSelEnd=0;
        final long context=cursorContext;
        final long session=++imeSession;
        final TouchText text=new TouchText(new TouchText.Sink(){
            @Override public boolean commit(String value){return GameActivity.nativeText(value,context);}
            @Override public boolean delete(int before){return GameActivity.nativeDelete(before,context);}
        });
        return new BaseInputConnection(this,false) {
            private boolean ready(){if(imeRequested&&session==imeSession&&textUi(context))return true;if(session==imeSession)hideKeyboard();return false;}
            @Override public boolean commitText(CharSequence value,int position){return ready()&&position==1&&text.commit(value);}
            @Override public boolean setComposingText(CharSequence value,int position){return ready()&&position==1&&text.compose(value);}
            @Override public boolean finishComposingText(){return ready()&&text.finish();}
            @Override public boolean deleteSurroundingText(int before,int after){return ready()&&text.delete(before,after);}
            @Override public boolean deleteSurroundingTextInCodePoints(int before,int after){return ready()&&text.deleteCodePoints(before,after);}
            @Override public CharSequence getTextBeforeCursor(int length,int flags){return ready()?text.before(length):"";}
            @Override public CharSequence getTextAfterCursor(int length,int flags){return "";}
            @Override public CharSequence getSelectedText(int flags){return null;}
            @Override public int getCursorCapsMode(int modes){return 0;}
            @Override public boolean performEditorAction(int action) {
                if(!ready())return false;
                boolean result=text.finish();hideKeyboard();return result;
            }
            @Override public boolean sendKeyEvent(KeyEvent event) {
                if(!ready())return false;
                int code=event.getKeyCode();
                if(code==KeyEvent.KEYCODE_DEL)return event.getAction()!=KeyEvent.ACTION_DOWN||text.backspace();
                if(code==KeyEvent.KEYCODE_ENTER||code==KeyEvent.KEYCODE_BACK) {
                    if(event.getAction()==KeyEvent.ACTION_DOWN)return performEditorAction(EditorInfo.IME_ACTION_DONE);
                    return true;
                }
                int unicode=event.getUnicodeChar();
                return event.getAction()==KeyEvent.ACTION_DOWN&&unicode>0&&text.commit(new String(Character.toChars(unicode)));
            }
        };
    }
    @Override public boolean onKeyPreIme(int code,KeyEvent event) {
        if(code==KeyEvent.KEYCODE_BACK&&imeRequested) {
            if(event.getAction()==KeyEvent.ACTION_UP)hideKeyboard();
            return true;
        }
        return super.onKeyPreIme(code,event);
    }
    private void mode(boolean next) {
        if(authoritative||(cursorContext&3)==1)return;
        release(); playing=next;layoutDirty=true;
        if(next&&pausedByControl) { input.pulse(TouchInput.PAUSE); pausedByControl=false; }
        if(next&&layoutPending&&(cursorContext&2)==0)editLayout();
        updateGyro(); invalidate();
    }
    void editLayout() {
        release(); editing=true;layoutPending=false;layoutDirty=true;resetDeadline=0;
        editControl=TouchLayout.FIRE;
        invalidate();
        announceForAccessibility(text("Drag a control. Tap Done to save.","Перетащите кнопки. Нажмите «Готово» для сохранения."));
    }
    void requestLayoutEditor() { layoutPending=true;syncContext(); }
    boolean canOfferInputPrompt() { return !editing&&!pieOpen&&!imeRequested&&menuId<0&&!input.hasContacts()&&(cursorContext&3)!=1; }
    boolean handleBack() {
        if(imeRequested) { hideKeyboard();return true; }
        if(editing) { finishEditing(false);return true; }
        if(pieOpen) { closePie();return true; }
        return false;
    }
    private void loadCustomLayout(PortPreferences p) {
        Arrays.fill(customX,Float.NaN);Arrays.fill(customY,Float.NaN);
        Arrays.fill(customSize,0);Arrays.fill(customDisplay,-1);
        if(p==null)return;
        for(int i=0;i<TouchLayout.COUNT;++i) {
            String name=TouchLayout.NAMES[i];
            float x=p.getFloat("touchX"+name,Float.NaN),y=p.getFloat("touchY"+name,Float.NaN);
            if(x>=0&&x<=1&&y>=0&&y<=1){customX[i]=x;customY[i]=y;}
            int size=p.getInt("touchSize"+name,0),mode=p.getInt("touchDisplay"+name,-1);
            if(size>=TouchLayout.MIN_DP&&size<=TouchLayout.MAX_DP)customSize[i]=size;
            if(TouchLayout.validDisplay(mode))customDisplay[i]=i==TouchLayout.MORE?mode&TouchLayout.ON_SCREEN:mode;
        }
    }
    private void finishEditing(boolean save) {
        release(); editing=false;layoutDirty=true;
        if(editOptions!=null)editOptions.dismiss();
        PortPreferences p=preferences();
        if(save&&p!=null) {
            for(int i=0;i<TouchLayout.COUNT;++i) {
                boolean custom=!Float.isNaN(customX[i]);
                p.put("touchX"+TouchLayout.NAMES[i],custom?Float.toString(customX[i]):"");
                p.put("touchY"+TouchLayout.NAMES[i],custom?Float.toString(customY[i]):"");
                p.put("touchSize"+TouchLayout.NAMES[i],customSize[i]>0?Integer.toString(customSize[i]):"");
                p.put("touchDisplay"+TouchLayout.NAMES[i],customDisplay[i]>=0?Integer.toString(customDisplay[i]):"");
            }
            activity().savePortSettings(null);
        } else loadCustomLayout(p);
        applyPendingSettings();
        updateGyro(); invalidate();
    }
    private void resetLayout() {
        long now=SystemClock.uptimeMillis();
        if(now>resetDeadline) {
            resetDeadline=now+RESET_CONFIRM_MS;postInvalidateDelayed(RESET_CONFIRM_MS);
            announceForAccessibility(text("Tap Reset again to confirm.","Нажмите «Сброс» ещё раз для подтверждения."));
            invalidate(); return;
        }
        loadCustomLayout(null);
        resetDeadline=0;layoutDirty=true; invalidate();
    }
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        releaseInput();
        boolean shown;
        if(Build.VERSION.SDK_INT>=30) {
            android.graphics.Insets bars=insets.getInsets(WindowInsets.Type.systemBars());
            safe.set(bars.left,bars.top,bars.right,bars.bottom);
            shown=insets.isVisible(WindowInsets.Type.ime());
        } else {
            int bottom=insets.getSystemWindowInsetBottom(),stableBottom=insets.getStableInsetBottom();
            shown=bottom>stableBottom;
            safe.set(insets.getSystemWindowInsetLeft(),insets.getSystemWindowInsetTop(),
                insets.getSystemWindowInsetRight(),Math.min(bottom,stableBottom));
        }
        if(shown)imeWasVisible=true;
        else if(imeWasVisible)hideKeyboard();
        cutoutSafe.set(0,0,0,0);
        if(Build.VERSION.SDK_INT>=28) {
            DisplayCutout cutout=insets.getDisplayCutout();
            if(cutout!=null) {
                cutoutSafe.set(cutout.getSafeInsetLeft(),cutout.getSafeInsetTop(),cutout.getSafeInsetRight(),cutout.getSafeInsetBottom());
                safe.left=Math.max(safe.left,cutout.getSafeInsetLeft());
                safe.top=Math.max(safe.top,cutout.getSafeInsetTop());
                safe.right=Math.max(safe.right,cutout.getSafeInsetRight());
                safe.bottom=Math.max(safe.bottom,cutout.getSafeInsetBottom());
            }
        }
        layoutDirty=true;invalidate(); return insets;
    }
    @Override protected void onSizeChanged(int width,int height,int oldWidth,int oldHeight) {
        release(); layoutDirty=true; super.onSizeChanged(width,height,oldWidth,oldHeight);
    }
    @Override public void onWindowFocusChanged(boolean value) {
        super.onWindowFocusChanged(value); focused=value;
        if(!value) release(); else updateGyro();
    }
    @Override protected void onDetachedFromWindow() {
        if(editing)finishEditing(false);
        removeCallbacks(contextPoll);removeCallbacks(uiPoll);removeCallbacks(hintCheck);removeCallbacks(rumblePoll);
        rumbleStop();release();super.onDetachedFromWindow();
    }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow();post(contextPoll);startRumble(); }
    private void syncContext() {
        long context=GameActivity.nativeTouchContext();
        int observed=GameActivity.nativeTouchVehicle();
        authoritative=observed>=0;
        int nextProfile=profileOverride>0?profileOverride:Math.max(0,observed);
        if(profile!=nextProfile){profile=nextProfile;layoutDirty=true;invalidate();}
        readDetails();
        if(context!=cursorContext) {
            int pickerId=weaponId;
            boolean keepPicker=weaponPicker&&(weaponSlots&PICKER_ACTIVE)!=0&&(context&3)==3;
            cursorContext=context;
            if(keepPicker)weaponPicker=false;
            release();playing=(context&3)==2;pausedByControl=false;
            if(keepPicker){weaponId=pickerId;weaponPicker=true;pickerRow=-1;weaponStick(weaponLastX,weaponLastY);}
            if(editing&&!playing)finishEditing(false);
            layoutDirty=true;updateGyro();invalidate();
            removeCallbacks(hintCheck);
            if(playing&&!editing)postDelayed(hintCheck,1500);
        }
        if(layoutPending&&(context&3)==2&&(vehicleFlags&CONTEXT_CINEMATIC)==0)editLayout();
    }
    /** Weapon bar and vehicle details, which change without a new input context. */
    private void readDetails() {
        long details=GameActivity.nativeTouchDetails();
        int changedHands=weaponSlots^(int)details;
        if((changedHands&(HUD_MAP|HANDS_MASK))!=0){layoutDirty=true;invalidate();}
        if(iosScheme()&&(changedHands&(15<<IOS_WEAPON_SHIFT))!=0)invalidate();
        weaponSlots=(int)details;
        int flags=(int)(details>>>32);
        if((((vehicleFlags^flags)&(CONTEXT_CINEMATIC|CONTEXT_VITALS))!=0||(changedHands&HANDS_PLASMID)!=0)&&playing) {
            removeCallbacks(hintCheck);
            postDelayed(hintCheck,1500);
        }
        if(flags!=vehicleFlags){
            int restricted=CONTEXT_CINEMATIC|CONTEXT_MOVEMENT_ONLY;
            boolean enteringScene=(~vehicleFlags&flags&restricted)!=0;
            vehicleFlags=flags;
            if(enteringScene&&playing) {
                if(editing&&(flags&CONTEXT_CINEMATIC)!=0)finishEditing(false);
                else if(!editing)releaseInput();
            }
            layoutDirty=true;updateGyro();invalidate();
        }
    }
    private boolean deploys() { return !editing&&profile!=0&&(vehicleFlags&VEHICLE_DEPLOY)!=0; }
    private float width() { return Math.max(1,getWidth()-safe.left-safe.right); }
    private float height() { return Math.max(1,getHeight()-safe.top-safe.bottom); }

    /** BioShock actions: obsolete UT3 controls keep their saved ids but stay hidden. */
    private int action(int control) {
        switch(control) {
        case TouchLayout.FIRE:case TouchLayout.FIRE2:return TouchInput.FIRE;
        case TouchLayout.ALT:return TouchInput.ALT;
        case TouchLayout.JUMP:return TouchInput.JUMP;
        case TouchLayout.DUCK:return TouchInput.DUCK;
        case TouchLayout.USE:return TouchInput.USE;
        case TouchLayout.WEAPON:return WEAPON;
        case TouchLayout.MORE:return MORE;
        case TouchLayout.PAUSE:return TouchInput.PAUSE;
        case TouchLayout.SETTINGS:return SETTINGS;
        case TouchLayout.KEYBOARD:return KEYBOARD;
        case TouchLayout.MODE:return MODE;
        case TouchLayout.AIM:return TouchInput.BEST;
        case TouchLayout.RESCUE:return TouchInput.RESCUE;
        case TouchLayout.HARVEST:return TouchInput.TAUNT;
        case TouchLayout.REROLL:return TouchInput.RELOAD;
        case TouchLayout.HACK:return TouchInput.HACK;
        default:return 0;
        }
    }
    private boolean visible(int control) {
        return baseVisible(control)&&handsAllow(control);
    }
    /**
     * Only what the hands can do: no Fire with empty hands, no weapon/plasmid
     * switch without both, no reload/EVE for the Wrench alone. Without the
     * game's answer every button stays.
     */
    private boolean handsAllow(int control) {
        int hands=weaponSlots;
        if(!editing) {
            if(control==TouchLayout.HACK)return (vehicleFlags&CONTEXT_HACK)!=0;
            if(control==TouchLayout.RESCUE)return (vehicleFlags&CONTEXT_RESCUE)!=0;
            if(control==TouchLayout.HARVEST)return (vehicleFlags&CONTEXT_HARVEST)!=0;
            if(control==TouchLayout.REROLL)return (vehicleFlags&CONTEXT_REROLL)!=0;
            if(control==TouchLayout.AIM)return (vehicleFlags&CONTEXT_CAN_AIM)!=0;
        }
        if(editing||(hands&HANDS_VALID)==0)return true;
        boolean holding=(hands&HANDS_HOLDING)!=0,plasmid=(hands&HANDS_PLASMID)!=0;
        if(control==TouchLayout.FIRE||control==TouchLayout.FIRE2)return holding;
        if(control==TouchLayout.ALT)return (hands&HANDS_WEAPON)!=0&&plasmid;
        if(control==TouchLayout.WEAPON)return holding&&(plasmid||(hands&HANDS_MELEE)==0);
        return true;
    }
    private int displayMode(int control) {
        if(customDisplay[control]>=0)return customDisplay[control];
        if(control==TouchLayout.FIRE2||(control==TouchLayout.MORE&&!showMore)||(control==TouchLayout.USE&&!showUse))return 0;
        return TouchLayout.ON_SCREEN|(TouchLayout.defaultRadial(control)?TouchLayout.IN_RADIAL:0);
    }
    private boolean editable(int control) {
        return control!=TouchLayout.KEYBOARD&&control!=TouchLayout.SETTINGS&&control!=TouchLayout.MODE&&action(control)!=0;
    }
    private boolean gameplayAllows(int control) {
        if(!playing||!touchEnabled||(cursorContext&3)==1)return false;
        if((vehicleFlags&CONTEXT_CINEMATIC)!=0)return control==TouchLayout.PAUSE;
        return (vehicleFlags&CONTEXT_MOVEMENT_ONLY)==0||control!=TouchLayout.USE;
    }
    private boolean baseVisible(int control) {
        if(editing)return editable(control)&&control!=TouchLayout.PAUSE;
        if(control==TouchLayout.SETTINGS||control==TouchLayout.MODE)return !authoritative&&(cursorContext&3)!=1;
        return editable(control)&&gameplayAllows(control)&&(displayMode(control)&TouchLayout.ON_SCREEN)!=0;
    }
    private int icon(int control) {
        switch(control) {
        case TouchLayout.FIRE:case TouchLayout.FIRE2:return TouchIcons.FIRE;
        case TouchLayout.ALT:return TouchIcons.SWITCH;
        case TouchLayout.JUMP:return TouchIcons.JUMP;
        case TouchLayout.DUCK:return TouchIcons.DUCK;
        case TouchLayout.USE:case TouchLayout.RESCUE:return TouchIcons.USE;
        case TouchLayout.AIM:return TouchIcons.ALT;
        case TouchLayout.HARVEST:return TouchIcons.HEAL;
        case TouchLayout.HACK:return TouchIcons.HACK;
        case TouchLayout.REROLL:return TouchIcons.RESET;
        case TouchLayout.WEAPON:return TouchIcons.RESET;
        case TouchLayout.BOARD:return TouchIcons.BOARD;
        case TouchLayout.MORE:return TouchIcons.MORE;
        case TouchLayout.PAUSE:return TouchIcons.PAUSE;
        case TouchLayout.SCORES:return TouchIcons.SCORES;
        case TouchLayout.SETTINGS:return TouchIcons.SETTINGS;
        case TouchLayout.KEYBOARD:return TouchIcons.KEYBOARD;
        default:return playing?TouchIcons.MENU:TouchIcons.PLAY;
        }
    }
    private String label(int control) {
        switch(control) {
        case TouchLayout.FIRE:return text("Fire","Огонь");
        case TouchLayout.FIRE2:return text("Fire 2","Огонь 2");
        case TouchLayout.ALT:return text("Weapon / plasmid","Оружие / плазмид");
        case TouchLayout.JUMP:return text("Jump","Прыжок");
        case TouchLayout.DUCK:return text("Crouch","Присесть");
        case TouchLayout.USE:return text("Use","Использовать");
        case TouchLayout.AIM:return text("Aim","Прицел");
        case TouchLayout.RESCUE:return text("Rescue","Спасти");
        case TouchLayout.HARVEST:return text("Harvest ADAM","Собрать ADAM");
        case TouchLayout.HACK:return text("Hack","Взлом");
        case TouchLayout.REROLL:return text("Search again","Обыскать ещё");
        case TouchLayout.WEAPON:return text("Reload / EVE / select","Зарядить / EVE / выбор");
        case TouchLayout.BOARD:return text("Ammo up (Q)","Патроны вверх (Q)");
        case TouchLayout.MORE:return text("More","Ещё");
        case TouchLayout.PAUSE:return text("Pause","Пауза");
        case TouchLayout.SCORES:return text("Quick save","Быстрое сохранение");
        case TouchLayout.SETTINGS:return text("Additional settings","Дополнительно");
        case TouchLayout.KEYBOARD:return text("Keyboard","Клавиатура");
        default:return playing?text("Menu","Меню"):text("Play","Игра");
        }
    }
    private String accessibilityLabel(int control) {
        switch(control) {
        case TouchLayout.WEAPON:return text("Tap to reload, swipe to cycle, hold to select a weapon or plasmid",
            "Касание — перезарядка, смахивание — смена, удержание — выбор оружия или плазмида");
        case TouchLayout.SCORES:return text("Quick save (F8)","Быстрое сохранение (F8)");
        case TouchLayout.MORE:return text("More actions","Ещё действия");
        default:return label(control);
        }
    }

    private float buttonSize(int control) {
        if(customSize[control]==0)return TouchLayout.size(control,dp,layoutScale);
        return Math.min(customSize[control]*dp,Math.max(TouchLayout.MIN_DP*dp,Math.min(width(),height())-8*dp));
    }
    private void buildLayout() {
        layoutDirty=false;occupied.clear();reserved.clear();
        layoutScale=TouchLayout.fitScale(scale,height()/dp);
        boolean gameplay=editing||playing;
        // The original HUD covers the whole surface; insets only bound the controls.
        if(gameplay)TouchLayout.reserveHud(0,0,getWidth(),getHeight(),uiScale,hudClearStage(),reserved);
        float left=safe.left,top=safe.top,right=getWidth()-safe.right,bottom=getHeight()-safe.bottom;
        // The combat cluster keeps its shape: one shared shift clears the HUD,
        // unless the player placed one of its members by hand.
        float shiftX=0,shiftY=0;
        if(gameplay) {
            boolean custom=false;ArrayList<float[]> squares=new ArrayList<>();
            for(int control:TouchLayout.CLUSTER) {
                if(!baseVisible(control))continue;
                if((displayMode(control)&TouchLayout.ON_SCREEN)==0)continue;
                custom|=!Float.isNaN(customX[control])||customSize[control]!=0;
                float size=buttonSize(control);
                float[] at=TouchLayout.anchor(control,left,top,right,bottom,dp,layoutScale,leftHanded);
                squares.add(new float[]{at[0]-size*.5f,at[1]-size*.5f,at[0]+size*.5f,at[1]+size*.5f});
            }
            float[] shift=custom?null:TouchLayout.clusterShift(squares,left,top,right,bottom,reserved,4*dp,4*dp,leftHanded);
            if(shift!=null){shiftX=shift[0];shiftY=shift[1];}
        }
        // Pause keeps the screen's top-left corner, past the cutout inset and the HUD reservation.
        shown[TouchLayout.PAUSE]=visible(TouchLayout.PAUSE);
        if(shown[TouchLayout.PAUSE]) {
            float[] r=TouchLayout.pauseCorner(dp);
            occupied.add(r);
            controlX[TouchLayout.PAUSE]=(r[0]+r[2])*.5f;controlY[TouchLayout.PAUSE]=(r[1]+r[3])*.5f;
            controlR[TouchLayout.PAUSE]=(r[2]-r[0])*.5f;
        }
        // Preview hidden buttons after placing the buttons that will appear during play.
        for(int pass=0;pass<(editing?4:2);++pass)for(int control:TouchLayout.ORDER) {
            if(control==TouchLayout.PAUSE)continue;
            boolean custom=gameplay&&!Float.isNaN(customX[control]);
            boolean ghost=editing&&(displayMode(control)&TouchLayout.ON_SCREEN)==0;
            if(ghost!=(pass>=2)||custom!=(pass%2==0))continue;
            shown[control]=visible(control);
            if(!shown[control])continue;
            float size=buttonSize(control);
            float[] at=TouchLayout.anchor(control,left,top,right,bottom,dp,layoutScale,leftHanded);
            if(custom) {
                float nx=leftHanded&&TouchLayout.mirrored(control)?1-customX[control]:customX[control];
                at[0]=left+nx*width();at[1]=top+customY[control]*height();
            } else if(TouchLayout.clustered(control)) { at[0]+=shiftX;at[1]+=shiftY; }
            // A player's own placement is kept as dropped; defaults avoid it and the HUD.
            float[] r=custom?TouchLayout.place(at[0],at[1],size,left,top,right,bottom,4*dp,0,null,null):
                TouchLayout.place(at[0],at[1],size,left,top,right,bottom,4*dp,4*dp,occupied,gameplay?reserved:null);
            if(r==null){shown[control]=false;continue;}
            occupied.add(r);
            controlX[control]=(r[0]+r[2])*.5f;controlY[control]=(r[1]+r[3])*.5f;controlR[control]=size*.5f;
        }
        if(editing)buildTools();
    }
    private void buildTools() {
        paint.setTextSize(14*dp);
        float gap=10*dp,h=40*dp,total=-gap;float[] widths=new float[tools.length];
        float maxWidth=(width()-(tools.length+1)*gap)/tools.length;
        for(int i=0;i<tools.length;++i) {
            widths[i]=Math.min(maxWidth,Math.max(96*dp,paint.measureText(toolLabel(i))+40*dp));
            total+=widths[i]+gap;
        }
        float x=safe.left+(width()-total)*.5f,y=safe.top+10*dp;
        for(int i=0;i<tools.length;++i){tools[i]=new float[]{x,y,x+widths[i],y+h};x+=widths[i]+gap;}
    }
    private int hit(float x,float y) {
        if(layoutDirty)buildLayout();
        int best=-1;float bestDistance=Float.POSITIVE_INFINITY;
        for(int c=0;c<TouchLayout.COUNT;++c) {
            if(!shown[c])continue;
            // A small slop helps fingertips; the nearest centre wins between neighbours.
            float distance=(float)Math.hypot(x-controlX[c],y-controlY[c]);
            if(distance<=controlR[c]+4*dp&&distance-controlR[c]<bestDistance){best=c;bestDistance=distance-controlR[c];}
        }
        return best;
    }
    private boolean inside(int control,float x,float y) {
        return control>=0&&shown[control]&&Math.hypot(x-controlX[control],y-controlY[control])<=controlR[control]+12*dp;
    }
    private boolean movementZone(float x,float y) {
        float nx=(x-safe.left)/width(),ny=(y-safe.top)/height();
        return (leftHanded?nx>.6f:nx<.4f)&&ny>.35f;
    }
    private void haptic(int type) {
        if(!hapticOn||!(hapticStrength>0f))return;
        if(hapticStrength>=1f){performHapticFeedback(type);return;}
        int amplitude=Math.max(1,Math.min(255,Math.round(255f*hapticStrength)));
        long ms=type==HapticFeedbackConstants.LONG_PRESS?40:type==HapticFeedbackConstants.CLOCK_TICK?12:18;
        try{
            if(vibrator!=null&&vibrator.hasVibrator()){
                vibrator.vibrate(VibrationEffect.createOneShot(ms,amplitude));
                return;
            }
        }catch(RuntimeException ignored){}
        performHapticFeedback(type);
    }

    @Override protected void onDraw(Canvas canvas) {
        if(layoutDirty)buildLayout();
        if(iosScheme()&&playing&&!editing){drawIos(canvas);return;}
        Typeface previousFont=paint.getTypeface();
        if(editing) {
            loadEditorStyle();
            if(editorFont!=null)paint.setTypeface(editorFont);
            canvas.drawColor(0x8C0B0D10);
            paint.setStyle(Paint.Style.FILL);
        }
        // The movement stick works without a picture.
        drawVitals(canvas);
        for(int c=0;c<TouchLayout.COUNT;++c)if(shown[c])drawControl(canvas,c);
        if(editing) {
            for(int c=0;c<TouchLayout.COUNT;++c)if(shown[c])drawEditorCaption(canvas,c);
            drawTools(canvas);
        }
        if(pieOpen)drawPie(canvas);
        else drawHint(canvas);
        paint.setTypeface(previousFont);
    }
    private boolean pressed(int control) {
        int a=action(control);
        if(a>0&&(input.held()&a)!=0)return true;
        for(int c:utility)if(c==control)return true;
        if(a==WEAPON)return weaponId>=0||(scrubId>=0&&scrubButton);
        if((a==MORE||a==SEATS)&&pieOpen&&pieSource==control)return true;
        return editing&&control==editControl;
    }
    private void drawControl(Canvas canvas,int c) {
        float x=controlX[c],y=controlY[c],r=controlR[c];
        if(!editing&&c>=TouchLayout.RESCUE)drawText(canvas,label(c),x,y+r+10*dp,11*dp,110*dp,0xFFFFFFFF);
        boolean active=pressed(c);
        float level=active||editing?Math.max(.82f,opacity):opacity;
        if(editing&&!active&&(displayMode(c)&TouchLayout.ON_SCREEN)==0)level*=.5f;
        paint.setStyle(Paint.Style.FILL);
        if(active) {
            paint.setColor(0x55000000|(ACCENT&0xFFFFFF));
            canvas.drawCircle(x,y,r+3*dp,paint);
        }
        paint.setColor(((int)(255*level)<<24)|TouchIcons.BUTTON_RGB);canvas.drawCircle(x,y,r,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth((active?2.4f:1.2f)*dp);
        paint.setColor(active?ACCENT:((int)(110*level)<<24)|0xFFFFFF);
        canvas.drawCircle(x,y,r-paint.getStrokeWidth()*.5f,paint);
        int ink=active?ACCENT:((int)(255*Math.min(1,.35f+level))<<24)|0xFFFFFF;
        TouchIcons.draw(canvas,paint,icon(c),x,y,Math.min(34*dp,r*1.05f),ink);
        if(c==TouchLayout.SCORES&&input.latched(TouchInput.SCORES)) {
            paint.setStyle(Paint.Style.FILL);paint.setColor(ACCENT);canvas.drawCircle(x+r*.7f,y-r*.7f,3.5f*dp,paint);
        }
    }
    private void drawEditorCaption(Canvas canvas,int c) {
        float r=controlR[c];
        drawEditorLabel(canvas,label(c),controlX[c],controlY[c]+r+11*dp,11*dp,Math.max(2*r+24*dp,80*dp),0xFFFFFFFF);
    }
    /** Pause clearance for the HUD's top-left group, in its 1280x720 stage units. */
    float hudClearStage() { return TouchLayout.hudClear(dp,getHeight()); }
    float hudControlClearStage() {
        if(!iosScheme())return hudClearStage();
        iosPlace();
        float edge=Math.max(iosRects[10],iosRects[14]);
        return (edge+6*dp)*720f/Math.max(1,getHeight())+
            (TouchLayout.VITALS_LEFT-TouchLayout.VITALS_STAGE[0])*1.5f;
    }
    /** The health/EVE gauge rectangle on screen while it is a control; null otherwise. */
    private float[] vitals() {
        if(!playing||editing||(vehicleFlags&(CONTEXT_VITALS|CONTEXT_CINEMATIC))!=CONTEXT_VITALS||
            (!vitalsControl&&!useGaugeForRadial()))return null;
        return TouchLayout.vitals(getWidth(),getHeight(),uiScale,hudControlClearStage());
    }
    private boolean useGaugeForRadial() {
        if((displayMode(TouchLayout.MORE)&TouchLayout.ON_SCREEN)!=0)return false;
        for(int mode:customDisplay)if(mode>=0&&(mode&TouchLayout.IN_RADIAL)!=0)return true;
        return false;
    }
    /** Pressed feedback only: the original gauge is the control's picture. */
    private void drawVitals(Canvas canvas) {
        float[] r=vitalsId>=0?vitals():null;
        if(r==null)return;
        float radius=(r[3]-r[1])*.25f;
        paint.setStyle(Paint.Style.FILL);paint.setColor(0x33000000|(ACCENT&0xFFFFFF));
        canvas.drawRoundRect(r[0],r[1],r[2],r[3],radius,radius,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2.4f*dp);paint.setColor(ACCENT);
        canvas.drawRoundRect(r[0],r[1],r[2],r[3],radius,radius,paint);
    }
    private boolean vitalsHit(float x,float y) {
        float[] r=vitals();
        return r!=null&&x>=r[0]&&x<=r[2]&&y>=r[1]&&y<=r[3];
    }
    private void drawText(Canvas canvas,String value,float x,float y,float size,float maxWidth,int color) {
        if(value==null||value.isEmpty()||!(maxWidth>0))return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(size);
        float measured=paint.measureText(value);
        if(measured>maxWidth)paint.setTextSize(size*maxWidth/measured);
        Paint.FontMetrics metrics=paint.getFontMetrics();
        canvas.drawText(value,x,y-(metrics.ascent+metrics.descent)*.5f,paint);
    }
    private void drawEditorLabel(Canvas canvas,String value,float x,float y,float size,float maxWidth,int color) {
        if(value==null||value.isEmpty())return;
        // Original font5's ё/Ё outlines are malformed; retain semantic/accessibility strings.
        if(editorFont!=null)value=value.replace('ё','е').replace('Ё','Е');
        float left=safe.left+4*dp,right=getWidth()-safe.right-4*dp;
        float top=safe.top+4*dp,bottom=getHeight()-safe.bottom-4*dp;
        maxWidth=Math.min(maxWidth,right-left);
        if(!(maxWidth>0)||!(bottom>top))return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setTextAlign(Paint.Align.LEFT);
        paint.setTextSize(size);
        float measured=paint.measureText(value);
        if(measured>maxWidth)paint.setTextSize(size*maxWidth/measured);
        paint.getTextBounds(value,0,value.length(),editorTextBounds);
        float fit=Math.min(1,Math.min(maxWidth/(editorTextBounds.width()+2f),(bottom-top)/(editorTextBounds.height()+2f)));
        if(fit<1){paint.setTextSize(paint.getTextSize()*fit);paint.getTextBounds(value,0,value.length(),editorTextBounds);}
        measured=paint.measureText(value);
        Paint.FontMetrics metrics=paint.getFontMetrics();
        float baseline=y-(metrics.ascent+metrics.descent)*.5f;
        x=clamp(x,left+measured*.5f-editorTextBounds.left,right+measured*.5f-editorTextBounds.right);
        baseline=clamp(baseline,top-editorTextBounds.top,bottom-editorTextBounds.bottom);
        paint.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(value,x,baseline,paint);
    }
    private void drawPill(Canvas canvas,float[] r,String value,boolean active) {
        paint.setStyle(Paint.Style.FILL);
        float h=r[3]-r[1];
        if(editorButtons[0]!=null) {
            int state=toolPressed>=0&&r==tools[toolPressed]?2:active?1:0;
            paint.setColor(0xFFFFFFFF);
            canvas.drawBitmap(editorButtons[state],null,new RectF(r[0],r[1],r[2],r[3]),paint);
        } else {
            paint.setColor(active?ACCENT:0xE6101317);
            canvas.drawRoundRect(r[0],r[1],r[2],r[3],h*.5f,h*.5f,paint);
        }
        drawText(canvas,value,(r[0]+r[2])*.5f,(r[1]+r[3])*.5f,Math.min(14*dp,h*.42f),r[2]-r[0]-16*dp,0xFFF9D59F);
    }
    private void loadEditorStyle() {
        if(editorStyleLoaded)return;
        editorStyleLoaded=true;
        String[] names={"native-editor-button-up.png","native-editor-button-over.png","native-editor-button-down.png"};
        try {
            for(int i=0;i<names.length;++i)try(InputStream stream=getContext().getAssets().open(names[i])) {
                editorButtons[i]=BitmapFactory.decodeStream(stream);
                if(editorButtons[i]==null)throw new IOException("Invalid native editor skin: "+names[i]);
            }
            editorFont=Typeface.createFromAsset(getContext().getAssets(),"native-editor-font.ttf");
        } catch(IOException|RuntimeException error) {
            java.util.Arrays.fill(editorButtons,null);
            android.util.Log.w("BioShock","Native editor assets unavailable",error);
        }
    }
    private String toolLabel(int tool) {
        switch(tool) {
        case TOOL_DONE:return text("Done","Готово");
        case TOOL_RESET:return text("Reset","Сброс");
        case TOOL_BUTTONS:return text("Buttons","Кнопки");
        default:return text("Cancel","Отмена");
        }
    }
    private void drawTools(Canvas canvas) {
        if(tools[0]==null)return;
        boolean armed=SystemClock.uptimeMillis()<resetDeadline;
        for(int i=0;i<tools.length;++i) {
            boolean confirm=i==TOOL_RESET&&armed;
            drawPill(canvas,tools[i],confirm?text("Reset?","Сбросить?"):toolLabel(i),confirm||toolPressed==i);
        }
        drawText(canvas,text("Drag to move. Pinch to resize. Tap Buttons for visibility.",
                "Перетащите кнопку для перемещения, сведите пальцы для изменения размера. Показ — в меню «Кнопки»."),
            getWidth()*.5f,tools[0][3]+20*dp,13*dp,width()-32*dp,0xFFFFFFFF);
    }
    private void drawHint(Canvas canvas) {
        if(hintText==null)return;
        if(SystemClock.uptimeMillis()>=hintUntil){hintText=null;return;}
        if(!playing||editing||!touchEnabled||!touchPrompts||(vehicleFlags&CONTEXT_CINEMATIC)!=0)return;
        paint.setTextSize(14*dp);
        float widthLimit=width()*.72f,measured=Math.min(paint.measureText(hintText),widthLimit);
        // Between the original centre messages (about a quarter down) and the crosshair.
        float cy=getHeight()*.37f,cx=getWidth()*.5f;
        float[] r={cx-measured*.5f-18*dp,cy-18*dp,cx+measured*.5f+18*dp,cy+18*dp};
        paint.setStyle(Paint.Style.FILL);paint.setColor(0xD0101317);
        canvas.drawRoundRect(r[0],r[1],r[2],r[3],18*dp,18*dp,paint);
        drawText(canvas,hintText,cx,cy,14*dp,widthLimit,0xFFFFFFFF);
    }

    private boolean iosScheme() { return touchEnabled&&(touchScheme==2||touchScheme==3); }
    private void clearIosPointers() {
        removeCallbacks(iosNextHold);iosNextId=-1;iosNextHeld=iosSelectorOpen=false;iosSelectorSelected=-1;
        for(int i=0;i<iosIds.length;++i){iosIds[i]=iosHits[i]=-1;iosSwitchOnRelease[i]=false;}
    }
    private void startRumble() {
        removeCallbacks(rumblePoll);
        if(!hapticOn||!(hapticStrength>0f)){rumbleStop();return;}
        postDelayed(rumblePoll,40);
    }
    private void rumbleStop() {
        if(!rumbleActive)return;
        rumbleActive=false;rumbleAmplitude=0;rumbleUntil=0;
        try{if(vibrator!=null)vibrator.cancel();}catch(RuntimeException ignored){}
    }
    private int iosPad() { return touchScheme==3?IosTouchLayout.PAD:IosTouchLayout.PHONE; }
    private static String[] iosEquipmentSprites(String prefix,int count) {
        String[] names=new String[count];
        for(int i=1;i<names.length;++i)names[i]=prefix+i;
        return names;
    }
    private void iosPlace() {
        // Transient system bars overlay the immersive surface; they must not resize the iOS controls.
        float left=cutoutSafe.left,top=cutoutSafe.top;
        float right=Math.max(left+1,getWidth()-cutoutSafe.right),bottom=Math.max(top+1,getHeight()-cutoutSafe.bottom);
        float displayScale=IosTouchLayout.scaleForDensity(iosPad(),right-left,bottom-top,iosUiScale,48*dp);
        iosRects=IosTouchLayout.place(iosPad(),left,top,right,bottom,displayScale,top,bottom);
    }
    private boolean iosVisible(int pad,int index) {
        if(iosSelectorOpen&&index>=10)return false;
        IosTouchLayout.Spec spec=IosTouchLayout.spec(pad,index);
        if((vehicleFlags&CONTEXT_CINEMATIC)!=0)return spec.action==IosTouchLayout.PAUSE;
        if(spec.gate!=0&&(vehicleFlags&spec.gate)==0)return false;
        if(spec.action==IosTouchLayout.USE&&!showUse)return false;
        if(spec.action==TouchInput.FIRE&&!handsAllow(TouchLayout.FIRE))return false;
        if(spec.action==TouchInput.ALT&&!handsAllow(TouchLayout.ALT))return false;
        if((spec.action==TouchInput.RELOAD||spec.action==TouchInput.NEXT)&&!handsAllow(TouchLayout.WEAPON))return false;
        return true;
    }
    private boolean[] iosAllow(int pad) {
        boolean[] allow=new boolean[IosTouchLayout.count(pad)];
        for(int i=0;i<allow.length;++i){
            IosTouchLayout.Spec spec=IosTouchLayout.spec(pad,i);
            allow[i]=spec.role!=IosTouchLayout.DRAW&&iosVisible(pad,i);
        }
        return allow;
    }
    private Bitmap iosSprite(String name) {
        if(iosSprites.containsKey(name))return iosSprites.get(name);
        Bitmap sprite=null;
        try(InputStream stream=getContext().getAssets().open("ios/"+name+".png")) {
            sprite=BitmapFactory.decodeStream(stream);
            if(sprite==null)throw new IOException("Invalid image");
        }catch(IOException e) {
            android.util.Log.w("BioShock","Original iOS sprite unavailable: "+name,e);
        }
        iosSprites.put(name,sprite);
        return sprite;
    }
    private void drawIosSprite(Canvas canvas,String name,float l,float t,float r,float b) {
        if(name==null)return;
        Bitmap sprite=iosSprite(name);
        if(sprite==null)return;
        iosSpriteRect.set(l,t,r,b);
        canvas.drawBitmap(sprite,null,iosSpriteRect,iosPaint);
    }
    private boolean iosPressed(int control) {
        for(int hit:iosHits)if(hit==control)return true;
        return false;
    }
    private String iosArtwork(IosTouchLayout.Spec spec,boolean pressed) {
        switch(spec.action) {
        case IosTouchLayout.FIRE:return IOS_WEAPON_SPRITES[(weaponSlots>>>IOS_WEAPON_SHIFT)&15];
        case IosTouchLayout.ALT:return IOS_ABILITY_SPRITES[(vehicleFlags>>>IOS_ABILITY_SHIFT)&31];
        case IosTouchLayout.RELOAD:
            if((vehicleFlags&IOS_ACTIVE_PLASMID)!=0)return pressed?"plasmid_reload_down":"plasmid_reload";
            break;
        case IosTouchLayout.DUCK:
            if((vehicleFlags&IOS_CROUCHED)!=0)return "stand";
            break;
        default:break;
        }
        return spec.sprite(pressed);
    }
    private void drawIos(Canvas canvas) {
        iosPlace();
        int pad=iosPad(),n=IosTouchLayout.count(pad);
        for(int i=0;i<n&&i*4+3<iosRects.length;++i){
            if(!iosVisible(pad,i))continue;
            IosTouchLayout.Spec spec=IosTouchLayout.spec(pad,i);
            float l=iosRects[i*4],t=iosRects[i*4+1],r=iosRects[i*4+2],b=iosRects[i*4+3];
            float x=(l+r)*.5f,y=(t+b)*.5f;
            boolean pressed=iosPressed(i);
            String sprite=iosArtwork(spec,pressed);
            boolean fire=spec.action==IosTouchLayout.FIRE,plasmid=spec.action==IosTouchLayout.ALT;
            boolean selected=(vehicleFlags&IOS_ACTIVE_PLASMID)!=0?plasmid:fire;
            if(sprite!=null&&(fire||plasmid)) {
                // WeaponPad XML places these layers independently of the tap targets.
                float scale=(r-l)/spec.w;
                boolean tablet=pad==IosTouchLayout.PAD;
                if(pressed){
                    float hx=l-(tablet?(fire?16:15):8)*scale,hy=t-(tablet?(fire?15:14):(fire?7:8))*scale;
                    float size=(tablet?96:50)*scale;
                    drawIosSprite(canvas,"highlight",hx,hy,hx+size,hy+size);
                }
                drawIosSprite(canvas,sprite,l,t,r,b);
                if(!selected&&!pressed){
                    float mx=l-(tablet?(fire?22:23):(fire?12:11))*scale,my=t-(tablet?(fire?21:19):11)*scale;
                    float size=(tablet?108:57)*scale;
                    drawIosSprite(canvas,"weapon_mask",mx,my,mx+size,my+size);
                }
            }else if(spec.action==IosTouchLayout.JUMP||spec.action==IosTouchLayout.AIM){
                float radius=Math.min(r-l,b-t)*.5f;
                paint.setStyle(Paint.Style.FILL);paint.setColor(pressed?0xC0CBBF9D:0x9012161C);
                canvas.drawCircle(x,y,radius,paint);
                TouchIcons.draw(canvas,paint,spec.action==IosTouchLayout.JUMP?TouchIcons.JUMP:TouchIcons.ALT,
                    x,y,radius*1.4f,0xFFE7D5AB);
            }else if(spec.action==IosTouchLayout.USE){
                drawIosSprite(canvas,"use_gear",l,t,r,b);
                drawIosSprite(canvas,sprite,l+(r-l)*IosTouchLayout.useButtonFraction(pad,0),t+(b-t)*IosTouchLayout.useButtonFraction(pad,1),
                    l+(r-l)*IosTouchLayout.useButtonFraction(pad,2),t+(b-t)*IosTouchLayout.useButtonFraction(pad,3));
            }else drawIosSprite(canvas,sprite,l,t,r,b);
            if(spec.caption!=IosTouchLayout.CAPTION_NONE) {
                drawText(canvas,spec.caption==IosTouchLayout.CAPTION_RESCUE?text("Rescue","Спасти"):
                    text("Harvest","Собрать"),x,y,(b-t)*.53f,(r-l)*.9f,0xFFE7D5AB);
            }
            if(spec.role==IosTouchLayout.MOVE){
                float radius=Math.min(r-l,b-t)*.5f,knob=radius*(88f/180f),reach=radius-knob;
                float kx=x+input.moveX()*reach,ky=y-input.moveY()*reach;
                drawIosSprite(canvas,"knob",kx-knob,ky-knob,kx+knob,ky+knob);
            }
        }
        if(iosSelectorOpen)drawIosSelector(canvas);
    }
    private int iosSelectorHit(float x,float y) {
        for(int i=0;i<3;++i) {
            if(((iosSelectorItems >>> (5*i))&31)==0)continue;
            int a=i*4;
            if(x>=iosSelectorRects[a]&&x<=iosSelectorRects[a+2]&&y>=iosSelectorRects[a+1]&&y<=iosSelectorRects[a+3])return i;
        }
        return -1;
    }
    private void drawIosSelector(Canvas canvas) {
        for(int i=0;i<3;++i) {
            int image=(iosSelectorItems >>> (5*i))&31;
            if(image==0)continue;
            int a=i*4;float l=iosSelectorRects[a],t=iosSelectorRects[a+1],r=iosSelectorRects[a+2],b=iosSelectorRects[a+3];
            drawIosSprite(canvas,i==iosSelectorSelected?"highlight":"weapon_mask",l,t,r,b);
            float inset=(r-l)*.15f;
            drawIosSprite(canvas,iosSelectorRow==0?IOS_WEAPON_SPRITES[image]:IOS_ABILITY_SPRITES[image],l+inset,t+inset,r-inset,b-inset);
        }
    }
    private int iosSlot(int id){for(int i=0;i<iosIds.length;++i)if(iosIds[i]==id)return i;return -1;}
    private int iosFree(){for(int i=0;i<iosIds.length;++i)if(iosIds[i]<0)return i;return -1;}
    private void iosPause() {
        if(pauseAction!=null&&pauseAction.getAsBoolean())return;
        release();input.pulse(TouchInput.PAUSE);
        if(!authoritative){playing=false;pausedByControl=true;layoutDirty=true;}
        updateGyro();invalidate();
    }
    private void iosDown(int id,float x,float y,long time) {
        if(iosSlot(id)>=0)return;
        if(pieOpen&&pieDown(id,x,y))return;
        if(vitalsId<0&&!pieOpen&&vitalsHit(x,y)) {
            vitalsId=id;vitalsX=x;vitalsY=y;postDelayed(vitalsHold,VITALS_HOLD_MS);
            haptic(HapticFeedbackConstants.VIRTUAL_KEY);invalidate();return;
        }
        if(ammoId<0&&ammoHit(x,y)) {
            ammoId=id;ammoX=x;ammoY=y;ammoTime=time;postDelayed(ammoHold,VITALS_HOLD_MS);
            haptic(HapticFeedbackConstants.VIRTUAL_KEY);return;
        }
        int pad=iosPad();
        int hit=IosTouchLayout.hit(pad,iosRects,iosAllow(pad),x,y,8*dp);
        if(hit>=0){
            IosTouchLayout.Spec spec=IosTouchLayout.spec(pad,hit);
            if(spec.action==IosTouchLayout.NEXT){
                if(iosNextId>=0)return;
                int slot=iosFree();
                if(slot>=0){
                    iosIds[slot]=id;iosHits[slot]=hit;iosNextId=id;iosNextHeld=false;
                    postDelayed(iosNextHold,100);
                }
                return;
            }
            if(spec.action==IosTouchLayout.FIRE||spec.action==IosTouchLayout.ALT){
                int slot=iosFree();if(slot<0)return;
                boolean active=(spec.action==IosTouchLayout.ALT)==((vehicleFlags&IOS_ACTIVE_PLASMID)!=0);
                if(input.down(id,x,y,active?TouchInput.FIRE:0,TouchInput.BUTTON,time)){
                    iosIds[slot]=id;iosHits[slot]=hit;iosSwitchOnRelease[slot]=!active;
                }
                return;
            }
            if(spec.role==IosTouchLayout.PULSE){
                int slot=iosFree();
                if(slot>=0){iosIds[slot]=id;iosHits[slot]=hit;}
                if(spec.action!=IosTouchLayout.PAUSE&&spec.action!=IosTouchLayout.MAP&&spec.action!=IosTouchLayout.USE&&spec.action!=IosTouchLayout.RELOAD)input.pulse(spec.action);
                haptic(HapticFeedbackConstants.VIRTUAL_KEY);
                return;
            }
            int role=spec.role==IosTouchLayout.MOVE?TouchInput.MOVEMENT:TouchInput.BUTTON;
            int bits=spec.role==IosTouchLayout.HOLD?spec.action:0;
            if(input.down(id,x,y,bits,role,time)){
                int slot=iosFree();
                if(slot>=0){iosIds[slot]=id;iosHits[slot]=hit;}
                haptic(HapticFeedbackConstants.VIRTUAL_KEY);
            }
            return;
        }
        if((vehicleFlags&CONTEXT_CINEMATIC)!=0)return;
        if(input.down(id,x,y,0,TouchInput.LOOK,time)){
            int slot=iosFree();
            if(slot>=0){iosIds[slot]=id;iosHits[slot]=-1;}
        }
    }
    private void iosMove(int id,float x,float y,long time) {
        if(id==iosNextId){iosSelectorSelected=iosSelectorOpen?iosSelectorHit(x,y):-1;invalidate();return;}
        if(id==swallowed||id==vitalsId||id==ammoId||id==pieId){pointerMove(id,x,y,time);return;}
        if(iosSlot(id)<0&&!input.owns(id))return;
        if(id==weaponId){weaponMove(x,y);return;}
        input.move(id,x,y,time);
    }
    private void iosUp(int id,float x,float y,long time) {
        if(id==swallowed||id==vitalsId||id==ammoId||id==pieId){pointerUp(id,x,y,time);return;}
        int slot=iosSlot(id);
        if(slot>=0){
            int hit=iosHits[slot];
            iosIds[slot]=iosHits[slot]=-1;
            if(hit>=0){
                int pad=iosPad(),action=IosTouchLayout.spec(pad,hit).action;
                boolean inside=IosTouchLayout.hit(pad,iosRects,iosAllow(pad),x,y,8*dp)==hit;
                if(id==iosNextId){
                    removeCallbacks(iosNextHold);
                    int selected=iosSelectorOpen?iosSelectorHit(x,y):-1;
                    if(selected>=0){
                        int image=(iosSelectorItems >>> (5*selected))&31;
                        GameActivity.nativeTouchWeapon(iosSelectorRow==0?8:9,image-1,cursorContext);
                    }else if(!iosNextHeld&&inside)input.pulse(TouchInput.NEXT);
                    iosNextId=-1;iosSelectorOpen=iosNextHeld=false;iosSelectorSelected=-1;invalidate();return;
                }
                if(iosSwitchOnRelease[slot]){
                    iosSwitchOnRelease[slot]=false;
                    if(inside)input.pulse(TouchInput.ALT);
                }
                if(action==IosTouchLayout.RELOAD&&inside){
                    if((vehicleFlags&IOS_ACTIVE_PLASMID)!=0)GameActivity.nativeTouchWeapon(10,0,cursorContext);
                    else input.pulse(TouchInput.RELOAD);
                }
                if((action==IosTouchLayout.USE||action==IosTouchLayout.PAUSE||action==IosTouchLayout.MAP)&&
                    IosTouchLayout.hit(pad,iosRects,iosAllow(pad),x,y,8*dp)==hit){
                    if(action==IosTouchLayout.PAUSE)iosPause();
                    // iOS OnLootButtonClick uses the same control to search again when offered.
                    else if(action==IosTouchLayout.USE&&(vehicleFlags&CONTEXT_REROLL)!=0)input.pulse(TouchInput.RELOAD);
                    else input.pulse(action);
                }
            }
        }
        if(input.owns(id))input.up(id,x,y,time);
    }
    private boolean iosOnTouch(MotionEvent event) {
        iosPlace();
        int action=event.getActionMasked(),index=event.getActionIndex();
        if(action==MotionEvent.ACTION_CANCEL){releaseInput();return true;}
        if(action==MotionEvent.ACTION_DOWN)requestUnbufferedDispatch(event);
        long time=event.getEventTime();
        if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN){
            iosDown(event.getPointerId(index),event.getX(index),event.getY(index),time);invalidate();
        }else if(action==MotionEvent.ACTION_MOVE){
            for(int sample=0;sample<=event.getHistorySize();++sample)for(int p=0;p<event.getPointerCount();++p){
                boolean history=sample<event.getHistorySize();
                iosMove(event.getPointerId(p),history?event.getHistoricalX(p,sample):event.getX(p),
                    history?event.getHistoricalY(p,sample):event.getY(p),history?event.getHistoricalEventTime(sample):time);
            }
            invalidate();
        }else if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP){
            iosUp(event.getPointerId(index),event.getX(index),event.getY(index),time);invalidate();
        }
        return true;
    }

    // ---------------------------------------------------------------- touch
    @Override public boolean onTouchEvent(MotionEvent event) {
        syncContext();
        if(iosScheme()&&playing&&!editing)return iosOnTouch(event);
        int action=event.getActionMasked(),index=event.getActionIndex();
        if(action==MotionEvent.ACTION_CANCEL) { releaseInput();return true; }
        // touchEnabled only hides the drawn gameplay controls. The touch still looks and moves.
        if(editing)return editTouch(event);
        int id=event.getPointerId(index);float x=event.getX(index),y=event.getY(index);long time=event.getEventTime();
        // Look samples reach the game at the panel's touch rate, not batched once per vsync.
        if(action==MotionEvent.ACTION_DOWN&&playing)requestUnbufferedDispatch(event);
        if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN) {
            pointerDown(id,x,y,time);invalidate();
        } else if(action==MotionEvent.ACTION_MOVE) {
            visualState(visualBefore);
            for(int sample=0;sample<=event.getHistorySize();++sample)for(int p=0;p<event.getPointerCount();++p) {
                boolean history=sample<event.getHistorySize();
                pointerMove(event.getPointerId(p),history?event.getHistoricalX(p,sample):event.getX(p),
                    history?event.getHistoricalY(p,sample):event.getY(p),history?event.getHistoricalEventTime(sample):time);
            }
            // A camera drag redraws nothing: this layer composes over the game every frame it changes.
            visualState(visualAfter);
            if(!java.util.Arrays.equals(visualBefore,visualAfter))invalidate();
        } else if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP) {
            pointerUp(id,x,y,time);invalidate();
        }
        return true;
    }
    /** The original weapon bar's settled rectangle and card pitch, while it is shown on foot. */
    private float[] weaponBar() {
        if(!playing||editing||(weaponSlots&BAR_SHOWN)==0)return null;
        return TouchLayout.weaponBar(0,0,getWidth(),getHeight(),uiScale,Integer.bitCount(weaponSlots&1023));
    }
    private boolean weaponBarHit(float x,float y) {
        float[] bar=weaponBar();
        // Down to the screen edge: a thumb on the bottom row still reaches the cards.
        return bar!=null&&x>=bar[0]-8*dp&&x<=bar[2]+8*dp&&y>=bar[1]-8*dp;
    }
    private final int[] visualBefore=new int[5],visualAfter=new int[5];
    /** What a move can change on screen: held actions, stick knob, pie selection, picker. */
    private void visualState(int[] out) {
        boolean moving=input.moving();
        out[0]=input.held();
        out[1]=moving?Math.round(input.moveX()*24):Integer.MIN_VALUE;
        out[2]=moving?Math.round(input.moveY()*24):0;
        out[3]=pieOpen?pieSelected:-3;
        out[4]=weaponPicker?1:0;
    }
    private void pointerDown(int id,float x,float y,long time) {
        int control=hit(x,y);
        if(playing&&(vehicleFlags&CONTEXT_CINEMATIC)!=0&&control!=TouchLayout.PAUSE)return;
        if(pieOpen&&pieDown(id,x,y))return;
        if(control>=0) {
            int a=action(control);
            if(a==MORE||a==SEATS) { openPie(id,x,y,a==MORE?PIE_ACTIONS:PIE_ABILITIES,control);return; }
            if(a==WEAPON) { weaponDown(id,x,y,time);return; }
            if(a==TouchInput.SCORES) { scoresDown(id,x,y,time);return; }
            if(a<0||a==TouchInput.PAUSE) {
                // Utilities act on release over the button, so a brushing thumb can slide off.
                for(int i=0;i<utility.length;++i)if(utility[i]<0){utility[i]=control;utilityIds[i]=id;break;}
                haptic(HapticFeedbackConstants.VIRTUAL_KEY);
                return;
            }
            // Second Fire can also move, leaving look to the other thumb.
            int role=control!=TouchLayout.FIRE2?TouchInput.BUTTON:input.moving()?TouchInput.PASSIVE:TouchInput.MOVEMENT;
            if(playing&&input.down(id,x,y,a,role,time)) {
                haptic(HapticFeedbackConstants.VIRTUAL_KEY);
            }
            return;
        }
        if(!playing) { uiDown(id,x,y);return; }
        if(vitalsId<0&&!pieOpen&&vitalsHit(x,y)) {
            vitalsId=id;vitalsX=x;vitalsY=y;
            postDelayed(vitalsHold,VITALS_HOLD_MS);
            haptic(HapticFeedbackConstants.VIRTUAL_KEY);invalidate();
            return;
        }
        if(ammoId<0&&ammoHit(x,y)) {
            ammoId=id;ammoX=x;ammoY=y;ammoTime=time;
            postDelayed(ammoHold,VITALS_HOLD_MS);
            haptic(HapticFeedbackConstants.VIRTUAL_KEY);return;
        }
        if(scrubId<0&&weaponBarHit(x,y)&&input.down(id,x,y,0,TouchInput.PASSIVE,time)) {
            if(!scrubStart(id,x,false))input.up(id,x,y,time);
            return;
        }
        if(centerId<0&&Math.abs(x-getWidth()*.5f)<=56*dp&&Math.abs(y-getHeight()*.5f)<=56*dp&&
            (vehicleFlags&(CONTEXT_USE|CONTEXT_HACK))!=0&&(vehicleFlags&(CONTEXT_RESCUE|CONTEXT_HARVEST))==0) {
            centerId=id;centerX=x;centerY=y;centerTime=time;centerContext=cursorContext;centerHeld=false;
            postDelayed(centerHold,CENTER_HOLD_MS);
        }
        input.down(id,x,y,0,movementZone(x,y)?TouchInput.MOVEMENT:TouchInput.LOOK,time);
    }
    private int utilitySlot(int id) {
        for(int i=0;i<utility.length;++i)if(utility[i]>=0&&utilityIds[i]==id)return i;
        return -1;
    }
    private void pointerMove(int id,float x,float y,long time) {
        if(id==swallowed)return;
        if(pieOpen&&id==pieId) { pieMove(x,y);return; }
        if(id==scrubId) { scrubMove(x);return; }
        if(id==weaponId) { weaponMove(x,y);return; }
        if(id==ammoId) {
            if(Math.hypot(x-ammoX,y-ammoY)>ViewConfiguration.get(getContext()).getScaledTouchSlop()*1.5f) {
                removeCallbacks(ammoHold);ammoId=-1;
                input.down(id,ammoX,ammoY,0,TouchInput.MOVEMENT,ammoTime);
                input.move(id,x,y,time);
            }
            return;
        }
        if(id==vitalsId) {
            // A swipe off the gauge is no tap: the contact is spent.
            if(Math.hypot(x-vitalsX,y-vitalsY)>ViewConfiguration.get(getContext()).getScaledTouchSlop()*1.5f) {
                removeCallbacks(vitalsHold);vitalsId=-1;swallowed=id;invalidate();
            }
            return;
        }
        if(utilitySlot(id)>=0||id==scoresId)return;
        if(id==menuId) {
            if(uiInput.active())uiInput.move(x,y);
            else if(!authoritative||(cursorContext&3)!=3)GameActivity.nativePointer(x/getWidth(),y/getHeight(),false);
            return;
        }
        if(id==centerId&&Math.hypot(x-centerX,y-centerY)>ViewConfiguration.get(getContext()).getScaledTouchSlop()) {
            removeCallbacks(centerHold);centerId=-1;
        }
        if(playing)input.move(id,x,y,time);
    }
    private void pointerUp(int id,float x,float y,long time) {
        if(id==centerId) {
            removeCallbacks(centerHold);centerId=-1;
            if(!centerHeld&&playing&&!editing&&time-centerTime<CENTER_HOLD_MS&&centerContext==cursorContext&&
                centerContext==GameActivity.nativeTouchContext()&&
                (vehicleFlags&CONTEXT_USE)!=0&&(vehicleFlags&(CONTEXT_RESCUE|CONTEXT_HARVEST))==0&&Math.hypot(x-centerX,y-centerY)<=ViewConfiguration.get(getContext()).getScaledTouchSlop()) {
                input.pulse(TouchInput.USE);haptic(HapticFeedbackConstants.VIRTUAL_KEY);
            }
        }
        if(id==swallowed) { swallowed=-1;return; }
        if(pieOpen&&id==pieId) { pieUp(x,y);return; }
        if(id==scrubId) { scrubUp(x,y,time);return; }
        if(id==weaponId) { weaponUp(x,y,time);return; }
        if(id==ammoId){removeCallbacks(ammoHold);ammoId=-1;return;}
        if(id==scoresId) { scoresUp(x,y,time);return; }
        if(id==vitalsId) {
            removeCallbacks(vitalsHold);vitalsId=-1;
            // UseHypoOfType MedHypo, the original F binding.
            if(vitalsControl) {
                input.pulse(TouchInput.HEAL);
                haptic(HapticFeedbackConstants.VIRTUAL_KEY);markHint(HINT_VITALS);
            }
            invalidate();
            return;
        }
        int slot=utilitySlot(id);
        if(slot>=0) {
            int control=utility[slot];utility[slot]=-1;
            if(inside(control,x,y))activate(control);
            return;
        }
        if(id==menuId) {
            if(uiInput.active())uiInput.end(x,y);
            else if(!authoritative||(cursorContext&3)!=3) {
                GameActivity.nativePointer(x/getWidth(),y/getHeight(),false);
                if(!GameActivity.nativeTouch(0,0,0,0,0,cursorContext))syncContext();
            }
            menuId=-1;performClick();
            return;
        }
        if(playing)input.up(id,x,y,time);
    }
    private void uiDown(int id,float x,float y) {
        if(menuId>=0)return;
        hideKeyboard();
        if(authoritative&&(cursorContext&3)==3) {
            if(uiInput.begin(x,y)) {
                menuId=id;
                if(uiInput.polling()){removeCallbacks(uiPoll);postOnAnimation(uiPoll);}
            } else syncContext();
        } else if(GameActivity.nativePointer(x/getWidth(),y/getHeight(),false)) {
            if(GameActivity.nativeTouch(0,0,0,0,TouchInput.FIRE,cursorContext))menuId=id;
            else syncContext();
        }
    }
    private void activate(int control) {
        switch(action(control)) {
        case MODE:mode(!playing);break;
        case KEYBOARD:showKeyboard();break;
        case SETTINGS:release();if(settingsAction!=null)settingsAction.run();break;
        case QUICKSAVE:release();input.pulse(TouchInput.SCORES);break;
        case TouchInput.PAUSE:
            if(pauseAction!=null&&pauseAction.getAsBoolean())break;
            // BioShock opens PauseGameAlias on Escape's own release, not on a second release.
            release();input.pulse(TouchInput.PAUSE);
            if(!authoritative){playing=false;pausedByControl=true;layoutDirty=true;}
            updateGyro();break;
        default:break;
        }
        invalidate();
    }

    // ------------------------------------------------- weapon button gestures
    private void weaponDown(int id,float x,float y,long time) {
        if(weaponId>=0||!input.down(id,x,y,0,TouchInput.PASSIVE,time))return;
        weaponId=id;weaponPicker=false;weaponSwipe=0;weaponSwiped=false;pickerRow=-1;pickerStep=0;weaponX=weaponLastX=x;weaponY=weaponLastY=y;
        postDelayed(weaponHold,PICK_HOLD_MS);
        haptic(HapticFeedbackConstants.VIRTUAL_KEY);
    }
    private void startPicker() {
        // BioShock opens its original PC weapon/plasmid selection with Shift.
        if(!input.hold(weaponId,TouchInput.PICKER)) { cancelWeapon();return; }
        weaponPicker=true;haptic(HapticFeedbackConstants.LONG_PRESS);
        weaponStick(weaponLastX,weaponLastY);invalidate();
    }
    private void weaponMove(float x,float y) {
        weaponLastX=x;weaponLastY=y;
        if(weaponPicker) { weaponStick(x,y);return; }
        // Once a finger swipes, even a slow swipe must not turn into a long press.
        float dx=x-weaponX,dy=y-weaponY;
        if(Math.hypot(dx,dy)>ViewConfiguration.get(getContext()).getScaledTouchSlop())removeCallbacks(weaponHold);
        if(Math.abs(dx)>=Math.abs(dy)*1.4f) {
            int step=(int)(dx/(32*dp));
            while(weaponSwipe!=step) {
                weaponSwiped=true;
                int direction=step>weaponSwipe?1:-1;
                input.pulse(direction>0?TouchInput.NEXT:TouchInput.PREVIOUS);
                weaponSwipe+=direction;weaponTick();
            }
        }
    }
    private void weaponStick(float x,float y) {
        if((weaponSlots&PICKER_ACTIVE)==0)return;
        float dy=y-weaponY;
        int row=dy<-24*dp?0:dy>24*dp?1:-1;
        int step=Math.max(-15,Math.min(15,Math.round((x-weaponX)/(48*dp))));
        if(row==pickerRow&&step==pickerStep)return;
        pickerRow=row;pickerStep=step;
        if(row>=0&&GameActivity.nativeTouchWeapon(row==0?4:5,step+15,cursorContext))weaponTick();
    }
    private void weaponUp(float x,float y,long time) {
        removeCallbacks(weaponHold);
        int id=weaponId;weaponId=-1;
        if(weaponPicker) {
            weaponStick(x,y);
            if((weaponSlots&PICKER_ACTIVE)!=0)GameActivity.nativeTouchWeapon(pickerRow>=0?6:7,15,cursorContext);
            weaponPicker=false;input.stick(0,0);input.up(id,x,y,time);return;
        }
        input.up(id,x,y,time);
        float dx=x-weaponX,dy=y-weaponY;
        if(!weaponSwiped&&Math.hypot(dx,dy)<=ViewConfiguration.get(getContext()).getScaledTouchSlop()*1.5f)input.pulse(TouchInput.RELOAD);
    }
    /** Begin a swipe on the original bar at the selected weapon; false when the bar is not shown. */
    private boolean scrubStart(int id,float x,boolean fromButton) {
        int mask=weaponSlots,selected=(mask>>10)&15,start=-1;
        int[] slots=TouchLayout.weaponSlots(mask);
        for(int i=0;i<slots.length;++i)if(slots[i]==selected)start=i;
        if((mask&BAR_SHOWN)==0||slots.length<2||start<0)return false;
        float[] bar=weaponBar();
        scrubId=id;scrubSlots=slots;scrubStart=scrubIndex=start;scrubX=x;scrubButton=fromButton;
        scrubStep=clamp(bar!=null?bar[4]:48*dp,36*dp,64*dp);
        if(!fromButton)haptic(HapticFeedbackConstants.VIRTUAL_KEY);
        return true;
    }
    private void scrubMove(float x) {
        int next=TouchLayout.weaponIndex(scrubStart,x-scrubX,scrubStep,scrubSlots.length);
        if(scrubId<0||next==scrubIndex)return;
        scrubIndex=next;
        // The original bar enlarges the card and names it; nothing switches until release.
        if(!GameActivity.nativeTouchWeapon(1,scrubSlots[next],cursorContext)) { scrubId=-1;input.cancel();syncContext();return; }
        weaponTick();
    }
    private void scrubUp(float x,float y,long time) {
        scrubMove(x);
        int id=scrubId;scrubId=-1;
        if(id<0)return;
        boolean changed=scrubIndex!=scrubStart;
        GameActivity.nativeTouchWeapon(changed?2:3,scrubSlots[scrubIndex],cursorContext);
        if(changed)haptic(HapticFeedbackConstants.VIRTUAL_KEY);
        input.up(id,x,y,time);
        if(changed&&!scrubButton)markHint(HINT_WEAPON);
    }
    /** A short vibration for each weapon a swipe passes. */
    private void weaponTick() {
        if(!hapticOn||!(hapticStrength>0f))return;
        try {
            if(vibrator!=null&&vibrator.hasVibrator()) {
                if(hapticStrength>=1f)vibrator.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK));
                else vibrator.vibrate(VibrationEffect.createOneShot(20,Math.max(1,Math.min(255,Math.round(255f*hapticStrength)))));
                return;
            }
        } catch(RuntimeException ignored) { }
        haptic(HapticFeedbackConstants.CLOCK_TICK);
    }
    private void cancelWeapon() {
        removeCallbacks(weaponHold);
        if(weaponPicker&&(weaponSlots&PICKER_ACTIVE)!=0)GameActivity.nativeTouchWeapon(7,15,cursorContext);
        if(weaponId>=0){input.stick(0,0);input.up(weaponId,weaponX,weaponY);}
        weaponId=-1;weaponPicker=false;invalidate();
    }

    private float ammoHudTop() { return getHeight()-140*uiScale*getHeight()/720f; }
    private boolean ammoHit(float x,float y) {
        if(!playing||editing||(weaponSlots&HANDS_HOLDING)==0||(weaponSlots&HANDS_MELEE)!=0)return false;
        float k=getHeight()/720f;
        if(iosScheme())return x>=getWidth()-260*uiScale*k&&x<=getWidth()-8*k&&
            y>=8*k&&y<=(8+140*uiScale)*k;
        return x>=8*k&&x<=210*uiScale*k&&y>=ammoHudTop()&&y<=getHeight()-12*k;
    }

    // ----------------------------------------------------------------- scores
    private void scoresDown(int id,float x,float y,long time) {
        if(scoresId>=0)return;
        haptic(HapticFeedbackConstants.VIRTUAL_KEY);
        if(input.latched(TouchInput.SCORES)) { input.toggle(TouchInput.SCORES);swallowed=id;return; }
        if(input.down(id,x,y,TouchInput.SCORES,TouchInput.PASSIVE,time)){scoresId=id;scoresTime=SystemClock.uptimeMillis();}
    }
    private void scoresUp(float x,float y,long time) {
        int id=scoresId;scoresId=-1;
        // A tap keeps the original scoreboard open: latch before the finger's hold ends.
        if(SystemClock.uptimeMillis()-scoresTime<SCORES_TAP_MS)input.toggle(TouchInput.SCORES);
        input.up(id,x,y,time);
    }

    // -------------------------------------------------------------------- pie
    private int[] pieItems(int kind) {
        if(kind==PIE_ABILITIES) return new int[]{TouchInput.WEAPON1,TouchInput.WEAPON1<<1,TouchInput.WEAPON1<<2,
            TouchInput.WEAPON1<<3,TouchInput.WEAPON1<<4,TouchInput.WEAPON1<<5,TouchInput.AMMO_UP,TouchInput.AMMO_DOWN,MORE_MAIN};
        if(kind==PIE_TOOLS)return new int[]{TouchInput.SCORES,TouchInput.MAP,TouchInput.LOG,
            TouchInput.WALK,SETTINGS,MORE_MAIN};
        int[] items=new int[TouchLayout.COUNT+4];
        int count=0;
        items[count++]=TouchInput.HEAL;
        if(inRadial(TouchLayout.HACK))items[count++]=TouchInput.HACK;
        items[count++]=TouchInput.HELP;
        if(inRadial(TouchLayout.RESCUE))items[count++]=TouchInput.RESCUE;
        if(inRadial(TouchLayout.HARVEST))items[count++]=TouchInput.TAUNT;
        items[count++]=SEATS;
        items[count++]=TOOLS;
        if(inRadial(TouchLayout.AIM))items[count++]=TouchInput.BEST;
        for(int control:TouchLayout.ORDER) {
            if(!TouchLayout.defaultRadial(control)&&availableInRadial(control))items[count++]=PIE_CONTROL-control;
        }
        return Arrays.copyOf(items,count);
    }
    private boolean inRadial(int control) {
        return (displayMode(control)&TouchLayout.IN_RADIAL)!=0;
    }
    private boolean availableInRadial(int control) {
        return inRadial(control)&&control!=TouchLayout.MORE&&editable(control)&&gameplayAllows(control)&&handsAllow(control);
    }
    private int pieIcon(int item) {
        if(item<=PIE_CONTROL)return icon(PIE_CONTROL-item);
        switch(item) {
        case TouchInput.SCORES:return TouchIcons.SAVE;
        case TouchInput.MAP:return TouchIcons.LOAD;
        case TouchInput.HELP:return TouchIcons.HELP;
        case TouchInput.BEST:return TouchIcons.ALT;
        case TouchInput.HEAL:return TouchIcons.HEAL;
        case TouchInput.HACK:return TouchIcons.HACK;
        case TouchInput.RESCUE:return TouchIcons.USE;
        case TouchInput.TAUNT:return TouchIcons.HELP;
        case TouchInput.WALK:return TouchIcons.WALK;
        case TouchInput.LOG:return TouchIcons.HORN;
        case TouchInput.AMMO_UP:case TouchInput.AMMO_DOWN:return TouchIcons.FIRE;
        case SEATS:return TouchIcons.MORE;
        case TOOLS:return TouchIcons.MENU;
        case MORE_MAIN:return TouchIcons.BACK;
        case SETTINGS:return TouchIcons.SETTINGS;
        default:return 0;
        }
    }
    /** Short caption inside a sector, or the full name in the pie's centre. */
    private String pieLabel(int item,boolean full) {
        if(item<=PIE_CONTROL) {
            int control=PIE_CONTROL-item;
            if(control==TouchLayout.WEAPON)return full?text("Reload / EVE","Зарядить / EVE"):text("Reload","Зарядить");
            if(control==TouchLayout.ALT&&!full)return text("Switch","Смена");
            return label(control);
        }
        for(int i=0;i<ABILITY_COUNT;++i)if(item==TouchInput.WEAPON1<<i)
            return full?text("Plasmid shortcut ","Выбор плазмида ")+(i+1):"F"+(i+1);
        switch(item) {
        case TouchInput.SCORES:return full?text("Quick save (F8)","Быстрое сохранение (F8)"):text("Save","Сохр.");
        case TouchInput.MAP:return full?text("Quick load (F9)","Быстрая загрузка (F9)"):text("Load","Загр.");
        case TouchInput.HELP:return full?text("Context help (M)","Помощь (M)"):text("Help","Помощь");
        case TouchInput.BEST:return full?text("Zoom cycle (Z)","Режим прицела (Z)"):text("Zoom","Прицел");
        case TouchInput.LOG:return full?text("Play unread audio log (T)","Прослушать аудиодневник (T)"):text("Log","Журнал");
        case TouchInput.HEAL:return full?text("First aid kit (F)","Аптечка (F)"):text("Heal","Лечить");
        case TouchInput.HACK:return full?text("Hack (V)","Взлом (V)"):text("Hack","Взлом");
        case TouchInput.RESCUE:return full?text("Rescue Little Sister (L)","Спасти Сестричку (L)"):text("Rescue","Спасти");
        case TouchInput.TAUNT:return full?text("Hint / harvest Little Sister (H)","Подсказка / собрать ADAM (H)"):text("Tip/ADAM","Сов/ADAM");
        case TouchInput.WALK:return full?text("Toggle walking (Ctrl)","Переключить ходьбу (Ctrl)"):text("Walk","Шаг");
        case TouchInput.AMMO_UP:return full?text("Previous ammo type (Q)","Предыдущие патроны (Q)"):text("Ammo+","Патр.+");
        case TouchInput.AMMO_DOWN:return full?text("Next ammo type (B)","Следующие патроны (B)"):text("Ammo-","Патр.-");
        case SEATS:return full?text("Plasmids and ammo types","Плазмиды и типы патронов"):text("Ammo","Патроны");
        case TOOLS:return full?text("Save, load and journals","Сохранение, загрузка и журналы"):text("More","Далее");
        case MORE_MAIN:return full?text("Back to actions","Назад к действиям"):text("Back","Назад");
        case SETTINGS:return full?text("Additional settings","Дополнительно"):text("Options","Опции");
        default:return "";
        }
    }
    private void openPie(int id,float x,float y,int kind,int source) {
        closePie();
        pieItems=pieItems(kind);pieKind=kind;pieSource=source;
        pieOpen=true;pieId=id;pieSelected=-1;pieDragged=pieSticky=false;pieStartX=x;pieStartY=y;
        pieR=pieRadius(pieItems.length);
        pieX=clamp(x,safe.left+pieR,Math.max(safe.left+pieR,getWidth()-safe.right-pieR));
        pieY=clamp(y,safe.top+pieR,Math.max(safe.top+pieR,getHeight()-safe.bottom-pieR));
        haptic(HapticFeedbackConstants.VIRTUAL_KEY);
        // The first time, the pie's own title teaches the faster hold-and-slide gesture.
        pieHint=kind!=PIE_ACTIONS||(hintsSeen&HINT_MORE)!=0?null:source<0?
            text("Hold the health gauge and slide toward an action","Удерживайте шкалу здоровья и проведите к действию"):
            text("Hold More and slide toward an action","Удерживайте «Ещё» и проведите к действию");
        if(pieHint!=null)markHint(HINT_MORE);
        invalidate();
    }
    /** Radius of the item icons: the middle of each sector's usable depth. */
    private float pieRing() { return pieR*.70f; }
    private float pieRadius(int count) {
        float preferred=count>5?128+6*Math.max(0,count-9):118;
        return Math.min(preferred*dp*Math.max(1,layoutScale),Math.min(height(),width())*.45f);
    }
    /**
     * Caption room in a sector: the chord at the inner edge of a caption under
     * the top icon, the narrowest place, so every caption keeps one size and
     * stays inside its own sector.
     */
    private float pieCaptionWidth(int count) {
        float edge=pieRing()-20*dp;
        return Math.min((float)(2*edge*Math.sin(Math.PI/Math.max(2,count)))-6*dp,pieR*.64f-6*dp);
    }
    private void closePie() {
        if(!pieOpen)return;
        pieOpen=false;pieId=-1;pieSelected=-1;pieSource=-1;invalidate();
    }
    /** True when the pie consumed this new contact. */
    private boolean pieDown(int id,float x,float y) {
        if(pieId>=0)return false; // Another finger already drags it: other contacts keep playing.
        int hit=TouchLayout.pieHit(x,y,pieX,pieY,pieR*.36f,pieR,pieItems.length);
        if(hit==-1) {
            // A kept-open pie is dismissed by the next touch elsewhere, which does nothing else.
            closePie();swallowed=id;return true;
        }
        pieId=id;pieSticky=true;pieSelected=hit>=0?hit:-1;invalidate();
        return true;
    }
    private void pieMove(float x,float y) {
        int next;
        if(pieSticky)next=TouchLayout.pieHit(x,y,pieX,pieY,pieR*.36f,pieR,pieItems.length);
        else {
            if(Math.hypot(x-pieStartX,y-pieStartY)>12*dp)pieDragged=true;
            // Marking-menu direction from the finger, wherever the clamped pie is drawn.
            next=pieDragged?TouchLayout.sector(x-pieStartX,y-pieStartY,pieItems.length,24*dp):-1;
        }
        if(next<0)next=-1;
        if(next!=pieSelected) {
            pieSelected=next;
            if(next>=0)haptic(HapticFeedbackConstants.CLOCK_TICK);
        }
    }
    private void pieUp(float x,float y) {
        pieMove(x,y);
        if(!pieSticky&&!pieDragged) {
            // A tap keeps the pie open for a second tap on an item.
            pieSticky=true;pieId=-1;pieSelected=-1;invalidate();return;
        }
        int selected=pieSelected;
        boolean center=pieSticky&&TouchLayout.pieHit(x,y,pieX,pieY,pieR*.36f,pieR,pieItems.length)==TouchLayout.CENTER;
        if(selected>=0||center||!pieSticky) {
            int item=selected>=0?pieItems[selected]:0;
            closePie();
            if(item!=0)pieSelect(item);
        } else { pieId=-1;pieSelected=-1; }
        invalidate();
    }
    private void pieSelect(int item) {
        haptic(HapticFeedbackConstants.VIRTUAL_KEY);
        if(item==SEATS) { openPie(-1,pieX,pieY,PIE_ABILITIES,pieSource);return; }
        if(item==TOOLS) { openPie(-1,pieX,pieY,PIE_TOOLS,pieSource);return; }
        if(item==MORE_MAIN) { openPie(-1,pieX,pieY,PIE_ACTIONS,pieSource);return; }
        if(item==SETTINGS){release();if(settingsAction!=null)settingsAction.run();return;}
        if(item<=PIE_CONTROL) {
            int control=PIE_CONTROL-item;
            if(!availableInRadial(control))return;
            if(control==TouchLayout.PAUSE){activate(control);return;}
            item=control==TouchLayout.WEAPON?TouchInput.RELOAD:action(control);
        }
        if(item==TouchInput.COMMAND)input.pulseDelayed(item,120);
        else input.pulse(item);
    }
    private void drawPie(Canvas canvas) {
        int n=pieItems.length;
        if(n==0)return;
        float r=pieR,inner=r*.36f,sweep=360f/n;
        paint.setStyle(Paint.Style.FILL);paint.setColor(0xE8101317);canvas.drawCircle(pieX,pieY,r,paint);
        if(pieSelected>=0) {
            paint.setColor(0xF0000000|(ACCENT&0xFFFFFF));
            canvas.drawArc(pieX-r,pieY-r,pieX+r,pieY+r,-90-sweep*.5f+pieSelected*sweep,sweep,true,paint);
        }
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.5f*dp);paint.setColor(0x40FFFFFF);
        for(int i=0;i<n;++i) {
            double a=Math.toRadians(-90-sweep*.5f+i*sweep);
            canvas.drawLine(pieX+(float)Math.cos(a)*inner,pieY+(float)Math.sin(a)*inner,
                pieX+(float)Math.cos(a)*r,pieY+(float)Math.sin(a)*r,paint);
        }
        canvas.drawCircle(pieX,pieY,r-.75f*dp,paint);
        paint.setStyle(Paint.Style.FILL);paint.setColor(0xF5181C22);canvas.drawCircle(pieX,pieY,inner,paint);
        float ring=pieRing(),caption=pieCaptionWidth(n);
        for(int i=0;i<n;++i) {
            double a=TouchLayout.sectorAngle(i,n);
            float ix=pieX+(float)Math.sin(a)*ring,iy=pieY-(float)Math.cos(a)*ring;
            int ink=i==pieSelected?0xFF101317:0xFFFFFFFF;
            int icon=pieIcon(pieItems[i]);
            if(icon!=0) {
                TouchIcons.draw(canvas,paint,icon,ix,iy-8*dp,24*dp,ink);
                drawText(canvas,pieLabel(pieItems[i],false),ix,iy+14*dp,11*dp,caption,ink);
            } else drawText(canvas,pieLabel(pieItems[i],false),ix,iy,24*dp,caption,ink);
        }
        // The centre shows what a release does; the full name sits in a pill clear of the pie.
        int selected=pieSelected>=0?pieItems[pieSelected]:0,centreIcon=selected!=0?pieIcon(selected):TouchIcons.CLOSE;
        if(centreIcon!=0)TouchIcons.draw(canvas,paint,centreIcon,pieX,pieY,Math.min(30*dp,inner*1.1f),selected!=0?ACCENT:0xB0FFFFFF);
        else drawText(canvas,pieLabel(selected,false),pieX,pieY,24*dp,inner*1.6f,ACCENT);
        String title=selected!=0?pieLabel(selected,true):pieHint!=null?pieHint:
            pieKind==PIE_ABILITIES?text("Choose an ability","Выберите способность"):text("Choose an action","Выберите действие");
        paint.setTextSize(14*dp);
        float titleWidth=Math.min(paint.measureText(title),pieTitleWidth());
        float ty=pieY-r-safe.top>=40*dp?pieY-r-22*dp:pieY+r+22*dp;
        float[] pill={pieX-titleWidth*.5f-16*dp,ty-16*dp,pieX+titleWidth*.5f+16*dp,ty+16*dp};
        paint.setStyle(Paint.Style.FILL);paint.setColor(0xE8101317);
        canvas.drawRoundRect(pill[0],pill[1],pill[2],pill[3],16*dp,16*dp,paint);
        drawText(canvas,title,pieX,ty,14*dp,pieTitleWidth(),selected!=0?ACCENT:0xFFFFFFFF);
    }
    private float pieTitleWidth() { return Math.min(pieR*2.4f,width()-32*dp); }

    // ------------------------------------------------------------------ hints
    private void offer(int kind,String english,String russian) {
        long now=SystemClock.uptimeMillis();
        if((hintsSeen&kind)!=0||(hintText!=null&&now<hintUntil))return;
        hintText=text(english,russian);hintUntil=now+HINT_MS;
        postInvalidateDelayed(HINT_MS+50);invalidate();
        markHint(kind);
    }
    private void markHint(int kind) {
        hintsSeen|=kind;
        PortPreferences p=preferences();
        if(p!=null){p.put("touchHints",hintsSeen);activity().savePortSettings(null);}
    }
    private void offerHint() {
        if(!playing||editing||!authoritative||!touchEnabled||!touchPrompts||(vehicleFlags&CONTEXT_CINEMATIC)!=0)return;
        long remaining=hintUntil-SystemClock.uptimeMillis();
        if(hintText!=null&&remaining>0){postDelayed(hintCheck,remaining+50);return;}
        if((hintsSeen&HINT_LOOK)==0)offer(HINT_LOOK,"Drag on the right to look · hold Fire and drag to aim while shooting",
            "Ведите пальцем справа — обзор · держите «Огонь» и ведите — стрельба с прицеливанием");
        else if(handsAllow(TouchLayout.WEAPON)&&(hintsSeen&HINT_WEAPON)==0)offer(HINT_WEAPON,"Reload: tap · cycle: swipe · hold and drag to choose a weapon or plasmid",
            "Перезарядка — касание · смена — смахивание · удерживайте и ведите, чтобы выбрать оружие или плазмид");
        else if((weaponSlots&HANDS_HOLDING)!=0&&(hintsSeen&HINT_AMMO)==0)offer(HINT_AMMO,"Hold the ammo counter to switch ammo types",
            "Удерживайте счётчик патронов, чтобы сменить тип боеприпасов");
        else if(vitalsControl&&(vehicleFlags&CONTEXT_VITALS)!=0&&(hintsSeen&HINT_VITALS)==0)offer(HINT_VITALS,"Health gauge: tap for a first aid kit · hold for more actions",
            "Шкала здоровья: касание — аптечка · удержание — другие действия");
        else if((weaponSlots&HANDS_PLASMID)!=0&&(hintsSeen&HINT_ACTIONS)==0)offer(HINT_ACTIONS,"Reload refills EVE while holding a plasmid",
            "Перезарядка с плазмидом пополняет EVE");
        else return;
        postDelayed(hintCheck,HINT_MS+50);
    }

    // ----------------------------------------------------------------- editor
    private int toolHit(float x,float y) {
        for(int i=0;i<tools.length;++i) {
            float[] r=tools[i];
            if(r!=null&&x>=r[0]-6*dp&&x<=r[2]+6*dp&&y>=r[1]-6*dp&&y<=r[3]+6*dp)return i;
        }
        return -1;
    }
    private boolean editTouch(MotionEvent event) {
        int action=event.getActionMasked(),index=event.getActionIndex(),id=event.getPointerId(index);
        float x=event.getX(index),y=event.getY(index);
        if(layoutDirty)buildLayout();
        if(action==MotionEvent.ACTION_DOWN) {
            int tool=toolHit(x,y);
            if(tool>=0&&toolId<0){toolId=id;toolPressed=tool;}
            else if(editId<0) {
                int control=hit(x,y);
                if(control>=0){editId=id;editControl=control;editDX=controlX[control]-x;editDY=controlY[control]-y;
                    haptic(HapticFeedbackConstants.VIRTUAL_KEY);}
            }
        } else if(action==MotionEvent.ACTION_POINTER_DOWN&&editId>=0&&pinchId<0) {
            int first=event.findPointerIndex(editId);
            if(first>=0) {
                float span=(float)Math.hypot(x-event.getX(first),y-event.getY(first));
                if(span>=12*dp){pinchId=id;pinchSpan=span;pinchSize=buttonSize(editControl)/dp;}
            }
        } else if(action==MotionEvent.ACTION_MOVE) {
            int pointer=editId<0?-1:event.findPointerIndex(editId);
            int other=pinchId<0?-1:event.findPointerIndex(pinchId);
            if(pointer>=0) {
                float px=event.getX(pointer),py=event.getY(pointer);
                if(other>=0) {
                    float span=(float)Math.hypot(px-event.getX(other),py-event.getY(other));
                    resizeEditedButton(pinchSize*span/pinchSpan);
                } else dragTo(editControl,px+editDX,py+editDY);
            }
        } else if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP) {
            if(id==toolId) {
                int tool=toolPressed;toolId=toolPressed=-1;
                if(toolHit(x,y)==tool)tool(tool);
            }
            if(id==editId)editId=pinchId=-1;
            else if(id==pinchId) {
                pinchId=-1;
                int first=event.findPointerIndex(editId);
                if(first>=0){editDX=controlX[editControl]-event.getX(first);editDY=controlY[editControl]-event.getY(first);}
            }
        }
        invalidate();return true;
    }
    private void dragTo(int control,float x,float y) {
        if(control<0)return;
        float nx=clamp((x-safe.left)/width(),0,1),ny=clamp((y-safe.top)/height(),0,1);
        customX[control]=leftHanded&&TouchLayout.mirrored(control)?1-nx:nx;customY[control]=ny;
        layoutDirty=true;
    }
    private void tool(int tool) {
        if(tool==TOOL_DONE)finishEditing(true);
        else if(tool==TOOL_RESET)resetLayout();
        else if(tool==TOOL_BUTTONS)openButtonOptions();
        else if(tool==TOOL_CANCEL)finishEditing(false);
    }
    private void resizeEditedButton(float diameter) {
        customSize[editControl]=Math.round(clamp(diameter,TouchLayout.MIN_DP,TouchLayout.MAX_DP));
        layoutDirty=true;invalidate();
    }
    private void openButtonOptions() {
        if(editOptions!=null)return;
        ScrollView scroll=new ScrollView(getContext());
        scroll.addView(new ButtonOptions());
        AlertDialog dialog=new AlertDialog.Builder(getContext())
            .setTitle(text("Configure buttons","Настройка кнопок"))
            .setView(scroll).setPositiveButton(text("Back to layout","К раскладке"),null).create();
        editOptions=dialog;
        dialog.setOnDismissListener(closed->{if(editOptions==closed)editOptions=null;});
        dialog.show();
    }
    private final class ButtonOptions extends LinearLayout {
        private final TextView sizeLabel;
        private final SeekBar size;
        private final CheckBox screen,radial;
        private boolean refreshing;

        ButtonOptions() {
            super(GameControls.this.getContext());
            setOrientation(VERTICAL);
            int padding=Math.round(24*dp);
            setPadding(padding,0,padding,0);
            int[] ids=Arrays.stream(TouchLayout.ORDER).filter(GameControls.this::editable).toArray();
            String[] names=new String[ids.length];
            int selected=0;
            for(int i=0;i<ids.length;++i) {
                names[i]=label(ids[i]);
                if(ids[i]==editControl)selected=i;
            }
            Spinner selection=new Spinner(getContext());
            ArrayAdapter<String> adapter=new ArrayAdapter<>(getContext(),android.R.layout.simple_spinner_item,names);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            selection.setAdapter(adapter);addView(selection,new LayoutParams(-1,Math.round(48*dp)));
            selection.setSelection(selected);

            sizeLabel=new TextView(getContext());addView(sizeLabel);
            size=new SeekBar(getContext());size.setMax(TouchLayout.MAX_DP-(int)TouchLayout.MIN_DP);
            size.setContentDescription(text("Button size","Размер кнопки"));
            addView(size,new LayoutParams(-1,Math.round(44*dp)));
            screen=new CheckBox(getContext());screen.setText(text("Show on screen","Показывать на экране"));addView(screen);
            radial=new CheckBox(getContext());radial.setText(text("Show in radial menu","Показывать в радиальном меню"));addView(radial);
            refresh();

            selection.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override public void onItemSelected(AdapterView<?> parent,View row,int position,long id) {
                    editControl=ids[position];refresh();GameControls.this.invalidate();
                }
                @Override public void onNothingSelected(AdapterView<?> parent) {}
            });
            size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(SeekBar bar,int value,boolean user) {
                    if(!user)return;
                    resizeEditedButton(value+TouchLayout.MIN_DP);
                    showSize(customSize[editControl]);
                }
                @Override public void onStartTrackingTouch(SeekBar bar) {}
                @Override public void onStopTrackingTouch(SeekBar bar) {}
            });
            screen.setOnCheckedChangeListener((box,checked)->place(TouchLayout.ON_SCREEN,checked));
            radial.setOnCheckedChangeListener((box,checked)->place(TouchLayout.IN_RADIAL,checked));
        }
        private void showSize(int diameter) {
            sizeLabel.setText(text("Size","Размер")+"  "+diameter+" dp");
        }
        private void refresh() {
            refreshing=true;
            int mode=displayMode(editControl);
            screen.setChecked((mode&TouchLayout.ON_SCREEN)!=0);
            radial.setChecked((mode&TouchLayout.IN_RADIAL)!=0);
            radial.setEnabled(editControl!=TouchLayout.MORE);
            boolean fixed=editControl==TouchLayout.PAUSE;
            int diameter=fixed?(int)TouchLayout.SIZE[TouchLayout.PAUSE]:
                customSize[editControl]>0?customSize[editControl]:Math.round(buttonSize(editControl)/dp);
            size.setEnabled(!fixed);size.setProgress(diameter-(int)TouchLayout.MIN_DP);showSize(diameter);
            refreshing=false;
        }
        private void place(int location,boolean checked) {
            if(refreshing)return;
            int mode=displayMode(editControl);
            customDisplay[editControl]=checked?mode|location:mode&~location;
            layoutDirty=true;GameControls.this.invalidate();
        }
    }

    // ------------------------------------------------------------------- gyro
    private void updateGyro() {
        if(input==null)return;
        boolean active=focused&&isShown()&&playing&&!editing&&!pieOpen&&(vehicleFlags&CONTEXT_CINEMATIC)==0&&gyroWanted(gyroMode,
            (input.held()&TouchInput.FIRE)!=0||(vehicleFlags&CONTEXT_FIRING)!=0,(vehicleFlags&CONTEXT_AIMING)!=0);
        if(active!=gyroActive) { sensorTime=0;gyroActive=active; }
        if(active&&gyro!=null&&!gyroListening)gyroListening=sensors.registerListener(this,gyro,SensorManager.SENSOR_DELAY_GAME);
        else if(!active)stopGyro();
    }
    static boolean gyroWanted(int mode,boolean firing,boolean aiming) {
        return mode==2||(mode==1&&firing)||(mode==3&&aiming)||(mode==4&&(firing||aiming));
    }
    private void stopGyro() {
        if(gyroListening&&sensors!=null)sensors.unregisterListener(this);
        gyroListening=false;sensorTime=0;
    }
    @Override public void onAccuracyChanged(Sensor sensor,int accuracy) { }
    @Override public void onSensorChanged(SensorEvent event) {
        if(!gyroActive||event.sensor.getType()!=Sensor.TYPE_GYROSCOPE) { sensorTime=0;return; }
        Display display=getDisplay();int rotation=display==null?Surface.ROTATION_0:display.getRotation();
        long previous=sensorTime;sensorTime=event.timestamp;
        if(rotation!=sensorRotation) { sensorRotation=rotation;return; }
        double dt=(event.timestamp-previous)*1e-9;
        if(previous==0||dt<=0||dt>.1)return;
        float pitchRate,yawRate;
        switch(rotation) {
        case Surface.ROTATION_90:pitchRate=event.values[1];yawRate=-event.values[0];break;
        case Surface.ROTATION_180:pitchRate=-event.values[0];yawRate=-event.values[1];break;
        case Surface.ROTATION_270:pitchRate=-event.values[1];yawRate=event.values[0];break;
        default:pitchRate=event.values[0];yawRate=event.values[1];
        }
        // Counts per degree are calibrated independently of render FPS.
        input.relativeLook(cameraCounts(gyroDelta(yawRate,dt)*gyroSensitivity),cameraCounts(gyroDelta(pitchRate,dt)*gyroSensitivity));
    }
    // UT3 on this device: 24.9975 units/count; BioShock: 62.49375. Keep the same angular response.
    static float cameraCounts(float value) { return value*4; }
    static float gyroDelta(float rate,double dt) {
        if(!Float.isFinite(rate)||dt<=0||dt>.1)return 0;
        float degrees=(float)(rate*180/Math.PI*dt),dead=(float)(.4*dt);
        return Math.copySign(Math.min(25,Math.max(0,Math.abs(degrees)-dead)),degrees);
    }

    // ---------------------------------------------------------- accessibility
    private static final int NODE_PIE=100,NODE_PIE_CENTER=120,NODE_TOOL=200;
    private String nodeLabel(int id) {
        if(id==NODE_TOOL+TOOL_RESET)return text("Reset layout, tap twice","Сбросить раскладку, нажмите дважды");
        if(id>=NODE_TOOL)return toolLabel(id-NODE_TOOL);
        if(id==NODE_PIE_CENTER)return text("Close","Закрыть");
        if(id>=NODE_PIE)return pieLabel(pieItems[id-NODE_PIE],true);
        return accessibilityLabel(id);
    }
    private boolean nodeValid(int id) {
        if(pieOpen)return id==NODE_PIE_CENTER||(id>=NODE_PIE&&id<NODE_PIE+pieItems.length);
        if(editing&&id>=NODE_TOOL&&id<NODE_TOOL+tools.length)return tools[id-NODE_TOOL]!=null;
        return id>=0&&id<TouchLayout.COUNT&&shown[id];
    }
    private Rect nodeBounds(int id) {
        float x,y,r;
        if(id>=NODE_TOOL){float[] t=tools[id-NODE_TOOL];return new Rect((int)t[0],(int)t[1],(int)t[2],(int)t[3]);}
        if(id==NODE_PIE_CENTER){x=pieX;y=pieY;r=pieR*.36f;}
        else if(id>=NODE_PIE) {
            double a=TouchLayout.sectorAngle(id-NODE_PIE,pieItems.length);
            x=pieX+(float)Math.sin(a)*pieRing();y=pieY-(float)Math.cos(a)*pieRing();r=24*dp;
        } else {x=controlX[id];y=controlY[id];r=controlR[id];}
        return new Rect((int)(x-r),(int)(y-r),(int)(x+r),(int)(y+r));
    }
    private void accessibilityClick(int id) {
        if(id>=NODE_TOOL)tool(id-NODE_TOOL);
        else if(id==NODE_PIE_CENTER)closePie();
        else if(id>=NODE_PIE){int item=pieItems[id-NODE_PIE];closePie();pieSelect(item);}
        else if(editing){editControl=id;openButtonOptions();}
        else {
            int a=action(id);
            if(a==MORE||a==SEATS)openPie(-1,controlX[id],controlY[id],a==MORE?PIE_ACTIONS:PIE_ABILITIES,id);
            else if(a==WEAPON)input.pulse(TouchInput.RELOAD);
            else if(a==TouchInput.SCORES)input.toggle(TouchInput.SCORES);
            else if(a<0||a==TouchInput.PAUSE)activate(id);
            else if(a>0)input.pulse(a);
            if(pieOpen)pieSticky=true;
        }
        invalidate();
    }
    private int nodeAt(float x,float y) {
        if(pieOpen) {
            int hit=TouchLayout.pieHit(x,y,pieX,pieY,pieR*.36f,pieR,pieItems.length);
            return hit==TouchLayout.CENTER?NODE_PIE_CENTER:hit>=0?NODE_PIE+hit:-1;
        }
        if(editing){int tool=toolHit(x,y);if(tool>=0)return NODE_TOOL+tool;}
        return hit(x,y);
    }
    private void accessibilityEvent(int id,int type) {
        if(id<0||getParent()==null||!nodeValid(id))return;
        AccessibilityEvent event=AccessibilityEvent.obtain(type);
        event.setSource(this,id);event.setPackageName(getContext().getPackageName());
        event.setContentDescription(nodeLabel(id));getParent().requestSendAccessibilityEvent(this,event);
    }
    @Override public boolean dispatchHoverEvent(MotionEvent event) {
        AccessibilityManager manager=(AccessibilityManager)getContext().getSystemService(Context.ACCESSIBILITY_SERVICE);
        if(manager!=null&&manager.isTouchExplorationEnabled()) {
            int next=event.getActionMasked()==MotionEvent.ACTION_HOVER_EXIT?-1:nodeAt(event.getX(),event.getY());
            if(next!=hovered) {
                accessibilityEvent(hovered,AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);hovered=next;
                accessibilityEvent(hovered,AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
            }
            return next>=0;
        }
        return super.dispatchHoverEvent(event);
    }
    private final AccessibilityNodeProvider accessibility=new AccessibilityNodeProvider() {
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
            if(layoutDirty)buildLayout();
            if(id==View.NO_ID) {
                AccessibilityNodeInfo info=AccessibilityNodeInfo.obtain(GameControls.this);onInitializeAccessibilityNodeInfo(info);
                if(pieOpen) {
                    for(int i=0;i<pieItems.length;++i)info.addChild(GameControls.this,NODE_PIE+i);
                    info.addChild(GameControls.this,NODE_PIE_CENTER);
                } else {
                    for(int i=0;i<TouchLayout.COUNT;++i)if(shown[i])info.addChild(GameControls.this,i);
                    if(editing)for(int i=0;i<tools.length;++i)if(tools[i]!=null)info.addChild(GameControls.this,NODE_TOOL+i);
                }
                return info;
            }
            if(!nodeValid(id))return null;
            AccessibilityNodeInfo info=AccessibilityNodeInfo.obtain();info.setSource(GameControls.this,id);info.setParent(GameControls.this);
            info.setPackageName(getContext().getPackageName());info.setClassName("android.widget.Button");
            String name=nodeLabel(id);
            info.setText(name);info.setContentDescription(name);info.setEnabled(true);info.setFocusable(true);
            info.setAccessibilityFocused(accessibilityFocus==id);
            info.setClickable(true);info.setVisibleToUser(isShown());
            Rect bounds=nodeBounds(id);info.setBoundsInParent(bounds);
            int[] location=new int[2];getLocationOnScreen(location);bounds.offset(location[0],location[1]);info.setBoundsInScreen(bounds);
            info.addAction(AccessibilityNodeInfo.ACTION_CLICK);info.addAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
            info.addAction(AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS);return info;
        }
        @Override public boolean performAction(int id,int action,Bundle args) {
            if(!nodeValid(id))return false;
            if(action==AccessibilityNodeInfo.ACTION_CLICK) { accessibilityClick(id);return true; }
            if(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS||action==AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
                accessibilityFocus=action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?id:-1;
                accessibilityEvent(id,action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?
                    AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED:AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                return true;
            }
            return false;
        }
    };
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return accessibility; }
    @Override public boolean performClick() { super.performClick();return true; }
}
