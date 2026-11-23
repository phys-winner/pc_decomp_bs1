_root._visible = false;

function PortPickerReveal()
{
   _root._visible = true;
}
function PortTouchSelect(Row, Step)
{
   var R = Number(Row);
   var Count = R == 0 ? _root.HandsIcons.weaponScrollBar.slotsFilled : _root.HandsIcons.abilityScrollBar.slotsFilled;
   _root.PortTouchClass = "";
   if(!(Count > 0)) return;
   var Slot = Math.max(0,Math.min(Count - 1,Math.floor((Count - 1) / 2) + Number(Step)));
   _root.PortTouchRow = R;
   _root.PortTouchSlot = Slot;
   for(var I = 0; I < _root.MaxWeapons; I++)
   {
      _root.HandsIcons.weaponScrollBar["wpnMenu" + I]._alpha = R == 0 && I == Slot ? 100 : 65;
   }
   for(var J = 0; J < _root.MaxAbilities; J++)
   {
      _root.HandsIcons.abilityScrollBar["abilityMenu" + J]._alpha = R == 1 && J == Slot ? 100 : 65;
   }
   if(R == 0)
   {
      _root.PortTouchClass = String(_root.HandsIcons.weaponScrollBar["wpnMenu" + Slot].contents.attachedMovieName);
      _root.SelectionName = _root.HandsIcons.weaponScrollBar["wpnMenu" + Slot].friendlyName;
      _root.HandsIcons.weaponScrollBar["wpnMenu" + Slot].gotoAndPlay(2);
   }
   else
   {
      _root.PortTouchClass = String(_root.HandsIcons.abilityScrollBar["abilityMenu" + Slot].contents.attachedMovieName);
      _root.SelectionName = _root.HandsIcons.abilityScrollBar["abilityMenu" + Slot].friendlyName;
      _root.HandsIcons.abilityScrollBar["abilityMenu" + Slot].gotoAndPlay(2);
   }
}
function PortTouchCommit()
{
   if(_root.PortTouchClass != undefined && _root.PortTouchClass != "" && _root.PortTouchClass != "Empty" && _root.PortTouchClass != "Cancel")
   {
      if(_root.PortTouchRow == 0) _root.SelectedWeapon(_root.PortTouchClass);
      else _root.SelectedAbility(_root.PortTouchClass);
   }
   _root.LBReleased();
}
