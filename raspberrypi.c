 /* A triple-buffering algorithm for Raspberry Pi graphics.   
  * Copyright (C) 2015 - Manuel Alfayate
  */

#include <bcm_host.h>
#include <pthread.h>
#include <stdbool.h>
#include "raspberrypi.h"

/* We have two "drawing surfaces" (abstract), one for running 
 * core and other for menu, each one backed by a dispmanx 
 * element and a set of buffers (resources in dispmanx terms). */
enum dispmanx_surface_type {
   MAIN_SURFACE,   /* Always the first surface */
   BACK_SURFACE    /* Always the last surface */
}; 

struct dispmanx_page
{
   /* Each page contains it's own resource handler 
    * instead of pointing to in by page number */
   DISPMANX_RESOURCE_HANDLE_T resource;
   bool used;
   /* Each page has it's own mutex for
    * isolating it's used flag access. */
   pthread_mutex_t page_used_mutex;

   /* This field will allow us to access the 
    * surface the page belongs to, for the vsync cb. */
   struct dispmanx_surface *surface;	
};

struct dispmanx_surface
{
   /* main surface has 3 pages, menu surface has 1 */
   unsigned int numpages;
   struct dispmanx_page *pages;
   /* The page that's currently on screen for this surface */
   struct dispmanx_page *current_page;
   unsigned int bpp;   

   VC_RECT_T src_rect;
   VC_RECT_T dst_rect;
   VC_RECT_T bmp_rect;
   
   /* Each surface has it's own element, and the resources are contained one in each page */
   DISPMANX_ELEMENT_HANDLE_T element;
   VC_DISPMANX_ALPHA_T alpha;    
   VC_IMAGE_TYPE_T pixformat;

   /* Surfaces with a higher layer will be on top of the ones with lower. Default is 0. */
   int layer;

   /* Internal frame dimensions */
   int width;
   int height;
   int pitch;

   /* External aspect for scaling */
   float aspect;

   /* Has the surface been setup already? */
   bool setup;
};

struct dispmanx_video
{
   uint64_t frame_count;
   DISPMANX_DISPLAY_HANDLE_T display;
   DISPMANX_UPDATE_HANDLE_T update;
   uint32_t vc_image_ptr;

   /* We abstract three "surfaces": main surface, menu surface and black back surface. */
   struct dispmanx_surface surfaces[3];

   /* For console blanking */
   int fb_fd;
   uint8_t *fb_addr;
   unsigned int screensize;
   uint8_t *screen_bck;

   /* Total dispmanx video dimensions. Not counting overscan settings. */
   unsigned int dispmanx_width;
   unsigned int dispmanx_height;

   /* For threading */
   pthread_cond_t  vsync_condition;	
   pthread_mutex_t vsync_cond_mutex;
   pthread_mutex_t pending_mutex;
   unsigned int pageflip_pending;

   /* Menu */
   bool menu_active;
} dispvars;

struct dispmanx_video dispvars;
struct dispmanx_video *_dispvars = &dispvars;

/* If no free page is available when called, wait for a page flip. */
static struct dispmanx_page *dispmanx_get_free_page(struct dispmanx_surface *surface) {
   unsigned i;
   struct dispmanx_page *page = NULL;

   while (!page)
   {
      /* Try to find a free page */
      for (i = 0; i < surface->numpages; ++i) {
         if (!surface->pages[i].used)
         {
            page = (surface->pages) + i;
            break;
         }
      }

      /* If no page is free at the moment,
       * wait until a free page is freed by vsync CB. */
      if (!page) {
	 pthread_mutex_lock(&_dispvars->vsync_cond_mutex);
	 pthread_cond_wait(&_dispvars->vsync_condition, &_dispvars->vsync_cond_mutex);
	 pthread_mutex_unlock(&_dispvars->vsync_cond_mutex);
      }
   }
   
   /* We mark the choosen page as used */
   pthread_mutex_lock(&page->page_used_mutex);
   page->used = true;
   pthread_mutex_unlock(&page->page_used_mutex);

   return page;
}

static void dispmanx_vsync_callback(DISPMANX_UPDATE_HANDLE_T u, void *data)
{
   struct dispmanx_page *page = data;
   struct dispmanx_surface *surface = page->surface;

   /* Marking the page as free must be done before the signaling
    * so when update_main continues (it won't continue until we signal) 
    * we can chose this page as free */
   if (surface->current_page) {
      pthread_mutex_lock(&surface->current_page->page_used_mutex);

      /* We mark as free the page that was visible until now */
      surface->current_page->used = false;

      pthread_mutex_unlock(&surface->current_page->page_used_mutex);
   }

  /* The page on which we issued the flip that
   * caused this callback becomes the visible one */
   surface->current_page = page;

   /* These two things must be isolated "atomically" to avoid getting 
    * a false positive in the pending_mutex test in update_main. */ 
   pthread_mutex_lock(&_dispvars->pending_mutex);
   
   _dispvars->pageflip_pending--;	
   pthread_cond_signal(&_dispvars->vsync_condition);
  
   pthread_mutex_unlock(&_dispvars->pending_mutex);
}

