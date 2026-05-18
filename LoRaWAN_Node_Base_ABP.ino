/*
 * || Project:          IoT Energy Meter with C/C++/FreeRTOS, Java/Spring,
 * ||                   TypeScript/Angular and Dart/Flutter.
 * || About:            End-to-end LoRaWAN network for monitoring electrical quantities.
 * || Version:          1.0
 * || Hardware:         ESP32 + RFM95W
 * || LoRaWAN Stack:    MCCI Arduino LMiC 3.3.0
 * || Network Server:   ChirpStack — AU915, sub-band 1 (ch 8–15 + 65)
 * || Activation:       ABP
 * || Author:           Adail dos Santos Silva
 * || E-mail:           adail101@hotmail.com
 * || WhatsApp:         +55 89 9 9412-9256
 * ||
 * || NOTES:
 * ||   • Requires MCCI Arduino LMiC v3.3.0 — v6.x breaks ABP LoRaWAN 1.0.x
 * ||     (MIC mismatch due to internal LoRaWAN 1.1 key derivation).
 * ||     Install: git clone
 * ||              https://github.com/AdailSilva/IBM-LMIC-LoRaWAN-MAC-in-C-library-v3.3.0.git
 * ||              MCCI_LoRaWAN_LMIC_library
 * ||   • ESP32 Arduino core 3.x conflicts with LMiC v3.3.0 hal_init symbol.
 * ||     Fix: sed -i 's/void hal_init(/void lmic_hal_init(/g' src/hal/hal.cpp
 * ||          sed -i 's/hal_init()/lmic_hal_init()/g'
 * ||               src/lmic/lmic.c src/hal/hal.cpp
 * ||
 * || SPDX-License-Identifier: MIT
 */

/********************************************************************
 _____              __ _                       _   _
/  __ \            / _(_)                     | | (_)
| /  \/ ___  _ __ | |_ _  __ _ _   _ _ __ __ _| |_ _  ___  _ __
| |    / _ \| '_ \|  _| |/ _` | | | | '__/ _` | __| |/ _ \| '_ \
| \__/\ (_) | | | | | | | (_| | |_| | | | (_| | |_| | (_) | | | |
 \____/\___/|_| |_|_| |_|\__, |\__,_|_|  \__,_|\__|_|\___/|_| |_|
                          __/ |
                         |___/
********************************************************************/

#include <Preferences.h>
#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>

/* ------------------------------------------------------------------ */
/* Hardware pin mapping — ESP32                                       */
/* ------------------------------------------------------------------ */
#define PIN_SCLK    18
#define PIN_MISO    19
#define PIN_MOSI    23
#define PIN_NSS      5
#define PIN_RESET   14
#define PIN_DIO0    34
#define PIN_DIO1    35  /* tied but not used by LMiC on this board    */
#define PIN_DIO2    39  /* not connected — kept for reference         */

/* ------------------------------------------------------------------ */
/* LoRaWAN credentials — ABP (ChirpStack AU915, sub-band 1)           */
/*                                                                    */
/* Copy these values from:                                            */
/*   ChirpStack → Devices → seu device → Activation (ABP)             */
/*                                                                    */
/* IMPORTANT: cast to (u1_t*) is required when passing PROGMEM arrays */
/* to LMIC_setSession() to avoid const-to-non-const conversion errors.*/
/* ------------------------------------------------------------------ */

/* DevAddr — 4 bytes, big-endian.                                     */
static const u4_t DEVADDR = 0x00aa88ca; /* <-- substitua */

/* NwkSKey — Network Session Key, 16 bytes, big-endian.               */
// 151F7779D7AAEE422A74568380DB7045
static const u1_t PROGMEM NWKSKEY[16] = { 0x15, 0x1F, 0x77, 0x79, 0xD7, 0xAA, 0xEE, 0x42, 0x2A, 0x74, 0x56, 0x83, 0x80, 0xDB, 0x70, 0x45 }; /* <-- substitua */

