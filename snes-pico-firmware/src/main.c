#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/malloc.h"

#include "pico/multicore.h"
#include "pico/mutex.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/systick.h"
#include "memory-bus.pio.h"
#include "retroachievements.h"

#include "hardware/timer.h"
#include <limits.h>
#include <math.h>

#define BUS_PIO pio0
#define BUS_SM 0

#define UART_ID uart0
#define BAUD_RATE 115200

#define SNES_D0 0
#define SNES_D1 1
#define SNES_D2 2
#define SNES_D3 3
#define SNES_D4 4
#define SNES_D5 5
#define SNES_D6 6
#define SNES_D7 7

#define SNES_A00 8
#define SNES_A01 9
#define SNES_A02 10
#define SNES_A03 11
#define SNES_A04 12
#define SNES_A05 13
#define SNES_A06 14
#define SNES_A07 15

#define SNES_A08 16
#define SNES_A09 17
#define SNES_A10 18
#define SNES_A11 19
#define SNES_A12 20
#define SNES_A13 21
#define SNES_A14 22
#define SNES_A15 23
#define SNES_A16 24

#define SNES_RW 25
#define SNES_M2 26    // F0
#define SNES_WRSEL 27 // F1

#define UART_TX_PIN 28 // GPIO pin for TX
#define UART_RX_PIN 29 // GPIO pin for RX

const uint16_t SNES_D[8] = {SNES_D0, SNES_D1, SNES_D2, SNES_D3, SNES_D4, SNES_D5, SNES_D6, SNES_D7};
const uint16_t SNES_A[17] = {SNES_A00, SNES_A01, SNES_A02, SNES_A03, SNES_A04, SNES_A05, SNES_A06, SNES_A07, SNES_A08, SNES_A09, SNES_A10, SNES_A11, SNES_A12, SNES_A13, SNES_A14, SNES_A15, SNES_A16};
const uint16_t SNES_F[3] = {SNES_RW, SNES_M2, SNES_WRSEL};

// CRC32 global variables
uint32_t crcBegin = 0xFFFFFFFF;
uint32_t crcEnd = 0xFFFFFFFF;

/*
 * PIO Bus Watcher
 */

volatile uint32_t busPIOemptyMask, busPIOstallMask;
volatile io_ro_32 *rxf;

uint32_t rawBusData;

mutex_t cpubusMutex;

/*
 * states
 */

uint8_t state = 0;
int nes_reseted = 0;
char md5[33];
uint8_t request_ongoing = 0;
uint32_t last_request = 0;
char ra_token[32];
char ra_user[256];
/*
 * Serial buffers
 */

#define SERIAL_BUFFER_SIZE 32768
u_char serial_buffer[SERIAL_BUFFER_SIZE];
u_char *serial_buffer_head = serial_buffer;
// u_char command[256];

// memory circular buffer

#define MEMORY_BUFFER_SIZE 2048
volatile int memory_head = 0;
volatile int memory_tail = 0;

struct _memory_unit
{
    uint32_t address;
    uint8_t data;
};

typedef struct _memory_unit memory_unit;
memory_unit memory_buffer[MEMORY_BUFFER_SIZE];

uint16_t unique_memory_addresses_count = 0;
uint32_t *unique_memory_addresses = NULL;
uint8_t *memory_data = NULL;

/*
 * handle GPIO
 */

void resetGPIO()
{
    for (int i = 0; i < 8; i += 1)
    {
        gpio_init(SNES_D[i]);
        gpio_set_dir(SNES_D[i], GPIO_IN);
        gpio_disable_pulls(SNES_D[i]);
    }
    for (int i = 0; i < 17; i += 1)
    {
        gpio_init(SNES_A[i]);
        gpio_set_dir(SNES_A[i], GPIO_IN);
        gpio_disable_pulls(SNES_A[i]);
    }
    for (int i = 0; i < 3; i += 1)
    {
        gpio_init(SNES_F[i]);
        gpio_set_dir(SNES_F[i], GPIO_IN);
        gpio_disable_pulls(SNES_F[i]);
    }
}

