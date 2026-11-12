// hardpush_esp32s3.ino
// Portiert vom Teensy 4.1 auf ESP32-S3
// USB-Host: EspUsbHost (eine Verbindung zum Push)
// Netzwerk: AppleMIDI (lathoub)
//
// Abhängigkeiten:
//   - EspUsbHost (tanakamasayuki)
//   - AppleMIDI (lathoub)

#include <WiFi.h>
#include <WiFiUdp.h>
#define APPLEMIDI_INITIATOR
#include <AppleMIDI.h>
#include <AppleMIDI.h>
#include <EspUsbHost.h>

#include "secrets.h"

// ===========================================================================
//  KONFIGURATION
// ===========================================================================
#define PUSH_DEBUG 1

const uint8_t PUSH_CABLE = 1;   // Cable 1 for sysex commands to push1. LED are cable 0
const char* WIFI_SSID = SECRET_SSID; // in file secret.h: #define SECRET_SSID "fill in ROUTER_SSID"
const char* WIFI_PASS = SECRET_PASS; // in file secret.h: #define SECRET_PASS "PASSWORD"
IPAddress REMOTE_IP(192, 168, 2, 37);   // Laptop mit rtpmidid
const uint16_t REMOTE_PORT = 5004;

// ===========================================================================
//  INSTANZEN
// ===========================================================================
APPLEMIDI_CREATE_DEFAULTSESSION_INSTANCE();
EspUsbHost usbHost;

// ===========================================================================
//  USB-EVENT-QUEUE
// ===========================================================================
// Die USB-Callbacks füllen nur die Queue; die Verarbeitung (inkl. delay()
// für Display-Updates) läuft in loop(), damit der USB-Hintergrund-Task
// nicht blockiert wird.
enum UsbEventType { EV_NOTE_ON, EV_NOTE_OFF, EV_CC, EV_PITCH, EV_AT };
struct UsbEvent {
    uint8_t type;
    uint8_t ch;
    uint8_t d1;
    uint8_t d2;
};
QueueHandle_t usbQueue = nullptr;