/* AppSKey — Application Session Key, 16 bytes, big-endian.           */
// FD91D55DCE3AF1023093E289A1D9F3DF
static const u1_t PROGMEM APPSKEY[16] = { 0xFD, 0x91, 0xD5, 0x5D, 0xCE, 0x3A, 0xF1, 0x02, 0x30, 0x93, 0xE2, 0x89, 0xA1, 0xD9, 0xF3, 0xDF }; /* <-- substitua */

/*
 * ABP does not use AppEUI / DevEUI / AppKey.
 * LMiC still requires these callbacks to be defined — no-op stubs.
 * Device EUI: 08FDBE4BC604E8F8
 */
void os_getArtEui(u1_t *buf) { (void)buf; }
void os_getDevEui(u1_t *buf) { (void)buf; }
void os_getDevKey(u1_t *buf) { (void)buf; }

/* ------------------------------------------------------------------ */
/* Frame counter persistence — NVS via Preferences                    */
/*                                                                    */
/* After a reset the LMiC seqnoUp counter restarts from 0.  By        */
/* default ChirpStack rejects any frame whose counter is lower than   */
/* the last accepted value (replay-attack protection), logging:       */
/*   "Frame-counter reset or rollover detected"                       */
/*                                                                    */
/* The fix is to persist seqnoUp in the ESP32 Non-Volatile Storage    */
/* (NVS) and restore it on every boot.  The counter is saved after    */
/* each transmission so that even after an unexpected reset the next  */
/* uplink always carries a strictly increasing value.                 */
/*                                                                    */
/* NVS namespace : "lmic"                                             */
/* NVS key       : "seqnoUp"                                          */
/* ------------------------------------------------------------------ */
static Preferences prefs;

/* Save the current uplink counter to NVS. */
static void saveSeqnoUp(void) {
    prefs.putUInt("seqnoUp", LMIC.seqnoUp);
    Serial.print(F("  seqnoUp saved to NVS: "));
    Serial.println(LMIC.seqnoUp);
}

/* Restore the uplink counter from NVS (returns 0 on first boot). */
static uint32_t loadSeqnoUp(void) {
    return prefs.getUInt("seqnoUp", 0);
}

/* ------------------------------------------------------------------ */
/* Remote reset command — downlink protocol                           */
/*                                                                    */
/* FPort : 101 (0x65)                                                 */
/*                                                                    */
/* Payload structure (8 bytes total):                                 */
/*                                                                    */
/*  Byte:  0     1     2     3     4     5     6     7                */
/*        ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐           */
/*        │ HDR │ HDR │ CMD │ CMD │ CMD │ CMD │TAIL │TAIL │           */
/*        │0xAD │0xA1 │0xDE │0xAD │0xBE │0xEF │0x0D │0x0A │           */
/*        └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘           */
/*                                                                    */
/*  Header  (2 bytes): 0xAD 0xA1  — "AdAil" signature                 */
/*  Command (4 bytes): 0xDE 0xAD 0xBE 0xEF — reset magic word         */
/*  Tail    (2 bytes): 0x0D 0x0A  — CRLF end-of-frame marker          */
/*                                                                    */
/* Any byte mismatch causes the command to be silently ignored.       */
/* The reset is deferred by RESET_DELAY_MS to allow Serial.flush()    */
/* to complete before rebooting.                                      */
/* ------------------------------------------------------------------ */
#define RESET_FPORT          101

#define RESET_CMD_LEN          8

#define RESET_HDR_0         0xAD
#define RESET_HDR_1         0xA1
#define RESET_CMD_0         0xDE
#define RESET_CMD_1         0xAD
#define RESET_CMD_2         0xBE
#define RESET_CMD_3         0xEF
#define RESET_TAIL_0        0x0D
#define RESET_TAIL_1        0x0A

#define RESET_DELAY_MS      2000  /* ms to wait before rebooting      */

/* ------------------------------------------------------------------ */
/* Application configuration                                          */
/* ------------------------------------------------------------------ */

/* Uplink payload — replace with real sensor data in production.      */
static uint8_t payload[] = "AdailSilva-IoT";

/*
 * Uplink interval in seconds.
 * Respect the AU915 1 % duty-cycle limit — do not set below 10 s
 * when using SF7/BW125.  The LMiC enforces the limit regardless,
 * but a reasonable base keeps scheduling predictable.
 */
