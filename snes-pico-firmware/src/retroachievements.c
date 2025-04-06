#include "retroachievements.h"
#include "rc_runtime_types.h"
#include "rc_client.h"
#include "rc_client_internal.h"

#include "rc_api_info.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_version.h"

#include "rc_internal.h"

uint32_t getAllUniqueMemoryAddressesCount(char **achievements, uint32_t achievements_count)
{
  uint32_t *all_memory_addresses = NULL;
  uint32_t all_memory_addresses_count = 0;

  for (int i = 0; i < achievements_count; i++)
  {
    uint32_t *result = getMemoryAddresses(achievements[i]);
    uint32_t count = getMemoryAddressesCount(achievements[i]);
    all_memory_addresses = realloc(all_memory_addresses, (all_memory_addresses_count + count) * sizeof(uint32_t));
    for (int j = 0; j < count; j++)
    {
      all_memory_addresses[all_memory_addresses_count++] = result[j];
    }
    free(result);
  }
  // sort all_memory_addresses array
  for (int i = 0; i < all_memory_addresses_count; i++)
  {
    for (int j = i + 1; j < all_memory_addresses_count; j++)
    {
      if (all_memory_addresses[i] > all_memory_addresses[j])
      {
        uint32_t temp = all_memory_addresses[i];
        all_memory_addresses[i] = all_memory_addresses[j];
        all_memory_addresses[j] = temp;
      }
    }
  }

  // get unique memory addresses and copy to unique_memory_addresses

  uint32_t unique_memory_addresses_count = 0;
  for (int i = 0; i < all_memory_addresses_count; i++)
  {
    if (i == 0 || all_memory_addresses[i] != all_memory_addresses[i - 1])
    {
      unique_memory_addresses_count++;
    }
  }
  free(all_memory_addresses);
  return unique_memory_addresses_count;
}

uint32_t *getAllUniqueMemoryAddresses(char **achievements, uint32_t achievements_count)
{
  uint32_t *all_memory_addresses = NULL;
  uint32_t all_memory_addresses_count = 0;

  for (int i = 0; i < achievements_count; i++)
  {
    uint32_t *result = getMemoryAddresses(achievements[i]);
    uint32_t count = getMemoryAddressesCount(achievements[i]);
    all_memory_addresses = realloc(all_memory_addresses, (all_memory_addresses_count + count) * sizeof(uint32_t));
    for (int j = 0; j < count; j++)
    {
      all_memory_addresses[all_memory_addresses_count++] = result[j];
    }
    free(result);
  }
  // sort all_memory_addresses array
  for (int i = 0; i < all_memory_addresses_count; i++)
  {
    for (int j = i + 1; j < all_memory_addresses_count; j++)
    {
      if (all_memory_addresses[i] > all_memory_addresses[j])
      {
        uint32_t temp = all_memory_addresses[i];
        all_memory_addresses[i] = all_memory_addresses[j];
        all_memory_addresses[j] = temp;
      }
    }
  }

  // get unique memory addresses and copy to unique_memory_addresses
  uint32_t unique_memory_addresses_count = getAllUniqueMemoryAddressesCount(achievements, achievements_count);
  uint32_t *unique_memory_addresses = (uint32_t *)malloc(unique_memory_addresses_count * sizeof(uint32_t));
  unique_memory_addresses_count = 0;
  for (int i = 0; i < all_memory_addresses_count; i++)
  {
    if (i == 0 || all_memory_addresses[i] != all_memory_addresses[i - 1])
    {
      unique_memory_addresses[unique_memory_addresses_count++] = all_memory_addresses[i];
    }
  }
  free(all_memory_addresses);
  return unique_memory_addresses;
}

uint32_t getMemoryAddressesCount(const char *achievementString)
{
  // printf("Achievement: %s\n", achievementString);
  int num_achievements = 7;
  int size = 0;
  const char *memaddr = achievementString;
  rc_trigger_t *self;
  rc_parse_state_t parse;
  rc_memref_t *memrefs;
  rc_init_parse_state(&parse, 0, 0, 0);
  rc_init_parse_state_memrefs(&parse, &memrefs);

  self = RC_ALLOC(rc_trigger_t, &parse);
  rc_parse_trigger_internal(self, &memaddr, &parse);
  rc_memref_t *memref = *parse.first_memref;
  uint32_t memref_count = 0;
  while (memref != NULL)
  {
    memref_count++;
    // printf("Memref: %p\n", memref->address);
    memref = memref->next;
  }
  // printf("Memref count: %d\n", memref_count);

  rc_destroy_parse_state(&parse);

  return memref_count;
}

uint32_t *getMemoryAddresses(const char *achievementString)
{
  uint32_t memref_count = getMemoryAddressesCount(achievementString);
  const char *memaddr = achievementString;
  rc_trigger_t *self;
  rc_parse_state_t parse;
  rc_memref_t *memrefs;
  rc_init_parse_state(&parse, 0, 0, 0);
  rc_init_parse_state_memrefs(&parse, &memrefs);

  self = RC_ALLOC(rc_trigger_t, &parse);
  rc_parse_trigger_internal(self, &memaddr, &parse);
  rc_memref_t *memref = *parse.first_memref;
  uint32_t *memoryAddresses = malloc(memref_count * sizeof(uint32_t));
  memref = *parse.first_memref;
  memref_count = 0;
  while (memref != NULL)
  {
    memoryAddresses[memref_count++] = memref->address;
    memref = memref->next;
  }
  rc_destroy_parse_state(&parse);

  return memoryAddresses;
}



// RCHEEVOS_API

// Write log messages to the console
static void log_message(const char *message, const rc_client_t *client)
{
    printf("%s\n", message);
}

// Initialize the RetroAchievements client
rc_client_t* initialize_retroachievements_client(rc_client_t *g_client, rc_client_read_memory_func_t read_memory, rc_client_server_call_t server_call)
{
     // Create the client instance (using a global variable simplifies this example)
    g_client = rc_client_create(read_memory, server_call);

    // Provide a logging function to simplify debugging
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_VERBOSE, log_message);


    // Disable hardcore - if we goof something up in the implementation, we don't want our
    // account disabled for cheating.
    rc_client_set_hardcore_enabled(g_client, 0);
    return g_client;
}

void shutdown_retroachievements_client(rc_client_t *g_client)
{
    if (g_client)
    {
        // Release resources associated to the client instance
        rc_client_destroy(g_client);
        g_client = NULL;
    }
}

