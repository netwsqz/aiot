#ifndef PORT_SHTC3_H
#define PORT_SHTC3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float Temp;
    float RH;
} shtc3_data_t;

void PortShtc3_Init(void);
shtc3_data_t PortShtc3_Read(void);

#ifdef __cplusplus
}
#endif

#endif
