/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C) 2019-2021 - James Leaver
 *  Copyright (C)      2021 - John Parton
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <SDL/SDL.h>
#include <SDL/SDL_video.h>

#include <retro_assert.h>
#include <gfx/video_frame.h>
#include <retro_assert.h>
#include <string/stdstring.h>
#include <encodings/utf.h>
#include <features/features_cpu.h>

#include "gfx.c"
#include "scaler_neon.c"
#include <sdkdir/mi_sys.h>
#include <sys/mman.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../../dingux/dingux_utils.h"

#include "../../verbosity.h"
#include "../../gfx/drivers_font_renderer/bitmap.h"
#include "../../configuration.h"
#include "../../retroarch.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define SDL_MIYOOMINI_WIDTH  640
#define SDL_MIYOOMINI_HEIGHT 480
#define RGUI_MENU_WIDTH  320
#define RGUI_MENU_HEIGHT 240
#define SDL_NUM_FONT_GLYPHS 256

typedef struct sdl_miyoomini_video sdl_miyoomini_video_t;
struct sdl_miyoomini_video
{
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;
   SDL_Surface *screen;
   SDL_Surface *menuscreen;
   SDL_Surface *menuscreen_rgui;
   bitmapfont_lut_t *osd_font;
   /* Scaling/padding/cropping parameters */
   unsigned content_width;
   unsigned content_height;
   unsigned frame_width;
   unsigned frame_height;
   unsigned video_x;
   unsigned video_y;
   unsigned video_w;
   unsigned video_h;
   void (*scale_func)(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp);
   enum dingux_rs90_softfilter_type softfilter_type;
   uint32_t font_colour32;
   bool rgb32;
   bool vsync;
   bool keep_aspect;
   bool scale_integer;
   bool menu_active;
   bool was_in_menu;
   bool quitting;
};

/* for OSD text */
   sdl_miyoomini_video_t *vid_tmp;
   char msg_tmp[256];
   void *framebuffer;

static void sdl_miyoomini_init_font_color(sdl_miyoomini_video_t *vid)
{
   settings_t *settings = config_get_ptr();
   uint32_t red         = 0xFF;
   uint32_t green       = 0xFF;
   uint32_t blue        = 0xFF;

   if (settings)
   {
      red   = (uint32_t)((settings->floats.video_msg_color_r * 255.0f) + 0.5f) & 0xFF;
      green = (uint32_t)((settings->floats.video_msg_color_g * 255.0f) + 0.5f) & 0xFF;
      blue  = (uint32_t)((settings->floats.video_msg_color_b * 255.0f) + 0.5f) & 0xFF;
   }

   /* Convert to XRGB8888 */
   vid->font_colour32 = (red << 16) | (green << 8) | blue;
}