// ===========================================================================
//  SCALES / KEYS / FARBEN / LAYOUTS (unverändert vom Teensy)
// ===========================================================================
const int numberOfScales = 26;
const byte scales[][numberOfScales] = {
    {0,1,2,3,4,5,6,7,8,9,10,11}, {0,2,4,5,7,9,11}, {0,2,3,5,7,8,10},
    {0,2,3,5,7,9,10}, {0,2,4,5,7,9,10}, {0,2,4,6,7,9,11},
    {0,1,3,5,7,8,10}, {0,1,3,4,7,8,10}, {0,1,3,4,6,7,9,10},
    {0,2,3,5,6,8,9,11}, {0,2,4,6,8,10}, {0,3,5,6,7,10},
    {0,3,5,7,10}, {0,2,4,7,9}, {0,2,3,5,7,8,11},
    {0,2,3,5,7,9,11}, {0,1,3,4,6,8,10}, {0,1,4,5,7,8,11},
    {0,2,3,6,7,8,11}, {0,1,4,5,7,8,10}, {0,2,3,7,8},
    {0,1,5,7,10}, {0,1,5,6,10}, {0,2,3,7,9},
    {0,1,3,4,7,8}, {0,1,3,4,5,6,8,10}
};
const char* nameOfScales[numberOfScales] = {
    "Chromatic","Major","Minor","Dorian","Mixolydian","Lydian","Phrygian",
    "Locrian","Diminished","Whole-half","Whole Tone","Minor Blues",
    "Minor Pentatonic","Major Pentatonic","Harmonic Minor","Melodic Minor",
    "Super Locrian","Bhairav","Hungarian Minor","Minor Gypsy","Hirojoshi",
    "In-Sen","Iwato","Kumoi","Pelog","Spanish"
};
const char* nameOfKeys[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
const byte scaleSizes[numberOfScales] = {12,7,7,7,7,7,7,7,8,8,6,6,5,5,7,7,7,7,7,7,5,5,5,5,6,8};

const byte numberOfColorSchemes = 8;
byte colors[numberOfColorSchemes][3] = {
    {51,80,48},{31,9,16},{55,38,30},{43,44,77},
    {47,69,36},{7,42,5},{15,18,21},{23,50,9}
};
const char* nameOfColors[numberOfColorSchemes] = {
    "Viol","Turtle","Fungus","Avatar","Passific","Hero","Slime","TMNT"
};

const byte numberOfLayouts = 6;
byte layouts[numberOfLayouts][2] = { {1,3},{3,1},{1,2},{2,1},{1,8},{8,1} };
const char* nameOfLayouts[numberOfLayouts] = {
    "4up","4across","3up","3across","Seq-horizontal","Seq-vertical"
};

// ===========================================================================
//  GLOBALE VARIABLEN
// ===========================================================================
byte scalePresetIndex;
byte instrumentPageIndex;
byte displayBugDelay = 26;
bool pressedNotes[128];

const byte numberOfPages = 4;
byte pageChannels[numberOfPages];
byte pageColors[numberOfPages];
byte pageOctaves[numberOfPages];
byte pageLayouts[numberOfPages];

const byte numberOfScalePresets = 4;
byte scalePresetTonics[numberOfPages];
byte scalePresetScales[numberOfPages];

volatile bool pushNeedsInit = false; // if device detected, initialize in loop()

// ===========================================================================
//  SYSEX-DATEN (unverändert vom Teensy)
// ===========================================================================
byte sysexUserModeMessage[] = {240,71,127,21,98,0,1,0,247};
byte sysexPolyPressure[]    = {240,71,127,21,92,0,1,0,247};
byte sysexChannelPressure[] = {240,71,127,21,92,0,1,1,247};

byte displayBuffer[4][68];
byte sysexDisplayBuffer[77] = {
    240,71,127,21,24,0,69,0,
    32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
    32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
    32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
    32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
    247
};
byte padSenseSyssexLow[] = {
    240,71,127,21,93,0,32,0,0,6,13,0,0,7,8,16,0,0,1,4,12,0,8,
    0,0,0,1,13,4,12,0,32,0,0,0,14,10,6,0,247
};

// ===========================================================================
//  MATH & NOTE (unverändert vom Teensy)
// ===========================================================================
int loopingNumbers(int number, int lastNumber) {
    if (number > 0 && number <= lastNumber) return number;
    if (number < 1) return lastNumber;
    return 1;
}
int indexInLoopingPattern(int anyIndex, int patternSize) {
    int maxPatternIndex = patternSize - 1;
    if (anyIndex >= 0) return anyIndex % patternSize;
    anyIndex = abs(anyIndex) - 1;
    return maxPatternIndex - (anyIndex % patternSize);
}
int loopingPatternOffset(int index, int patternSize) {
    if (index >= 0) return index / patternSize;
    return ((index + 1) / patternSize) - 1;
}
int getRelativeScaleNote(int index) {
    return loopingPatternOffset(index, scaleSizes[scalePresetScales[scalePresetIndex]]) * 12
         + scales[scalePresetScales[scalePresetIndex]]
                [indexInLoopingPattern(index, scaleSizes[scalePresetScales[scalePresetIndex]])];
}
byte pushButtonIndexToScaleIndex(byte index) {
    return (index / 8) * getYlayout(instrumentPageIndex)
         + (index % 8) * getXlayout(instrumentPageIndex);
}
byte pushButtonIndexToAbsoluteNote(byte index) {
    return scalePresetTonics[scalePresetIndex]
         + pageOctaves[instrumentPageIndex] * 12
         + getRelativeScaleNote(pushButtonIndexToScaleIndex(index));
}
bool legalRange(byte value) { return value < 128; }

void killAllPressedNotesDIN(byte channel) {
    for (int i = 0; i < 128; i++)
        if (pressedNotes[i]) MIDI.sendNoteOff(i, 0, channel);
}

// ===========================================================================
//  SENDEN AN DEN PUSH – rohe 4-Byte-USB-MIDI-Pakete, Cable 1
// ===========================================================================
void pushSendPacket(uint8_t cin, uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t packet[4] = { (uint8_t)((0 << 4) | cin), status, data1, data2 };
    if (PUSH_DEBUG) {
        Serial.printf("[PAD-LED]  packet: %02X %02X %02X %02X\n",
                      packet[0], packet[1], packet[2], packet[3]);
    }
    usbHost.midiSend(packet, 4);
}

void pushSendNoteOn(byte note, byte velocity) {
    pushSendPacket(0x9, 0x90, note, velocity);
    // usbHost.midiSendNoteOn(note, velocity, 1, 0);   // Kanal 1, Cable 1
}
void pushSendNoteOff(byte note) {
    pushSendPacket(0x8, 0x80, note, 0);
    // usbHost.midiSendNoteOff(note, 0, 1, PUSH_CABLE);
}
void pushSendControlChange(byte number, byte value) {
    pushSendPacket(0xB, 0xB0, number, value);
    // usbHost.midiSendControlChange(number, value, 1, PUSH_CABLE);
}

// SysEx in 4-Byte-USB-MIDI-Pakete verpacken
void pushSendSysExRaw(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t packet[4] = { (uint8_t)(PUSH_CABLE << 4), 0, 0, 0 };
        size_t chunk = (len - i >= 3) ? 3 : (len - i);
        for (size_t j = 0; j < chunk; j++) packet[1 + j] = data[i + j];

        uint8_t cin;
        if (i == 0) {
            cin = 0x4;   // SysEx Start / Continue
        } else if (i + chunk >= len) {
            cin = (chunk == 1) ? 0x5 : (chunk == 2) ? 0x6 : 0x7;   // SysEx Ende
        } else {
            cin = 0x4;   // SysEx Continue
        }
        packet[0] |= cin;

        usbHost.midiSend(packet, 4);
        i += chunk;
        delayMicroseconds(500);   // Push 1 ist bei SysEx timing-empfindlich
    }
}

