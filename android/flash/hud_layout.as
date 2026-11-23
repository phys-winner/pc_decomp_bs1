function SetRes(XRes, YRes)
{
   var _loc1_ = _root;
   _loc1_.XResolution = XRes;
   _loc1_.YResolution = YRes;
   if(YRes / XRes == 0.75)
   {
      _loc1_.PortStageWidth = 960;
      _loc1_.CameraOverlay.CameraOverlay43.CameraOverlay43Contents._visible = true;
      _loc1_.CameraOverlay.CameraOverlay43.CameraIndicator._visible = true;
      _loc1_.CameraOverlay.CameraOverlay43.FocusAnimation._visible = true;
      _loc1_.CameraOverlay.CameraOverlay169.CameraOverlay169Contents._visible = false;
      _loc1_.CameraOverlay.CameraOverlay169.CameraIndicator._visible = false;
      _loc1_.CameraOverlay.CameraOverlay169.FocusAnimation._visible = false;
   }
   else
   {
      // The original 16:9 stage; the port widens it through PortHudLayout after native uniform scaling.
      _loc1_.PortStageWidth = 1280;
      _loc1_.CameraOverlay.CameraOverlay43.CameraOverlay43Contents._visible = false;
      _loc1_.CameraOverlay.CameraOverlay43.CameraIndicator._visible = false;
      _loc1_.CameraOverlay.CameraOverlay43.FocusAnimation._visible = false;
      _loc1_.CameraOverlay.CameraOverlay169.CameraOverlay169Contents._visible = true;
      _loc1_.CameraOverlay.CameraOverlay169.CameraIndicator._visible = true;
      _loc1_.CameraOverlay.CameraOverlay169.FocusAnimation._visible = true;
   }
   _loc1_.PortAspectLayout = 1;
   _loc1_.PortLayout();
}
function PortHudLayout(Width, Scale, Clear, Mode)
{
   // From the port after native uniform scaling: Width = visible stage width (0 keeps SetRes),
   // Scale = interface scale, Clear = stage x the top-left group must start after (the Pause button),
   // Mode 1 moves the whole stage for that clearance and pulls the other groups back.
   if(Number(Width) >= 960)
   {
      _root.PortStageWidth = Number(Width);
   }
   _root.PortHudScale = Number(Scale);
   _root.PortHudClear = Number(Clear);
   _root.PortHudMode = Number(Mode);
   _root.PortLayout();
}
function PortSaveOriginals()
{
   if(_root.PortSaved == 1)
   {
      return undefined;
   }
   _root.PortSaved = 1;
   if(_root.healthBar != undefined)
   {
      _root.PortX_healthBar = _root.healthBar._x;
      _root.PortY_healthBar = _root.healthBar._y;
      _root.PortXS_healthBar = _root.healthBar._xscale;
      _root.PortYS_healthBar = _root.healthBar._yscale;
   }
   if(_root.bioAmmoBar != undefined)
   {
      _root.PortX_bioAmmoBar = _root.bioAmmoBar._x;
      _root.PortY_bioAmmoBar = _root.bioAmmoBar._y;
      _root.PortXS_bioAmmoBar = _root.bioAmmoBar._xscale;
      _root.PortYS_bioAmmoBar = _root.bioAmmoBar._yscale;
   }
   if(_root.hypoScrollBar != undefined)
   {
      _root.PortX_hypoScrollBar = _root.hypoScrollBar._x;
      _root.PortY_hypoScrollBar = _root.hypoScrollBar._y;
      _root.PortXS_hypoScrollBar = _root.hypoScrollBar._xscale;
      _root.PortYS_hypoScrollBar = _root.hypoScrollBar._yscale;
   }
   if(_root.GathererHealth != undefined)
   {
      _root.PortX_GathererHealth = _root.GathererHealth._x;
      _root.PortY_GathererHealth = _root.GathererHealth._y;
      _root.PortXS_GathererHealth = _root.GathererHealth._xscale;
      _root.PortYS_GathererHealth = _root.GathererHealth._yscale;
   }
   if(_root.NeedleHolder != undefined)
   {
      _root.PortX_NeedleHolder = _root.NeedleHolder._x;
      _root.PortY_NeedleHolder = _root.NeedleHolder._y;
      _root.PortXS_NeedleHolder = _root.NeedleHolder._xscale;
      _root.PortYS_NeedleHolder = _root.NeedleHolder._yscale;
   }
   if(_root.flipTimer != undefined)
   {
      _root.PortX_flipTimer = _root.flipTimer._x;
      _root.PortY_flipTimer = _root.flipTimer._y;
      _root.PortXS_flipTimer = _root.flipTimer._xscale;
      _root.PortYS_flipTimer = _root.flipTimer._yscale;
   }
   if(_root.ammoScrollBar != undefined)
   {
      _root.PortX_ammoScrollBar = _root.ammoScrollBar._x;
      _root.PortY_ammoScrollBar = _root.ammoScrollBar._y;
      _root.PortXS_ammoScrollBar = _root.ammoScrollBar._xscale;
      _root.PortYS_ammoScrollBar = _root.ammoScrollBar._yscale;
   }
   if(_root.CurrentAbilityName != undefined)
   {
      _root.PortX_CurrentAbilityName = _root.CurrentAbilityName._x;
      _root.PortY_CurrentAbilityName = _root.CurrentAbilityName._y;
      _root.PortXS_CurrentAbilityName = _root.CurrentAbilityName._xscale;
      _root.PortYS_CurrentAbilityName = _root.CurrentAbilityName._yscale;
   }
   if(_root.centerBox.ReticleHolder != undefined)
   {
      _root.PortX_centerBox_ReticleHolder = _root.centerBox.ReticleHolder._x;
      _root.PortY_centerBox_ReticleHolder = _root.centerBox.ReticleHolder._y;
      _root.PortXS_centerBox_ReticleHolder = _root.centerBox.ReticleHolder._xscale;
      _root.PortYS_centerBox_ReticleHolder = _root.centerBox.ReticleHolder._yscale;
   }
   if(_root.centerBox.EnemyType != undefined)
   {
      _root.PortX_centerBox_EnemyType = _root.centerBox.EnemyType._x;
      _root.PortY_centerBox_EnemyType = _root.centerBox.EnemyType._y;
      _root.PortXS_centerBox_EnemyType = _root.centerBox.EnemyType._xscale;
      _root.PortYS_centerBox_EnemyType = _root.centerBox.EnemyType._yscale;
   }
   if(_root.centerBox.FrozenHealth != undefined)
   {
      _root.PortX_centerBox_FrozenHealth = _root.centerBox.FrozenHealth._x;
      _root.PortY_centerBox_FrozenHealth = _root.centerBox.FrozenHealth._y;
      _root.PortXS_centerBox_FrozenHealth = _root.centerBox.FrozenHealth._xscale;
      _root.PortYS_centerBox_FrozenHealth = _root.centerBox.FrozenHealth._yscale;
   }
   if(_root.centerBox.focusArea != undefined)
   {
      _root.PortX_centerBox_focusArea = _root.centerBox.focusArea._x;
      _root.PortY_centerBox_focusArea = _root.centerBox.focusArea._y;
      _root.PortXS_centerBox_focusArea = _root.centerBox.focusArea._xscale;
      _root.PortYS_centerBox_focusArea = _root.centerBox.focusArea._yscale;
   }
   if(_root.centerBox.ReRoll != undefined)
   {
      _root.PortX_centerBox_ReRoll = _root.centerBox.ReRoll._x;
      _root.PortY_centerBox_ReRoll = _root.centerBox.ReRoll._y;
      _root.PortXS_centerBox_ReRoll = _root.centerBox.ReRoll._xscale;
      _root.PortYS_centerBox_ReRoll = _root.centerBox.ReRoll._yscale;
   }
   if(_root.centerBox.GathererSaveFocus != undefined)
   {
      _root.PortX_centerBox_GathererSaveFocus = _root.centerBox.GathererSaveFocus._x;
      _root.PortY_centerBox_GathererSaveFocus = _root.centerBox.GathererSaveFocus._y;
      _root.PortXS_centerBox_GathererSaveFocus = _root.centerBox.GathererSaveFocus._xscale;
      _root.PortYS_centerBox_GathererSaveFocus = _root.centerBox.GathererSaveFocus._yscale;
   }
   if(_root.centerBox.GathererKillFocus != undefined)
   {
      _root.PortX_centerBox_GathererKillFocus = _root.centerBox.GathererKillFocus._x;
      _root.PortY_centerBox_GathererKillFocus = _root.centerBox.GathererKillFocus._y;
      _root.PortXS_centerBox_GathererKillFocus = _root.centerBox.GathererKillFocus._xscale;
      _root.PortYS_centerBox_GathererKillFocus = _root.centerBox.GathererKillFocus._yscale;
   }
   if(_root.centerBox.subtitleTextHolder != undefined)
   {
      _root.PortX_centerBox_subtitleTextHolder = _root.centerBox.subtitleTextHolder._x;
      _root.PortY_centerBox_subtitleTextHolder = _root.centerBox.subtitleTextHolder._y;
      _root.PortXS_centerBox_subtitleTextHolder = _root.centerBox.subtitleTextHolder._xscale;
      _root.PortYS_centerBox_subtitleTextHolder = _root.centerBox.subtitleTextHolder._yscale;
   }
   if(_root.centerBox.trainingTextHolder != undefined)
   {
      _root.PortX_centerBox_trainingTextHolder = _root.centerBox.trainingTextHolder._x;
      _root.PortY_centerBox_trainingTextHolder = _root.centerBox.trainingTextHolder._y;
      _root.PortXS_centerBox_trainingTextHolder = _root.centerBox.trainingTextHolder._xscale;
      _root.PortYS_centerBox_trainingTextHolder = _root.centerBox.trainingTextHolder._yscale;
   }
   if(_root.centerBox.AreaNameMovie != undefined)
   {
      _root.PortX_centerBox_AreaNameMovie = _root.centerBox.AreaNameMovie._x;
      _root.PortY_centerBox_AreaNameMovie = _root.centerBox.AreaNameMovie._y;
      _root.PortXS_centerBox_AreaNameMovie = _root.centerBox.AreaNameMovie._xscale;
      _root.PortYS_centerBox_AreaNameMovie = _root.centerBox.AreaNameMovie._yscale;
   }
   if(_root.centerBox.SubDrop != undefined)
   {
      _root.PortX_centerBox_SubDrop = _root.centerBox.SubDrop._x;
      _root.PortY_centerBox_SubDrop = _root.centerBox.SubDrop._y;
      _root.PortXS_centerBox_SubDrop = _root.centerBox.SubDrop._xscale;
      _root.PortYS_centerBox_SubDrop = _root.centerBox.SubDrop._yscale;
   }
   if(_root.rightBox.chatBoxMovie != undefined)
   {
      _root.PortX_rightBox_chatBoxMovie = _root.rightBox.chatBoxMovie._x;
      _root.PortY_rightBox_chatBoxMovie = _root.rightBox.chatBoxMovie._y;
      _root.PortXS_rightBox_chatBoxMovie = _root.rightBox.chatBoxMovie._xscale;
      _root.PortYS_rightBox_chatBoxMovie = _root.rightBox.chatBoxMovie._yscale;
   }
   if(_root.rightBox.AdamCount != undefined)
   {
      _root.PortX_rightBox_AdamCount = _root.rightBox.AdamCount._x;
      _root.PortY_rightBox_AdamCount = _root.rightBox.AdamCount._y;
      _root.PortXS_rightBox_AdamCount = _root.rightBox.AdamCount._xscale;
      _root.PortYS_rightBox_AdamCount = _root.rightBox.AdamCount._yscale;
   }
   if(_root.rightBox.CreditCount != undefined)
   {
      _root.PortX_rightBox_CreditCount = _root.rightBox.CreditCount._x;
      _root.PortY_rightBox_CreditCount = _root.rightBox.CreditCount._y;
      _root.PortXS_rightBox_CreditCount = _root.rightBox.CreditCount._xscale;
      _root.PortYS_rightBox_CreditCount = _root.rightBox.CreditCount._yscale;
   }
   if(_root.rightBox.UnreadLogsIndicator != undefined)
   {
      _root.PortX_rightBox_UnreadLogsIndicator = _root.rightBox.UnreadLogsIndicator._x;
      _root.PortY_rightBox_UnreadLogsIndicator = _root.rightBox.UnreadLogsIndicator._y;
      _root.PortXS_rightBox_UnreadLogsIndicator = _root.rightBox.UnreadLogsIndicator._xscale;
      _root.PortYS_rightBox_UnreadLogsIndicator = _root.rightBox.UnreadLogsIndicator._yscale;
   }
   if(_root.rightBox.QuestUnreadLogsIndicator != undefined)
   {
      _root.PortX_rightBox_QuestUnreadLogsIndicator = _root.rightBox.QuestUnreadLogsIndicator._x;
      _root.PortY_rightBox_QuestUnreadLogsIndicator = _root.rightBox.QuestUnreadLogsIndicator._y;
      _root.PortXS_rightBox_QuestUnreadLogsIndicator = _root.rightBox.QuestUnreadLogsIndicator._xscale;
      _root.PortYS_rightBox_QuestUnreadLogsIndicator = _root.rightBox.QuestUnreadLogsIndicator._yscale;
   }
   if(_root.rightBox.logHolder != undefined)
   {
      _root.PortX_rightBox_logHolder = _root.rightBox.logHolder._x;
      _root.PortY_rightBox_logHolder = _root.rightBox.logHolder._y;
      _root.PortXS_rightBox_logHolder = _root.rightBox.logHolder._xscale;
      _root.PortYS_rightBox_logHolder = _root.rightBox.logHolder._yscale;
   }
   if(_root.WidescreenBars != undefined)
   {
      _root.PortX_WidescreenBars = _root.WidescreenBars._x;
      _root.PortXS_WidescreenBars = _root.WidescreenBars._xscale;
   }
   if(_root.ShutterMovie != undefined)
   {
      _root.PortX_ShutterMovie = _root.ShutterMovie._x;
      _root.PortXS_ShutterMovie = _root.ShutterMovie._xscale;
   }
   if(_root.CameraOverlay != undefined)
   {
      _root.PortX_CameraOverlay = _root.CameraOverlay._x;
      _root.PortXS_CameraOverlay = _root.CameraOverlay._xscale;
   }
}
function PortLayout()
{
   var W = Number(_root.PortStageWidth);
   if(!(W >= 960))
   {
      return undefined;
   }
   PortSaveOriginals();
   var S = Number(_root.PortHudScale);
   if(!(S >= 1))
   {
      S = 1;
   }
   if(S > 1.5)
   {
      S = 1.5;
   }
   // The top-left group's own left edge (the hypo gear) starts at stage x 40.
   var Shift = Number(_root.PortHudClear) - 40 * S;
   if(!(Shift > 0))
   {
      Shift = 0;
   }
   // Mode 1: the stage itself moves right by the clearance (the gauges keep their own x,
   // which the original HUD script measures); everything else is pulled back.
   var Root = Number(_root.PortHudMode) == 1 ? Shift : 0;
   var Clip = Shift - Root;
   _root._x = Root;
   var CX = (W - 960) / 2 - Root;
   var RX = W - 960 - Root;
   var Wide = W > 1280 ? W / 1280 : 1;
   _root.centerBox._x = CX;
   _root.rightBox._x = RX;
   // Modal centre panels (weapon/plasmid selection, photo grade) are already screen-sized.
   _root.HandsIcons._x = CX;
   _root.PhotoDisplayBox._x = CX;
   // Letterbox bars and the shutter keep their full-width coverage; the viewfinder stays round.
   if(_root.WidescreenBars != undefined && _root.PortXS_WidescreenBars != undefined)
   {
      _root.WidescreenBars._x = _root.PortX_WidescreenBars * Wide - Root;
      _root.WidescreenBars._xscale = _root.PortXS_WidescreenBars * Wide;
   }
   if(_root.ShutterMovie != undefined && _root.PortXS_ShutterMovie != undefined)
   {
      _root.ShutterMovie._x = _root.PortX_ShutterMovie * Wide - Root;
      _root.ShutterMovie._xscale = _root.PortXS_ShutterMovie * Wide;
   }
   if(_root.CameraOverlay != undefined && _root.PortX_CameraOverlay != undefined)
   {
      _root.CameraOverlay._x = _root.PortX_CameraOverlay + (W > 1280 ? (W - 1280) / 2 : 0) - Root;
   }
   if(_root.healthBar != undefined && _root.PortXS_healthBar != undefined)
   {
      _root.healthBar._xscale = _root.PortXS_healthBar * S;
      _root.healthBar._yscale = _root.PortYS_healthBar * S;
      _root.healthBar._x = 0 + (_root.PortX_healthBar - 0) * S + Clip;
      _root.healthBar._y = 0 + (_root.PortY_healthBar - 0) * S;
   }
   if(_root.bioAmmoBar != undefined && _root.PortXS_bioAmmoBar != undefined)
   {
      _root.bioAmmoBar._xscale = _root.PortXS_bioAmmoBar * S;
      _root.bioAmmoBar._yscale = _root.PortYS_bioAmmoBar * S;
      _root.bioAmmoBar._x = 0 + (_root.PortX_bioAmmoBar - 0) * S + Clip;
      _root.bioAmmoBar._y = 0 + (_root.PortY_bioAmmoBar - 0) * S;
   }
   if(_root.hypoScrollBar != undefined && _root.PortXS_hypoScrollBar != undefined)
   {
      _root.hypoScrollBar._xscale = _root.PortXS_hypoScrollBar * S;
      _root.hypoScrollBar._yscale = _root.PortYS_hypoScrollBar * S;
      _root.hypoScrollBar._x = 0 + (_root.PortX_hypoScrollBar - 0) * S + Clip;
      _root.hypoScrollBar._y = 0 + (_root.PortY_hypoScrollBar - 0) * S;
   }
   if(_root.GathererHealth != undefined && _root.PortXS_GathererHealth != undefined)
   {
      _root.GathererHealth._xscale = _root.PortXS_GathererHealth * S;
      _root.GathererHealth._yscale = _root.PortYS_GathererHealth * S;
      _root.GathererHealth._x = 0 + (_root.PortX_GathererHealth - 0) * S + Clip;
      _root.GathererHealth._y = 0 + (_root.PortY_GathererHealth - 0) * S;
   }
   if(_root.NeedleHolder != undefined && _root.PortXS_NeedleHolder != undefined)
   {
      _root.NeedleHolder._xscale = _root.PortXS_NeedleHolder * S;
      _root.NeedleHolder._yscale = _root.PortYS_NeedleHolder * S;
      _root.NeedleHolder._x = 0 + (_root.PortX_NeedleHolder - 0) * S + Clip;
      _root.NeedleHolder._y = 0 + (_root.PortY_NeedleHolder - 0) * S;
   }
   if(_root.flipTimer != undefined && _root.PortXS_flipTimer != undefined)
   {
      _root.flipTimer._xscale = _root.PortXS_flipTimer * S;
      _root.flipTimer._yscale = _root.PortYS_flipTimer * S;
      _root.flipTimer._x = 0 + (_root.PortX_flipTimer - 0) * S - Root;
      _root.flipTimer._y = 360 + (_root.PortY_flipTimer - 360) * S;
   }
   if(_root.ammoScrollBar != undefined && _root.PortXS_ammoScrollBar != undefined)
   {
      _root.ammoScrollBar._xscale = _root.PortXS_ammoScrollBar * S;
      _root.ammoScrollBar._yscale = _root.PortYS_ammoScrollBar * S;
      _root.ammoScrollBar._x = 0 + (_root.PortX_ammoScrollBar - 0) * S - Root;
      _root.ammoScrollBar._y = 720 + (_root.PortY_ammoScrollBar - 720) * S;
      _root.ammoScrollBarDefaultY = _root.ammoScrollBar._y;
   }
   if(_root.CurrentAbilityName != undefined && _root.PortXS_CurrentAbilityName != undefined)
   {
      _root.CurrentAbilityName._xscale = _root.PortXS_CurrentAbilityName * S;
      _root.CurrentAbilityName._yscale = _root.PortYS_CurrentAbilityName * S;
      _root.CurrentAbilityName._x = 0 + (_root.PortX_CurrentAbilityName - 0) * S - Root;
      _root.CurrentAbilityName._y = 720 + (_root.PortY_CurrentAbilityName - 720) * S;
      _root.CurrentAbilityNameDefaultY = _root.CurrentAbilityName._y;
   }
   if(_root.centerBox.ReticleHolder != undefined && _root.PortXS_centerBox_ReticleHolder != undefined)
   {
      _root.centerBox.ReticleHolder._xscale = _root.PortXS_centerBox_ReticleHolder * S;
      _root.centerBox.ReticleHolder._yscale = _root.PortYS_centerBox_ReticleHolder * S;
      _root.centerBox.ReticleHolder._x = 480 + (_root.PortX_centerBox_ReticleHolder - 480) * S;
      _root.centerBox.ReticleHolder._y = 360 + (_root.PortY_centerBox_ReticleHolder - 360) * S;
   }
   if(_root.centerBox.EnemyType != undefined && _root.PortXS_centerBox_EnemyType != undefined)
   {
      _root.centerBox.EnemyType._xscale = _root.PortXS_centerBox_EnemyType * S;
      _root.centerBox.EnemyType._yscale = _root.PortYS_centerBox_EnemyType * S;
      _root.centerBox.EnemyType._x = 480 + (_root.PortX_centerBox_EnemyType - 480) * S;
      _root.centerBox.EnemyType._y = 360 + (_root.PortY_centerBox_EnemyType - 360) * S;
   }
   if(_root.centerBox.FrozenHealth != undefined && _root.PortXS_centerBox_FrozenHealth != undefined)
   {
      _root.centerBox.FrozenHealth._xscale = _root.PortXS_centerBox_FrozenHealth * S;
      _root.centerBox.FrozenHealth._yscale = _root.PortYS_centerBox_FrozenHealth * S;
      _root.centerBox.FrozenHealth._x = 480 + (_root.PortX_centerBox_FrozenHealth - 480) * S;
      _root.centerBox.FrozenHealth._y = 360 + (_root.PortY_centerBox_FrozenHealth - 360) * S;
   }
   if(_root.centerBox.focusArea != undefined && _root.PortXS_centerBox_focusArea != undefined)
   {
      _root.centerBox.focusArea._xscale = _root.PortXS_centerBox_focusArea * S;
      _root.centerBox.focusArea._yscale = _root.PortYS_centerBox_focusArea * S;
      _root.centerBox.focusArea._x = 480 + (_root.PortX_centerBox_focusArea - 480) * S;
      _root.centerBox.focusArea._y = 360 + (_root.PortY_centerBox_focusArea - 360) * S;
   }
   if(_root.centerBox.ReRoll != undefined && _root.PortXS_centerBox_ReRoll != undefined)
   {
      _root.centerBox.ReRoll._xscale = _root.PortXS_centerBox_ReRoll * S;
      _root.centerBox.ReRoll._yscale = _root.PortYS_centerBox_ReRoll * S;
      _root.centerBox.ReRoll._x = 480 + (_root.PortX_centerBox_ReRoll - 480) * S;
      _root.centerBox.ReRoll._y = 360 + (_root.PortY_centerBox_ReRoll - 360) * S;
   }
   if(_root.centerBox.GathererSaveFocus != undefined && _root.PortXS_centerBox_GathererSaveFocus != undefined)
   {
      _root.centerBox.GathererSaveFocus._xscale = _root.PortXS_centerBox_GathererSaveFocus * S;
      _root.centerBox.GathererSaveFocus._yscale = _root.PortYS_centerBox_GathererSaveFocus * S;
      _root.centerBox.GathererSaveFocus._x = 480 + (_root.PortX_centerBox_GathererSaveFocus - 480) * S;
      _root.centerBox.GathererSaveFocus._y = 360 + (_root.PortY_centerBox_GathererSaveFocus - 360) * S;
   }
   if(_root.centerBox.GathererKillFocus != undefined && _root.PortXS_centerBox_GathererKillFocus != undefined)
   {
      _root.centerBox.GathererKillFocus._xscale = _root.PortXS_centerBox_GathererKillFocus * S;
      _root.centerBox.GathererKillFocus._yscale = _root.PortYS_centerBox_GathererKillFocus * S;
      _root.centerBox.GathererKillFocus._x = 480 + (_root.PortX_centerBox_GathererKillFocus - 480) * S;
      _root.centerBox.GathererKillFocus._y = 360 + (_root.PortY_centerBox_GathererKillFocus - 360) * S;
   }
   if(_root.centerBox.subtitleTextHolder != undefined && _root.PortXS_centerBox_subtitleTextHolder != undefined)
   {
      _root.centerBox.subtitleTextHolder._xscale = _root.PortXS_centerBox_subtitleTextHolder * S;
      _root.centerBox.subtitleTextHolder._yscale = _root.PortYS_centerBox_subtitleTextHolder * S;
      _root.centerBox.subtitleTextHolder._x = 480 + (_root.PortX_centerBox_subtitleTextHolder - 480) * S;
      _root.centerBox.subtitleTextHolder._y = 720 + (_root.PortY_centerBox_subtitleTextHolder - 720) * S;
   }
   if(_root.centerBox.trainingTextHolder != undefined && _root.PortXS_centerBox_trainingTextHolder != undefined)
   {
      _root.centerBox.trainingTextHolder._xscale = _root.PortXS_centerBox_trainingTextHolder * S;
      _root.centerBox.trainingTextHolder._yscale = _root.PortYS_centerBox_trainingTextHolder * S;
      _root.centerBox.trainingTextHolder._x = 480 + (_root.PortX_centerBox_trainingTextHolder - 480) * S;
      _root.centerBox.trainingTextHolder._y = 720 + (_root.PortY_centerBox_trainingTextHolder - 720) * S;
   }
   if(_root.centerBox.AreaNameMovie != undefined && _root.PortXS_centerBox_AreaNameMovie != undefined)
   {
      _root.centerBox.AreaNameMovie._xscale = _root.PortXS_centerBox_AreaNameMovie * S;
      _root.centerBox.AreaNameMovie._yscale = _root.PortYS_centerBox_AreaNameMovie * S;
      _root.centerBox.AreaNameMovie._x = 480 + (_root.PortX_centerBox_AreaNameMovie - 480) * S;
      _root.centerBox.AreaNameMovie._y = 720 + (_root.PortY_centerBox_AreaNameMovie - 720) * S;
   }
   if(_root.centerBox.SubDrop != undefined && _root.PortXS_centerBox_SubDrop != undefined)
   {
      _root.centerBox.SubDrop._xscale = _root.PortXS_centerBox_SubDrop * S;
      _root.centerBox.SubDrop._yscale = _root.PortYS_centerBox_SubDrop * S;
      _root.centerBox.SubDrop._x = 480 + (_root.PortX_centerBox_SubDrop - 480) * S;
      _root.centerBox.SubDrop._y = 720 + (_root.PortY_centerBox_SubDrop - 720) * S;
   }
   if(_root.rightBox.chatBoxMovie != undefined && _root.PortXS_rightBox_chatBoxMovie != undefined)
   {
      _root.rightBox.chatBoxMovie._xscale = _root.PortXS_rightBox_chatBoxMovie * S;
      _root.rightBox.chatBoxMovie._yscale = _root.PortYS_rightBox_chatBoxMovie * S;
      _root.rightBox.chatBoxMovie._x = 960 + (_root.PortX_rightBox_chatBoxMovie - 960) * S;
      _root.rightBox.chatBoxMovie._y = 0 + (_root.PortY_rightBox_chatBoxMovie - 0) * S;
   }
   if(_root.rightBox.AdamCount != undefined && _root.PortXS_rightBox_AdamCount != undefined)
   {
      _root.rightBox.AdamCount._xscale = _root.PortXS_rightBox_AdamCount * S;
      _root.rightBox.AdamCount._yscale = _root.PortYS_rightBox_AdamCount * S;
      _root.rightBox.AdamCount._x = 960 + (_root.PortX_rightBox_AdamCount - 960) * S;
      _root.rightBox.AdamCount._y = 360 + (_root.PortY_rightBox_AdamCount - 360) * S;
   }
   if(_root.rightBox.CreditCount != undefined && _root.PortXS_rightBox_CreditCount != undefined)
   {
      _root.rightBox.CreditCount._xscale = _root.PortXS_rightBox_CreditCount * S;
      _root.rightBox.CreditCount._yscale = _root.PortYS_rightBox_CreditCount * S;
      _root.rightBox.CreditCount._x = 960 + (_root.PortX_rightBox_CreditCount - 960) * S;
      _root.rightBox.CreditCount._y = 360 + (_root.PortY_rightBox_CreditCount - 360) * S;
   }
   if(_root.rightBox.UnreadLogsIndicator != undefined && _root.PortXS_rightBox_UnreadLogsIndicator != undefined)
   {
      _root.rightBox.UnreadLogsIndicator._xscale = _root.PortXS_rightBox_UnreadLogsIndicator * S;
      _root.rightBox.UnreadLogsIndicator._yscale = _root.PortYS_rightBox_UnreadLogsIndicator * S;
      _root.rightBox.UnreadLogsIndicator._x = 960 + (_root.PortX_rightBox_UnreadLogsIndicator - 960) * S;
      _root.rightBox.UnreadLogsIndicator._y = 360 + (_root.PortY_rightBox_UnreadLogsIndicator - 360) * S;
   }
   if(_root.rightBox.QuestUnreadLogsIndicator != undefined && _root.PortXS_rightBox_QuestUnreadLogsIndicator != undefined)
   {
      _root.rightBox.QuestUnreadLogsIndicator._xscale = _root.PortXS_rightBox_QuestUnreadLogsIndicator * S;
      _root.rightBox.QuestUnreadLogsIndicator._yscale = _root.PortYS_rightBox_QuestUnreadLogsIndicator * S;
      _root.rightBox.QuestUnreadLogsIndicator._x = 960 + (_root.PortX_rightBox_QuestUnreadLogsIndicator - 960) * S;
      _root.rightBox.QuestUnreadLogsIndicator._y = 360 + (_root.PortY_rightBox_QuestUnreadLogsIndicator - 360) * S;
   }
   if(_root.rightBox.logHolder != undefined && _root.PortXS_rightBox_logHolder != undefined)
   {
      _root.rightBox.logHolder._xscale = _root.PortXS_rightBox_logHolder * S;
      _root.rightBox.logHolder._yscale = _root.PortYS_rightBox_logHolder * S;
      _root.rightBox.logHolder._x = 960 + (_root.PortX_rightBox_logHolder - 960) * S;
      // Keep the radio portrait above the lower-right touch controls.
      _root.rightBox.logHolder._y = 500 + (_root.PortY_rightBox_logHolder - 720) * S;
      _root.logHolderDefaultY = _root.rightBox.logHolder._y;
   }
   _root.SubYOffset = 100 * S;
}
