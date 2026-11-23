function FillProgressBar(ProgressPercentage)
{
   _root.PortLoadingProgress = Math.max(0, Math.min(1, Number(ProgressPercentage)));
   _root.PortLoadingFrame();
}
function PortLoadingFrame()
{
   if(!_root.PortLoadingReady)
   {
      // Frame actions precede placement; the original filler is also offstage.
      if(!(_root.ProgressBarFiller._width > 0) || !(_root.ProgressBarNew._y >= 0)) { return; }
      _root.ProgressBarFillerWidth = _root.ProgressBarFiller._width;
      _root.PortLoadingBarX = _root.ProgressBarFiller._x;
      _root.PortLoadingReady = true;
   }
   // Full paths only: this player loses clips kept in local variables.
   _root.ProgressBarFiller._y = _root.ProgressBarNew._y;
   _root.ProgressBarNew._visible = false;
   var Width = _root.ProgressBarFillerWidth;
   if(_root.PortLoadingProgress > 0)
   {
      _root.ProgressBarFiller._x = _root.PortLoadingBarX;
      _root.ProgressBarFiller._width = Width * _root.PortLoadingProgress;
   }
   else
   {
      // A moving segment means busy; it does not invent a loading percentage.
      var Phase = (getTimer() % 1800) / 900;
      if(Phase > 1) { Phase = 2 - Phase; }
      _root.ProgressBarFiller._width = Width * 0.18;
      _root.ProgressBarFiller._x = _root.PortLoadingBarX + Width * 0.82 * (Phase - 0.5);
   }
}
_root.PortLoadingReady = false;
_root.PortLoadingProgress = 0;
_root.onEnterFrame = function() { _root.PortLoadingFrame(); };
_root.PortLoadingFrame();