static const uint32_t TX_INTERVAL_S = 10;

/* LMiC job handle for the periodic uplink. */
static osjob_t txjob;

/* ------------------------------------------------------------------ */
/* Pin mapping struct                                                 */
/* ------------------------------------------------------------------ */
const lmic_pinmap lmic_pins = {
    .nss   = PIN_NSS,
    .rxtx  = LMIC_UNUSED_PIN,
    .rst   = PIN_RESET,
    /* DIO2 is not required for LoRa mode on the RFM95W. */
    .dio   = { PIN_DIO0, PIN_DIO1, LMIC_UNUSED_PIN },
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */
void do_send(osjob_t *j);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Print a byte as two hex digits with a leading zero if needed. */
static void printHex2(uint8_t v) {
    if (v < 0x10) Serial.print('0');
    Serial.print(v, HEX);
}

/* Print a key array as XX-XX-XX-… */
static void printKey(const uint8_t *key, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (i) Serial.print('-');
        printHex2(key[i]);
    }
    Serial.println();
}

/* Print the ABP session credentials on startup for quick verification. */
static void printSessionKeys(void) {
    uint8_t nwkKey[16];
    uint8_t artKey[16];
    memcpy_P(nwkKey, NWKSKEY, 16);
    memcpy_P(artKey, APPSKEY, 16);

    Serial.print(F("  DevAddr : 0x")); Serial.println(DEVADDR, HEX);
    Serial.print(F("  NwkSKey : ")); printKey(nwkKey, sizeof(nwkKey));
    Serial.print(F("  AppSKey : ")); printKey(artKey, sizeof(artKey));
    Serial.print(F("  seqnoUp : ")); Serial.println(LMIC.seqnoUp);
}

/* ------------------------------------------------------------------ */
/* Remote reset handler                                               */
/*                                                                    */
/* Validates the 8-byte reset command received on FPort 101.          */
/* Returns true if the payload matches the expected pattern exactly.  */
/* ------------------------------------------------------------------ */
static bool isResetCommand(const uint8_t *data, uint8_t len) {
    if (len != RESET_CMD_LEN) return false;

    return (data[0] == RESET_HDR_0 &&
            data[1] == RESET_HDR_1 &&
            data[2] == RESET_CMD_0 &&
            data[3] == RESET_CMD_1 &&
            data[4] == RESET_CMD_2 &&
            data[5] == RESET_CMD_3 &&
            data[6] == RESET_TAIL_0 &&
            data[7] == RESET_TAIL_1);
}

