#pragma once

#include "event.hpp"

int uart_init();
void uart_deinit();

Event uart_read();