/**
 * @file monitor.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "monitor.h"
#if USE_MONITOR

#ifndef MONITOR_SDL_INCLUDE_PATH
#define MONITOR_SDL_INCLUDE_PATH <SDL2/SDL.h>
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include MONITOR_SDL_INCLUDE_PATH
#include "../indev/keyboard.h"
#include "../indev/mouse.h"
#include "../indev/mousewheel.h"

/*********************
 *      DEFINES
 *********************/
#define SDL_REFR_PERIOD 50 /*ms*/

#ifndef MONITOR_ZOOM
#define MONITOR_ZOOM 1
#endif

#ifndef MONITOR_HOR_RES
#define MONITOR_HOR_RES LV_HOR_RES
#endif

#ifndef MONITOR_VER_RES
#define MONITOR_VER_RES LV_VER_RES
#endif

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  SDL_Surface *surface;
  volatile bool sdl_refr_qry;
#if MONITOR_DOUBLE_BUFFERED
  uint32_t *tft_fb_act;
#else
  uint32_t *tft_fb;
#endif
} monitor_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void window_create(monitor_t *m);
static void window_update(monitor_t *m);
int quit_filter(void *userdata, SDL_Event *event);
static void monitor_sdl_clean_up(void);
static void monitor_sdl_init(void);
static void sdl_event_handler(lv_timer_t *t);
static void monitor_sdl_refr(lv_timer_t *t);

/***********************
 *   GLOBAL PROTOTYPES
 ***********************/

/**********************
 *  STATIC VARIABLES
 **********************/
monitor_t monitor;

#if MONITOR_DUAL
monitor_t monitor2;
#endif

static volatile bool sdl_inited = false;
static volatile bool sdl_quit_qry = false;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Initialize the monitor
 */
void monitor_init(void) {
  monitor_sdl_init();
  lv_timer_create(sdl_event_handler, 10, NULL);
}

uint32_t pixelbuf[800 * 600];
/**
 * Flush a buffer to the marked area
 * @param drv pointer to driver where this function belongs
 * @param area an area where to copy `color_p`
 * @param color_p an array of pixel to copy to the `area` part of the screen
 */
void monitor_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                   lv_color_t *color_p) {
  lv_coord_t hres = disp_drv->hor_res;
  lv_coord_t vres = disp_drv->ver_res;

  // printf("x1:%d,y1:%d,x2:%d,y2:%d\n", area->x1, area->y1, area->x2,
  // area->y2);

  /*Return if the area is out the screen*/
  if (area->x2 < 0 || area->y2 < 0 || area->x1 > hres - 1 ||
      area->y1 > vres - 1) {
    lv_disp_flush_ready(disp_drv);
    return;
  }

#if MONITOR_DOUBLE_BUFFERED
  monitor.tft_fb_act = (uint32_t *)color_p;
#else /*MONITOR_DOUBLE_BUFFERED*/

  int32_t y;
#if LV_COLOR_DEPTH != 24 &&                                                    \
    LV_COLOR_DEPTH !=                                                          \
        32 /*32 is valid but support 24 for backward compatibility too*/
  int32_t x;
  for (y = area->y1; y <= area->y2 && y < disp_drv->ver_res; y++) {
    for (x = area->x1; x <= area->x2; x++) {
      monitor.tft_fb[y * disp_drv->hor_res + x] = lv_color_to32(*color_p);
      color_p++;
    }
  }
#else
  uint32_t w = lv_area_get_width(area);
  for (y = area->y1; y <= area->y2 && y < disp_drv->ver_res; y++) {
    memcpy(&pixelbuf[y * MONITOR_HOR_RES + area->x1], color_p,
           w * sizeof(lv_color_t));

    memcpy(&monitor.tft_fb[y * MONITOR_HOR_RES + area->x1], color_p,
           w * sizeof(lv_color_t));
    color_p += w;
  }
  FILE *fp = fopen("/home/oliver/2.ram", "wb+");
  if (fp) {
    fwrite(pixelbuf, 4, 800 * 600, fp);
    fclose(fp);
  }

#endif
#endif /*MONITOR_DOUBLE_BUFFERED*/

  monitor.sdl_refr_qry = true;

  /* TYPICALLY YOU DO NOT NEED THIS
   * If it was the last part to refresh update the texture of the window.*/
  if (lv_disp_flush_is_last(disp_drv)) {
    monitor_sdl_refr(NULL);
  }

  /*IMPORTANT! It must be called to tell the system the flush is ready*/
  lv_disp_flush_ready(disp_drv);
}

