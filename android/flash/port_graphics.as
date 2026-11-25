function PortGraphicsSet(Key, Value)
{
   if(Key == "renderResolutions")
   {
      _root.PortRenderScales = [];
      _root.PortRenderLabels = new Object();
      var Begin = 0;
      while(Begin < Value.length)
      {
         var End = Value.indexOf(",",Begin);
         if(End < 0) End = Value.length;
         var Equals = Value.indexOf("=",Begin);
         var Scale = Value.substring(Begin,Equals);
         _root.PortRenderScales[_root.PortRenderScales.length] = Scale;
         _root.PortRenderLabels[Scale] = Value.substring(Equals + 1,End);
         Begin = End + 1;
      }
   }
   if(Key == "renderScale") _root.PortRenderScale = Value;
   if(Key == "uiScale")
   {
      _root.PortInterfaceScale = Number(Value);
      _root.PortMenuLayout();
   }
   else _root.PortUpdateResolution();
}
function PortUpdateResolution()
{
   if(!_root.PortRenderScales) return;
   var Count = _root.PortRenderScales.length;
   if(_root._currentframe != 7 || Count <= 0) return;
   var Index = 0;
   while(Index < Count - 1 && _root.PortRenderScales[Index] != _root.PortRenderScale) Index++;
   var Label = _root.PortRenderLabels[_root.PortRenderScales[Index]];
   _root.NumResolutionOptions = Math.max(2,Count);
   _root.UpdateSliderPos(Count == 1 ? 1 : Index,_root.NumResolutionOptions,_root.PCScreenResolution);
   _root.DoubleTab11.OptionLabel = (_root.PortRussian ? "Разрешение: " : "Resolution: ") + Label;
}
function PortPickResolution(Value, Step)
{
   if(!_root.PortRenderScales) return;
   var Count = _root.PortRenderScales.length;
   if(Count <= 0) return;
   if(Step != 0)
   {
      Value = 0;
      while(Value < Count - 1 && _root.PortRenderScales[Value] != _root.PortRenderScale) Value++;
      Value = (Value + Step + Count) % Count;
   }
   var Choice = _root.PortRenderScales[Math.max(0,Math.min(Count - 1,Value))];
   if(Choice != _root.PortRenderScale)
   {
      _root.PortRenderScale = Choice;
      fscommand("PortSettings","renderScale=" + Choice);
   }
}
function PortMenuLayout()
{
   var Frame = _root._currentframe;
   var Scale = _root.PortInterfaceScale;
   var Large = Frame >= 6 && Frame <= 10 && Scale >= 1.2;
   var Section = _root.PortPage && Frame == _root.PortSectionFrame;
   var ShowBack = Frame >= 7 && Frame <= 10 && !Section;
   _root.PortBack._visible = ShowBack;
   _root.PortBack._x = ShowBack ? 185 : -16384;
   _root.PortBack.Caption = _root.PortRussian ? "Назад" : "Back";
   var ClassicAspect = _root.YResolution / _root.XResolution == .75;
   var XScale = Large && !ClassicAspect ? Math.min(Section ? 1.39 : 1.21,Scale) : 1;
   var YScale = Large ? Math.min(Section ? 1.5 : 1.33,Scale) : 1;
   var Left = (ClassicAspect ? -160 : 0) + 640 * (1 - XScale);
   if(Frame != _root.PortZoomFrame || Section != _root.PortZoomSection ||
      XScale != _root.PortZoomX || YScale != _root.PortZoomY || Left != _root.PortZoomLeft)
   {
      _root.PortZoomFrame = Frame;
      _root.PortZoomSection = Section;
      _root.PortZoomX = XScale;
      _root.PortZoomY = YScale;
      _root.PortZoomLeft = Left;
      _root._xscale = XScale * 100;
      _root._yscale = YScale * 100;
      _root._x = Left;
      // Anchor at the bottom so the original Apply/Back buttons remain on screen.
      _root._y = 720 * (1 - YScale);
      if(_root.PortTitleFace) _root.PortTitleFace._visible = !Large;
      if(_root.PortTitleShadow) _root.PortTitleShadow._visible = !Large;
      if(_root.PortTitleArt) _root.PortTitleArt._visible = !Large && !Section;
   }
}