static void dispmanx_surface_setup(int width, int height, int pitch, float aspect,
   struct dispmanx_surface *surface)
{
   int i, dst_width, dst_height, dst_xpos, dst_ypos;

   /* Allocate memory for all the pages in each surface
    * and initialize variables inside each page's struct. */
   surface->pages = calloc(surface->numpages, sizeof(struct dispmanx_page));
   for (i = 0; i < surface->numpages; i++) {
      surface->pages[i].used = false;   
      surface->pages[i].surface = surface;   
      pthread_mutex_init(&surface->pages[i].page_used_mutex, NULL); 
   }

   /* Internal frame dimensions. Pitch is total pitch including info 
    * between scanlines */
   surface->width = width;
   surface->height = height;
   surface->pitch = pitch;
   surface->aspect = aspect; 
   surface->current_page = NULL; 
 
   /* The "visible" width obtained from the core pitch. We blit based on 
    * the "visible" width, for cores with things between scanlines. */
   int visible_width = pitch / surface->bpp;
 
   dst_width  = _dispvars->dispmanx_height * aspect;	
   dst_height = _dispvars->dispmanx_height;
   
   /* If we obtain a scaled image width that is bigger than the physical screen width,
   * then we keep the physical screen width as our maximun width. */
   if (dst_width > _dispvars->dispmanx_width) 
      dst_width = _dispvars->dispmanx_width;
   
   dst_xpos = (_dispvars->dispmanx_width - dst_width) / 2;
   dst_ypos = (_dispvars->dispmanx_height - dst_height) / 2;

   /* We configure the rects now. */
   vc_dispmanx_rect_set(&surface->dst_rect, dst_xpos, dst_ypos, dst_width, dst_height);
   vc_dispmanx_rect_set(&surface->bmp_rect, 0, 0, width, height);	
   vc_dispmanx_rect_set(&surface->src_rect, 0, 0, width << 16, height << 16);	

   for (i = 0; i < surface->numpages; i++) {
      surface->pages[i].resource = vc_dispmanx_resource_create(surface->pixformat, 
	    visible_width, height, &(_dispvars->vc_image_ptr));
   }
   /* Add element. */
   _dispvars->update = vc_dispmanx_update_start(0);

   surface->element = vc_dispmanx_element_add(
      _dispvars->update,_dispvars->display, surface->layer, 
      &surface->dst_rect, surface->pages[0].resource, 
      &surface->src_rect, DISPMANX_PROTECTION_NONE,
      &surface->alpha, 0, (DISPMANX_TRANSFORM_T)0);

   vc_dispmanx_update_submit_sync(_dispvars->update);		
}

static void dispmanx_surface_init( 
   int bpp,
   int pixformat,
   int layer,
   unsigned alpha,
   int numpages,
   struct dispmanx_surface *surface)
{
   /* Setup surface parameters */
   surface->numpages = numpages;
   surface->bpp = bpp;
   surface->layer = layer;
   surface->pixformat = pixformat;

   /* Internal frame dimensions. We leave them as 0 until we get 
    * to the gfx_frame function where we get image width and height. */
   surface->width  = 0;
   surface->height = 0;
   surface->pitch  = 0;

   /* Transparency disabled */
   surface->alpha.flags = DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS;
   surface->alpha.opacity = alpha;
   surface->alpha.mask = 0;

   /* This will be true when we have allocated mem for the pages and 
    * created their element, resources, etc.. */
   surface->setup = false;
}

/* Auxiliary function so we don't have to pass a surface to update in games where only
 * a surface is used (most cases), so we use this instead of dispmanx_surface_update()
 * and it destroys the logic of all this surfaces stuff, but it's adequate for most cases */
void dispmanx_main_surface_update(const void *frame)
{
   struct dispmanx_page *page = NULL;
   struct dispmanx_surface *surface = &_dispvars->surfaces[MAIN_SURFACE];  
  
   /* Wait until last issued flip completes to get a free page. Also, 
      dispmanx doesn't support issuing more than one pageflip.*/
   pthread_mutex_lock(&_dispvars->pending_mutex);
   if (_dispvars->pageflip_pending > 0)
   {
      pthread_cond_wait(&_dispvars->vsync_condition, &_dispvars->pending_mutex);
   }
   pthread_mutex_unlock(&_dispvars->pending_mutex);
  
   page = dispmanx_get_free_page(surface);
 
   /* Frame blitting */
   vc_dispmanx_resource_write_data(page->resource, surface->pixformat,
      surface->pitch, (void*)frame, &(surface->bmp_rect));
   
   /* Issue a page flip that will be done at the next vsync. */
   _dispvars->update = vc_dispmanx_update_start(0);

   vc_dispmanx_element_change_source(_dispvars->update, surface->element,
         page->resource);

   vc_dispmanx_update_submit(_dispvars->update, dispmanx_vsync_callback, (void*)page);

   pthread_mutex_lock(&_dispvars->pending_mutex);
   _dispvars->pageflip_pending++;	
   pthread_mutex_unlock(&_dispvars->pending_mutex);
}

