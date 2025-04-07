#include "esp_err.h"

#ifndef UTILS
#define UTILS

esp_err_t mountSPIFFS(char *path, char *label, int max_files);

#endif /* UTILS */