void killAllNotesUSB() { for (int i = 0; i < 128; i++) pushSendNoteOff(i); }

// ===========================================================================
//  DISPLAY (unverändert vom Teensy, aber mit pushSend*)
// ===========================================================================
void displayButton(byte index, bool triggered) {
    byte note = pushButtonIndexToAbsoluteNote(index);
    if (!legalRange(note)) { pushSendNoteOff(index + 36); return; }
    byte buttonState;
    if (triggered) buttonState = 2;
    else if (pushButtonIndexToScaleIndex(index)
             % scaleSizes[scalePresetScales[scalePresetIndex]] == 0) buttonState = 1;
    else buttonState = 0;
    pushSendNoteOn(index + 36, colors[pageColors[instrumentPageIndex]][buttonState]);
}

void initPageSelectButtons() {
    delay(displayBugDelay);
    for (int i = 0; i < numberOfPages; i++)
        pushSendControlChange(i + 106, colors[pageColors[i]][0]);
}
void initScalePresetSelectButtons() {
    delay(displayBugDelay);
    for (int i = 0; i < numberOfScalePresets; i++)
        pushSendControlChange(i + 102, i == scalePresetIndex ? 2 : 1);
}
void initPushSettings() {
    delay(30);
    pushSendSysExRaw(sysexUserModeMessage, 9);
    delay(30);
    pushSendSysExRaw(padSenseSyssexLow, 40);
    delay(30);
}
void initGrid() { for (int i = 0; i < 64; i++) displayButton(i, false); }
void initOtherButtons() {
    for (int i = 54; i < 56; i++) pushSendControlChange(i, 4);
    initPageSelectButtons();
    initScalePresetSelectButtons();
    for (int i = 20; i < 22; i++) pushSendControlChange(i, 7);
    for (int i = 24; i < 28; i++) pushSendControlChange(i, 7);
}
void addToDisplay(String text, byte row, byte section, byte position) {
    byte buf[text.length() + 1];
    text.getBytes(buf, text.length() + 1);
    for (uint i = 0; i < text.length(); i++)
        displayBuffer[row][i + 17 * section + position] = buf[i];
}
void addDisplayTop() {
    removeDisplayRow(0);
    addToDisplay(String("Scale preset: ") + (scalePresetIndex + 1), 0, 0, 0);
    addToDisplay(String("Track preset: ") + (instrumentPageIndex + 1), 0, 2, 0);
}
void addToDisplay8Section(String text, byte row, byte section, bool center) {
    byte index = section - 1;
    for (uint i = 0; i < 8; i++) displayBuffer[row][i + 9 * index - index / 2] = 32;
    byte buf[text.length() + 1];
    text.getBytes(buf, text.length() + 1);
    byte centerAlignText = (text.length() < 7 && center) ? (4 - (text.length() + 1) / 2) : 0;
    for (uint i = 0; i < 8 && i < text.length(); i++)
        displayBuffer[row][i + 9 * index - index / 2 + centerAlignText] = buf[i];
}
void addButtonLabelAndData(byte button, String label, String value) {
    addToDisplay8Section(label, 2, button, true);
    addToDisplay8Section(value, 3, button, true);
}
void removeDisplaySection(byte row, byte section) {
    for (int i = 0; i < 17; i++) displayBuffer[row][i + 17 * section] = 32;
}
void removeDisplayRow(byte row) {
    for (int i = 0; i < 68; i++) displayBuffer[row][i] = 32;
}
void showDisplayRow(byte row) {
    sysexDisplayBuffer[4] = row + 24;
    for (int i = 0; i < 68; i++) sysexDisplayBuffer[i + 8] = displayBuffer[row][i];

    if (PUSH_DEBUG) Serial.printf("[DISPLAY] Zeile %u\n", row);
    pushSendSysExRaw(sysexDisplayBuffer, 77);
    delay(displayBugDelay);
}
void initDisplay() {
    addDisplayTop();
    byte sep[68] = {6,6,6,6,6,6,6,6,32,6,6,6,6,6,6,6,6,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                    6,6,6,6,6,6,6,6,32,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,32,6,6,6,6,6,6,6,6};
    for (int i = 0; i < 68; i++) displayBuffer[1][i] = sep[i];
    addButtonLabelAndData(1, "Mode", nameOfScales[scalePresetScales[scalePresetIndex]]);
    addButtonLabelAndData(2, "Tonic", nameOfKeys[scalePresetTonics[scalePresetIndex]]);
    addButtonLabelAndData(5, "Channel", String(pageChannels[instrumentPageIndex]));
    addButtonLabelAndData(6, "Color", nameOfColors[pageColors[instrumentPageIndex]]);
    addButtonLabelAndData(7, "Layout", nameOfLayouts[pageLayouts[instrumentPageIndex]]);
    addButtonLabelAndData(8, "Octave", String(pageOctaves[instrumentPageIndex]));
    for (int r = 0; r < 4; r++) showDisplayRow(r);
}

