#ifndef KERNEL_RTC_H
#define KERNEL_RTC_H

#include "../include/bootinfo.h"

/* Wall-clock time from the CMOS real-time clock. FAT stores a creation and a
   modification timestamp on every directory entry, so a filesystem that can
   write needs a clock - which is why this arrived earlier than the roadmap
   had it. */
typedef struct {
    boot_uint16_t year;
    boot_uint8_t month;   /* 1-12 */
    boot_uint8_t day;     /* 1-31 */
    boot_uint8_t hour;    /* 0-23 */
    boot_uint8_t minute;
    boot_uint8_t second;
} RTC_TIME;

void rtc_read(RTC_TIME* time);

/* Packed into the two 16-bit fields a FAT directory entry uses:
   date = year-1980 << 9 | month << 5 | day
   time = hour << 11 | minute << 5 | second/2  */
boot_uint16_t rtc_fat_date(const RTC_TIME* time);
boot_uint16_t rtc_fat_time(const RTC_TIME* time);

#endif
