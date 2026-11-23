#include "com.h"

static const uint8_t IID_IGameExplorer[16] = {
    0x72, 0xFB, 0xB2, 0xE7, 0x28, 0xD7, 0xB3, 0x49,
    0xA5, 0xF2, 0x18, 0xEB, 0xF5, 0xF1, 0x34, 0x9E,
};
static const uint8_t IID_IUnknown_GE[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46,
};

COM_METHOD(ge, QueryInterface, 3)
{
    if (void_guid_equal(ARG(1), IID_IGameExplorer) || void_guid_equal(ARG(1), IID_IUnknown_GE)) {
        void_com_addref((void_com_object *)void_com_host(THIS_));
        MEM32(ARG(2)) = THIS_;
        return S_OK_;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(ge, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(ge, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(ge, Unused3, 1) { return E_NOTIMPL_; }
COM_METHOD(ge, Unused4, 1) { return E_NOTIMPL_; }
COM_METHOD(ge, Unused5, 1) { return E_NOTIMPL_; }
COM_METHOD(ge, CheckAccess, 3)
{
    if (!ARG(2) || !void_vm_writable(ARG(2), 4)) return E_POINTER_;
    MEM32(ARG(2)) = 1;
    return S_OK_;
}

static const void_fn ge_methods[] = {
    ge_QueryInterface, ge_AddRef, ge_Release, ge_Unused3, ge_Unused4, ge_Unused5, ge_CheckAccess,
};
static void_com_class ge_class = { "IGameExplorer", 7, ge_methods, 0 };

uint32_t bioshock_game_explorer_create(void)
{
    void_com_object *object = (void_com_object *)calloc(1, sizeof *object);
    return object ? void_com_wrap(object, &ge_class, 0) : 0;
}
