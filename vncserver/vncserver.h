#ifndef VNCSERVER_H_
#define VNCSERVER_H_

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#ifndef LV_DRV_NO_CONF
#ifdef LV_CONF_INCLUDE_SIMPLE
#include "lv_drv_conf.h"
#else
#include "../../lv_drv_conf.h"
#endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
void vncserver_init(void);
void vncserver_exit(void);
void vncserver_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p);
void vncserver_get_sizes(uint32_t *width, uint32_t *height);
int vnc_mouse_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data);
/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