void initCRC32()
{
    for (int i = 0; i < 8; i += 1)
    {
        gpio_init(SNES_D[i]);
        gpio_set_dir(SNES_D[i], GPIO_IN);
        gpio_pull_down(SNES_D[i]);
    }
    for (int i = 0; i < 17; i += 1)
    {
        gpio_init(SNES_A[i]);
        gpio_set_dir(SNES_A[i], GPIO_OUT);
        gpio_set_drive_strength(SNES_A[i], GPIO_DRIVE_STRENGTH_12MA);
    }
    for (int i = 0; i < 3; i += 1)
    {
        gpio_init(SNES_F[i]);
        gpio_set_dir(SNES_F[i], GPIO_OUT);
        gpio_set_drive_strength(SNES_F[i], GPIO_DRIVE_STRENGTH_12MA);
    }
}

void finishCRC32()
{
    for (int i = 0; i < 8; i += 1)
    {
        gpio_init(SNES_D[i]);
        gpio_set_dir(SNES_D[i], GPIO_IN);
        gpio_disable_pulls(SNES_D[i]);
    }
    for (int i = 0; i < 17; i += 1)
    {
        gpio_init(SNES_A[i]);
        gpio_set_dir(SNES_A[i], GPIO_IN);
        gpio_disable_pulls(SNES_A[i]);
    }
    for (int i = 0; i < 3; i += 1)
    {
        gpio_init(SNES_F[i]);
        gpio_set_dir(SNES_F[i], GPIO_IN);
        gpio_disable_pulls(SNES_F[i]);
    }
}

/*
 * handle DMA
 */

#define BUFFER_SIZE 8192 // Tamanho de cada buffer

volatile uint32_t buffer_a[BUFFER_SIZE];
volatile uint32_t buffer_b[BUFFER_SIZE];
volatile bool readA;
volatile bool readingA;
volatile bool readingB;

int dma_chan_0, dma_chan_1; // Canais do DMA

// DMA channel handler, not in memory to speed it up
void __not_in_flash_func(dma_handler)()
{

    // Did channel0 triggered the irq?
    if (dma_channel_get_irq0_status(dma_chan_0))
    {
        // Clear the irq
        dma_channel_acknowledge_irq0(dma_chan_0);
        // Rewrite the write address without triggering the channel
        dma_channel_set_write_addr(dma_chan_0, buffer_a, false);
        // dma_channel_set_write_addr(dma_chan_1, buffer_b, true);
        if (readingB)
        {
            printf("m_");
        }
    }
    else
    {
        // Clear the irq
        dma_channel_acknowledge_irq0(dma_chan_1);
        // Rewrite the write address without triggering the channel
        dma_channel_set_write_addr(dma_chan_1, buffer_b, false);
        // dma_channel_set_write_addr(dma_chan_0, buffer_a, true);
        if (readingA)
        {
            printf("m_");
        }
    }
}

void setup_dma()
{
    memset((void *)buffer_a, 0, BUFFER_SIZE * sizeof(uint32_t));
    memset((void *)buffer_b, 0, BUFFER_SIZE * sizeof(uint32_t));

    dma_chan_0 = dma_claim_unused_channel(true);
    dma_chan_1 = dma_claim_unused_channel(true);

    // Canal 0: Copia do FIFO do PIO para buffer_a

    dma_channel_config c0 = dma_channel_get_default_config(dma_chan_0);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0, false);
    channel_config_set_write_increment(&c0, true);
    channel_config_set_dreq(&c0, pio_get_dreq(BUS_PIO, BUS_SM, false));
    channel_config_set_chain_to(&c0, dma_chan_1); // Quando terminar, ativa o canal 1
    // channel_config_set_irq_quiet(&c0, true);      // Não gera interrupção quando terminar
    channel_config_set_high_priority(&c0, true);
    channel_config_set_enable(&c0, true);

    dma_channel_set_irq0_enabled(dma_chan_0, true); // Enable IRQ 0

    // Canal 1 : Copia do FIFO do PIO para buffer_b
    dma_channel_config c1 = dma_channel_get_default_config(dma_chan_1);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, true);
    channel_config_set_dreq(&c1, pio_get_dreq(BUS_PIO, BUS_SM, false));
    channel_config_set_chain_to(&c1, dma_chan_0); // Quando terminar, ativa o canal 0
    // channel_config_set_irq_quiet(&c1, true);      // Não gera interrupção
    channel_config_set_high_priority(&c1, true);
    channel_config_set_enable(&c1, true);

    dma_channel_set_irq0_enabled(dma_chan_1, true); // Enable IRQ 0

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    irq_set_priority(DMA_IRQ_0, 0);

    dma_channel_configure(
        dma_chan_1, &c1,
        buffer_b,              // Destino
        &BUS_PIO->rxf[BUS_SM], // Fonte: FIFO do PIO
        BUFFER_SIZE,           // Tamanho da transferência
        false);

    dma_channel_configure(
        dma_chan_0, &c0,
        buffer_a,              // Destino
        &BUS_PIO->rxf[BUS_SM], // Fonte: FIFO do PIO
        BUFFER_SIZE,           // Tamanho da transferência
        true);
}