/* Print OSD text, direct draw to framebuffer, 32bpp, 2x, rotate180 */
static void sdl_miyoomini_print_msg(void) {
   /* Note: Cannot draw text in padding region
    * (padding region is never cleared, so
    * any text pixels would remain as garbage) */
   uint32_t screen_width  = vid_tmp->video_w;
   uint32_t screen_height = vid_tmp->video_h;
   int32_t x_pos          = screen_width - (FONT_WIDTH_STRIDE * 2);
   uint32_t y_pos         = (FONT_HEIGHT + FONT_WIDTH_STRIDE) * 2;
   void *screen_buf       = framebuffer
                          + ((vinfo.yoffset + vid_tmp->video_y) * SDL_MIYOOMINI_WIDTH * sizeof(uint32_t))
                          + (vid_tmp->video_x * sizeof(uint32_t));
   bool **font_lut        = vid_tmp->osd_font->lut;
   const char *str        = msg_tmp;

   if (flipFence) { MI_GFX_WaitAllDone(FALSE, flipFence); flipFencetmp = 0; } /* to prevent flicker, in gfx.c */

   while (!string_is_empty(str))
   {
      /* Check for out of bounds x coordinates */
      if (x_pos - (FONT_WIDTH_STRIDE * 2) - 1 < 0) return;

      /* Deal with spaces first, for efficiency */
      if (*str == ' ')
         str++;
      else
      {
         uint32_t i, j;
         bool *symbol_lut;
         uint32_t symbol = utf8_walk(&str);

         /* Stupid hack: 'oe' ligatures are not really
          * standard extended ASCII, so we have to waste
          * CPU cycles performing a conversion from the
          * unicode values... */
         if (symbol == 339) /* Latin small ligature oe */
            symbol = 156;
         if (symbol == 338) /* Latin capital ligature oe */
            symbol = 140;

         if (symbol >= SDL_NUM_FONT_GLYPHS)
            continue;

         symbol_lut = font_lut[symbol];

         for (j = 0; j < FONT_HEIGHT; j++)
         {
            uint32_t buff_offset = ((y_pos - (j * 2) ) * SDL_MIYOOMINI_WIDTH) + x_pos;

            for (i = 0; i < FONT_WIDTH; i++)
            {
               if (*(symbol_lut + i + (j * FONT_WIDTH)))
               {
                  uint32_t *screen_buf_ptr = (uint32_t*)screen_buf + buff_offset - (i * 2) - 2;

                  /* Text pixel + right shadow (1) */
                  screen_buf_ptr[+0] = 0;
                  screen_buf_ptr[+1] = 0;
                  screen_buf_ptr[+2] = vid_tmp->font_colour32;
                  screen_buf_ptr[+3] = vid_tmp->font_colour32;

                  /* Text pixel + right shadow (2) */
                  screen_buf_ptr[-SDL_MIYOOMINI_WIDTH+0] = 0;
                  screen_buf_ptr[-SDL_MIYOOMINI_WIDTH+1] = 0;
                  screen_buf_ptr[-SDL_MIYOOMINI_WIDTH+2] = vid_tmp->font_colour32;
                  screen_buf_ptr[-SDL_MIYOOMINI_WIDTH+3] = vid_tmp->font_colour32;

                  /* Bottom shadow (1) */
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*2)+0] = 0;
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*2)+1] = 0;
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*2)+2] = 0;
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*2)+3] = 0;

                  /* Bottom shadow (2) */
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*3)+0] = 0;
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*3)+1] = 0;
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*3)+2] = 0;
                  screen_buf_ptr[-(SDL_MIYOOMINI_WIDTH*3)+3] = 0;
               }
            }
         }
      }
      x_pos -= FONT_WIDTH_STRIDE * 2;
   }
}

static void sdl_miyoomini_gfx_free(void *data)
{
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;

   if (!vid)
      return;

   if (vid->osd_font)
      bitmapfont_free_lut(vid->osd_font);

   if (vid->screen) { GFX_FreeSurface(vid->screen); vid->screen = NULL; }
   if (vid->menuscreen) { GFX_FreeSurface(vid->menuscreen); vid->menuscreen = NULL; }
   if (vid->menuscreen_rgui) { GFX_FreeSurface(vid->menuscreen_rgui); vid->menuscreen_rgui = NULL; }
   if (framebuffer) { munmap(framebuffer, finfo.smem_len); framebuffer = NULL; }
   GFX_Quit();

   free(vid);
}

static void sdl_miyoomini_input_driver_init(
      const char *input_driver_name, const char *joypad_driver_name,
      input_driver_t **input, void **input_data)
{
   /* Sanity check */
   if (!input || !input_data)
      return;

   *input      = NULL;
   *input_data = NULL;

   /* If input driver name is empty, cannot
    * initialise anything... */
   if (string_is_empty(input_driver_name))
      return;

   if (string_is_equal(input_driver_name, "sdl_dingux"))
   {
      *input_data = input_driver_init_wrap(&input_sdl_dingux,
            joypad_driver_name);

      if (*input_data)
         *input = &input_sdl_dingux;

      return;
   }

#if defined(HAVE_SDL) || defined(HAVE_SDL2)
   if (string_is_equal(input_driver_name, "sdl"))
   {
      *input_data = input_driver_init_wrap(&input_sdl,
            joypad_driver_name);

      if (*input_data)
         *input = &input_sdl;

      return;
   }
#endif

#if defined(HAVE_UDEV)
   if (string_is_equal(input_driver_name, "udev"))
   {
      *input_data = input_driver_init_wrap(&input_udev,
            joypad_driver_name);

      if (*input_data)
         *input = &input_udev;

      return;
   }
#endif

#if defined(__linux__)
   if (string_is_equal(input_driver_name, "linuxraw"))
   {
      *input_data = input_driver_init_wrap(&input_linuxraw,
            joypad_driver_name);

      if (*input_data)
         *input = &input_linuxraw;

      return;
   }
#endif
}