#if MONITOR_DUAL

/**
 * Flush a buffer to the marked area
 * @param drv pointer to driver where this function belongs
 * @param area an area where to copy `color_p`
 * @param color_p an array of pixel to copy to the `area` part of the screen
 */
void monitor_flush2(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                    lv_color_t *color_p) {
  lv_coord_t hres = disp_drv->hor_res;
  lv_coord_t vres = disp_drv->ver_res;

  /*Return if the area is out the screen*/
  if (area->x2 < 0 || area->y2 < 0 || area->x1 > hres - 1 ||
      area->y1 > vres - 1) {
    lv_disp_flush_ready(disp_drv);
    return;
  }

#if MONITOR_DOUBLE_BUFFERED
  monitor2.tft_fb_act = (uint32_t *)color_p;

  monitor2.sdl_refr_qry = true;

  /*IMPORTANT! It must be called to tell the system the flush is ready*/
  lv_disp_flush_ready(disp_drv);
#else

  int32_t y;
#if LV_COLOR_DEPTH != 24 &&                                                    \
    LV_COLOR_DEPTH !=                                                          \
        32 /*32 is valid but support 24 for backward compatibility too*/
  int32_t x;
  for (y = area->y1; y <= area->y2 && y < disp_drv->ver_res; y++) {
    for (x = area->x1; x <= area->x2; x++) {
      monitor2.tft_fb[y * disp_drv->hor_res + x] = lv_color_to32(*color_p);
      color_p++;
    }
  }
#else
  uint32_t w = lv_area_get_width(area);
  for (y = area->y1; y <= area->y2 && y < disp_drv->ver_res; y++) {
    memcpy(&monitor2.tft_fb[y * disp_drv->hor_res + area->x1], color_p,
           w * sizeof(lv_color_t));
    color_p += w;
  }
#endif

  monitor2.sdl_refr_qry = true;

  /* TYPICALLY YOU DO NOT NEED THIS
   * If it was the last part to refresh update the texture of the window.*/
  if (lv_disp_flush_is_last(disp_drv)) {
    monitor_sdl_refr(NULL);
  }

  /*IMPORTANT! It must be called to tell the system the flush is ready*/
  lv_disp_flush_ready(disp_drv);
#endif
}
#endif

void screenshot() {

  SDL_Rect clip;
  clip.x = 0;
  clip.y = 0;

  // Get the size of the screen to be taken
  SDL_GetWindowSize(monitor.window, &clip.w, &clip.h);

  // Get the window surface
  SDL_Surface *surface = SDL_GetWindowSurface(monitor.window);

  // Make sire you have the surface
  if (surface == NULL) {
    return;
  }

  // Copy the pixels in the renderer to the surface's pixels
  SDL_RenderReadPixels(monitor.renderer, &clip,
                       SDL_GetWindowPixelFormat(monitor.window),
                       surface->pixels, surface->pitch);

  SDL_SaveBMP(surface, "/home/oliver/1.bmp");
  // Tidy up surface
  SDL_FreeSurface(surface);
}

void *get_monitor_param(int32_t *ptr) {
  memcpy(ptr, pixelbuf, 800 * 600 * 4);
  return pixelbuf;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * SDL main thread. All SDL related task have to be handled here!
 * It initializes SDL, handles drawing and the mouse.
 */

static void sdl_event_handler(lv_timer_t *t) {
  (void)t;

  /*Refresh handling*/
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
#if USE_MOUSE != 0
    mouse_handler(&event);
#endif

#if USE_MOUSEWHEEL != 0
    mousewheel_handler(&event);
#endif

#if USE_KEYBOARD
    keyboard_handler(&event);
#endif
    if ((&event)->type == SDL_WINDOWEVENT) {
      switch ((&event)->window.event) {
#if SDL_VERSION_ATLEAST(2, 0, 5)
      case SDL_WINDOWEVENT_TAKE_FOCUS:
#endif
      case SDL_WINDOWEVENT_EXPOSED:
        window_update(&monitor);
#if MONITOR_DUAL
        window_update(&monitor2);
#endif
        break;
      default:
        break;
      }
    }
  }

  /*Run until quit event not arrives*/
  if (sdl_quit_qry) {
    monitor_sdl_clean_up();
    exit(0);
  }
}

/**
 * SDL main thread. All SDL related task have to be handled here!
 * It initializes SDL, handles drawing and the mouse.
 */

