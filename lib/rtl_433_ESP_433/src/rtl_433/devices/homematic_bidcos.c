/** @file
    HomeMatic BidCoS / eQ-3 header-only decoder for device identification.

    Copyright (C) 2026 Linkra DUO Project

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/**
HomeMatic BidCoS header-only decoder.

This decoder does NOT decrypt payloads. It only extracts the unencrypted
header fields to identify HomeMatic / eQ-3 devices on the RF network.

Protocol: GFSK, 10 kbps, 868.3 MHz (CC1101-based)
Deviation: ±19.042 kHz

Packet structure (after preamble 0xAA...):

    SSSS LL CC FF TT AAAAAA BBBBBB [encrypted payload] [CRC16]

- S: 4 byte sync word (0xE9CA repeated, or 0xE9CAE9CA)
- L: 1 byte length (of remaining bytes after length, excluding CRC)
- C: 1 byte message counter (0x00-0xFF, increments per message)
- F: 1 byte control flags
    - Bit 7: WAKEUP
    - Bit 6: WAKEMEUP
    - Bit 5: CFG (config mode)
    - Bit 4: (reserved)
    - Bit 3: BURST
    - Bit 2: BIDI (bidirectional)
    - Bit 1: RPTED (repeated by router)
    - Bit 0: RPTEN (repeat enabled)
- T: 1 byte message type
- A: 3 byte sender address
- B: 3 byte destination address (0x000000 = broadcast)
- Payload: variable, AES-encrypted in newer firmware
- CRC16: 2 byte CRC (CCITT)

Known message types:
    0x00 = Device Info
    0x01 = Configuration
    0x02 = ACK / Response
    0x03 = AES Key Exchange
    0x04 = AES Challenge Response
    0x10 = Info Actuator Status
    0x11 = Set Actuator
    0x12 = Info Paired (Thermostat)
    0x3E = Pair Serial
    0x3F = Pair Config
    0x40 = Set Value
    0x41 = TH Sensor Data
    0x53 = Water Sensor
    0x58 = Climate Event
    0x5A = Power Event
    0x5E = Power Meter
    0x70 = Weather Event
*/

#include "decoder.h"

static char const *homematic_type_str(int type)
{
    switch (type) {
    case 0x00: return "Device Info";
    case 0x01: return "Configuration";
    case 0x02: return "ACK/Response";
    case 0x03: return "AES Key Exchange";
    case 0x04: return "AES Challenge";
    case 0x10: return "Actuator Status";
    case 0x11: return "Set Actuator";
    case 0x12: return "Thermostat Paired";
    case 0x3E: return "Pair Serial";
    case 0x3F: return "Pair Config";
    case 0x40: return "Set Value";
    case 0x41: return "TH Sensor Data";
    case 0x53: return "Water Sensor";
    case 0x58: return "Climate Event";
    case 0x5A: return "Power Event";
    case 0x5E: return "Power Meter";
    case 0x70: return "Weather Event";
    default:   return "Unknown";
    }
}

