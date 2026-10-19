#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void hlp_protocol_init(void);
void hlp_protocol_reset_session(void);
bool hlp_protocol_take_session_reset(void);
uint32_t hlp_protocol_status_age_ms(uint32_t now_ms);
void hlp_protocol_feed(const uint8_t *data, size_t length);
void hlp_protocol_send_line(const char *line);