/*
 * handle pio
 */

void setupPIO()
{
    for (int i = 0; i < 28; i++) // reset all GPIOs connected to NES
        gpio_init(i);
    uint offset = pio_add_program(BUS_PIO, &memoryBus_program);
    memoryBus_program_init(BUS_PIO, BUS_SM, offset, (float) 5.0f);
}

/*
 * Memory circula buffer
 */

unsigned int memory_buffer_size()
{
    return (memory_head - memory_tail + MEMORY_BUFFER_SIZE) % MEMORY_BUFFER_SIZE;
}

void add_to_memory_buffer(uint32_t address, uint8_t data)
{
    memory_buffer[memory_head].address = address;
    memory_buffer[memory_head].data = data;
    memory_head = (memory_head + 1) % MEMORY_BUFFER_SIZE;
    if (memory_head == memory_tail)
    {
        printf("Buffer full\n");
        // Buffer full, discard or replace oldest data
        memory_tail = (memory_tail + 1) % MEMORY_BUFFER_SIZE;
    }
}

memory_unit read_from_memory_buffer()
{
    if (memory_head == memory_tail)
    {
        // Buffer vazio
        memory_unit empty;
        empty.address = 0;
        empty.data = 0;
        return empty;
    }
    memory_unit data = memory_buffer[memory_tail];
    memory_tail = (memory_tail + 1) % MEMORY_BUFFER_SIZE;
    return data;
}

// handle memory bus

void print_buffer(uint32_t *buffer, int index)
{
    int min = index - 7;
    int max = index + 1;
    if (min < 0)
    {
        min = 0;
    }
    if (max >= BUFFER_SIZE)
    {
        max = BUFFER_SIZE - 1;
    }
    for (int i = min; i <= max; i += 1)
    {
        printf("%p\n", buffer[i]);
    }
    printf("\n");
}

inline void try_add_to_circular_buffer(uint32_t address, uint8_t data)
{

    // printf("A: %p, D: %p\n", address, data);
    if (address == 0x017f5e)
    {
        printf("Address: %p, Data: %p\n", address, data);
    }
    uint8_t found = 0;
    // make a binary search to check if the address is on the unique memory addresses list
    int left = 0;
    int right = unique_memory_addresses_count - 1;
    while (left <= right)
    {

        // calculating mid point
        int mid = left + (right - left) / 2;

        // Check if key is present at mid
        if (unique_memory_addresses[mid] == address)
        {
            add_to_memory_buffer(address, data);
            // if (address == 0x000dc1 || address == 0x0013bf)
            // {
            //     printf("Address: %p, Data: %p\n", address, data);
            // }

            return;
        }

        // If key greater than arr[mid], ignore left half
        if (unique_memory_addresses[mid] < address)
        {
            left = mid + 1;
        }

        // If key is smaller than or equal to arr[mid],
        // ignore right half
        else
        {
            right = mid - 1;
        }
    }

    // for (int j = 0; j < unique_memory_addresses_count; j += 1)
    // {
    //     if (unique_memory_addresses[j] == address)
    //     {
    //         found = 1;
    //         break;
    //     }
    // }
    // if (found == 1) // 0x4014 is already on the unique memory addresses list
    // {
    //     add_to_memory_buffer(address, data);
    // }
    // if (address == 0x017f5e)
    // {
    //     printf("Address: %p, Data: %p\n", address, data);
    // }
}

