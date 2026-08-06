#include "rtc.h"
#include "io.h"

#define CMOS_ADDRESS 0x70U
#define CMOS_DATA 0x71U

#define REGISTER_SECOND 0x00U
#define REGISTER_MINUTE 0x02U
#define REGISTER_HOUR 0x04U
#define REGISTER_DAY 0x07U
#define REGISTER_MONTH 0x08U
#define REGISTER_YEAR 0x09U
#define REGISTER_CENTURY 0x32U
#define REGISTER_STATUS_A 0x0AU
#define REGISTER_STATUS_B 0x0BU

#define STATUS_A_UPDATE_IN_PROGRESS 0x80U
#define STATUS_B_24_HOUR 0x02U
#define STATUS_B_BINARY 0x04U

static boot_uint8_t cmos_read(boot_uint8_t reg) {
    /* Bit 7 of the index port masks NMI. Keep it set while addressing the
       CMOS: an NMI arriving between the index write and the data read would
       leave the index pointing somewhere else. */
    outb(CMOS_ADDRESS, (boot_uint8_t)(reg | 0x80U));
    io_wait();
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return (cmos_read(REGISTER_STATUS_A) & STATUS_A_UPDATE_IN_PROGRESS) != 0;
}

static boot_uint8_t from_bcd(boot_uint8_t value) {
    return (boot_uint8_t)((value & 0x0FU) + ((value >> 4) * 10U));
}

void rtc_read(RTC_TIME* time) {
    boot_uint8_t status;
    boot_uint8_t century;
    RTC_TIME first;
    int attempts;

    if (!time) return;

    /* Read twice and accept only when both agree. The clock ticks while we
       are reading it, and a tick between the seconds and the hours field
       would produce a time that never existed. */
    for (attempts = 0; attempts < 10; attempts++) {
        while (update_in_progress());
        first.second = cmos_read(REGISTER_SECOND);
        first.minute = cmos_read(REGISTER_MINUTE);
        first.hour = cmos_read(REGISTER_HOUR);
        first.day = cmos_read(REGISTER_DAY);
        first.month = cmos_read(REGISTER_MONTH);
        first.year = cmos_read(REGISTER_YEAR);
        century = cmos_read(REGISTER_CENTURY);

        while (update_in_progress());
        if (first.second == cmos_read(REGISTER_SECOND) &&
            first.minute == cmos_read(REGISTER_MINUTE) &&
            first.hour == cmos_read(REGISTER_HOUR) &&
            first.day == cmos_read(REGISTER_DAY)) break;
    }

    status = cmos_read(REGISTER_STATUS_B);

    if (!(status & STATUS_B_BINARY)) {
        boot_uint8_t raw_hour = first.hour;
        first.second = from_bcd(first.second);
        first.minute = from_bcd(first.minute);
        /* Bit 7 of the hour is the PM flag in 12-hour mode and must survive
           the BCD conversion, so mask it off first and restore it after. */
        first.hour = (boot_uint8_t)(from_bcd((boot_uint8_t)(raw_hour & 0x7FU)) |
                                    (raw_hour & 0x80U));
        first.day = from_bcd(first.day);
        first.month = from_bcd(first.month);
        first.year = from_bcd((boot_uint8_t)first.year);
        century = from_bcd(century);
    }

    if (!(status & STATUS_B_24_HOUR) && (first.hour & 0x80U)) {
        first.hour = (boot_uint8_t)(((first.hour & 0x7FU) + 12U) % 24U);
    }

    time->second = first.second;
    time->minute = first.minute;
    time->hour = (boot_uint8_t)(first.hour & 0x7FU);
    time->day = first.day;
    time->month = first.month;
    /* The century register is optional and reads as 0 or garbage on machines
       that lack it; fall back to assuming the 2000s. */
    if (century >= 19 && century <= 21)
        time->year = (boot_uint16_t)(century * 100U + first.year);
    else
        time->year = (boot_uint16_t)(2000U + first.year);
}

boot_uint16_t rtc_fat_date(const RTC_TIME* time) {
    boot_uint16_t year;
    if (!time || time->year < 1980) return 0;
    year = (boot_uint16_t)(time->year - 1980);
    return (boot_uint16_t)((year << 9) | ((time->month & 0x0FU) << 5) |
                           (time->day & 0x1FU));
}

boot_uint16_t rtc_fat_time(const RTC_TIME* time) {
    if (!time) return 0;
    /* FAT stores seconds in two-second units - the format has never had
       one-second resolution on the modification timestamp. */
    return (boot_uint16_t)(((time->hour & 0x1FU) << 11) |
                           ((time->minute & 0x3FU) << 5) |
                           ((time->second / 2U) & 0x1FU));
}
