#include "chooser.h"

#include <glib.h>
#include <string.h>

void sfe_chooser_free(SfeChooser* chooser)
{
    if (chooser == NULL)
        return;
    g_free(chooser->title);
    g_free(chooser->folder);
    g_free(chooser->name);
    g_free(chooser->accept_label);
    g_free(chooser->parent);
    g_free(chooser->filters);
    g_free(chooser->choices);
    g_free(chooser);
}

bool sfe_chooser_parse_mode(const char* text, SfeChooseMode* mode)
{
    static const struct {
        const char* name;
        SfeChooseMode mode;
    } modes[] = { { "open", SFE_CHOOSE_OPEN }, { "save", SFE_CHOOSE_SAVE }, { "folder", SFE_CHOOSE_FOLDER } };
    for (size_t i = 0; text != NULL && i < G_N_ELEMENTS(modes); i++) {
        if (strcmp(text, modes[i].name) == 0) {
            *mode = modes[i].mode;
            return true;
        }
    }
    return false;
}