void handleMemoryBus()
{
    // To be executed on second core
    mutex_init(&cpubusMutex);
    mutex_enter_blocking(&cpubusMutex); // Default is that this thread is in charge of the bus and its history array. We only yield occasionally.

    setupPIO();
    setup_dma();
    pio_sm_set_enabled(BUS_PIO, BUS_SM, true);

    uint32_t addressValue = 0;
    uint32_t lastAddressValue = 0;
    uint8_t data = 0;
    uint8_t lastData = 0;
    // uint8_t romsel = 0;
    uint8_t rw = 0;
    uint8_t last_rw = 0;
    readA = true;
    readingA = false;
    readingB = false;

    u_int32_t read_count = 0;
    while (1)
    {
        if (!dma_channel_is_busy(dma_chan_0) && readA)
        {
            readingA = true;
            readA = false;
            for (int i = 0; i < BUFFER_SIZE; i += 1)
            {

                rawBusData = buffer_a[i];
                addressValue = (rawBusData >> 8) & 0x1FFFF;
                data = rawBusData;
                // if (addressValue == 0x017f5e) {
                //     printf("Address: %p, Data: %p\n", addressValue, data);
                // }

                rw = (rawBusData >> 25) & 0x1;

                // if (addressValue == 0x000dc1 || addressValue == 0x0013bf) {
                //     printf("Address: %p, Data: %p, RW: %p\n", addressValue, data, rw);
                // }
                if (addressValue != lastAddressValue && last_rw == 0)
                {
                    try_add_to_circular_buffer(lastAddressValue, lastData);
                }
                lastAddressValue = addressValue;
                lastData = data;
                last_rw = rw;
            }
            readingA = false;
        }
        else if (!dma_channel_is_busy(dma_chan_1) && !readA)
        {
            readingB = true;
            readA = true;
            for (int i = 0; i < BUFFER_SIZE; i += 1)
            {
                rawBusData = buffer_b[i];
                addressValue = (rawBusData >> 8) & 0x1FFFF;
                data = rawBusData;
                rw = (rawBusData >> 25) & 0x1;
                // if (addressValue == 0x017f5e) {
                //     //printf("Address: %p, Data: %p\n", addressValue, data);
                //     printf("Address: %p, Data: %p, RW: %p\n", addressValue, data, rw);

                // }

                // if (addressValue == 0x000dc1 || addressValue == 0x0013bf) {
                //     printf("Address: %p, Data: %p, RW: %p\n", addressValue, data, rw);
                // }
                if (addressValue != lastAddressValue && last_rw == 0)
                {
                    try_add_to_circular_buffer(lastAddressValue, lastData);
                }
                lastAddressValue = addressValue;
                lastData = data;
                last_rw = rw;
            }
            readingB = false;
        }
    }
}

bool prefix(const char *pre, const char *str)
{
    return strncmp(pre, str, strlen(pre)) == 0;
}

// RCHEEVOS

typedef struct
{
    rc_client_server_callback_t callback;
    void *callback_data;
} async_callback_data;

rc_client_t *g_client = NULL;
static void *g_callback_userdata = &g_client; /* dummy data */
char rcheevos_userdata[16];

async_callback_data async_data;

typedef struct
{
    uint8_t id;
    async_callback_data async_data;
} async_callback_data_id;

#define MAX_ASYNC_CALLBACKS 5
async_callback_data_id async_handlers[MAX_ASYNC_CALLBACKS];
uint8_t async_handlers_index = 0;

uint8_t request_id = 0;

/*
 * FIFO for achievements the user won
 */

#define FIFO_SIZE 5

typedef struct
{
    uint32_t buffer[FIFO_SIZE];
    int head;
    int tail;
    int count;
} FIFO_t;

void fifo_init(FIFO_t *fifo)
{
    fifo->head = 0;
    fifo->tail = 0;
    fifo->count = 0;
}

bool fifo_is_empty(FIFO_t *fifo)
{
    return fifo->count == 0;
}

bool fifo_is_full(FIFO_t *fifo)
{
    return fifo->count == FIFO_SIZE;
}

bool fifo_enqueue(FIFO_t *fifo, uint32_t value)
{
    if (fifo_is_full(fifo))
    {
        return false; // FIFO cheia
    }
    fifo->buffer[fifo->tail] = value;
    fifo->tail = (fifo->tail + 1) % FIFO_SIZE;
    fifo->count++;
    return true;
}