static void handleDownlink(void) {
    if (LMIC.dataLen == 0) return;

    uint8_t  fport = LMIC.frame[LMIC.dataBeg - 1];
    uint8_t  len   = LMIC.dataLen;
    uint8_t *data  = &LMIC.frame[LMIC.dataBeg];

    /* Print every downlink regardless of port. */
    Serial.print(F("  Downlink received — "));
    Serial.print(len);
    Serial.print(F(" byte(s) on FPort "));
    Serial.print(fport);
    Serial.print(F(": "));
    for (uint8_t i = 0; i < len; ++i) {
        printHex2(data[i]);
        Serial.print(' ');
    }
    Serial.println();

    /* ---------------------------------------------------------- */
    /* Remote reset command — FPort 101                           */
    /* ---------------------------------------------------------- */
    if (fport == RESET_FPORT) {
        Serial.println(F("  [RESET PORT] Command received on FPort 101 — validating..."));

        if (isResetCommand(data, len)) {
            Serial.println(F("  [RESET] Valid reset command — rebooting in 2 s..."));
            Serial.flush();
            delay(RESET_DELAY_MS);
            esp_restart();
        } else {
            Serial.println(F("  [RESET] Invalid payload — command ignored."));
            Serial.print(F("  Expected: "));
            Serial.printf("%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                RESET_HDR_0, RESET_HDR_1,
                RESET_CMD_0, RESET_CMD_1, RESET_CMD_2, RESET_CMD_3,
                RESET_TAIL_0, RESET_TAIL_1);
            Serial.print(F("  Received: "));
            for (uint8_t i = 0; i < len; ++i) {
                printHex2(data[i]);
                if (i < len - 1) Serial.print(' ');
            }
            Serial.println();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Session diagnostics — called on EV_TXSTART                         */
/* ------------------------------------------------------------------ */
static void print_info_session(void) {
    Serial.print(F("LMiC NwkSKey: "));
    for (int i = 0; i < 16; i++) {
        if (LMIC.nwkKey[i] < 0x10) Serial.print('0');
        Serial.print(LMIC.nwkKey[i], HEX);
    }
    Serial.println();

    Serial.print(F("LMiC AppSKey: "));
    for (int i = 0; i < 16; i++) {
        if (LMIC.artKey[i] < 0x10) Serial.print('0');
        Serial.print(LMIC.artKey[i], HEX);
    }
    Serial.println();

    Serial.print(F("LMiC DevAddr: 0x"));
    Serial.println(LMIC.devaddr, HEX);
    Serial.println();

    Serial.print(F("Frame raw: "));
    for (int i = 0; i < LMIC.dataLen + LMIC.dataBeg; i++) {
        if (LMIC.frame[i] < 0x10) Serial.print('0');
        Serial.print(LMIC.frame[i], HEX);
        Serial.print(' ');
    }
    Serial.println();

    Serial.print(F("FCnt/seqnoUp: ")); Serial.println(LMIC.seqnoUp);
    Serial.print(F("FCnt/seqnoDn: ")); Serial.println(LMIC.seqnoDn);

    /* Print MIC — last 4 bytes of the frame. */
    uint16_t frameLen = LMIC.dataBeg + LMIC.dataLen;
    if (frameLen >= 4) {
        Serial.print(F("  MIC: "));
        for (uint8_t i = frameLen - 4; i < frameLen; i++) {
            if (LMIC.frame[i] < 0x10) Serial.print('0');
            Serial.print(LMIC.frame[i], HEX);
            if (i < frameLen - 1) Serial.print('-');
        }
        Serial.println();
    }
}

/* ------------------------------------------------------------------ */
/* LMiC event handler                                                 */
/* ------------------------------------------------------------------ */

/*****************************
 _____           _
/  __ \         | |
| /  \/ ___   __| | ___
| |    / _ \ / _` |/ _ \
| \__/\ (_) | (_| |  __/
 \____/\___/ \__,_|\___|
*****************************/

void onEvent(ev_t ev) {
    //Serial.print(os_getTime());
    //Serial.print(F(": "));

    switch (ev) {

        /*
         * EV_JOINING / EV_JOINED / EV_JOIN_TXCOMPLETE are never fired
         * in ABP mode — kept here only for completeness / future reuse
         * if the activation mode is changed to OTAA.
         */
        case EV_JOINING:
            Serial.println(F("EV_JOINING"));
            break;

        case EV_JOIN_TXCOMPLETE:
            Serial.println(F("EV_JOIN_TXCOMPLETE: no JoinAccept"));
            break;

        case EV_JOIN_FAILED:
            Serial.println(F("EV_JOIN_FAILED"));
            break;

        case EV_REJOIN_FAILED:
            Serial.println(F("EV_REJOIN_FAILED"));
            break;

        case EV_JOINED:
            /*
             * Not fired in ABP.  In OTAA, LMIC_setLinkCheckMode(0)
             * must be called here a second time because the JOIN
             * procedure re-enables it automatically.
             * In ABP a single call in setup() is sufficient.
             */
            Serial.println(F("EV_JOINED"));
            break;

        case EV_TXSTART:
            Serial.println();
            Serial.println(F("EV_TXSTART"));
            print_info_session();
            break;

        case EV_RXSTART:
            /*
             * Do NOT print here — any Serial output during RX setup
             * consumes time and can mis-align the receive window.
             */
            break;

        case EV_TXCOMPLETE:
            Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));

            if (LMIC.txrxFlags & TXRX_ACK) {
                Serial.println(F("  ACK received."));
            }

            /*
             * Handle downlink — including the remote reset command.
             * Called here because LMIC.dataLen and LMIC.frame are
             * only valid inside EV_TXCOMPLETE.
             */
            handleDownlink();

            /*
             * Persist the uplink frame counter to NVS immediately
             * after each successful transmission.
             *
             * The LMiC increments seqnoUp before firing EV_TXCOMPLETE,
             * so the value stored here is already the next counter to
             * be used — guaranteeing a strictly increasing sequence
             * even after an unexpected reset between transmissions.
             *
             * NOTE: saveSeqnoUp() is called AFTER handleDownlink() so
             * that if a reset command is received, the counter is still
             * saved before esp_restart() is called inside handleDownlink().
             */
            saveSeqnoUp();

            /* Schedule the next uplink. */
            os_setTimedCallback(&txjob,
                                os_getTime() + sec2osticks(TX_INTERVAL_S),
                                do_send);
            break;

        case EV_TXCANCELED:
            Serial.println(F("EV_TXCANCELED"));
            break;

        case EV_LINK_DEAD:
            /*
             * Fired when the network has not acknowledged several
             * consecutive uplinks.  In ABP this typically means the
             * frame counter diverged after a reset.
             * With NVS persistence this should no longer occur.
             */
            Serial.println(F("EV_LINK_DEAD"));
            break;

        case EV_LINK_ALIVE:
            Serial.println(F("EV_LINK_ALIVE"));
            break;

        case EV_RESET:
            Serial.println(F("EV_RESET"));
            break;

        case EV_RXCOMPLETE:
            /* Class B ping-slot reception (not used here). */
            Serial.println(F("EV_RXCOMPLETE"));
            break;

        case EV_SCAN_TIMEOUT:   Serial.println(F("EV_SCAN_TIMEOUT"));   break;
        case EV_BEACON_FOUND:   Serial.println(F("EV_BEACON_FOUND"));   break;
        case EV_BEACON_MISSED:  Serial.println(F("EV_BEACON_MISSED"));  break;
        case EV_BEACON_TRACKED: Serial.println(F("EV_BEACON_TRACKED")); break;
        case EV_LOST_TSYNC:     Serial.println(F("EV_LOST_TSYNC"));     break;

        default:
            Serial.print(F("Unknown event: "));
            Serial.println((unsigned)ev);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Uplink function                                                    */
/* ------------------------------------------------------------------ */

void do_send(osjob_t *j) {
    if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println(F("OP_TXRXPEND — previous job still running, skipping."));
        return;
    }

    // Força limpeza de qualquer frame em cache.
    //LMIC_clrTxData();

    // Reseta o frame counter explicitamente.
    //LMIC.seqnoUp = 0;
    //LMIC.seqnoDn = 0;

    // Força payload diferente a cada envio.
    //static uint8_t counter = 0;
    //payload[0] = counter++;  // Altera o primeiro byte.

    /* TODO: read real sensor data into payload[] before calling setTxData2. */
    LMIC_setTxData2(1, payload, sizeof(payload) - 1, 0 /* unconfirmed */);
    Serial.println(F("Packet queued."));
}

/* ------------------------------------------------------------------ */
/* Setup                                                              */
/* ------------------------------------------------------------------ */

/*****************************
 _____      _
/  ___|    | |
\ `--.  ___| |_ _   _ _ __
 `--. \/ _ \ __| | | | '_ \
/\__/ /  __/ |_| |_| | |_) |
\____/ \___|\__|\__,_| .__/
                     | |
                     |_|
*****************************/

void setup() {
    Serial.begin(9600);
    delay(100);

    /* Explicit SPI initialisation required on ESP32. */
    SPI.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, PIN_NSS);

    Serial.println(F("Starting — ChirpStack AU915 ABP"));

#ifdef VCC_ENABLE
    /* Pinoccio Scout power rail enable. */
    pinMode(VCC_ENABLE, OUTPUT);
    digitalWrite(VCC_ENABLE, HIGH);
    delay(1000);
#endif

    /*
     * Open the NVS namespace before os_init() so that loadSeqnoUp()
     * is ready to be called after LMIC_setSession().
     * false = read/write mode.
     */
    prefs.begin("lmic", false);

    os_init();
    LMIC_reset();
    //LMIC.seqnoUp = 0;  // Garante contador zerado.
    //LMIC.seqnoDn = 0;

    /* -------------------------------------------------------------- */
    /* Provision the ABP session — replaces the OTAA JOIN procedure.  */
    /*                                                                */
    /* LMIC_setSession() installs the device address and session keys */
    /* immediately, without any over-the-air exchange.                */
    /*                                                                */
    /* netID 0x000000 = ChirpStack default.                           */
    /* Use 0x000013 for The Things Network (TTN).                     */
    /* -------------------------------------------------------------- */
    uint8_t nwkKey[16];
    uint8_t artKey[16];
    //memcpy(nwkKey, NWKSKEY, 16);
    //memcpy(artKey, APPSKEY, 16);
    memcpy_P(nwkKey, NWKSKEY, 16);
    memcpy_P(artKey, APPSKEY, 16);

    //LMIC_setSession(0x13 /* netID AU915 */, DEVADDR, nwkKey, artKey);   /* The Things Network - TTN */
    LMIC_setSession(0x000000 /* netID AU915 */, DEVADDR, nwkKey, artKey); /* ChirpStack */

    /*
     * Restore the uplink frame counter from NVS.
     *
     * CRITICAL: this call MUST come AFTER LMIC_setSession() because
     * setSession() resets seqnoUp to 0 internally.  Restoring here
     * overwrites that zero with the persisted value.
     *
     * On the very first boot (no NVS key yet) loadSeqnoUp() returns 0
     * which is correct — the first uplink will carry FCnt = 0.
     */
    LMIC.seqnoUp = loadSeqnoUp();

    Serial.println(F("ABP session provisioned:"));
    printSessionKeys();

    /* -------------------------------------------------------------- */
    /* Channel plan — ChirpStack AU915, sub-band 1                    */
    /* Uplink:   ch  8–15  (916.8–918.2 MHz, 200 kHz, BW125)          */
    /* Uplink:   ch 65     (917.5 MHz, BW500) — ADR / join            */
    /* Downlink: ch  0– 7  (923.3–924.7 MHz, BW500) — managed by NS   */
    /* -------------------------------------------------------------- */
    for (u1_t b  =  0; b  <  8; ++b)   LMIC_disableSubBand(b);
    for (u1_t ch =  0; ch < 72; ++ch)  LMIC_disableChannel(ch);
    for (u1_t ch =  8; ch <= 15; ++ch) LMIC_enableChannel(ch);
    LMIC_enableChannel(65);

    /* Adaptive Data Rate off — fixed SF7/BW125/14 dBm. */
    LMIC_setAdrMode(0);

    /*
     * Link-check mode.
     * In OTAA this must be called twice (here and inside EV_JOINED)
     * because the JOIN procedure re-enables it automatically.
     * In ABP EV_JOINED never fires, so one call here is sufficient.
     */
    LMIC_setLinkCheckMode(0);

    /* DR5 = SF7/BW125 for AU915 — highest data rate, lowest range.   */
    LMIC_setDrTxpow(DR_SF7, 14);

    /*
     * RX2 window — AU915/ChirpStack default:
     *   923.3 MHz, SF12/BW500 → DR_SF12CR (DR8).
     * In OTAA the JOIN Accept CFList sets this automatically.
     * In ABP there is no JOIN Accept, so it must be set manually.
     */
    LMIC.dn2Dr = DR_SF12CR;

    /*
     * Clock-error compensation.
     * The ESP32 XTAL is accurate enough that 1 % is sufficient to
     * widen the RX preamble detection window without introducing a
     * significant delay before the window opens.
     */
    LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100);

    /* Kick off the first uplink. */
    do_send(&txjob);
}

/* ------------------------------------------------------------------ */
/* Main loop — hand control to the LMiC cooperative scheduler         */
/* ------------------------------------------------------------------ */

/*****************************
 _
| |
| |     ___   ___  _ __
| |    / _ \ / _ \| '_ \
| |___| (_) | (_) | |_) |
\_____/\___/ \___/| .__/
                  | |
                  |_|
*****************************/

void loop() {
    os_runloop_once();
}
