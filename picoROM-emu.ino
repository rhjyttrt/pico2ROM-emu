#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "tusb.h"

// usb cdc check
static inline bool stdio_usb_connected(void) {
    return tud_cdc_connected();
}

#define LED_ACTIVE_LOW  0

// bus pins
#define ADDR_BUS_SHIFT  0
#define ADDR_BUS_MASK   0x7FFF
#define PIN_PHI2        15
#define DATA_BUS_MASK   ((0x7F << 16) | (1u << 26))
#define PIN_RW          27
#define PIN_ROM_CS      28
#define LED_NOT_FOUND   24
#define LED_LOADED      25

#define PHI2_MASK       (1u << PIN_PHI2)
#define RW_MASK         (1u << PIN_RW)
#define CS_MASK         (1u << PIN_ROM_CS)

#define MAX_ROM_LIMIT   (32 * 1024)

// rx ring buffer
#define RX_RING_BUFFER_SIZE 1024
volatile uint8_t rx_ring_buffer[RX_RING_BUFFER_SIZE];
volatile uint16_t rx_head = 0;
volatile uint16_t rx_tail = 0;

// sram rom buffer
__attribute__((aligned(32))) uint8_t rom_buffer[MAX_ROM_LIMIT];

volatile bool rom_ready = false;
volatile uint32_t bus_read_counter = 0;

// set led state
static inline void set_led_state(uint pin, bool state) {
#if LED_ACTIVE_LOW
    gpio_put(pin, !state);
#else
    gpio_put(pin, state);
#endif
}

// update rx buffer
void update_rx_buffer(void) {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        uint16_t next_head = (rx_head + 1) & (RX_RING_BUFFER_SIZE - 1);
        if (next_head != rx_tail) {
            rx_ring_buffer[rx_head] = (uint8_t)c;
            rx_head = next_head;
        }
    }
}

// check available rx bytes
bool rx_available_bytes(void) {
    update_rx_buffer();
    return (rx_head != rx_tail);
}

// read byte from rx buffer
int rx_read_byte(void) {
    update_rx_buffer();
    if (rx_head == rx_tail) return -1;
    uint8_t c = rx_ring_buffer[rx_tail];
    rx_tail = (rx_tail + 1) & (RX_RING_BUFFER_SIZE - 1);
    return c;
}

// parse hex nibble
static inline uint8_t parse_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0xFF;
}

// parse hex byte
static inline uint8_t parse_hex_byte(const char *p, bool *valid) {
    uint8_t n1 = parse_hex_nibble(p[0]);
    uint8_t n2 = parse_hex_nibble(p[1]);
    if (n1 == 0xFF || n2 == 0xFF) {
        *valid = false;
        return 0;
    }
    return (n1 << 4) | n2;
}

// process single intel hex line
static bool process_hex_line(const char* line, uint32_t *extended_address_base, bool *eof_reached) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strlen(p) < 11 || p[0] != ':') return true; 

    bool valid = true;
    uint8_t len = parse_hex_byte(&p[1], &valid);
    if (!valid) return false;

    size_t byte_count = 5 + len; 
    uint8_t line_bytes[261];
    uint8_t checksum = 0;

    for (size_t i = 0; i < byte_count; i++) {
        uint8_t b = parse_hex_byte(&p[1 + (i * 2)], &valid);
        if (!valid) return false;
        line_bytes[i] = b;
        checksum += b;
    }

    if (checksum != 0) return false; 

    uint16_t addr = ((uint16_t)line_bytes[1] << 8) | line_bytes[2];
    uint8_t type = line_bytes[3];

    if (type == 0x00) {
        for (int i = 0; i < len; i++) {
            uint32_t final_addr = *extended_address_base + addr + i;
            rom_buffer[final_addr & (MAX_ROM_LIMIT - 1)] = line_bytes[4 + i];
        }
    } else if (type == 0x02) {
        *extended_address_base = (uint32_t)(((uint16_t)line_bytes[4] << 8) | line_bytes[5]) << 4;
    } else if (type == 0x04) {
        *extended_address_base = (uint32_t)(((uint16_t)line_bytes[4] << 8) | line_bytes[5]) << 16;
    } else if (type == 0x01) {
        *eof_reached = true;
    }
    return true;
}

// receive hex file over serial
bool receive_hex_from_serial(void) {
    memset(rom_buffer, 0xFF, MAX_ROM_LIMIT);

    // wait for usb connection
    while (!stdio_usb_connected()) {
        busy_wait_us_32(100000);
    }

    printf("\n[serial] waiting for hex file stream...\n");

    static char line[1024];
    int line_idx = 0;
    uint32_t extended_address_base = 0;
    bool eof_reached = false;
    uint32_t last_byte_time = 0;
    uint32_t line_count = 0;
    bool started_receiving = false;

    while (!eof_reached) {
        if (rx_available_bytes()) {
            started_receiving = true;
            last_byte_time = to_ms_since_boot(get_absolute_time());
            
            int c_val = rx_read_byte();
            if (c_val < 0) continue;
            char c = (char)c_val;

            if (c == '\r' || c == '\n') {
                if (line_idx > 0) {
                    line[line_idx] = '\0';
                    line_idx = 0;
                    if (!process_hex_line(line, &extended_address_base, &eof_reached)) {
                        printf("\n[error] hex error!\n");
                        return false;
                    }
                    line_count++;
                    if (line_count % 20 == 0) {
                        printf(".");
                    }
                }
            } else if (line_idx < 1023) {
                line[line_idx++] = c;
            } else {
                printf("\n[error] buffer overflow!\n");
                return false;
            }
        } else {
            // timeout if transfer stalls
            if (started_receiving && (to_ms_since_boot(get_absolute_time()) - last_byte_time > 30000)) {
                printf("\n[error] serial timeout!\n");
                return false;
            }
        }
    }

    printf("\n[serial] hex file loaded!\n");
    return true;
}