bool fifo_dequeue(FIFO_t *fifo, uint32_t *value)
{
    if (fifo_is_empty(fifo))
    {
        return false; // FIFO vazia
    }
    *value = fifo->buffer[fifo->head];
    fifo->head = (fifo->head + 1) % FIFO_SIZE;
    fifo->count--;
    return true;
}

void fifo_print(FIFO_t *fifo)
{
    printf("FIFO: ");
    int index = fifo->head;
    for (int i = 0; i < fifo->count; i++)
    {
        printf("%u ", fifo->buffer[index]);
        index = (index + 1) % FIFO_SIZE;
    }
    printf("\n");
}

FIFO_t achievements_fifo;

// not all address are validated, so it is better not use it
static uint32_t read_memory_do_nothing(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
    return num_bytes;
}

static uint32_t read_memory_init(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
    // handle address mirror
    // if (address <= 0x1FFF)
    // {
    //     address &= 0x07FF;
    // }

    for (int j = 0; j < num_bytes; j += 1)
    {
        address += j;
        uint8_t found = 0;
        for (int i = 0; i < unique_memory_addresses_count; i += 1)
        {
            if (address == unique_memory_addresses[i])
            {
                found = 1;
                break;
            }
        }
        if (found == 0)
        {
            printf("init address %p, num_bytes: %d\n", address, num_bytes);
            unique_memory_addresses_count += 1;
            unique_memory_addresses = (uint32_t *)realloc(unique_memory_addresses, unique_memory_addresses_count * sizeof(uint32_t));
            unique_memory_addresses[unique_memory_addresses_count - 1] = address;
        }
        else
        {
            printf("init address %p, num_bytes: %d (already monitored)\n", address, num_bytes);
        }
        buffer[j] = 0;
    }
    return num_bytes;
}

static uint32_t read_memory_ingame(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
    // handle address mirror
    // if (address <= 0x1FFF)
    // {
    //     address &= 0x07FF;
    // }
    for (int i = 0; i < unique_memory_addresses_count; i += 1)
    {
        if (address == unique_memory_addresses[i])
        {
            for (int j = 0; j < num_bytes; j += 1)
            {
                buffer[j] = memory_data[i + j];
            }
            break;
        }
    }
    return num_bytes;
}

static void rc_client_login_callback(int result, const char *error_message, rc_client_t *client, void *callback_userdata)
{
    if (result == RC_OK)
    {
        printf("Login success\n");
        state = 6; // load game
    }
    else
    {
        printf("Login failed\n");
    }
}

static void rc_client_load_game_callback(int result, const char *error_message, rc_client_t *client, void *callback_userdata)
{
    if (result == RC_OK)
    {
        state = 8; // read from circular buffer
        if (rc_client_is_game_loaded(g_client))
        {
            printf("Game loaded\n");
            const rc_client_game_t *game = rc_client_get_game_info(g_client);
            char url[256];
            rc_client_game_get_image_url(game, url, sizeof(url));
            char aux[512];
            sprintf(aux, "GAME_INFO=%lu;%s;%s\r\n", (unsigned long)game->id, game->title, url);
            printf(aux);
            uart_puts(UART_ID, aux);
        }
        rc_client_set_read_memory_function(g_client, read_memory_init);
        rc_client_do_frame(g_client);

        // bubble sort unique_memory_addresses
        for (int i = 0; i < unique_memory_addresses_count; i += 1)
        {
            for (int j = 0; j < unique_memory_addresses_count - i - 1; j += 1)
            {
                if (unique_memory_addresses[j] > unique_memory_addresses[j + 1])
                {
                    uint16_t temp = unique_memory_addresses[j];
                    unique_memory_addresses[j] = unique_memory_addresses[j + 1];
                    unique_memory_addresses[j + 1] = temp;
                }
            }
        }
        memory_data = (uint8_t *)malloc(unique_memory_addresses_count * sizeof(uint8_t));
        memset(memory_data, 0, unique_memory_addresses_count * sizeof(uint8_t));
        rc_client_set_read_memory_function(g_client, read_memory_ingame);
        multicore_launch_core1(handleMemoryBus);
    }
    else
    {
        printf("Game not loaded\n");
    }
}

static void achievement_triggered(const rc_client_achievement_t *achievement)
{
    fifo_enqueue(&achievements_fifo, achievement->id);
}

