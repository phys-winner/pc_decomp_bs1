#ifndef BIOSHOCK_AUDIO_COMPAT_H
#define BIOSHOCK_AUDIO_COMPAT_H

#include <aaudio/AAudio.h>
#include <dlfcn.h>

static inline void aaudio_game_attributes(AAudioStreamBuilder *builder, aaudio_content_type_t content)
{
    /* Stream attributes arrived after Android 8's initial AAudio API. */
    void (*set_usage)(AAudioStreamBuilder *, aaudio_usage_t) = dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage");
    void (*set_content)(AAudioStreamBuilder *, aaudio_content_type_t) = dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setContentType");
    if (set_usage) set_usage(builder, AAUDIO_USAGE_GAME);
    if (set_content) set_content(builder, content);
}

#endif
