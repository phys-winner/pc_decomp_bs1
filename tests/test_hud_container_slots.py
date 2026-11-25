"""The HUD must clear deferred empty-slot text when a loot panel closes."""
from pathlib import Path
import shutil
import subprocess
import unittest

from tools.patch_hud_layout import fix_container_slot_lifecycle

ROOT = Path(__file__).resolve().parents[1]

ORIGINAL = '''function OpenContainer()
{
   _root.centerBox.focusArea.gotoAndPlay(44);
   _root.centerBox.focusArea.hackFocus._visible = false;
}
function CloseContainer()
{
   _root.centerBox.focusArea.gotoAndPlay(105);
   _root.centerBox.focusArea.hackFocus._visible = true;
}
function SetContainerItem(index, id, friendlyName, count)
{
   var _loc1_ = index;
   var _loc2_ = _root;
   var _loc3_ = id;
   itemName = _loc3_;
   slotName = "itemName" + _loc1_;
   containerItem = "containerItem" + _loc1_;
   if(count > 1)
   {
      _loc2_.centerBox.focusArea[slotName][containerItem] = count + " " + friendlyName;
   }
   else
   {
      _loc2_.centerBox.focusArea[slotName][containerItem] = friendlyName;
   }
   containerQty = "containerQty" + _loc1_;
   _loc2_.centerBox.focusArea[containerQty] = count;
   _loc2_.centerBox.focusArea["itemIcon" + _loc1_].attachMovie(_loc3_,_loc3_,10);
}
'''


class HudContainerSlotsTest(unittest.TestCase):
    def test_empty_slot_labels_only_survive_while_container_is_open(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js unavailable")
        patched = fix_container_slot_lifecycle(ORIGINAL)
        harness = r'''
var slots=[];
for(var i=0;i<3;i++) {
  slots[i]={}; slots[i]["containerItem"+i]="stale";
}
var focus={gotoAndPlay:function(){},hackFocus:{},itemIcon0:{attachMovie:function(){}},
 itemIcon1:{attachMovie:function(){}},itemIcon2:{attachMovie:function(){}}};
for(var j=0;j<3;j++)focus["itemName"+j]=slots[j];
var _root={centerBox:{focusArea:focus}};
@SCRIPT@
OpenContainer();
SetContainerItem(0,"Item","Пусто",1);
SetContainerItem(1,"Item","Пусто",1);
SetContainerItem(2,"Item","Пусто",1);
if(slots.some(function(s,i){return s["containerItem"+i]!=="Пусто";}))throw new Error("open loot panel lost slot labels");
CloseContainer();
if(slots.some(function(s,i){return s["containerItem"+i]!=="";}))throw new Error("close left stale loot text");
SetContainerItem(0,"Item","late",1);
if(slots[0].containerItem0!=="")throw new Error("closed panel accepted late slot text");
'''.replace("@SCRIPT@", patched)
        result = subprocess.run([node, "-e", harness], cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