static int homematic_bidcos_decode(r_device *decoder, bitbuffer_t *bitbuffer)
{
    /*
     * Search for the BidCoS sync word 0xE9CA in the bitbuffer.
     * The full sync is often 0xE9CAE9CA but we match on 2 bytes
     * to catch partial sync patterns too.
     */
    uint8_t const sync[] = {0xE9, 0xCA};

    int events = 0;

    for (int row = 0; row < bitbuffer->num_rows; ++row) {
        /* Need at least sync(16) + header(80) = 96 bits minimum */
        if (bitbuffer->bits_per_row[row] < 96)
            continue;

        unsigned bit_offset = 0;

        while ((bit_offset = bitbuffer_search(bitbuffer, row, bit_offset,
                sync, 16))
                + 96 <= bitbuffer->bits_per_row[row]) {

            bit_offset += 16; /* skip past sync word */

            /* Check if there's a second sync word (0xE9CA repeated) */
            uint8_t maybe_sync[2];
            bitbuffer_extract_bytes(bitbuffer, row, bit_offset, maybe_sync, 16);
            if (maybe_sync[0] == 0xE9 && maybe_sync[1] == 0xCA) {
                bit_offset += 16; /* skip second sync copy */
            }

            /* Need at least 10 bytes (80 bits) for the header */
            if (bit_offset + 80 > bitbuffer->bits_per_row[row])
                break;

            uint8_t b[12];
            bitbuffer_extract_bytes(bitbuffer, row, bit_offset, b, 80);

            int len     = b[0];
            int counter = b[1];
            int flags   = b[2];
            int type    = b[3];
            uint32_t sender   = ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 8) | b[6];
            uint32_t receiver = ((uint32_t)b[7] << 16) | ((uint32_t)b[8] << 8) | b[9];

            /*
             * Sanity checks to avoid false positives:
             * - Length must be reasonable (9..64 bytes)
             * - Sender address must not be 0x000000 or 0xFFFFFF
             * - Counter and flags are any value (0x00-0xFF valid)
             */
            if (len < 9 || len > 64) {
                bit_offset += 8; /* try next position */
                continue;
            }
            if (sender == 0x000000 || sender == 0xFFFFFF) {
                bit_offset += 8;
                continue;
            }

            /* Additional plausibility: BidCoS flags have known patterns */
            /* Bit 4 is typically 0 in most messages */
            if (flags & 0x10) {
                /* Unusual but not impossible — allow but note */
            }

            char sender_str[8];
            char receiver_str[8];
            snprintf(sender_str, sizeof(sender_str), "%06X", sender);
            snprintf(receiver_str, sizeof(receiver_str), "%06X", receiver);

            char const *type_str = homematic_type_str(type);

            /* Decode flag bits for display */
            int flag_wakeup  = (flags >> 7) & 1;
            int flag_burst   = (flags >> 3) & 1;
            int flag_bidi    = (flags >> 2) & 1;
            int flag_rpted   = (flags >> 1) & 1;
            int flag_cfg     = (flags >> 5) & 1;

            char flags_str[32];
            snprintf(flags_str, sizeof(flags_str), "%s%s%s%s%s",
                     flag_wakeup ? "WAKE " : "",
                     flag_cfg    ? "CFG " : "",
                     flag_burst  ? "BURST " : "",
                     flag_bidi   ? "BIDI " : "",
                     flag_rpted  ? "RPTED " : "");
            /* Trim trailing space */
            int flen = strlen(flags_str);
            if (flen > 0 && flags_str[flen - 1] == ' ')
                flags_str[flen - 1] = '\0';
            if (flen == 0)
                snprintf(flags_str, sizeof(flags_str), "NONE");

            /* clang-format off */
            data_t *data = data_make(
                    "model",        "",                 DATA_STRING, "HomeMatic-BidCoS",
                    "id",           "Sender ID",        DATA_INT,    (int)sender,
                    "sender",       "Sender Addr",      DATA_STRING, sender_str,
                    "receiver",     "Receiver Addr",    DATA_STRING, receiver_str,
                    "type_str",     "Message Type",     DATA_STRING, type_str,
                    "type_id",      "Type ID",          DATA_INT,    type,
                    "counter",      "Counter",          DATA_INT,    counter,
                    "flags",        "Flags",            DATA_STRING, flags_str,
                    "flags_raw",    "Flags Raw",        DATA_INT,    flags,
                    "length",       "Length",           DATA_INT,    len,
                    NULL);
            /* clang-format on */

            decoder_output_data(decoder, data);
            events++;
            return 1; /* one event per call is sufficient */
        }
    }

    return events > 0 ? events : DECODE_ABORT_EARLY;
}

static char const *const output_fields[] = {
        "model",
        "id",
        "sender",
        "receiver",
        "type_str",
        "type_id",
        "counter",
        "flags",
        "flags_raw",
        "length",
        NULL,
};

/*
 * HomeMatic BidCoS uses GFSK at 10 kbps on 868.3 MHz.
 * In async CC1101 mode, this appears as FSK_PULSE_PCM with 100 µs bit width.
 *
 * Note: We also try a wider tolerance since the CC1101 async demodulation
 * may produce slightly variable pulse widths.
 */
r_device const homematic_bidcos = {
        .name        = "HomeMatic BidCoS / eQ-3 (header only, 868.3 MHz, 10 kbps)",
        .modulation  = FSK_PULSE_PCM,
        .short_width = 100,  /* 10 kbps = 100 µs per bit */
        .long_width  = 100,  /* NRZ: bit width = pulse width */
        .reset_limit = 1500, /* gap indicating end of message */
        .decode_fn   = &homematic_bidcos_decode,
        .disabled    = 0,
        .fields      = output_fields,
};
