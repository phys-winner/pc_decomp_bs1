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
   if(_root.PortTouchPromptMode != 1 || Text == "") return Text;
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
   var Russian = _root.PortTouchLanguage == "rus";
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
   var Types = ["UseFocus","HackFocus","GathererSaveFocus","GathererKillFocus","ReRollFocus"];
   for(var Index = 0; Index < Types.length; Index++)
   {
      var Type = Types[Index];
      var Original = _root.PortContextMessage(_root["PortOriginal_" + Type]);
      if(Original != "") _root.HUDMessage(Type,Original);
   }
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