/* Clear border x3 screens in framebuffer (rotate180) */
static void sdl_miyoomini_clear_border(unsigned x, unsigned y, unsigned w, unsigned h) {
   if ( (x == 0) && (y == 0) && (w == SDL_MIYOOMINI_WIDTH) && (h == SDL_MIYOOMINI_HEIGHT) ) return;
   if ( (w == 0) || (h == 0) ) { GFX_ClearFrameBuffer(); return; }

   uint32_t x0 = SDL_MIYOOMINI_WIDTH - (x + w); /* left margin , right margin = x */
   uint32_t y0 = SDL_MIYOOMINI_HEIGHT - (y + h); /* top margin , bottom margin = y */
   uint32_t sl = x0 * sizeof(uint32_t); /* left buffer size */
   uint32_t sr = x * sizeof(uint32_t); /* right buffer size */
   uint32_t st = y0 * SDL_MIYOOMINI_WIDTH * sizeof(uint32_t); /* top buffer size */
   uint32_t sb = y * SDL_MIYOOMINI_WIDTH * sizeof(uint32_t); /* bottom buffer size */
   uint32_t srl = sr + sl;
   uint32_t stl = st + sl;
   uint32_t srb = sr + sb;
   uint32_t srbtl = srl + sb + st;
   uint32_t sw = w * sizeof(uint32_t); /* pitch */
   uint32_t ss = SDL_MIYOOMINI_WIDTH * sizeof(uint32_t); /* stride */
   uintptr_t fbPa = finfo.smem_start; /* framebuffer physical address (from gfx.c) */
   uint32_t i;

   if (stl) MI_SYS_MemsetPa(fbPa, 0, stl); /* 1st top + 1st left */
   fbPa += stl + sw;
   for (i=h-1; i>0; i--, fbPa += ss) {
      if (srl) MI_SYS_MemsetPa(fbPa, 0, srl); /* right + left */
   }
   if (srbtl) MI_SYS_MemsetPa(fbPa, 0, srbtl); /* last right + bottom + top + 1st left */
   fbPa += srbtl + sw;
   for (i=h-1; i>0; i--, fbPa += ss) {
      if (srl) MI_SYS_MemsetPa(fbPa, 0, srl); /* right + left */
   }
   if (srbtl) MI_SYS_MemsetPa(fbPa, 0, srbtl); /* last right + bottom + top + 1st left */
   fbPa += srbtl + sw;
   for (i=h-1; i>0; i--, fbPa += ss) {
      if (srl) MI_SYS_MemsetPa(fbPa, 0, srl); /* right + left */
   }
   if (srb) MI_SYS_MemsetPa(fbPa, 0, srb); /* last right + last bottom */
}