// ===========================================================================
//  LAYOUT (unverändert)
// ===========================================================================
byte getXlayout(byte pageIndex) { return layouts[pageLayouts[pageIndex]][0]; }
byte getYlayout(byte pageIndex) { return layouts[pageLayouts[pageIndex]][1]; }

// ===========================================================================
//  CC-BEHANDLUNG (Push -> Laptop, per AppleMIDI)
// ===========================================================================
void handleCC(byte cc, byte val) {
    if (cc == 64) {
        MIDI.sendControlChange(64, val, pageChannels[instrumentPageIndex]);
        return;
    }
    if (cc == 54 || cc == 55) {
        killAllPressedNotesDIN(pageChannels[instrumentPageIndex]);
        if (cc == 54 && val == 127)
            pageOctaves[instrumentPageIndex] = constrain(pageOctaves[instrumentPageIndex] - 1, 0, 8);
        else if (cc == 55 && val == 127)
            pageOctaves[instrumentPageIndex] = constrain(pageOctaves[instrumentPageIndex] + 1, 0, 8);
        addButtonLabelAndData(8, "Octave", String(pageOctaves[instrumentPageIndex]));
        showDisplayRow(3); initGrid();
        return;
    }
    if (cc == 14) {
        killAllPressedNotesDIN(pageChannels[instrumentPageIndex]);
        if (val < 64)
            scalePresetScales[scalePresetIndex] =
                indexInLoopingPattern(scalePresetScales[scalePresetIndex] + 1, numberOfScales);
        else
            scalePresetScales[scalePresetIndex] =
                indexInLoopingPattern(scalePresetScales[scalePresetIndex] - 1, numberOfScales);
        addButtonLabelAndData(1, "Mode", nameOfScales[scalePresetScales[scalePresetIndex]]);
        showDisplayRow(3); initGrid();
        return;
    }
    if ((cc == 20 || cc == 21) && val == 127) {
        killAllPressedNotesDIN(pageChannels[instrumentPageIndex]);
        if (cc == 20) {
            scalePresetScales[scalePresetIndex] =
                indexInLoopingPattern(scalePresetScales[scalePresetIndex] + 1, numberOfScales);
            addButtonLabelAndData(1, "Mode", nameOfScales[scalePresetScales[scalePresetIndex]]);
            showDisplayRow(3); initGrid();
        } else {
            scalePresetTonics[scalePresetIndex] =
                indexInLoopingPattern(scalePresetTonics[scalePresetIndex] + 1, 12);
            addButtonLabelAndData(2, "Tonic", nameOfKeys[scalePresetTonics[scalePresetIndex]]);
            showDisplayRow(3);
        }
        return;
    }
    if (cc >= 24 && cc <= 27 && val == 127) {
        if (cc == 24) {
            killAllPressedNotesDIN(pageChannels[instrumentPageIndex]);
            pageChannels[instrumentPageIndex] = loopingNumbers(pageChannels[instrumentPageIndex] + 1, 16);
            addButtonLabelAndData(5, "Channel", String(pageChannels[instrumentPageIndex]));
        } else if (cc == 25) {
            pageColors[instrumentPageIndex] = indexInLoopingPattern(pageColors[instrumentPageIndex] + 1, numberOfColorSchemes);
            addButtonLabelAndData(6, "Color", nameOfColors[pageColors[instrumentPageIndex]]);
            initGrid(); initPageSelectButtons();
        } else if (cc == 26) {
            pageLayouts[instrumentPageIndex] = indexInLoopingPattern(pageLayouts[instrumentPageIndex] + 1, numberOfLayouts);
            addButtonLabelAndData(7, "Layout", nameOfLayouts[pageLayouts[instrumentPageIndex]]);
            initGrid();
        } else if (cc == 27) {
            killAllPressedNotesDIN(pageChannels[instrumentPageIndex]);
            pageOctaves[instrumentPageIndex] = indexInLoopingPattern(pageOctaves[instrumentPageIndex] + 1, 9);
            addButtonLabelAndData(8, "Octave", String(pageOctaves[instrumentPageIndex]));
            initGrid();
        }
        showDisplayRow(2); showDisplayRow(3);
        return;
    }
    if (cc >= 102 && cc <= 105 && val == 127) {
        killAllPressedNotesDIN(pageChannels[instrumentPageIndex]);
        scalePresetIndex = cc - 102;
        addDisplayTop();
        addButtonLabelAndData(1, "Mode", nameOfScales[scalePresetScales[scalePresetIndex]]);
        addButtonLabelAndData(2, "Tonic", nameOfKeys[scalePresetTonics[scalePresetIndex]]);
        initScalePresetSelectButtons();
        showDisplayRow(0); showDisplayRow(3); initGrid();
        return;
    }
    if (cc >= 106 && cc <= 109 && val == 127) {
        killAllPressedNotesDIN(pageChannels[instrumentPageIndex]);
        instrumentPageIndex = cc - 106;
        addDisplayTop();
        addButtonLabelAndData(5, "Channel", String(pageChannels[instrumentPageIndex]));
        addButtonLabelAndData(6, "Color", nameOfColors[pageColors[instrumentPageIndex]]);
        addButtonLabelAndData(7, "Layout", nameOfLayouts[pageLayouts[instrumentPageIndex]]);
        addButtonLabelAndData(8, "Octave", String(pageOctaves[instrumentPageIndex]));
        showDisplayRow(0); showDisplayRow(3); initGrid();
        return;
    }
}

