#ifndef TODO_WEB_H
#define TODO_WEB_H

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t TodoWeb_Init(void);
esp_err_t TodoWeb_Start(void);
esp_err_t TodoWeb_Stop(void);
bool TodoWeb_IsRunning(void);

#ifdef __cplusplus
}
#endif

#endif