static void sdl_miyoomini_set_output(
      sdl_miyoomini_video_t* vid,
      unsigned width, unsigned height, bool rgb32)
{
   vid->content_width  = width;
   vid->content_height = height;

   /* Calculate scaling factor */
   uint32_t xmul = (SDL_MIYOOMINI_WIDTH<<16) / width;
   uint32_t ymul = (SDL_MIYOOMINI_HEIGHT<<16) / height;
   uint32_t mul = xmul < ymul ? xmul : ymul;
   uint32_t mul_int = (mul>>16);

   /* Select upscaler to use */
   uint32_t scale_mul = 1;
   if ((vid->scale_integer)||(vid->softfilter_type == DINGUX_RS90_SOFTFILTER_POINT)) {
      if (mul >= (640<<16)/256) scale_mul = 4;		/* w <= 256 & h <= 192 */
      else if (mul >= (640<<16)/512) scale_mul = 2;	/* w <= 512 & h <= 384 */
   }
   vid->frame_width = width * scale_mul;
   vid->frame_height = height * scale_mul;
   switch (scale_mul) {
      case 2:
         vid->scale_func = rgb32 ? scale2x_n32 : scale2x_n16;
         break;
      case 4:
         vid->scale_func = rgb32 ? scale4x_n32 : scale4x_n16;
         break;
      default:
         vid->scale_func = rgb32 ? scale1x_n32 : scale1x_n16;
         break;
   }

   /* Change to aspect/fullscreen scaler when integer & screen size is over (no crop) */
   if ( (vid->scale_integer) && (!mul_int) ) {
      vid->scale_integer = false;
   }

   if (vid->scale_integer) {
      /* Integer Scaling */
      vid->video_w = width * mul_int;
      vid->video_h = height * mul_int;
      if (!vid->keep_aspect) {
         /* Integer + Fullscreen , keep 4:3 for CRT console emulators */
         uint32_t Wx3 = vid->video_w * 3;
         uint32_t Hx4 = vid->video_h * 4;
         if (Wx3 != Hx4) {
            if (Wx3 > Hx4) vid->video_h = Wx3 / 4;
            else           vid->video_w = Hx4 / 3;
         }
      }
      vid->video_x = (SDL_MIYOOMINI_WIDTH - vid->video_w) >> 1;
      vid->video_y = (SDL_MIYOOMINI_HEIGHT - vid->video_h) >> 1;
   } else if (vid->keep_aspect) {
      /* Aspect Scaling */
      if (xmul > ymul) {
         vid->video_w  = (width * SDL_MIYOOMINI_HEIGHT) / height;
         vid->video_h = SDL_MIYOOMINI_HEIGHT;
         vid->video_x = (SDL_MIYOOMINI_WIDTH - vid->video_w) >> 1;
         vid->video_y = 0;
      } else {
         vid->video_w  = SDL_MIYOOMINI_WIDTH;
         vid->video_h = (height * SDL_MIYOOMINI_WIDTH) / width;
         vid->video_x = 0;
         vid->video_y = (SDL_MIYOOMINI_HEIGHT - vid->video_h) >> 1;
      }
   } else {
      /* Fullscreen */
      vid->video_w = SDL_MIYOOMINI_WIDTH;
      vid->video_h = SDL_MIYOOMINI_HEIGHT;
      vid->video_x = 0;
      vid->video_y = 0;
   }
/* for DEBUG
   fprintf(stderr,"cw:%d ch:%d fw:%d fh:%d x:%d y:%d w:%d h:%d mul:%f scale_mul:%d\n",width,height,
         vid->frame_width,vid->frame_height,vid->video_x,vid->video_y,vid->video_w,vid->video_h,(float)mul/(1<<16),scale_mul);
*/
   /* Attempt to change video mode */
   if (vid->screen) GFX_FreeSurface(vid->screen);
   vid->screen = GFX_CreateRGBSurface(
         0, vid->frame_width, vid->frame_height, rgb32 ? 32 : 16, 0, 0, 0, 0);

   /* Check whether selected display mode is valid */
   if (unlikely(!vid->screen)) RARCH_ERR("[MI_GFX]: Failed to init GFX surface\n");
   else {
      /* Clear border */
      GFX_WaitAllDone();
      sdl_miyoomini_clear_border(vid->video_x, vid->video_y, vid->video_w, vid->video_h);
   }
}