void dispmanx_surface_update(const void *frame, struct dispmanx_surface *surface)
{
   struct dispmanx_page *page = NULL;
   
   /* Wait until last issued flip completes to get a free page. Also, 
      dispmanx doesn't support issuing more than one pageflip.*/
   pthread_mutex_lock(&_dispvars->pending_mutex);
   if (_dispvars->pageflip_pending > 0)
   {
      pthread_cond_wait(&_dispvars->vsync_condition, &_dispvars->pending_mutex);
   }
   pthread_mutex_unlock(&_dispvars->pending_mutex);
  
   page = dispmanx_get_free_page(surface);
 
   /* Frame blitting */
   vc_dispmanx_resource_write_data(page->resource, surface->pixformat,
      surface->pitch, (void*)frame, &(surface->bmp_rect));
   
   /* Issue a page flip that will be done at the next vsync. */
   _dispvars->update = vc_dispmanx_update_start(0);

   vc_dispmanx_element_change_source(_dispvars->update, surface->element,
         page->resource);

   vc_dispmanx_update_submit(_dispvars->update, dispmanx_vsync_callback, (void*)page);

   pthread_mutex_lock(&_dispvars->pending_mutex);
   _dispvars->pageflip_pending++;	
   pthread_mutex_unlock(&_dispvars->pending_mutex);
}

static void dispmanx_blank_console ()
{
   /* Note that a 2-pixels array is needed to accomplish console blanking because with 1-pixel
    * only the write data function doesn't work well, so when we do the only resource 
    * change in the surface update function, we will be seeing a distorted console. */
   uint16_t image[2] = {0x0000, 0x0000};
   float aspect = (float)_dispvars->dispmanx_width / (float)_dispvars->dispmanx_height;   

   dispmanx_surface_init(
      1, 
      VC_IMAGE_RGB565, 
      -1, 
      255, 
      1,
      &_dispvars->surfaces[BACK_SURFACE]);
   dispmanx_surface_setup(2, 2, 4, aspect, &_dispvars->surfaces[BACK_SURFACE]);
   dispmanx_surface_update(&image, &_dispvars->surfaces[BACK_SURFACE]);
}

void dispmanx_init(int src_width, int src_height, int src_bpp, int src_total_pitch, bool keep_aspect)
{
   float aspect;  

   bcm_host_init();
   _dispvars->display = vc_dispmanx_display_open(0 /* LCD */);
   /* If the console framebuffer has active overscan settings, 
    * the user must have overscan_scale=1 in config.txt to have 
    * the same size for both fb console and dispmanx. */
   graphics_get_display_size(_dispvars->display, &_dispvars->dispmanx_width, &_dispvars->dispmanx_height);

   if (keep_aspect) {
      aspect = (float)src_width / (float)src_height;
   } else {
      /* This is unnecesary but allows us to have a general case surface_setup function. */
      aspect = (float)_dispvars->dispmanx_width / (float)_dispvars->dispmanx_height;
   }

   /* Setup surface parameters */
   _dispvars->vc_image_ptr     = 0;
   _dispvars->pageflip_pending = 0;	
   _dispvars->menu_active      = false;
  
   /* Initialize the rest of the mutexes and conditions. */
   pthread_cond_init(&_dispvars->vsync_condition, NULL);
   pthread_mutex_init(&_dispvars->vsync_cond_mutex, NULL);
   pthread_mutex_init(&_dispvars->pending_mutex, NULL);

   /* We create the surfaces we need here. */
   dispmanx_blank_console();
   
   dispmanx_surface_init(
      src_bpp / 8, 
      src_bpp == 32 ? VC_IMAGE_XRGB8888 : VC_IMAGE_RGB565, 
      0   /* layer */, 
      255 /* alpha */, 
      3,  /* numpages */
      &_dispvars->surfaces[MAIN_SURFACE]);

   dispmanx_surface_setup(src_width, src_height, src_total_pitch, aspect,
      &_dispvars->surfaces[MAIN_SURFACE]);

}

static void dispmanx_surface_free(struct dispmanx_surface *surface)
{
   int i;	
   
   for (i = 0; i < surface->numpages; i++) { 
      vc_dispmanx_resource_delete(surface->pages[i].resource);
      surface->pages[i].used = false;   
      pthread_mutex_destroy(&surface->pages[i].page_used_mutex); 
   }
   
   free(surface->pages);
   
   _dispvars->update = vc_dispmanx_update_start(0);
   vc_dispmanx_element_remove(_dispvars->update, surface->element);
   vc_dispmanx_update_submit_sync(_dispvars->update);		

   surface->setup = false;
}

void dispmanx_videoquit()
{
   int i;
   for (i = 0; i < BACK_SURFACE; i++) {
      if (_dispvars->surfaces[i].setup) {
         dispmanx_surface_free(&_dispvars->surfaces[i]);
      }
   }

   /* Close display and deinitialize. */
   vc_dispmanx_display_close(_dispvars->display);
   bcm_host_deinit();

   /* Destroy mutexes and conditions. */
   pthread_mutex_destroy(&_dispvars->pending_mutex);
   pthread_mutex_destroy(&_dispvars->vsync_cond_mutex);
   pthread_cond_destroy(&_dispvars->vsync_condition);		
}
