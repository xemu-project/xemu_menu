#include <crypt/rc4.h>
#include <crypt/sha1.h>
#include <hal/video.h>
#include <stdlib.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "main.h"

#define MAX_LINES      32
#define MAX_CHARACTERS 512
static char char_pool[MAX_CHARACTERS];
static int pool_offset;

static int dvd_region_index = 0;
static int language_index = 0;
static unsigned char mac_address[6];
static ULONG video_flags = 0;
static ULONG audio_flags = 0;
static ULONG av_region = 0;
static ULONG game_region_index = 0;
static int time_zone_offset = 0;
static int dirty = 0;

#define EEPROM_SMBUS_ADDRESS  0xA8
#define EEPROM_FACTORY_OFFSET 0x30
typedef struct
{
    unsigned int checksum;           // 0x30
    unsigned char serial_number[12]; // 0x34
    unsigned char mac_address[6];    // 0x40
    unsigned char padding1[2];       // 0x46
    unsigned char online_key[16];    // 0x48
    unsigned int av_region;          // 0x58
    unsigned char padding2[4];       // 0x5C
} eeprom_factory_settings_t;

static void update_eeprom_text(void);

// https://github.com/xemu-project/xemu/blob/9d5cf0926aa6f8eb2221e63a2e92bd86b02afae0/hw/xbox/eeprom_generation.c#L25
static unsigned int xbox_eeprom_crc(unsigned char *data, unsigned int len)
{
    unsigned int high = 0;
    unsigned int low = 0;
    for (unsigned int i = 0; i < len / 4; i++) {
        unsigned int val = ((unsigned int *)data)[i];
        uint64_t sum = ((uint64_t)high << 32) | low;

        high = (sum + val) >> 32;
        low += val;
    }
    return ~(high + low);
}

static void apply_settings(void)
{
    ULONG type = 4;
    ExSaveNonVolatileSetting(XC_DVD_REGION, type, &dvd_region_index, sizeof(dvd_region_index));
    ExSaveNonVolatileSetting(XC_LANGUAGE, type, &language_index, sizeof(language_index));
    ExSaveNonVolatileSetting(XC_VIDEO, type, &video_flags, sizeof(video_flags));
    ExSaveNonVolatileSetting(XC_AUDIO, type, &audio_flags, sizeof(audio_flags));
    ExSaveNonVolatileSetting(XC_TIMEZONE_BIAS, type, &time_zone_offset, sizeof(time_zone_offset));

    // Can't use ExSaveNonVolatileSetting for the factory settings,
    // so we write it directly to the EEPROM and manually calculate the checksum for this region.
    eeprom_factory_settings_t factory_settings;
    unsigned char *factory_settings_ptr8 = (unsigned char *)&factory_settings;
    for (unsigned int i = 0; i < sizeof(factory_settings); i++) {
        HalReadSMBusValue(EEPROM_SMBUS_ADDRESS, EEPROM_FACTORY_OFFSET + i, FALSE, (ULONG *)(&factory_settings_ptr8[i]));
    }
    factory_settings.av_region = av_region;
    memcpy(factory_settings.mac_address, mac_address, sizeof(factory_settings.mac_address));
    factory_settings.checksum = xbox_eeprom_crc(&factory_settings_ptr8[4],
                                                sizeof(factory_settings) - sizeof(factory_settings.checksum));
    for (unsigned int i = 0; i < sizeof(factory_settings); i++) {
        HalWriteSMBusValue(EEPROM_SMBUS_ADDRESS, EEPROM_FACTORY_OFFSET + i, FALSE, factory_settings_ptr8[i]);
    }

    dirty = 0;
    update_eeprom_text();
}

static void increment_game_region(void)
{
    static const ULONG regions[4] = {GAME_REGION_NA, GAME_REGION_JAPAN, GAME_REGION_EUROPE, GAME_REGION_MANUFACTURING};
    int index = 0;
    for (int i = 0; i < 4; i++) {
        if (regions[i] == game_region_index) {
            index = i;
            index = (index + 1) % 4;
            break;
        }
    }
    game_region_index = regions[index];
    dirty = 1;
    update_eeprom_text();
}

static void increment_dvd_region(void)
{
    dvd_region_index = (dvd_region_index + 1) % 7;
    dirty = 1;
    update_eeprom_text();
}

static void increment_language(void)
{
    language_index = (language_index + 1) % 10;
    dirty = 1;
    update_eeprom_text();
}

static void increment_video_region(void)
{
    static const ULONG regions[4] = {AV_REGION_NTSC, AV_REGION_PAL, AV_REGION_NTSCJ, AV_REGION_PALM};
    int index = 0;
    for (int i = 0; i < 4; i++) {
        if (regions[i] == av_region) {
            index = i;
            index = (index + 1) % 4;
            break;
        }
    }
    av_region = regions[index];
    dirty = 1;
    update_eeprom_text();
}