static void event_handler(const rc_client_event_t *event, rc_client_t *client)
{
    switch (event->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        achievement_triggered(event->achievement);
        break;

    default:
        printf("Unhandled event %d\n", event->type);
        break;
    }
}

// This is the callback function for the asynchronous HTTP call (which is not provided in this example)
static void http_callback(int status_code, const char *content, size_t content_size, void *userdata, const char *error_message)
{
    // Prepare a data object to pass the HTTP response to the callback
    rc_api_server_response_t server_response;
    memset(&server_response, 0, sizeof(server_response));
    server_response.body = content;
    server_response.body_length = content_size;
    server_response.http_status_code = status_code;

    // handle non-http errors (socket timeout, no internet available, etc)
    if (status_code == 0 && error_message)
    {
        // assume no server content and pass the error through instead
        server_response.body = error_message;
        server_response.body_length = strlen(error_message);
        // Let rc_client know the error was not catastrophic and could be retried. It may decide to retry or just
        // immediately pass the error to the callback. To prevent possible retries, use RC_API_SERVER_RESPONSE_CLIENT_ERROR.
        server_response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    }

    // Get the rc_client callback and call it
    async_callback_data *async_data = (async_callback_data *)userdata;
    async_data->callback(&server_response, async_data->callback_data);
}

rc_clock_t get_pico_millisecs(const rc_client_t *client)
{
    return to_ms_since_boot(get_absolute_time());
}

// This is the HTTP request dispatcher that is provided to the rc_client. Whenever the client
// needs to talk to the server, it will call this function.
static void server_call(const rc_api_request_t *request,
                        rc_client_server_callback_t callback, void *callback_data, rc_client_t *client)
{
    char buffer[512];
    async_data.callback = callback;
    async_data.callback_data = callback_data;
    char method[8];
    if (request->post_data)
    {
        strcpy(method, "POST");
    }
    else
    {
        strcpy(method, "GET");
    }
    sprintf(buffer, "REQ=%02hhX;M:%s;U:%s;D:%s\r\n", request_id, method, request->url, request->post_data);
    async_handlers[async_handlers_index].id = request_id;
    async_handlers[async_handlers_index].async_data.callback = callback;
    async_handlers[async_handlers_index].async_data.callback_data = callback_data;
    async_handlers_index = async_handlers_index + 1 % MAX_ASYNC_CALLBACKS;
    request_id += 1;
    printf("REQ=%s\n", request->post_data); // DEBUG
    request_ongoing += 1;
    last_request = to_ms_since_boot(get_absolute_time());
    uart_puts(UART_ID, buffer);
}

#include <malloc.h>

const char pico_version_command[] = "PICO_FIRMWARE_VERSION=0.7\r\n";
const char nes_reseted_command[] = "NES_RESETED\r\n";
const char buffer_overflow_command[] = "BUFFER_OVERFLOW\r\n";

uint64_t last_frame_processed = 0;

