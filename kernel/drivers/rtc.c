#include "rtc.h"
#include "io.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static int get_update_in_progress_flag(void) {
    outb(CMOS_ADDRESS, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

static uint8_t get_rtc_register(int reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

void read_rtc(rtc_time_t *time) {
    uint8_t last_second;
    uint8_t last_minute;
    uint8_t last_hour;
    uint8_t last_day;
    uint8_t last_month;
    uint8_t last_year;
    uint8_t registerB;

    // Make sure an update isn't in progress
    while (get_update_in_progress_flag());
    time->second = get_rtc_register(0x00);
    time->minute = get_rtc_register(0x02);
    time->hour = get_rtc_register(0x04);
    time->day = get_rtc_register(0x07);
    time->month = get_rtc_register(0x08);
    time->year = get_rtc_register(0x09);

    do {
        last_second = time->second;
        last_minute = time->minute;
        last_hour = time->hour;
        last_day = time->day;
        last_month = time->month;
        last_year = time->year;

        while (get_update_in_progress_flag());
        time->second = get_rtc_register(0x00);
        time->minute = get_rtc_register(0x02);
        time->hour = get_rtc_register(0x04);
        time->day = get_rtc_register(0x07);
        time->month = get_rtc_register(0x08);
        time->year = get_rtc_register(0x09);
    } while ((last_second != time->second) || (last_minute != time->minute) || 
             (last_hour != time->hour) || (last_day != time->day) || 
             (last_month != time->month) || (last_year != time->year));

    registerB = get_rtc_register(0x0B);

    // Convert BCD to binary values if necessary
    if (!(registerB & 0x04)) {
        time->second = (time->second & 0x0F) + ((time->second / 16) * 10);
        time->minute = (time->minute & 0x0F) + ((time->minute / 16) * 10);
        time->hour = ( (time->hour & 0x0F) + (((time->hour & 0x70) / 16) * 10) ) | (time->hour & 0x80);
        time->day = (time->day & 0x0F) + ((time->day / 16) * 10);
        time->month = (time->month & 0x0F) + ((time->month / 16) * 10);
        time->year = (time->year & 0x0F) + ((time->year / 16) * 10);
    }

    // Convert 12 hour clock to 24 hour clock if necessary
    if (!(registerB & 0x02) && (time->hour & 0x80)) {
        time->hour = ((time->hour & 0x7F) + 12) % 24;
    }

    // Calculate the full (4-digit) year
    time->year += 2000;
}