// setup bus gpio pins
void setup_bus_pins(void) {
    const uint target_pins[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,16,17,18,19,20,21,22,26,PIN_PHI2,PIN_RW,PIN_ROM_CS};
    sio_hw->gpio_oe_clr = DATA_BUS_MASK; 
    
    for (size_t i = 0; i < sizeof(target_pins)/sizeof(target_pins[0]); i++) {
        uint pin = target_pins[i];
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin); 
    }

    gpio_pull_up(PIN_ROM_CS);
    gpio_pull_down(PIN_PHI2);

    gpio_set_input_hysteresis_enabled(PIN_PHI2, true);
    gpio_set_input_hysteresis_enabled(PIN_ROM_CS, true);
    gpio_set_input_hysteresis_enabled(PIN_RW, true);

    const uint data_pins[] = {16,17,18,19,20,21,22,26};
    for (size_t i = 0; i < sizeof(data_pins)/sizeof(data_pins[0]); i++) {
        gpio_set_drive_strength(data_pins[i], GPIO_DRIVE_STRENGTH_8MA);
        gpio_set_slew_rate(data_pins[i], GPIO_SLEW_RATE_FAST);
    }
}

// shutdown usb hardware
void shutdown_usb(void) {
    if (stdio_usb_connected()) {
        printf("starting emulator core...\n");
        Serial.flush();
    }
    delay(50);

    irq_set_enabled(USBCTRL_IRQ, false);
    usb_hw->sie_ctrl = 0;
}

// core 1 emulation loop
void __not_in_flash_func(rom_emulator_task)(void) {
    (void)save_and_disable_interrupts();

    io_ro_32 * const gpio_in_reg     = &sio_hw->gpio_in;
    io_rw_32 * const gpio_set_reg    = &sio_hw->gpio_set;
    io_rw_32 * const gpio_clr_reg    = &sio_hw->gpio_clr;
    io_rw_32 * const gpio_oe_set_reg = &sio_hw->gpio_oe_set;
    io_rw_32 * const gpio_oe_clr_reg = &sio_hw->gpio_oe_clr;

    const uint32_t data_mask = DATA_BUS_MASK;

    while (rom_ready) {
        uint32_t gpio_in;
        
        do {
            gpio_in = *gpio_in_reg;
        } while (!(gpio_in & PHI2_MASK) || (gpio_in & CS_MASK));

        asm volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");
        gpio_in = *gpio_in_reg; 

        uint16_t address = (gpio_in >> ADDR_BUS_SHIFT) & ADDR_BUS_MASK;

        if ((gpio_in & RW_MASK) && !(gpio_in & CS_MASK)) { 
            uint8_t data = rom_buffer[address];

            uint32_t data_out = ((uint32_t)(data & 0x7F) << 16) | 
                                ((uint32_t)(data & 0x80) << 19);

            *gpio_clr_reg = data_mask;
            *gpio_set_reg = data_out;
            __dmb(); 
            *gpio_oe_set_reg = data_mask; 

            bus_read_counter++;

            uint32_t release_sample;
            do {
                release_sample = *gpio_in_reg;
            } while ((release_sample & PHI2_MASK) && !(release_sample & CS_MASK));

            asm volatile("nop\nnop\nnop\nnop");
            *gpio_oe_clr_reg = data_mask; 
        } else { 
            uint32_t release_sample;
            do {
                release_sample = *gpio_in_reg;
            } while ((release_sample & PHI2_MASK) && !(release_sample & CS_MASK));
        }
    }
}

// core 1 entry point
void __not_in_flash_func(core1_entry)(void) {
    while (!rom_ready) {
        busy_wait_us_32(1000);
    }
    
    __dmb();
    __isb();

    rom_emulator_task();
}

// main setup
void setup() {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    delay(10);
    
    qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~QMI_M0_TIMING_CLKDIV_BITS) | (3u << QMI_M0_TIMING_CLKDIV_LSB);
    
    if (!set_sys_clock_khz(300000, true)) {
        set_sys_clock_khz(250000, true);
    }

    delay(150);

    Serial.begin(115200);

    set_led_state(LED_LOADED, false);
    gpio_init(LED_LOADED);
    gpio_set_dir(LED_LOADED, GPIO_OUT);

    set_led_state(LED_NOT_FOUND, false);
    gpio_init(LED_NOT_FOUND);
    gpio_set_dir(LED_NOT_FOUND, GPIO_OUT);

    delay(300);

    multicore_launch_core1(core1_entry);

    // receive rom image
    if (!receive_hex_from_serial()) {
        if (stdio_usb_connected()) printf("[error] upload failed! freezing.\n");
        while (true) {
            set_led_state(LED_NOT_FOUND, true);
            busy_wait_us_32(100000);
            set_led_state(LED_NOT_FOUND, false);
            busy_wait_us_32(100000);
        }
    }

    shutdown_usb();

    setup_bus_pins();
    set_led_state(LED_LOADED, true);

    __dsb();
    rom_ready = true; 
}

// main loop
void loop() {
    static uint32_t last_check_time = 0;
    static uint32_t prev_read_count = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_check_time >= 30) { 
        last_check_time = now;

        uint32_t current_count = *(volatile uint32_t *)&bus_read_counter;
        __dmb();
        
        // pulse activity led
        if (current_count != prev_read_count) {
            set_led_state(LED_LOADED, false);
            busy_wait_us_32(800);
            set_led_state(LED_LOADED, true);
            prev_read_count = current_count;
        } else {
            set_led_state(LED_LOADED, true);
        }
    }
}