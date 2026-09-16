#pragma once
#include <stddef.h>
#include "esp_err.h"
/* Exact-origin mTLS requests. Caller owns output; no redirects. */
esp_err_t device_api(const char *path,const char *json,void *output,size_t capacity,size_t *length);