static void monitor_sdl_refr(lv_timer_t *t) {
  (void)t;

  /*Refresh handling*/
  if (monitor.sdl_refr_qry != false) {
    monitor.sdl_refr_qry = false;
    window_update(&monitor);
  }

#if MONITOR_DUAL
  if (monitor2.sdl_refr_qry != false) {
    monitor2.sdl_refr_qry = false;
    window_update(&monitor2);
  }
#endif
}

int quit_filter(void *userdata, SDL_Event *event) {
  (void)userdata;

  if (event->type == SDL_WINDOWEVENT) {
    if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
      sdl_quit_qry = true;
    }
  } else if (event->type == SDL_QUIT) {
    sdl_quit_qry = true;
  }

  return 1;
}

static void monitor_sdl_clean_up(void) {
  SDL_DestroyTexture(monitor.texture);
  SDL_DestroyRenderer(monitor.renderer);
  SDL_DestroyWindow(monitor.window);

#if MONITOR_DUAL
  SDL_DestroyTexture(monitor2.texture);
  SDL_DestroyRenderer(monitor2.renderer);
  SDL_DestroyWindow(monitor2.window);

#endif

  SDL_Quit();
}

static void monitor_sdl_init(void) {
  /*Initialize the SDL*/
  SDL_Init(SDL_INIT_VIDEO);

  SDL_SetEventFilter(quit_filter, NULL);

  window_create(&monitor);
#if MONITOR_DUAL
  window_create(&monitor2);
  int x, y;
  SDL_GetWindowPosition(monitor2.window, &x, &y);
  SDL_SetWindowPosition(monitor.window,
                        x + (MONITOR_HOR_RES * MONITOR_ZOOM) / 2 + 10, y);
  SDL_SetWindowPosition(monitor2.window,
                        x - (MONITOR_HOR_RES * MONITOR_ZOOM) / 2 - 10, y);
#endif

  sdl_inited = true;
}

static void window_create(monitor_t *m) {
  m->window = SDL_CreateWindow(
      "TFT Simulator", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      MONITOR_HOR_RES * MONITOR_ZOOM, MONITOR_VER_RES * MONITOR_ZOOM,
      0); /*last param. SDL_WINDOW_BORDERLESS to hide borders*/

  m->renderer = SDL_CreateRenderer(m->window, -1, SDL_RENDERER_SOFTWARE);
  m->texture = SDL_CreateTexture(m->renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STATIC, MONITOR_HOR_RES,
                                 MONITOR_VER_RES);
  SDL_SetTextureBlendMode(m->texture, SDL_BLENDMODE_BLEND);

  m->surface = SDL_GetWindowSurface(m->window);
  /*Initialize the frame buffer to gray (77 is an empirical value) */
#if MONITOR_DOUBLE_BUFFERED
  SDL_UpdateTexture(m->texture, NULL, m->tft_fb_act,
                    MONITOR_HOR_RES * sizeof(uint32_t));
#else
  m->tft_fb =
      (uint32_t *)malloc(sizeof(uint32_t) * MONITOR_HOR_RES * MONITOR_VER_RES);
  memset(m->tft_fb, 0x44, MONITOR_HOR_RES * MONITOR_VER_RES * sizeof(uint32_t));
#endif

  m->sdl_refr_qry = true;
}

static void window_update(monitor_t *m) {
#if MONITOR_DOUBLE_BUFFERED == 0
  SDL_UpdateTexture(m->texture, NULL, m->tft_fb,
                    MONITOR_HOR_RES * sizeof(uint32_t));
#else
  if (m->tft_fb_act == NULL)
    return;
  SDL_UpdateTexture(m->texture, NULL, m->tft_fb_act,
                    MONITOR_HOR_RES * sizeof(uint32_t));
#endif
  SDL_RenderClear(m->renderer);
#if LV_COLOR_SCREEN_TRANSP
  SDL_SetRenderDrawColor(m->renderer, 0xff, 0, 0, 0xff);
  SDL_Rect r;
  r.x = 0;
  r.y = 0;
  r.w = MONITOR_HOR_RES;
  r.w = MONITOR_VER_RES;
  SDL_RenderDrawRect(m->renderer, &r);
#endif

  /*Update the renderer with the texture containing the rendered image*/
  SDL_RenderCopy(m->renderer, m->texture, NULL, NULL);
  SDL_RenderPresent(m->renderer);
}

#endif /*USE_MONITOR*/