// ===========================================================================
//  USB-CALLBACK – füllt nur die Queue (kein blockierendes delay!)
// ===========================================================================
void onUsbMidi(const EspUsbHostMidiMessage& msg) {
    uint8_t status = msg.status;
    uint8_t type = status & 0xF0;

    UsbEvent ev;
    ev.ch = (status & 0x0F) + 1;

    if (type == 0x90 && msg.data2 > 0) {
        ev.type = EV_NOTE_ON;  ev.d1 = msg.data1; ev.d2 = msg.data2;
    } else if (type == 0x80 || (type == 0x90 && msg.data2 == 0)) {
        ev.type = EV_NOTE_OFF; ev.d1 = msg.data1; ev.d2 = msg.data2;
    } else if (type == 0xB0) {
        ev.type = EV_CC;       ev.d1 = msg.data1; ev.d2 = msg.data2;
    } else if (type == 0xE0) {
        ev.type = EV_PITCH;    ev.d1 = msg.data1; ev.d2 = msg.data2;
    } else if (type == 0xD0) {
        ev.type = EV_AT;       ev.d1 = msg.data1; ev.d2 = 0;
    } else {
        return;   // andere Nachrichten ignorieren
    }

    if (PUSH_DEBUG) {
        Serial.printf("[USB] cable=%u status=%02X data1=%u data2=%u\n",
                      msg.cable, msg.status, msg.data1, msg.data2);
    }
    xQueueSend(usbQueue, &ev, 0);
}

