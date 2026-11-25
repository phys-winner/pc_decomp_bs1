function PortContextBit(Type)
{
   if(Type == "UseFocus") return 1;
   if(Type == "HackFocus") return 2;
   if(Type == "GathererSaveFocus") return 4;
   if(Type == "GathererKillFocus") return 8;
   if(Type == "ReRollFocus") return 16;
   return 0;
}
function PortPublishContext()
{
   var Flags = _root.PortContextFlags;
   if(!(Flags >= 0 && Flags <= 31)) Flags = 0;
   fscommand("PortTouchContext",String(Flags));
}
function PortContextMessage(Msg)
{
   // This Flash player renders missing values as "Undefined" and does not compare them like JavaScript undefined.
   var Text = String(Msg);
   if(Text == "Undefined" || Text == "undefined" || Text == "Null" || Text == "null") return "";
   return Text;
}
function PortRememberContext(Type, Msg)
{
   var Bit = _root.PortContextBit(Type);
   if(Bit == 0) return;
   Msg = _root.PortContextMessage(Msg);
   _root["PortOriginal_" + Type] = Msg;
   var Before = _root.PortContextFlags;
   if(!(Before >= 0 && Before <= 31)) Before = 0;
   _root.PortContextFlags = Msg != "" ? Before | Bit : Before & ~Bit;
   if(_root.PortContextFlags != Before) _root.PortPublishContext();
}
function PortPrompt(Type, Msg)
{
   if(_root.PortContextBit(Type) == 0) return Msg;
   var Text = _root.PortContextMessage(Msg);
   var Mode = _root.PortTouchPromptMode;
   if(Mode == 0 || Text == "") return Text;
   var KeyStart = -1;
   var Begin = 0;
   var Next = Text.indexOf(" (");
   // Original indexOf's second argument counts UTF-8 bytes; substring keeps Cyrillic offsets consistent.
   while(Next >= 0)
   {
      KeyStart = Begin + Next;
      Begin = KeyStart + 2;
      Next = Text.substring(Begin).indexOf(" (");
   }
   if(KeyStart >= 0 && Text.substring(Text.length - 1) == ")") Text = Text.substring(0,KeyStart);
   if(Mode == 4) return Text;
   var Russian = _root.PortTouchLanguage == "rus";
   var Hack = Type == "HackFocus" || Type == "GathererKillFocus";
   var Rescue = Type == "GathererSaveFocus";
   // Private-use glyphs baked into both HUD movies by patch_hud_layout.py.
   if(Mode == 2) return Text + (Type == "UseFocus" ? " \uE063" : Hack ? " \uE064" : Rescue ? " \uE065" : " (RT)");
   if(Mode == 3)
   {
      if(Type == "UseFocus") return Text + " \uE060";
      if(Hack) return Text + " \uE061";
      if(Rescue) return Text + " \uE062";
      return Text + " (R2)";
   }
   if(Type == "UseFocus") return Text + (Russian ? " (тап по центру)" : " (tap screen center)");
   if(Type == "HackFocus") return Text + (Russian ? " (удерживайте центр)" : " (hold screen center)");
   if(Type == "GathererSaveFocus") return Text + (Russian ? " (кнопка Спасти)" : " (Rescue button)");
   if(Type == "GathererKillFocus") return Text + (Russian ? " (кнопка Собрать)" : " (Harvest button)");
   return Text + (Russian ? " (кнопка Обыскать)" : " (Search again button)");
}
function PortTouchPrompts(Mode, Language)
{
   _root.PortTouchPromptMode = Number(Mode);
   _root.PortTouchLanguage = Language;
   if(_root.PortTouchPromptMode == 4) _root.HandsIcons._visible = false;
   var Types = ["UseFocus","HackFocus","GathererSaveFocus","GathererKillFocus","ReRollFocus"];
   for(var Index = 0; Index < Types.length; Index++)
   {
      var Type = Types[Index];
      var Original = _root.PortContextMessage(_root["PortOriginal_" + Type]);
      if(Original != "") _root.HUDMessage(Type,Original);
   }
   if(_root.PortLayout != undefined) _root.PortLayout();
   _root.PortAmmoCounts();
   _root.PortPublishContext();
}
_root.PortContextFlags = 0;
_root.PortTouchPromptMode = 0;
_root.PortTouchLanguage = "int";
_root.PortOriginal_UseFocus = "";
_root.PortOriginal_HackFocus = "";
_root.PortOriginal_GathererSaveFocus = "";
_root.PortOriginal_GathererKillFocus = "";
_root.PortOriginal_ReRollFocus = "";

function PortIosSelect(Category, ClassName)
{
   if(Category == "weapon") fscommand("SelectWeapon", "class=" + ClassName);
   else if(Category == "plasmid") fscommand("SelectPlasmid", "class=" + ClassName);
   else if(Category == "eve" && ClassName == "BioAmmoHypo") fscommand("UseHypo", "class=BioAmmoHypo");
}

function AmmoSelectionMenu(Direction)
{
   _root.PortOriginalAmmoSelectionMenu(Direction);
   if(_root.PortTouchPromptMode == 4)
   {
      _root.AmmoButtonReleased();
      _root.ResetAmmoMenu();
   }
}

function PortAmmoCounts()
{
   if(_root.ammoScrollBar == undefined) return;
   // The compact iOS panel keeps only SelectedAmmoIcon's counter.
   for(var Index = 0; Index < 3; Index++)
   {
      var Menu = _root.ammoScrollBar["ammoMenu" + Index];
      if(Menu != undefined && Menu.count != undefined) Menu.count._visible = _root.PortTouchPromptMode != 4;
   }
}