static void video_increment_aspect_ratio(void)
{
    if (video_flags & VIDEO_WIDESCREEN) {
        video_flags &= ~VIDEO_WIDESCREEN;
        video_flags |= VIDEO_LETTERBOX;
    } else if (video_flags & VIDEO_LETTERBOX) {
        video_flags &= ~VIDEO_LETTERBOX;
    } else {
        video_flags |= VIDEO_WIDESCREEN;
    }
    dirty = 1;
    update_eeprom_text();
}

static void video_increment_refresh_rate(void)
{
    ULONG index = (video_flags >> 22) & 0x03;
    index = (index + 1) % 4;
    video_flags &= ~(VIDEO_50Hz | VIDEO_60Hz);
    video_flags |= index << 22;
    dirty = 1;
    update_eeprom_text();
}

static void video_toggle_480p(void)
{
    video_flags ^= VIDEO_MODE_480P;
    dirty = 1;
    update_eeprom_text();
}

static void video_toggle_720p(void)
{
    video_flags ^= VIDEO_MODE_720P;
    dirty = 1;
    update_eeprom_text();
}

static void video_toggle_1080i(void)
{
    video_flags ^= VIDEO_MODE_1080I;
    dirty = 1;
    update_eeprom_text();
}

static void audio_increment_channel(void)
{
    ULONG index = (audio_flags & AUDIO_FLAG_CHANNEL_MASK) >> 0;
    index = (index + 1) % 3;
    audio_flags &= ~AUDIO_FLAG_CHANNEL_MASK;
    audio_flags |= index << 0;
    dirty = 1;
    update_eeprom_text();
}

static void audio_increment_encoding(void)
{
    ULONG index = (audio_flags & AUDIO_FLAG_ENCODING_MASK) >> 16;
    index = (index + 1) % 4;
    audio_flags &= ~AUDIO_FLAG_ENCODING_MASK;
    audio_flags |= index << 16;
    dirty = 1;
    update_eeprom_text();
}

static void mac_address_generate(void)
{
    // One Xbox consoles the first byte of the MAC address is always 0x00,
    // the second and third byte seem to be the same few patterns.
    // The last three bytes are random.
    mac_address[0] = 0x00;
    int r = rand() % 3;
    if (r == 0) {
        mac_address[1] = 0x50;
        mac_address[2] = 0xf2;
    } else if (r == 1) {
        mac_address[1] = 0x0d;
        mac_address[2] = 0x3a;
    } else {
        mac_address[1] = 0x12;
        mac_address[2] = 0x5a;
    }
    mac_address[3] = rand() % 256;
    mac_address[4] = rand() % 256;
    mac_address[5] = rand() % 256;
    dirty = 1;
    update_eeprom_text();
}

static void increment_timezone_bios(void)
{
    // The time zone offset is in minutes, so we can increment it by 30 minutes at a time
    // to account for time zones with 30 minute offsets.
    time_zone_offset -= 30;
    if (time_zone_offset < -720) { // -12 hours
        time_zone_offset = 720; // wrap around to 12 hours
    }
    dirty = 1;
    update_eeprom_text();
}

static MenuItem menu_items[MAX_LINES];
static Menu menu = {
    .item = menu_items,
    .item_count = sizeof(menu_items) / sizeof(MenuItem),
    .selected_index = 0};

static void query_eeprom(void)
{
    ULONG data, type;
    ExQueryNonVolatileSetting(XC_DVD_REGION, &type, &data, sizeof(data), NULL);
    dvd_region_index = data & 0xFF;

    ExQueryNonVolatileSetting(XC_LANGUAGE, &type, &data, sizeof(data), NULL);
    language_index = data & 0xFF;

    ExQueryNonVolatileSetting(XC_VIDEO, &type, &data, sizeof(data), NULL);
    video_flags = data;

    ExQueryNonVolatileSetting(XC_FACTORY_AV_REGION, &type, &data, sizeof(data), NULL);
    av_region = data;

    ExQueryNonVolatileSetting(XC_TIMEZONE_BIAS, &type, &data, sizeof(data), NULL);
    time_zone_offset = data;

    ExQueryNonVolatileSetting(XC_FACTORY_GAME_REGION, &type, &data, sizeof(data), NULL);
    game_region_index = data;

    ExQueryNonVolatileSetting(XC_FACTORY_ETHERNET_ADDR, &type, mac_address, sizeof(mac_address), NULL);
}

static void push_line(int line, void *callback, const char *format, ...)
{
    char *text_buffer = &char_pool[pool_offset];

    va_list args;
    va_start(args, format);
    int written = vsnprintf(text_buffer, MAX_CHARACTERS - pool_offset, format, args);
    va_end(args);

    assert(line < MAX_LINES);
    menu_items[line].label = text_buffer;
    menu_items[line].callback = callback;

    pool_offset += written + 1;
    assert(pool_offset < MAX_CHARACTERS);
}