static void *sdl_miyoomini_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   sdl_miyoomini_video_t *vid                    = NULL;
   uint32_t sdl_subsystem_flags                  = SDL_WasInit(0);
   settings_t *settings                          = config_get_ptr();
   const char *input_driver_name                 = settings->arrays.input_driver;
   const char *joypad_driver_name                = settings->arrays.input_joypad_driver;

   /* Initialise graphics subsystem, if required */
   if (sdl_subsystem_flags == 0)
   {
      if (SDL_Init(SDL_INIT_VIDEO) < 0)
         return NULL;
   }
   else if ((sdl_subsystem_flags & SDL_INIT_VIDEO) == 0)
   {
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
         return NULL;
   }

   vid = (sdl_miyoomini_video_t*)calloc(1, sizeof(*vid));
   if (!vid)
      return NULL;
   vid_tmp = vid;

   vid->ff_frame_time_min = 16667;

   GFX_Init();
   if (framebuffer) munmap(framebuffer, finfo.smem_len);
   framebuffer = mmap(0, finfo.smem_len, PROT_WRITE, MAP_SHARED, fd_fb, 0);
   if (vid->menuscreen) GFX_FreeSurface(vid->menuscreen);
   vid->menuscreen = GFX_CreateRGBSurface(
         0, SDL_MIYOOMINI_WIDTH, SDL_MIYOOMINI_HEIGHT, 16, 0, 0, 0, 0);
   if (vid->menuscreen_rgui) GFX_FreeSurface(vid->menuscreen_rgui);
   vid->menuscreen_rgui = GFX_CreateRGBSurface(
         0, RGUI_MENU_WIDTH, RGUI_MENU_HEIGHT, 16, 0, 0, 0, 0);
   if (!framebuffer||!vid->menuscreen||!vid->menuscreen_rgui)
   {
      RARCH_ERR("[MI_GFX]: Failed to init GFX surface\n");
      goto error;
   }

   vid->content_width   = SDL_MIYOOMINI_WIDTH;
   vid->content_height  = SDL_MIYOOMINI_HEIGHT;
   vid->rgb32           = video->rgb32;
   vid->vsync           = video->vsync;
   vid->keep_aspect     = settings->bools.video_dingux_ipu_keep_aspect;
   vid->scale_integer   = settings->bools.video_scale_integer;
   vid->softfilter_type = (enum dingux_rs90_softfilter_type)
         settings->uints.video_dingux_rs90_softfilter_type;
   vid->menu_active     = false;
   vid->was_in_menu     = false;
   vid->quitting        = false;
   vid->last_frame_time = 0;

   sdl_miyoomini_set_output(vid, vid->content_width, vid->content_height, vid->rgb32);

/* TODO/FIXME: GFX_BLOCKING causes audio stuttering
   GFX_SetFlipFlags(vid->vsync ? GFX_BLOCKING : 0);
*/

   sdl_miyoomini_input_driver_init(input_driver_name,
         joypad_driver_name, input, input_data);

   /* Initialise OSD font */
   sdl_miyoomini_init_font_color(vid);

   vid->osd_font = bitmapfont_get_lut();

   if (!vid->osd_font ||
       vid->osd_font->glyph_max <
            (SDL_NUM_FONT_GLYPHS - 1))
   {
      RARCH_ERR("[SDL1]: Failed to init OSD font\n");
      goto error;
   }

   return vid;

error:
   sdl_miyoomini_gfx_free(vid);
   return NULL;
}

static bool sdl_miyoomini_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   sdl_miyoomini_video_t* vid = (sdl_miyoomini_video_t*)data;

   /* Return early if:
    * - Input sdl_miyoomini_video_t struct is NULL
    *   (cannot realistically happen)
    * - Menu is inactive and input 'content' frame
    *   data is NULL (may happen when e.g. a running
    *   core skips a frame) */
   if (unlikely(!vid || (!frame && !vid->menu_active)))
      return true;

   /* If fast forward is currently active, we may
    * push frames at an 'unlimited' rate. Since the
    * display has a fixed refresh rate of 60 Hz (or
    * potentially 50 Hz on OpenDingux Beta), this
    * represents wasted effort. We therefore drop any
    * 'excess' frames in this case.
    * (Note that we *only* do this when fast forwarding.
    * Attempting this trick while running content normally
    * will cause bad frame pacing) */
   if (unlikely(video_info->input_driver_nonblock_state))
   {
      retro_time_t current_time = cpu_features_get_time_usec();

      if ((current_time - vid->last_frame_time) <
            vid->ff_frame_time_min)
         return true;

      vid->last_frame_time = current_time;
   }

