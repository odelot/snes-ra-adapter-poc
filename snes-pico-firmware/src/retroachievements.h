#ifndef __RETROACHIEVEMENTS_H__
#define __RETROACHIEVEMENTS_H__

#ifdef __PICO__
#include "pico/stdlib.h"
#else
#include "stdlib.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include "rc_client.h"

uint32_t* getMemoryAddresses(const char* achievementString);
uint32_t getMemoryAddressesCount (const char* achievementString);
uint32_t* getAllUniqueMemoryAddresses (char **achievements, uint32_t achievements_count);
uint32_t getAllUniqueMemoryAddressesCount (char **achievements, uint32_t achievements_count);

rc_client_t* initialize_retroachievements_client(rc_client_t *g_client, rc_client_read_memory_func_t read_memory, rc_client_server_call_t server_call);
void shutdown_retroachievements_client(rc_client_t *g_client);

#endif