// ===========================================================================
//  APPLEMIDI-CALLBACKS – Feedback vom Laptop an den Push (LEDs)
// ===========================================================================
void onAppleNoteOn(byte channel, byte pitch, byte velocity) {
    if (channel != pageChannels[instrumentPageIndex]) return;
    if (PUSH_DEBUG) Serial.printf("[RTP] NoteOn  ch=%u note=%u vel=%u\n", channel, pitch, velocity);
    for (int i = 0; i < 64; i++)
        if (pitch == pushButtonIndexToAbsoluteNote(i)) displayButton(i, true);
}

void onAppleNoteOff(byte channel, byte pitch, byte velocity) {
    if (channel != pageChannels[instrumentPageIndex]) return;
    if (PUSH_DEBUG) Serial.printf("[RTP] NoteOff ch=%u note=%u\n", channel, pitch);
    for (int i = 0; i < 64; i++)
        if (pitch == pushButtonIndexToAbsoluteNote(i)) displayButton(i, false);
}

// ===========================================================================
//  SETUP
// ===========================================================================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("=== hardpush ESP32-S3 (EspUsbHost + AppleMIDI) ===");

    // --- WLAN ---
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\nWLAN ok, IP %s\n", WiFi.localIP().toString().c_str());

    // --- AppleMIDI ---
    MIDI.begin();
    AppleMIDI.setHandleConnected([](const APPLEMIDI_NAMESPACE::ssrc_t &ssrc, const char* name) {
        Serial.printf("[RTP] Verbunden: %s\n", name);
        // AppleMIDI.sendInvite(REMOTE_IP, REMOTE_PORT);
    });
    AppleMIDI.setHandleDisconnected([](const APPLEMIDI_NAMESPACE::ssrc_t &ssrc) {
        Serial.println("[RTP] Getrennt");
    });
    MIDI.setHandleNoteOn(onAppleNoteOn);
    MIDI.setHandleNoteOff(onAppleNoteOff);
    AppleMIDI.sendInvite(REMOTE_IP, REMOTE_PORT);
    Serial.println("[RTP] Invite an Laptop gesendet");

    // --- USB-Host (EspUsbHost) ---
    usbQueue = xQueueCreate(32, sizeof(UsbEvent));
    usbHost.onMidiMessage(onUsbMidi);
    usbHost.onDeviceConnected([](const EspUsbHostDeviceInfo &device) {
        Serial.printf("[USB] Gerät verbunden: VID=%04X PID=%04X\n", device.vid, device.pid);
        pushNeedsInit = true;   // kein delay() im USB-Task!
    });
    usbHost.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
        Serial.println("[USB] Gerät getrennt");
    });
    usbHost.begin();

    // --- Konfiguration laden (Dummy-Werte, da SD-Karte entfällt) ---
    instrumentPageIndex = 0;
    scalePresetIndex    = 0;
    for (int i = 0; i < numberOfPages; i++) {
        pageChannels[i] = i + 1;
        pageColors[i]   = i;
        pageLayouts[i]  = 0;
        pageOctaves[i]  = 3;
    }
    for (int i = 0; i < numberOfScalePresets; i++) {
        scalePresetScales[i] = 1;   // Major
        scalePresetTonics[i] = 0;   // C
    }

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 68; c++) displayBuffer[r][c] = 32;

    killAllNotesUSB();
    Serial.println("Setup abgeschlossen.");
}