#ifdef HAVE_MENU
   menu_driver_frame(video_info->menu_is_alive, video_info);
#endif

   if (msg) {
      memcpy(msg_tmp, msg, sizeof(msg_tmp));
      flip_callback = sdl_miyoomini_print_msg; /* in gfx.c */
   } else {
      flip_callback = NULL;
   }

   if (likely(!vid->menu_active))
   {
      /* Update video mode if we were in the menu on
       * the previous frame, or width/height have changed */
      if (unlikely((vid->was_in_menu) ||
            (vid->content_width  != width) ||
            (vid->content_height != height))) {
         sdl_miyoomini_set_output(vid, width, height, vid->rgb32);
         vid->was_in_menu = false;
      }
      /* Blit frame to SDL surface */
      vid->scale_func((void*)frame, vid->screen->pixels, width, height, pitch, vid->screen->pitch);
      GFX_UpdateRect(vid->screen, vid->video_x, vid->video_y, vid->video_w, vid->video_h);
   }
   else
   {
      /* If this is the first frame that the menu
       * is active, update video mode */
      if (!vid->was_in_menu) {
         sdl_miyoomini_set_output(vid, SDL_MIYOOMINI_WIDTH, SDL_MIYOOMINI_HEIGHT, false);
         vid->was_in_menu = true;
      }
      scale2x_n16(vid->menuscreen_rgui->pixels, vid->menuscreen->pixels, RGUI_MENU_WIDTH, RGUI_MENU_HEIGHT, 0,0);
      GFX_Flip(vid->menuscreen);
   }
   return true;
}

static void sdl_miyoomini_set_texture_enable(void *data, bool state, bool full_screen)
{
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;

   if (unlikely(!vid))
      return;

   vid->menu_active = state;
}

static void sdl_miyoomini_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;

   if (unlikely(
         !vid ||
         rgb32 ||
         (width > RGUI_MENU_WIDTH) ||
         (height > RGUI_MENU_HEIGHT)))
      return;

   memcpy_neon(vid->menuscreen_rgui->pixels, (void*)frame, width * height * sizeof(uint16_t));
}

static void sdl_miyoomini_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   bool vsync            = !toggle;

   if (unlikely(!vid))
      return;

   /* Check whether vsync status has changed */
   if (vid->vsync != vsync)
   {
      vid->vsync              = vsync;
/*    TODO/FIXME: GFX_BLOCKING causes audio stuttering
      GFX_SetFlipFlags(vsync ? GFX_BLOCKING : 0);
*/
   }
}

static void sdl_miyoomini_gfx_check_window(sdl_miyoomini_video_t *vid)
{
   SDL_Event event;

   SDL_PumpEvents();
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUITMASK))
   {
      if (event.type != SDL_QUIT)
         continue;

      vid->quitting = true;
      break;
   }
}

static bool sdl_miyoomini_gfx_alive(void *data)
{
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;

   if (unlikely(!vid))
      return false;

   sdl_miyoomini_gfx_check_window(vid);
   return !vid->quitting;
}

static bool sdl_miyoomini_gfx_focus(void *data) { return true; }
static bool sdl_miyoomini_gfx_suppress_screensaver(void *data, bool enable) { return false; }
static bool sdl_miyoomini_gfx_has_windowed(void *data) { return false; }

static void sdl_miyoomini_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;

   if (unlikely(!vid))
      return;

   vp->x           = vid->video_x;
   vp->y           = vid->video_y;
   vp->width       = vid->video_w;
   vp->height      = vid->video_h;
   vp->full_width  = SDL_MIYOOMINI_WIDTH;
   vp->full_height = SDL_MIYOOMINI_HEIGHT;
}

static float sdl_miyoomini_get_refresh_rate(void *data) { return 60.0f; }

