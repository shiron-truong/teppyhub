#pragma once
#include <stddef.h>
#include <stdint.h>
void hlp_protocol_init(void);
void hlp_protocol_feed(const uint8_t *data, size_t length);
void hlp_protocol_send_line(const char *line);