// ===========================================================================
//  LOOP
// ===========================================================================
void loop() {
    // AppleMIDI-Empfang
    MIDI.read();

    if (pushNeedsInit) {
        pushNeedsInit = false;
        delay(500);         // hier ist delay() unkritisch
        initPushSettings();
        initGrid();
        initOtherButtons();
        initDisplay();
    }

    // USB-Events aus der Queue verarbeiten
    UsbEvent ev;
    while (xQueueReceive(usbQueue, &ev, 0) == pdTRUE) {

        if (ev.type == EV_NOTE_ON) {
            if (PUSH_DEBUG) Serial.printf("[PUSH] NoteOn  ch=%u note=%u vel=%u\n", ev.ch, ev.d1, ev.d2);
            if (ev.d1 > 35 && ev.d1 < 100) {
                byte mapped = pushButtonIndexToAbsoluteNote(ev.d1 - 36);
                if (PUSH_DEBUG) Serial.printf("  Pad %u -> Note %u\n", ev.d1 - 36, mapped);
                if (legalRange(mapped)) {
                    byte vel = ev.d2 * 20;
                    if (vel > 126) vel = 126;
                    MIDI.sendNoteOn(mapped, vel, pageChannels[instrumentPageIndex]);
                    pressedNotes[mapped] = true;
                }
            }
            displayButton(ev.d1 - 36, true);

        } else if (ev.type == EV_NOTE_OFF) {
            if (PUSH_DEBUG) Serial.printf("[PUSH] NoteOff ch=%u note=%u\n", ev.ch, ev.d1);
            if (ev.d1 > 35 && ev.d1 < 100) {
                byte mapped = pushButtonIndexToAbsoluteNote(ev.d1 - 36);
                if (legalRange(mapped)) {
                    MIDI.sendNoteOff(mapped, 0, pageChannels[instrumentPageIndex]);
                    pressedNotes[mapped] = false;
                }
            }
            displayButton(ev.d1 - 36, false);

        } else if (ev.type == EV_CC) {
            if (PUSH_DEBUG) Serial.printf("[PUSH] CC ch=%u cc=%u val=%u\n", ev.ch, ev.d1, ev.d2);
            handleCC(ev.d1, ev.d2);

        } else if (ev.type == EV_PITCH) {
            int bend = ((int)ev.d2 << 7) | ev.d1;
            MIDI.sendPitchBend(bend - 8192, pageChannels[instrumentPageIndex]);

        } else if (ev.type == EV_AT) {
            // Channel Pressure (0xD0) hat keine Convenience-Methode in AppleMIDI;
            // wird hier bei Bedarf als rohe Nachricht gesendet.
        }
    }
}