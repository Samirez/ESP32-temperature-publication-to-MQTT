#ifndef hl_wifi_h
#define hl_wifi_h

#include <esp_event.h>

void hl_wifi_init(void);
void wifi_init(void);
void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

#endif
