#include <stdbool.h>

/* fw declaration of this type since we ned it on some fn prototypes here. Will be defined in .c file.*/
struct dispmanx_surface;

void dispmanx_init(int src_width, int src_height, int src_bpp, int src_visible_pitch, bool keep_aspect);
void dispmanx_main_surface_update(const void *frame);
void dispmanx_update(const void *frame);
void dispmanx_videoquit();
