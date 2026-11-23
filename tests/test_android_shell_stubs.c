#include <stdint.h>

void *ut3_dxvk_native_window(void) { return 0; }
int void_android_render_ready(void) { return 0; }
__attribute__((weak)) int void_android_display_size(int *w, int *h) { (void)w; (void)h; return 0; }
