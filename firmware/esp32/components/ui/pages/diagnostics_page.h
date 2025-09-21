#ifndef DIAGNOSTICS_PAGE_H
#define DIAGNOSTICS_PAGE_H

#include "lvgl.h"

void diagnostics_page_init(void);
void diagnostics_page_create(lv_obj_t *parent);
void diagnostics_page_stop(void);

#endif // DIAGNOSTICS_PAGE_H
