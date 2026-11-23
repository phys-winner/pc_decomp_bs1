function PortText(English, Russian)
{
   return _root.PortRussian ? Russian : English;
}
function PortSectionKeys(Page)
{
   if(Page == 1) return ["fpsLimit","hdr","uiScale","playerFov","renderScale","upscaler","anisotropy","weaponFov"];
   if(Page == 3) return ["graphicsSettings","controlSettings","inputSettings","gameSettings"];
   if(Page == 5) return ["physicsRate","cutsceneFps","fixScrounger","mouseRaw"];
   if(Page == 4) return ["touchEnabled","touchPrompts","touchUse"];
   return ["touchSensitivity","touchAcceleration","touchInvertY","touchGyro","touchGyroSensitivity","touchVitals",
      "touchScale","touchOpacity","touchLeftHanded","touchMore","editLayout","resetLayout"];
}
function PortSectionSplit(Page)
{
   // Rows before the split fill the left column; the rest the right one.
   return Page == 2 ? 6 : (Page == 1 ? 4 : 0);
}
function PortChoices(Key)
{
   if(Key == "renderer") return ["vulkan","vulkan-turnip","vulkan-panvk","gles"];
   if(Key == "fpsLimit")
   {
      var Text = _root.PortValues.fpsChoices;
      if(Text == undefined) return ["0"];
      // The original String prototype supports indexOf/substring, but has no split method.
      var Choices = [];
      var Begin = 0;
      var End = Text.indexOf(",",Begin);
      while(End >= 0)
      {
         Choices[Choices.length] = Text.substring(Begin,End);
         Begin = End + 1;
         End = Text.indexOf(",",Begin);
      }
      Choices[Choices.length] = Text.substring(Begin);
      return Choices;
   }
   if(Key == "mobileShaders" || Key == "hdr" || Key == "touchAcceleration" || Key == "touchInvertY" || Key == "touchLeftHanded" || Key == "touchMore" || Key == "touchVitals" || Key == "touchEnabled" || Key == "touchUse" || Key == "touchPrompts" || Key == "cutsceneFps" || Key == "fixScrounger" || Key == "mouseRaw") return ["0","1"];
   if(Key == "touchGyro") return ["0","1","2","3","4"];
   if(Key == "renderScale") return ["50","60","70","80","90","100"];
   if(Key == "physicsRate") return ["30","60"];
   if(Key == "anisotropy") return ["0","4","8","16"];
   if(Key == "upscaler") return ["0","1"];
   if(Key == "uiScale") return ["1","1.1","1.2","1.3","1.4","1.5"];
   if(Key == "touchScale") return ["0.65","0.8","0.9","1","1.1","1.2","1.4","1.6"];
   if(Key == "touchOpacity") return ["0.2","0.35","0.5","0.65","0.8","1"];
   if(Key == "touchGyroSensitivity") return ["0.1","0.25","0.5","0.75","1","1.5","2","3","4","5"];
   return ["0.04","0.06","0.1","0.15","0.22","0.3","0.4","0.6","0.8","1","1.5","2","3"];
}
function PortCaption(Key)
{
   var Value = _root.PortValues[Key];
   // This player's AS1 equality also holds for "0" == undefined: a "0" value reads as "..." here.
   if(Value == undefined) Value = "...";
   if(Key == "renderer") return _root.PortText("Renderer: ","Рендер: ") + (Value == "gles" ? "OpenGL ES" : (Value == "vulkan" ? "Vulkan" : (Value == "vulkan-turnip" ? "Vulkan (Turnip)" : (Value == "vulkan-panvk" ? "Vulkan (PanVK)" : Value))));
   if(Key == "fpsLimit") return _root.PortText("FPS limit: ","Ограничение FPS: ") + (Number(Value) == 0 ? _root.PortText("Display refresh rate","Частота дисплея") : Math.round(Number(Value) * 2) / 2);
   if(Key == "renderScale") return _root.PortText("Render scale: ","Масштаб рендера: ") + Value + "%";
   if(Key == "playerFov") return _root.PortText("Player FOV: ","Обзор игрока: ") + Value + _root.PortText(" deg"," град.");
   if(Key == "weaponFov") return _root.PortText("Weapon FOV: ","Обзор оружия: ") + Value + _root.PortText(" deg"," град.");
   if(Key == "upscaler") return _root.PortText("Upscaling: ","Масштабирование: ") +
      (Value == "1" || Value == "2" ? "FSR 1" : _root.PortText("Off","Выкл."));
   if(Key == "touchEnabled") return _root.PortText("On-screen controls: ","Наэкранное управление: ") + _root.PortBoolean(Value);
   if(Key == "touchUse") return _root.PortText("Use button: ","Кнопка Использовать: ") + _root.PortBoolean(Value);
   if(Key == "touchPrompts") return _root.PortText("Touch prompts: ","Сенсорные подсказки: ") + _root.PortBoolean(Value);
   if(Key == "mobileShaders") return _root.PortText("Mobile shaders: ","Мобильные шейдеры: ") + _root.PortBoolean(Value);
   if(Key == "hdr") return _root.PortText("HDR rendering: ","HDR-рендеринг: ") + _root.PortBoolean(Value);
   if(Key == "uiScale") return _root.PortText("Interface scale: ","Масштаб интерфейса: ") + _root.PortPercent(Value);
   if(Key == "touchSensitivity") return _root.PortText("Sensitivity: ","Чувствительность: ") + Value;
   if(Key == "touchAcceleration") return _root.PortText("Look acceleration: ","Ускорение обзора: ") + _root.PortBoolean(Value);
   if(Key == "graphicsSettings") return _root.PortText("Graphics...","Графика...");
   if(Key == "controlSettings") return _root.PortText("Touch controls...","Сенсорное управление...");
   if(Key == "inputSettings") return _root.PortText("Input and prompts...","Ввод и подсказки...");
   if(Key == "gameSettings") return _root.PortText("Game and fixes...","Игра и исправления...");
   if(Key == "anisotropy") return _root.PortText("Anisotropy: ","Анизотропия: ") + (Number(Value) > 1 ? Math.round(Number(Value)) + "x" : _root.PortText("Game","Игра"));
   if(Key == "physicsRate") return _root.PortText("Physics: ","Физика: ") + (Value == "..." ? Value : Math.round(Number(Value)) + _root.PortText(" Hz"," Гц") + (Number(Value) == 30 ? _root.PortText(" (original)"," (оригинал)") : ""));
   if(Key == "cutsceneFps") return _root.PortText("Cutscenes at 60 FPS: ","Ролики до 60 FPS: ") + _root.PortBoolean(Value);
   if(Key == "fixScrounger") return _root.PortText("Keep container items: ","Защита предметов: ") + _root.PortBoolean(Value);
   if(Key == "mouseRaw") return _root.PortText("Raw mouse: ","Мышь без сглаживания: ") + _root.PortBoolean(Value);
   if(Key == "touchInvertY") return _root.PortText("Invert Y: ","Инверсия по Y: ") + _root.PortBoolean(Value);
   if(Key == "touchGyro") return _root.PortText("Gyroscope: ","Гироскоп: ") + (Number(Value) == 1 ? _root.PortText("While firing","При огне") : (Number(Value) == 2 ? _root.PortText("Always","Всегда") : (Number(Value) == 3 ? _root.PortText("While aiming","При прицеле") : (Number(Value) == 4 ? _root.PortText("Fire or aim","Огонь/прицел") : _root.PortText("Off","Выкл.")))));
   if(Key == "touchGyroSensitivity") return _root.PortText("Gyro sensitivity: ","Гироскоп, чувств.: ") + Value;
   if(Key == "touchScale") return _root.PortText("Size: ","Размер: ") + _root.PortPercent(Value);
   if(Key == "touchOpacity") return _root.PortText("Opacity: ","Непрозрачность: ") + _root.PortPercent(Value);
   if(Key == "touchLeftHanded") return _root.PortText("Left-handed: ","Для левши: ") + _root.PortBoolean(Value);
   if(Key == "touchMore") return _root.PortText("More button: ","Кнопка Еще: ") + _root.PortBoolean(Value);
   if(Key == "touchVitals" || Key == "touchEnabled" || Key == "touchUse" || Key == "touchPrompts") return _root.PortText("Health gauge: ","Шкала здоровья: ") + _root.PortBoolean(Value);
   if(Key == "editLayout") return _root.PortText("Configure buttons...","Настроить кнопки...");
   return _root.PortText("Reset positions","Сбросить расстановку");
}
function PortPercent(Value)
{
   return Value == "..." ? Value : Math.round(Number(Value) * 100) + "%";
}
function PortBoolean(Value)
{
   return Value == "1" ? _root.PortText("On","Вкл.") : _root.PortText("Off","Выкл.");
}
function PortSet(Key, Value)
{
   _root.PortValues[Key] = Value;
   _root.PortRender();
}
function PortEntryCaption()
{
   return _root.PortSectionPage == 2 ? _root.PortText("Touch controls...","Сенсорное управление...") : _root.PortText("Additional...","Дополнительно...");
}
function PortLayout()
{
   var Keys = _root.PortKeys;
   var Split = _root.PortSectionSplit(_root.PortPage);
   for(var Row = 0; Row < 12; Row++)
   {
      // Full paths only: this player loses clips kept in local variables.
      _root.PortPanel["Row" + Row]._visible = Row < Keys.length;
      if(Row == 3 || Row == 7)
      {
         _root.PortPanel["Row" + Row].Value._x = _root.PortPage == 1 ? -16384 : 0;
         _root.PortPanel["Row" + Row].Fov._x = _root.PortPage == 1 ? 0 : -16384;
      }
      if(!Split)
      {
         _root.PortPanel["Row" + Row]._x = 640;
         _root.PortPanel["Row" + Row]._y = 300 + Row * 70;
      }
      else if(Row < Split)
      {
         _root.PortPanel["Row" + Row]._x = 410;
         _root.PortPanel["Row" + Row]._y = 312 + Row * 56;
      }
      else
      {
         _root.PortPanel["Row" + Row]._x = 870;
         _root.PortPanel["Row" + Row]._y = 312 + (Row - Split) * 56;
      }
   }
   _root.PortPanel.ColumnLeft._visible = Split > 0;
   _root.PortPanel.ColumnRight._visible = Split > 0;
}
function PortRender()
{
   _root.PortEntry.Caption = _root.PortEntryCaption();
   if(!_root.PortPage) return;
   var Keys = _root.PortKeys;
   _root.maxSelections = Keys.length + 1;
   if(_root.selectorPos < 0 || _root.selectorPos >= _root.maxSelections) _root.selectorPos = 0;
   for(var Row = 0; Row < 12; Row++)
   {
      _root.PortPanel["Row" + Row].Value.Caption = Row < Keys.length ? _root.PortCaption(Keys[Row]) : "";
      if(_root.PortPage == 1 && (Row == 3 || Row == 7))
      {
         _root.PortPanel["Row" + Row].Fov.Caption = _root.PortCaption(Keys[Row]);
         var Low = Row == 3 ? 55 : 40;
         var High = Row == 3 ? 120 : 100;
         var Value = _root.PortValues[Keys[Row]];
         if(Value == undefined) Value = Row == 3 ? 75 : 60;
         _root.PortPanel["Row" + Row].Fov.Thumb._x = -126 + (Number(Value) - Low) * 252 / (High - Low);
      }
      // The left clip mirrors the glyph supported by the original text renderer.
      _root.PortPanel["Row" + Row].Left.Caption = ">";
      _root.PortPanel["Row" + Row].Right.Caption = ">";
      _root.PortPanel["Row" + Row].Focus._visible = _root.selectorPos == Row;
      _root.PortPanel["Row" + Row].Focus.gotoAndStop(2);
   }
   _root.PortPanel.Back.Focus._visible = _root.selectorPos == Keys.length;
   _root.PortPanel.Back.Focus.gotoAndStop(2);
   _root.PortPanel.Back.Caption = _root.PortText("Back","Назад");
   if(_root.PortPage == 3)
   {
      _root.PortPanel.Header = _root.PortText("ADDITIONAL PORT SETTINGS","ДОПОЛНИТЕЛЬНЫЕ НАСТРОЙКИ ПОРТА");
      _root.PortPanel.ColumnLeft.Caption = "";
      _root.PortPanel.ColumnRight.Caption = "";
   }
   else if(_root.PortPage == 1)
   {
      _root.PortPanel.Header = _root.PortText("ADDITIONAL GRAPHICS SETTINGS","ДОПОЛНИТЕЛЬНЫЕ НАСТРОЙКИ ГРАФИКИ");
      _root.PortPanel.ColumnLeft.Caption = "";
      _root.PortPanel.ColumnRight.Caption = "";
   }
   else if(_root.PortPage == 5)
   {
      _root.PortPanel.Header = _root.PortText("GAME AND FIXES","ИГРА И ИСПРАВЛЕНИЯ");
      _root.PortPanel.ColumnLeft.Caption = "";
      _root.PortPanel.ColumnRight.Caption = "";
   }
   else if(_root.PortPage == 4)
   {
      _root.PortPanel.Header = _root.PortText("INPUT AND PROMPTS","ВВОД И ПОДСКАЗКИ");
      _root.PortPanel.ColumnLeft.Caption = "";
      _root.PortPanel.ColumnRight.Caption = "";
   }
   else
   {
      _root.PortPanel.Header = _root.PortText("TOUCH CONTROLS","СЕНСОРНОЕ УПРАВЛЕНИЕ");
      _root.PortPanel.ColumnLeft.Caption = _root.PortText("LOOK","ОБЗОР");
      _root.PortPanel.ColumnRight.Caption = _root.PortText("BUTTONS","КНОПКИ");
   }
   _root.PortPanel.Status = _root.PortStatus != "" ? _root.PortStatus : (_root.PortPage == 5 ?
      _root.PortText("60 Hz physics is smoother and costs CPU. The fixes are on by default.",
         "Физика 60 Гц плавнее, но нагружает CPU. Исправления включены по умолчанию.") : (_root.PortPage == 4 ?
      _root.PortText("Tap screen center to use; hold it to hack. Changes are saved automatically.",
         "Касание центра — действие; удержание — взлом. Изменения сохраняются сразу.") : (_root.PortPage == 3 ?
      _root.PortText("Choose a section. Changes are saved automatically.","Выберите раздел. Изменения сохраняются сразу.") : (_root.PortPage == 1 ?
      _root.PortText("Anisotropy applies after a restart.","Анизотропия применяется после перезапуска.") :
      _root.PortText("Health gauge: tap for a first aid kit, hold for more. Changes are saved automatically.",
         "Шкала здоровья: касание — аптечка, удержание — меню Еще. Изменения сохраняются сразу.")))));
}
function PortFocus(Row)
{
   _root.selectorPos = Row;
   _root.PortRender();
}
function PortFovPress(Row)
{
   if(_root.PortPage != 1) return;
   _root.PortFovRow = Row;
   _root.PortFocus(Row);
   _root.PortFovTrack();
}
function PortFovTrack()
{
   var Row = _root.PortFovRow;
   if(Row < 0) return;
   var T = (_root._xmouse - _root.PortPanel["Row" + Row]._x + 126) / 252;
   if(T < 0) T = 0;
   if(T > 1) T = 1;
   _root.PortValues[_root.PortKeys[Row]] = Row == 3 ? 55 + Math.round(T * 65) : 40 + Math.round(T * 60);
}
function PortFovRelease()
{
   var Row = _root.PortFovRow;
   if(Row < 0) return;
   _root.PortFovTrack();
   _root.PortFovRow = -1;
   var Key = _root.PortKeys[Row];
   fscommand("PortSettings",Key + "=" + _root.PortValues[Key]);
}
function PortAdjust(Row, Direction)
{
   if(!_root.PortPage) return;
   var Key = _root.PortKeys[Row];
   if(Key == "playerFov" || Key == "weaponFov")
   {
      var Low = Key == "playerFov" ? 55 : 40;
      var High = Key == "playerFov" ? 120 : 100;
      var Value = _root.PortValues[Key];
      if(Value == undefined) Value = Key == "playerFov" ? 75 : 60;
      Value = Number(Value) + Direction;
      if(Value < Low) Value = Low;
      if(Value > High) Value = High;
      fscommand("PortSettings",Key + "=" + Value);
      return;
   }
   if(Key == "graphicsSettings" || Key == "controlSettings" || Key == "inputSettings" || Key == "gameSettings")
   {
      _root.PortOpenSection(Key == "graphicsSettings" ? 1 : (Key == "controlSettings" ? 2 : (Key == "gameSettings" ? 5 : 4)));
      return;
   }
   if(Key == "editLayout" || Key == "resetLayout")
   {
      if(Key == "editLayout" && _root.bInMainMenu)
      {
         _root.PortStatus = _root.PortText("Enter the game before positioning controls.","Войдите в игру, чтобы расположить кнопки.");
         return;
      }
      fscommand("PortSettings",Key + "=1");
      _root.PortStatus = Key == "editLayout" ? _root.PortText("Return to the game to edit buttons.","Вернитесь в игру, чтобы настроить кнопки.") : _root.PortText("Button layout reset.","Раскладка кнопок сброшена.");
      if(Key == "editLayout") fscommand("ResumeSelected");
      return;
   }
   var Choices = _root.PortChoices(Key);
   var Current = _root.PortValues[Key];
   var Position = 0;
   var Distance = 100000;
   for(var Index = 0; Index < Choices.length; Index++)
   {
      if(Choices[Index] == Current) { Position = Index; break; }
      if(Key != "renderer" && Math.abs(Number(Choices[Index]) - Number(Current)) < Distance)
      {
         Position = Index;
         Distance = Math.abs(Number(Choices[Index]) - Number(Current));
      }
   }
   Position = (Position + Direction + Choices.length) % Choices.length;
   if(Key == "hdr") _root.PortStatus = Choices[Position] == "0" ? _root.PortText("Without HDR: deeper blacks and more contrast, no haze.","Без HDR: глубже черный и выше контраст, без дымки.") : "";
   if(Key == "fixScrounger") _root.PortStatus = Choices[Position] == "1" ? _root.PortText("Search again (Scrounger) only re-rolls empty containers: no lost quest items.",
      "Обыскать еще (Scrounger) - только пустые: квестовые предметы не пропадут.") :
      _root.PortText("Original: a re-roll can delete items, Langford's safe included.","Оригинал: повторный обыск может удалить предметы, и в сейфе Лэнгфорда.");
   if(Key == "cutsceneFps") _root.PortStatus = _root.PortText("Above 60 FPS the original cutscenes can be misframed.","Выше 60 FPS оригинальные ролики могут сбиваться в кадре.");
   if(Key == "mouseRaw") _root.PortStatus = _root.PortText("For a connected mouse; turning it off applies from the next level.","Для подключенной мыши; выключение — со следующего уровня.");
   fscommand("PortSettings",Key + "=" + Choices[Position]);
   fscommand("TriggerEffectEvent","Event=UIFrontEndAccepted, Tag=");
}
function PortShow(Page)
{
   _root.PortFovRow = -1;
   _root.PortPage = Page;
   _root.PortStatus = "";
   for(var Index = 0; Index < _root.PortOriginalNames.length; Index++)
   {
      _root[_root.PortOriginalNames[Index]]._visible = Page == 0;
   }
   _root.PortEntry._visible = Page == 0;
   // This player still hit-tests descendants of an invisible parent clip.
   _root.PortPanel._x = Page == 0 ? -16384 : 0;
   _root.PortPanel._visible = Page != 0;
   if(!Page)
   {
      _root.maxSelections = _root.PortOriginalSelections;
      _root.ResetList();
      _root.PortRender();
      return;
   }
   _root.PortKeys = _root.PortSectionKeys(Page);
   _root.maxSelections = _root.PortKeys.length + 1;
   _root.selectorPos = 0;
   _root.ScrollBar0._visible = false;
   _root.PortLayout();
   _root.PortRender();
   fscommand("PortSettings","refresh");
}
function AcceptOrSelect()
{
   if(!_root.PortPage) { _root.PortOriginal_AcceptOrSelect(); return; }
   if(_root.selectorPos < _root.PortKeys.length) _root.PortAdjust(_root.selectorPos,1);
   else _root.CancelOrExit();
}
function CancelOrExit()
{
   if(!_root.PortPage) { _root.PortOriginal_CancelOrExit(); return; }
   fscommand("TriggerEffectEvent","Event=UIFrontEndBack, Tag=");
   if(_root.PortFromAdditional)
   {
      if(_root.PortPage != 3) _root.PortOpenSection(3);
      else { _root.PortFromAdditional = false; _root.SwitchToPCOptionsScreen(); }
      return;
   }
   _root.PortShow(0);
}
function UIHighlightLeft()
{
   if(!_root.PortPage) _root.PortOriginal_UIHighlightLeft();
   else if(_root.selectorPos < _root.PortKeys.length) _root.PortAdjust(_root.selectorPos,-1);
}
function UIHighlightUp()
{
   if(_root._currentframe != _root.PortSectionFrame || !_root.PortPage) _root.PortOriginal_UIHighlightUp();
   else _root.PortFocus((_root.selectorPos + _root.maxSelections - 1) % _root.maxSelections);
}
function UIHighlightDown()
{
   if(_root._currentframe != _root.PortSectionFrame || !_root.PortPage) _root.PortOriginal_UIHighlightDown();
   else _root.PortFocus((_root.selectorPos + 1) % _root.maxSelections);
}
function UIHighlightRight()
{
   if(!_root.PortPage) _root.PortOriginal_UIHighlightRight();
   else if(_root.selectorPos < _root.PortKeys.length) _root.PortAdjust(_root.selectorPos,1);
}
function OnEnterFrame()
{
   if(_root.PortFovRow >= 0) _root.PortFovTrack();
   _root.PortRender();
   if(!_root.PortPage) _root.PortOriginal_OnEnterFrame();
}
_root.PortValues = new Object();
_root.PortPage = 0;
_root.PortSectionFrame = _root._currentframe;
_root.PortEntry.Caption = _root.PortEntryCaption();
_root.PortShow(_root.PortRequested == _root.PortSectionPage || (_root.PortRequested == 3 || _root.PortRequested == 4 || _root.PortRequested == 5) ? _root.PortRequested : 0);
_root.PortRequested = 0;
