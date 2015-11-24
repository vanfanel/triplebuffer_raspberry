#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "raspberrypi.h"

#define src_width 384
#define src_height 118

extern struct dispmanx_vars *_dispvars;

void clear_screen (int width, int height, void* pixels) {
	// Clear screen
	
	int i;
	for (i = 0; i < width * height ; i++)
		((uint16_t *)pixels)[i] = 0x0000;
}

int main () {

	
	int i, j, k, m;
	/* In this example, the visible pitch and the total pitch are the same, but we could
	   have things the between the "useful" scanlines so in that case they would be different.*/
	int visible_pitch = (src_width * 2); /*2 bpp*/

	uint16_t *pixels = malloc (src_width * src_height * sizeof(uint16_t));
	dispmanx_init(src_width, src_height, 16, visible_pitch, false);
	
	clear_screen (src_width, src_height,  pixels);
	
	for (m = 0; m < 2; m++) {
		for (j = 0; j < src_width - 50; j++) {
			
			clear_screen (src_width, src_height,  pixels);
			
			for (i = 0; i < src_height; i++) {
				
				for (k = 0; k < 50; k++) 
					pixels[i * src_width + j + k] = 0x0FF0;
				
			}
		
			dispmanx_update(pixels);
		}
	}
	free (pixels);
	
	dispmanx_videoquit();
	
	return 0;
}
