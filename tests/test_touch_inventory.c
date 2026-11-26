/* Reuse the live-movie fixture and its populated player inventory. */
#define main baseline_touch_tests
#include "test_bioshock_touch.c"
#undef main

static int cinematic;
int void_bioshock_is_cinematic(void) { return cinematic; }

int main(void)
{
    assert(baseline_touch_tests() == 0);
    touch_live_pawn = PAWN;
    touch_hud_movie = HUD;
    menu_scene = 0;
    menu_picker = 0;
    uint32_t pistol = touch_class(MEM32(PAWN + 2204));
    uint32_t weapon_root = MEM32(MEM32(pistol + 56) + 56);
    uint32_t camera = class("ResearchCamera", weapon_root);
    put(0x1E4000 + 44, camera);
    put(0x1D8000 + 5 * 4, 0x1E4000);
    put(PAWN + 2244, 6);
    put(PAWN + 2248, 6);
    void_bioshock_touch_options(1, 1);
    uint32_t snapshot[TOUCH_INVENTORY_WORDS];
    unsigned size = void_touch_inventory(snapshot, 1u << 28);
    assert(size == TOUCH_INVENTORY_HEADER + 18);
    assert(snapshot[0] == 5 && snapshot[1] == 4 && snapshot[2] == 2 && snapshot[3] == 2 && snapshot[4] == 1);
    const unsigned images[] = {1, 2, 4, 7, 8, 2, 5, 8, 11};
    for (unsigned i = 0; i < sizeof images / sizeof *images; ++i)
        assert(snapshot[TOUCH_INVENTORY_HEADER + i * 2] == images[i]);
    assert(snapshot[5] == snapshot[TOUCH_INVENTORY_HEADER + 3]);
    assert(snapshot[6] == snapshot[TOUCH_INVENTORY_HEADER + 11]);
    uint32_t shotgun = snapshot[TOUCH_INVENTORY_HEADER + 5];
    uint32_t winter = snapshot[TOUCH_INVENTORY_HEADER + 15];
    assert(void_touch_inventory_request((11 << 8) | 4, shotgun) == 1);
    assert(!strcmp(ios_category, "weapon") && !strcmp(ios_item, "Shotgun"));
    assert(void_touch_inventory_request((12 << 8) | 8, winter) == 1);
    assert(!strcmp(ios_category, "plasmid") && !strcmp(ios_item, "IcicleAssaultAbility"));
    assert(void_touch_inventory_request((11 << 8) | 4, winter) == -1);
    assert(void_touch_inventory_request((12 << 8) | 4, shotgun) == -1);
    put(PAWN + 2244, 2);
    assert(void_touch_inventory_request((11 << 8) | 4, shotgun) == -1);

    /* Restore the item to isolate context checks. */
    put(PAWN + 2244, 6);
    for (unsigned test = 0; test < 7; ++test) {
        touch_live_pawn = test == 0 ? 0 : PAWN;
        touch_hud_movie = test == 1 ? 0 : HUD;
        menu_scene = test == 2 ? PAUSE : 0;
        menu_picker = test == 3;
        touch_scene_input = test == 4 ? SCENE_INPUT_MOVEMENT : SCENE_INPUT_NORMAL;
        cinematic = test == 5;
        void_bioshock_touch_options(1, test == 6 ? 4 : 1);
        assert(void_touch_inventory(snapshot, 0) == 0);
        assert(void_touch_inventory_request((11 << 8) | 4, shotgun) == -1);
    }
    puts("PASS Classic inventory order, current classes, validated selection and rejected stale items and contexts");
    return 0;
}