int main()
{
    // setup
    stdio_init_all();
    set_sys_clock_khz(250000, true);

    resetGPIO();

    // memset(history, 0, 256 * sizeof(uint32_t));

    uart_init(UART_ID, BAUD_RATE);

    // Configura os pinos GPIO para a UART
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Habilita a porta UART
    uart_set_hw_flow(UART_ID, true, true);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);
    printf(pico_version_command);
    memset(serial_buffer, '\0', SERIAL_BUFFER_SIZE);

    unsigned int frame_counter = 0;

    // Do nothing on core 0
    while (true)
    {
        // handle on going request and timeout
        if (request_ongoing > 0)
        {
            uint32_t current_time = to_ms_since_boot(get_absolute_time());
            if (current_time - last_request > 30000)
            {
                printf("request timeout\n");
                request_ongoing = 0;
            }
        }

        // if there is no request on the fly and there is an achievement to be sent
        if (request_ongoing == 0 && fifo_is_empty(&achievements_fifo) == false)
        {
            uint32_t achievement_id;
            fifo_dequeue(&achievements_fifo, &achievement_id);
            const rc_client_achievement_t *achievement = rc_client_get_achievement_info(g_client, achievement_id);
            char url[128];
            const char *title = achievement->title;
            rc_client_achievement_get_image_url(achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url));
            char aux[512];
            memset(aux, 0, 512);
            sprintf(aux, "A=%lu;%s;%s\r\n", (unsigned long)achievement_id, title, url);
            uart_puts(UART_ID, aux);
            printf(aux);
        }

        if (state == 1)
        {
            // read CRC
            // handleCRC32();
            state = 2;
        }
        if (state == 6)
        {
            // load the game
            rc_client_begin_load_game(g_client, md5, rc_client_load_game_callback, g_callback_userdata);
            state = 7;
        }
        if (state == 8)
        {
            // read from circular buffer

            if (memory_buffer_size() > 0)
            {
                // printf("r");
                memory_unit memory = read_from_memory_buffer();
                // buffer not empty
                if (nes_reseted == 0 && memory.address < 0x07FF) // started writing on memory ram
                {
                    nes_reseted = 1;
                    uart_puts(UART_ID, nes_reseted_command); // NES_RESETED\r\n
                    for (int i = 0; i < unique_memory_addresses_count; i += 1)
                    {
                        printf("%03X ", unique_memory_addresses[i]);
                    }
                    printf("\n");
                }

                // if (memory.address == 0x4014)
                // {
                //     // printf(".");
                //     // best place to detect a frame so far
                //     // u_int64_t now = to_ms_since_boot(get_absolute_time());
                //     // if ((now - last_frame_processed) > 15) // skip frame if we are too close to the last one
                //     // {
                //     rc_client_do_frame(g_client);
                //     // printf("F_");
                //     last_frame_processed = to_ms_since_boot(get_absolute_time());
                //     // } else {
                //     //     printf("S_");
                //     // }

                //     // memory dump during a frame for DEBUG
                //     // printf("\nF\n");
                //     // for (int i = 0; i < unique_memory_addresses_count; i += 1)
                //     // {
                //     //     //  "0xH006c=0_0xH0029=33_0xP0101>d0xP0101", // mega man 5
                //     //     if (unique_memory_addresses[i] == 0x006C || unique_memory_addresses[i] == 0x0029 || unique_memory_addresses[i] == 0x0101)
                //     //     {
                //     //         printf("%03X ", memory_data[i]);
                //     //     }
                //     // }
                //     // printf ("\n");

                //     // debug memory circular buffer size
                //     frame_counter += 1;
                //     if (frame_counter % 1800 == 0) //~ 30 seconds
                //     {
                //         if (memory_buffer_size() > 0)
                //         {
                //             printf("F: %d, BS: %d\n", frame_counter, memory_buffer_size());
                //         }
                //     }
                // }
                // else
                {
                    // if (memory.address <= 0x1FFF)
                    // {
                    //     memory.address = memory.address & 0x07FF; // handle ram mirror
                    // }
                    for (int i = 0; i < unique_memory_addresses_count; i += 1)
                    {
                        if (memory.address == unique_memory_addresses[i])
                        {
                            memory_data[i] = memory.data;
                            break;
                        }
                    }
                }

                // simualte a frame every 33ms with we cannot detect any
                // example of need: punchout
                u_int64_t now = to_ms_since_boot(get_absolute_time());
                if ((now - last_frame_processed) > 16)
                {
                    // printf("*");
                    rc_client_do_frame(g_client);
                    last_frame_processed = now;
                    //     for (int i = 0; i < unique_memory_addresses_count; i += 1)
                    //     {
                    //         //  "0xH006c=0_0xH0029=33_0xP0101>d0xP0101", // mega man 5
                    //         if (unique_memory_addresses[i] == 0x006C || unique_memory_addresses[i] == 0x0029 || unique_memory_addresses[i] == 0x0101)
                    //         {
                    //             printf("%03X ", memory_data[i]);
                    //         }
                    //     }
                    //     printf ("\n");
                }
            }
        }
        if (uart_is_readable(UART_ID))
        {

            char received_char = uart_getc(UART_ID);
            // printf(serial_buffer);
            serial_buffer_head[0] = received_char;
            serial_buffer_head += 1;
            if (serial_buffer_head - serial_buffer == SERIAL_BUFFER_SIZE)
            {
                memset(serial_buffer, 0, SERIAL_BUFFER_SIZE);
                printf(buffer_overflow_command); // BUFFER_OVERFLOW\r\n
                continue;
            }
            char *pos = NULL;
            char *current_char = serial_buffer_head - 1;
            if (current_char[0] == '\n')
            {
                char *previous_char = current_char - 1;
                if (previous_char[0] == '\r')
                {
                    pos = previous_char;
                }
            }
            if (pos != NULL)
            {
                if (((unsigned char *)pos) - serial_buffer == 0)
                {
                    memset(serial_buffer, 0, 2); // Clear the buffer since we are reading char by char
                    serial_buffer_head = serial_buffer;
                    continue;
                }
                int len = serial_buffer_head - serial_buffer;
                serial_buffer_head[0] = '\0';
                char *command;
                command = serial_buffer;

                // printf("CMD=%s\r\n", command);
                if (prefix("RESP=", command))
                {
                    printf("L:RESP\n");
                    request_ongoing -= 1;
                    char *response_ptr = command + 5;
                    char aux[8];
                    strncpy(aux, response_ptr, 2);
                    aux[2] = '\0';
                    uint8_t request_id = (uint8_t)strtol(aux, NULL, 16);
                    response_ptr += 3;
                    strncpy(aux, response_ptr, 3);
                    aux[3] = '\0';
                    response_ptr += 4;
                    uint16_t http_code = (uint16_t)strtol(aux, NULL, 16);
                    for (int i = 0; i < MAX_ASYNC_CALLBACKS; i += 1)
                    {
                        if (async_handlers[i].id == request_id)
                        {
                            if (request_id == 2)
                            {
                                printf("RESP=%s\n", response_ptr);
                            }
                            async_callback_data async_data = async_handlers[i].async_data;
                            http_callback(http_code, response_ptr, strlen(response_ptr), &async_data, NULL);
                            break;
                        }
                    }
                }
                else if (prefix("TOKEN_AND_USER", command))
                {
                    // example TOKEN_AND_USER=odelot,token
                    printf("L:TOKEN_AND_USER\n");
                    char *token_ptr = command + 15;
                    int comma_index = 0;
                    len = strlen(token_ptr);
                    for (int i = 0; i < len; i += 1)
                    {
                        if (token_ptr[i] == ',')
                        {
                            comma_index = i;
                            break;
                        }
                    }
                    memset(ra_token, '\0', 32);
                    memset(ra_user, '\0', 256);
                    strncpy(ra_token, token_ptr, comma_index);
                    strncpy(ra_user, token_ptr + comma_index + 1, len - comma_index - 1 - 2);
                    printf("USER=%s\r\n", ra_user);
                    printf("TOKEN=%s\r\n", ra_token);
                }
                else if (prefix("CRC_FOUND_MD5", command))
                {
                    printf("L:CRC_FOUND_MD5\n");
                    char *md5_ptr = command + 14;
                    strncpy(md5, md5_ptr, 32);
                    md5[32] = '\0';
                    printf("MD5=%s\r\n", md5);
                }
                else if (prefix("RESET", command)) // RESET
                {
                    printf("L:RESET\r\n");
                    uint f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
                    printf("pll_sys  = %dkHz\n", f_pll_sys);
                    fifo_init(&achievements_fifo);
                    state = 0;
                    nes_reseted = 0;
                    memset(md5, '\0', 33);
                    crcBegin = 0xFFFFFFFF;
                    resetGPIO();

                    free(unique_memory_addresses);
                    free(memory_data);
                    unique_memory_addresses = NULL;
                    memory_data = NULL;
                    unique_memory_addresses_count = 0;
                }
                else if (prefix("READ_CRC", command))
                {
                    printf("L:READ_CRC\n");
                    state = 1;
                    printf("STATE=%d\r\n", state);
                }
                else if (prefix("START_WATCH", command))
                {
                    printf("L:START_WATCH\n");
                    // init rcheevos

                    g_client = initialize_retroachievements_client(g_client, read_memory_do_nothing, server_call);
                    rc_client_get_user_agent_clause(g_client, rcheevos_userdata, sizeof(rcheevos_userdata)); // TODO: send to esp32 before doing requests
                    rc_client_set_event_handler(g_client, event_handler);
                    rc_client_set_get_time_millisecs_function(g_client, get_pico_millisecs);
                    rc_client_begin_login_with_token(g_client, ra_user, ra_token, rc_client_login_callback, g_callback_userdata);
                    state = 5;
                }
                memset(serial_buffer, 0, len); // Clear the buffer since we are reading char by char
                serial_buffer_head = serial_buffer;
            }
        }
    }
}