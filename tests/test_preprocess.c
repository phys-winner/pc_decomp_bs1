#include "mojoshader.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char source[] =
        "#define USING_PER_PIXEL_LIGHTING\n"
        "#define USING_PER_PIXEL_LIGHTING\n"
        "#define LITERAL(_type, _name, _default) static _type _name = _default;\n"
        "LITERAL(mtbMaskBindInfo, EdgeOpacityBind, { 0, 0 })\n";
    const MOJOSHADER_preprocessData *data = MOJOSHADER_preprocess(NULL, source,
        (unsigned int)strlen(source), NULL, 0, NULL, NULL, NULL, NULL, NULL);
    if (!data || data->error_count) {
        if (data && data->error_count) fprintf(stderr, "%s\n", data->errors[0].error);
        return 1;
    }
    if (!strstr(data->output, "static") || !strstr(data->output, "mtbMaskBindInfo") ||
        !strstr(data->output, "EdgeOpacityBind")) {
        fprintf(stderr, "unexpected preprocessed output:\n%s\n", data->output);
        return 2;
    }
    MOJOSHADER_freePreprocessData(data);
    puts("PASS HLSL initializer braces preserve three macro arguments");
    return 0;
}