static void update_eeprom_text(void)
{
    int line = 0;
    pool_offset = 0;
    memset(menu_items, 0, sizeof(menu_items));

    push_line(line++, NULL, "EEPROM Settings");

    if (dirty) {
        push_line(line++, apply_settings, "Apply unsaved changes");
    } else {
        push_line(line++, apply_settings, "Apply");
    }

    const char *game_region;
    switch (game_region_index) {
        case 0x00000001: game_region = "North America"; break;
        case 0x00000002: game_region = "Japan"; break;
        case 0x00000004: game_region = "Europe and Australia"; break;
        case 0x80000000: game_region = "Manufacturing"; break;
        default: game_region = "Unknown";
    }
    push_line(line++, NULL, "Game Region: %s", game_region);

    // clang-format off
    const char *dvd_region;
    switch (dvd_region_index) {
        case 0: dvd_region = "0 None"; break;
        case 1: dvd_region = "1 USA, Canada"; break;
        case 2: dvd_region = "2 Europe, Japan, Middle East"; break;
        case 3: dvd_region = "3 Southeast Asia, South Korea"; break;
        case 4: dvd_region = "4 Latin America, Australia"; break;
        case 5: dvd_region = "5 Eastern Europe, Russia, Africa"; break;
        case 6: dvd_region = "6 China"; break;
        default: dvd_region = "Unknown";
    }
    push_line(line++, increment_dvd_region, "DVD Region: %s", dvd_region);

    const char *language;
    switch (language_index) {
        case 0: language = "0 Not Set"; break;
        case 1: language = "1 English"; break;
        case 2: language = "2 Japanese"; break;
        case 3: language = "3 German"; break;
        case 4: language = "4 French"; break;
        case 5: language = "5 Spanish"; break;
        case 6: language = "6 Italian"; break;
        case 7: language = "7 Korean"; break;
        case 8: language = "8 Chinese"; break;
        case 9: language = "9 Portuguese"; break;
        default: language = "Unknown";
    }
    push_line(line++, increment_language, "Language: %s", language);

    const char *region;
    switch (av_region) {
        case AV_REGION_NTSC: region = "NTSC"; break;
        case AV_REGION_NTSCJ: region = "NTSC Japan"; break;
        case AV_REGION_PAL: region = "PAL"; break;
        case AV_REGION_PALM: region = "PAL Brazil"; break;
        default: region = "Invalid Region";
    }
    push_line(line++, increment_video_region, "Video Region: %s", region);

    push_line(line++, NULL, "Video Flags: 0x%08lx", video_flags);

    push_line(line++, video_increment_aspect_ratio, "  Aspect Ratio: %s",
                   (video_flags & VIDEO_WIDESCREEN) ? "Widescreen" :
                   (video_flags & VIDEO_LETTERBOX) ? "Letterbox" : "Normal");

    push_line(line++, video_increment_refresh_rate, "  Refresh Rate: %s",
                   (video_flags & VIDEO_50Hz && video_flags & VIDEO_60Hz) ? "50Hz / 60Hz" :
                   (video_flags & VIDEO_50Hz) ? "50Hz" :
                   (video_flags & VIDEO_60Hz) ? "60Hz" : "Not set");

    push_line(line++, video_toggle_480p, "  480p: [%c]", (video_flags & VIDEO_MODE_480P) ? 'x' : ' ');
    push_line(line++, video_toggle_720p, "  720p: [%c]", (video_flags & VIDEO_MODE_720P) ? 'x' : ' ');
    push_line(line++, video_toggle_1080i, "  1080i: [%c]", (video_flags & VIDEO_MODE_1080I) ? 'x' : ' ');

    push_line(line++, NULL, "Audio Flags: 0x%08lx", audio_flags);

    push_line(line++, audio_increment_channel, "  Channel Configuration: %s",
                (audio_flags & AUDIO_FLAG_CHANNEL_MONO) ? "Mono" :
                (audio_flags & AUDIO_FLAG_CHANNEL_SURROUND) ? "Surround" : "Stereo");

    push_line(line++, audio_increment_encoding, "  Encoding: %s",
                (audio_flags & AUDIO_FLAG_ENCODING_AC3 && audio_flags & AUDIO_FLAG_ENCODING_DTS) ? "AC3 / DTS" :
                (audio_flags & AUDIO_FLAG_ENCODING_AC3) ? "AC3" :
                (audio_flags & AUDIO_FLAG_ENCODING_DTS) ? "DTS" : "None");
    // clang-format on

    push_line(line++, mac_address_generate, "MAC Address: %02x:%02x:%02x:%02x:%02x:%02x",
              mac_address[0], mac_address[1], mac_address[2],
              mac_address[3], mac_address[4], mac_address[5]);

    push_line(line++, increment_timezone_bios, "Time Zone Offset: %.1f hours", ((float)-time_zone_offset)/60.0f);

    menu.item_count = line;
    printf("pool_offset: %d, menu.item_count: %d\n", pool_offset, menu.item_count);
}

void menu_eeprom_activate(void)
{
    query_eeprom();
    update_eeprom_text();

    menu_push(&menu);
}