static void sdl_miyoomini_set_filtering(void *data, unsigned index, bool smooth, bool ctx_scaling)
{
   sdl_miyoomini_video_t *vid                       = (sdl_miyoomini_video_t*)data;
   settings_t *settings                             = config_get_ptr();
   enum dingux_rs90_softfilter_type softfilter_type = (settings) ?
         (enum dingux_rs90_softfilter_type)settings->uints.video_dingux_rs90_softfilter_type :
               DINGUX_RS90_SOFTFILTER_POINT;

   if (!vid || !settings)
      return;

   /* Update software filter setting, if required */
   if (vid->softfilter_type != softfilter_type)
   {
      vid->softfilter_type = softfilter_type;
      sdl_miyoomini_set_output(vid, vid->content_width,
            vid->content_height, vid->menu_active ? false : vid->rgb32);
   }
}

static void sdl_miyoomini_apply_state_changes(void *data)
{
   sdl_miyoomini_video_t *vid  = (sdl_miyoomini_video_t*)data;
   settings_t *settings   = config_get_ptr();
   bool keep_aspect       = (settings) ? settings->bools.video_dingux_ipu_keep_aspect : true;
   bool integer_scaling   = (settings) ? settings->bools.video_scale_integer : false;

   if (!vid || !settings)
      return;

   if ((vid->keep_aspect != keep_aspect) ||
       (vid->scale_integer != integer_scaling))
   {
      vid->keep_aspect   = keep_aspect;
      vid->scale_integer = integer_scaling;

      /* Aspect/scaling changes require all frame
       * dimension/padding/cropping parameters to
       * be recalculated. Easiest method is to just
       * (re-)set the current output video mode
       * Note: If menu is active, colour depth is
       * overridden to 16 bit */
      sdl_miyoomini_set_output(vid, vid->content_width,
            vid->content_height, vid->menu_active ? false : vid->rgb32);
   }
}

static uint32_t sdl_miyoomini_get_flags(void *data) { return 0; }

static const video_poke_interface_t sdl_miyoomini_poke_interface = {
   sdl_miyoomini_get_flags,
   NULL,
   NULL,
   NULL,
   sdl_miyoomini_get_refresh_rate,
   sdl_miyoomini_set_filtering,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   NULL, /* set_aspect_ratio */
   sdl_miyoomini_apply_state_changes,
   sdl_miyoomini_set_texture_frame,
   sdl_miyoomini_set_texture_enable,
   NULL, /* set_osd_msg */
   NULL, /* sdl_show_mouse */
   NULL, /* sdl_grab_mouse_toggle */
   NULL, /* get_current_shader */
   NULL, /* get_current_software_framebuffer */
   NULL, /* get_hw_render_interface */
   NULL, /* set_hdr_max_nits */
   NULL, /* set_hdr_paper_white_nits */
   NULL, /* set_hdr_contrast */
   NULL  /* set_hdr_expand_gamut */
};

static void sdl_miyoomini_get_poke_interface(void *data, const video_poke_interface_t **iface)
{
   *iface = &sdl_miyoomini_poke_interface;
}

static bool sdl_miyoomini_gfx_set_shader(void *data,
      enum rarch_shader_type type, const char *path) { return false; }

video_driver_t video_sdl_rs90 = {
   sdl_miyoomini_gfx_init,
   sdl_miyoomini_gfx_frame,
   sdl_miyoomini_gfx_set_nonblock_state,
   sdl_miyoomini_gfx_alive,
   sdl_miyoomini_gfx_focus,
   sdl_miyoomini_gfx_suppress_screensaver,
   sdl_miyoomini_gfx_has_windowed,
   sdl_miyoomini_gfx_set_shader,
   sdl_miyoomini_gfx_free,
   "sdl_rs90",
   NULL,
   NULL, /* set_rotation */
   sdl_miyoomini_gfx_viewport_info,
   NULL, /* read_viewport  */
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   NULL,
#endif
#ifdef HAVE_VIDEO_LAYOUT
  NULL,
#endif
   sdl_miyoomini_get_poke_interface
};
