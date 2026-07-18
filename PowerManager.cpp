#include "wled.h"

#define COUNT_OF(x) ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

// external usermod: this ID is not present in WLED's const.h (overridable in case of a clash)
#ifndef USERMOD_ID_POWERMANAGER
  #define USERMOD_ID_POWERMANAGER 200
#endif

#ifndef POWERMANAGER_MAX_RELAYS
  #define POWERMANAGER_MAX_RELAYS 4
#else
  #if POWERMANAGER_MAX_RELAYS>16
    #undef POWERMANAGER_MAX_RELAYS
    #define POWERMANAGER_MAX_RELAYS 16
    #warning Maximum relays set to 16
  #endif
#endif

#ifndef POWERMANAGER_PINS
  #define POWERMANAGER_PINS -1
  #define POWERMANAGER_ENABLED false
#else
  #define POWERMANAGER_ENABLED true
#endif

#ifndef POWERMANAGER_HA_DISCOVERY
  #define POWERMANAGER_HA_DISCOVERY false
#endif

#ifndef POWERMANAGER_DELAYS
  #define POWERMANAGER_DELAYS 0
#endif

#ifndef POWERMANAGER_EXTERNALS
  #define POWERMANAGER_EXTERNALS false
#endif

#ifndef POWERMANAGER_INVERTS
  #define POWERMANAGER_INVERTS false
#endif

#ifndef POWERMANAGER_SEGMENTS
  #define POWERMANAGER_SEGMENTS -1
#endif

#ifndef POWERMANAGER_STABILIZE
  #define POWERMANAGER_STABILIZE 1
#endif

// relay 0 doubles as the dedicated "Master AC relay" (e.g. the AC-side trigger of the main PSU):
// when enabled it is on while any segment is on. Only relay 0 can take this role.
#ifndef POWERMANAGER_MASTER
  #define POWERMANAGER_MASTER false
#endif

// sync main power with the Master AC relay: when it cuts because every segment was switched off,
// main power is switched off too (UI/MQTT/HA show off); switching a segment on restores it
#ifndef POWERMANAGER_MASTER_MAIN_SYNC
  #define POWERMANAGER_MASTER_MAIN_SYNC false
#endif

// take over all relays: unconfigured relays (pin set, but no segment link and not externally
// controlled) stay off until they are given a role, instead of following main power (default WLED
// behavior). Avoids surprises once part of the relays is segment-coupled.
#ifndef POWERMANAGER_TAKEOVER
  #define POWERMANAGER_TAKEOVER false
#endif

// special relay_t.segment value: relay is on while *any* segment is on (master PSU mode)
#define POWERMANAGER_SEG_ANY 99

#ifndef POWERMANAGER_NAMES
  #define POWERMANAGER_NAMES ""
#endif

#ifndef POWERMANAGER_NAME_LEN
  #define POWERMANAGER_NAME_LEN 33 // max relay name length including terminator
#endif

// anti-flash blackout around segment power-on: black frames sent before / kept up after switching the port
#ifndef POWERMANAGER_BLACK_PRE_MS
  #define POWERMANAGER_BLACK_PRE_MS 200
#endif
#ifndef POWERMANAGER_BLACK_POST_MS
  #define POWERMANAGER_BLACK_POST_MS 200
#endif

// minimum time a port stays off before it may be re-energised: lets the LED strip's capacitors
// discharge so the chips reset properly (re-powering a half-discharged strip can latch a white
// flash no matter what data is streaming)
#ifndef POWERMANAGER_MIN_OFF_MS
  #define POWERMANAGER_MIN_OFF_MS 2000
#endif

// power-on blackout phases (relay_t.boPhase)
#define BO_NONE 0
#define BO_PRE  1  // sending black frames, port still off
#define BO_POST 2  // port energised, still sending black frames
#define BO_FADE 3  // still black; cancelled stale transition is being destroyed, clean fade starts next frame

#define BO_FADE_TIMEOUT_MS        250  // max wait in BO_FADE for a cancelled transition to be destroyed
#define POWERMANAGER_FADE_GRACE_MS 1000 // extra wait for a lingering fade-out before cutting power anyway

#define WLED_DEBOUNCE_THRESHOLD 50 //only consider button input of at least 50ms as valid (debouncing)

#define ON  true
#define OFF false

// I2C port expander types
#define EXPANDER_NONE    0
#define EXPANDER_PCF8574 1
#define EXPANDER_AW9523  2

#if defined(USERMOD_USE_PCF8574) && defined(USERMOD_USE_AW9523)
  #error "PowerManager: define either USERMOD_USE_PCF8574 or USERMOD_USE_AW9523, not both"
#endif

#if defined(USERMOD_USE_PCF8574)
  #define POWERMANAGER_EXPANDER EXPANDER_PCF8574
#elif defined(USERMOD_USE_AW9523)
  #define POWERMANAGER_EXPANDER EXPANDER_AW9523
#else
  #ifndef POWERMANAGER_EXPANDER
    #define POWERMANAGER_EXPANDER EXPANDER_NONE
  #endif
#endif

#ifndef PCF8574_ADDRESS
  #define PCF8574_ADDRESS 0x20  // some may start at 0x38
#endif

#ifndef AW9523_ADDRESS
  #define AW9523_ADDRESS 0x58   // AD1=AD0=GND; 0x58-0x5B depending on AD1/AD0 straps
#endif

#ifndef AW9523_P0_PUSHPULL
  #define AW9523_P0_PUSHPULL true // P0_x port drive mode: push-pull (recommended for relays), false = open-drain
#endif

#if POWERMANAGER_EXPANDER == EXPANDER_AW9523
  #define POWERMANAGER_EXPANDER_ADDR AW9523_ADDRESS
#else
  #define POWERMANAGER_EXPANDER_ADDR PCF8574_ADDRESS
#endif

// AW9523(B) register map (registers 0x03, 0x05, 0x07 and 0x13 are the P1 counterparts of P0 at 0x02, 0x04, 0x06 and 0x12)
#define AW9523_REG_IN0    0x00  // input state (read-only)
#define AW9523_REG_OUT0   0x02  // output state: 0=low, 1=high
#define AW9523_REG_CFG0   0x04  // direction: 0=output, 1=input
#define AW9523_REG_INT0   0x06  // interrupt: 0=enabled, 1=disabled
#define AW9523_REG_ID     0x10  // read-only, returns 0x23
#define AW9523_REG_GCR    0x11  // global control: bit4 P0 push-pull, bits 0-1 LED current range
#define AW9523_REG_MODE0  0x12  // port mode: 0=LED, 1=GPIO
#define AW9523_ID_VALUE   0x23
#define AW9523_GCR_GPOMD  0x10  // GCR bit4: P0 port push-pull when set

/*
 * Power Manager usermod
 *
 * Handles multiple relay/MOSFET power outputs (direct GPIO or PCF8574/AW9523 I2C expanders).
 * Relays can be named and coupled to segments: a coupled relay follows its segment's on/off
 * state to cut the actual supply power of individual LED sections, with anti-flash power
 * sequencing, PSU stabilization and a dedicated Master AC relay (relay 0) that is always the
 * last to cut and can mirror WLED's main power state.
 * See readme.md and the "Segment coupling & power sequencing" section below for details.
 *
 * This usermod grew out of WLED's built-in multi_relay usermod:
 *   multi_relay written and maintained by @blazoncek (with contributions noted in its history)
 *   power sequencing / segment coupling extensions and rename by @Quindor (intermit.tech)
 * Settings saved by multi_relay are migrated automatically (see readFromConfig()).
 */


typedef struct relay_t {
  int8_t pin;
  struct { // reduces memory footprint
    bool active   : 1;  // is the relay waiting to be switched
    bool invert   : 1;  // does On mean 1 or 0
    bool state    : 1;  // 1 relay is On, 0 relay is Off
    bool external : 1;  // is the relay externally controlled
    int8_t button : 4;  // which button triggers relay
    bool segSeen  : 1;  // segment coupling: linked segment observed active this session (guards deletion detection against boot races)
    uint8_t boPhase : 2; // power-on blackout phase (BO_NONE/BO_PRE/BO_POST/BO_FADE)
  };
  uint16_t delayOn;     // seconds to wait before switching the relay on
  uint16_t delayOff;    // seconds to wait before switching the relay off
  int8_t segment;       // segment this relay follows: -1 = not coupled, 0..MAX_NUM_SEGMENTS-1 = segment id, 99 = any segment on
  char name[POWERMANAGER_NAME_LEN]; // user-friendly name (e.g. physical output port), shown in UI/Info/HA
} Relay;


class PowerManager : public Usermod {

  private:
    // array of relays
    Relay    _relay[POWERMANAGER_MAX_RELAYS];

    uint32_t _switchTimerStart; // switch timer start time
    // segment coupling & power sequencing state (see section comment in the implementation)
    uint32_t _pendingSince[POWERMANAGER_MAX_RELAYS]; // start of a pending delayed switch (0 = nothing pending)
    uint32_t _boStart[POWERMANAGER_MAX_RELAYS];      // start of the current blackout phase
    uint32_t _onAt[POWERMANAGER_MAX_RELAYS];         // last switch-on time (master stabilization window)
    uint32_t _offAt[POWERMANAGER_MAX_RELAYS];        // last switch-off time (minimum off-time gate)
    bool     _oldMode;          // old brightness
    bool     enabled;           // usermod enabled
    bool     initDone;          // status of initialisation
    uint8_t  expanderType;      // I2C port expander type (EXPANDER_NONE/EXPANDER_PCF8574/EXPANDER_AW9523)
    uint8_t  expanderAddr;      // I2C address of port expander
    bool     awP0PushPull;      // AW9523: drive P0_x port in push-pull mode (instead of open-drain)
    bool     awFound;           // AW9523: chip detected on I2C bus (ID register check)
    uint16_t awOutputState;     // AW9523: shadow of output registers (bit n = port n; 0-7 = P0_x, 8-15 = P1_x)
    uint16_t boPreMs;           // blackout: black frames before segment power-on (0 with boPostMs 0 = disabled)
    uint16_t boPostMs;          // blackout: keep sending black frames this long after power-on
    uint16_t stabilizeSec;      // PSU stabilization (seconds): after the Master AC relay powers on,
                                // the strip stays black and dependent coupled relays wait this long
    uint16_t minOffMs;          // minimum port off-time before re-energising (LED capacitor discharge)
    bool     masterEnabled;     // relay 0 acts as the Master AC relay (segment = POWERMANAGER_SEG_ANY)
    bool     masterMainSync;    // main power follows the Master AC relay (off when it cuts, back on with a segment)
    bool     _autoMainOff;      // this usermod switched main power off (a segment turning on restores it)
    bool     takeOverRelays;    // unconfigured relays stay off instead of following main power
    bool     HAautodiscovery;
    uint16_t periodicBroadcastSec;
    unsigned long lastBroadcast;

    // strings to reduce flash memory usage (used more than twice)
    static const char _name[];
    static const char _legacyName[];
    static const char _enabled[];
    static const char _relay_str[];
    static const char _delay_str[];
    static const char _activeHigh[];
    static const char _external[];
    static const char _button[];
    static const char _broadcast[];
    static const char _HAautodiscovery[];
    static const char _pcf8574[];
    static const char _pcfAddress[];
    static const char _expander[];
    static const char _expanderAddr[];
    static const char _pushPull[];
    static const char _switch[];
    static const char _toggle[];
    static const char _Command[];
    static const char _segment_str[];
    static const char _delayOn_str[];
    static const char _delayOff_str[];
    static const char _stabilize_str[];
    static const char _blackPre[];
    static const char _blackPost[];
    static const char _minOff[];
    static const char _master[];
    static const char _mainSync[];
    static const char _takeOver[];

    void handleOffTimer();
    void handleSegmentCoupling();
    void handleBlackout();
    void beginFadePhase(uint8_t r);
    void restartSegmentFade(uint8_t r);
    void cancelSegTransitions(uint8_t r);
    bool isSegFading(uint8_t r);
    bool masterHoldsOn();
    bool dependentsStillOn();
    void setSegmentLink(uint8_t relay, int seg, bool persist = true);
    void InitHtmlAPIHandle();
    int getValue(String data, char separator, int index);
    uint8_t getActiveRelayCount();

    // number of output ports the configured expander provides (expander pins are 100 .. 100+count-1)
    inline uint8_t expanderPortCount() { return expanderType == EXPANDER_AW9523 ? 16 : 8; }

    // relay is coupled to a segment (exclusively segment-driven, no external/button control)
    inline bool isCoupled(uint8_t r) { return _relay[r].segment >= 0; }

    // invoke fn(Segment&) for every active segment the relay is coupled to
    // (its own segment, or all of them for an "any segment" master relay)
    template<typename FN> void forOwnedSegments(uint8_t r, FN fn) {
      if (_relay[r].segment == POWERMANAGER_SEG_ANY) {
        for (unsigned s = 0; s < strip.getSegmentsNum(); s++) {
          Segment &seg = strip.getSegment(s);
          if (seg.isActive()) fn(seg);
        }
      } else if ((uint8_t)_relay[r].segment < strip.getSegmentsNum()) {
        Segment &seg = strip.getSegment(_relay[r].segment);
        if (seg.isActive()) fn(seg);
      }
    }

    byte IOexpanderWrite(byte address, byte _data);
    byte IOexpanderRead(int address);

    // AW9523 register access (byte pairs use the chip's auto-incrementing register pointer)
    byte    awRegWrite8(uint8_t reg, uint8_t value);
    byte    awRegWrite16(uint8_t reg, uint16_t value);
    int16_t awRegRead8(uint8_t reg); // returns -1 on I2C error
    void    initAW9523(uint16_t state, uint16_t used);

    void publishMqtt(int relay);
#ifndef WLED_DISABLE_MQTT
    void publishHomeAssistantAutodiscovery();
#endif

  public:
    /**
     * constructor
     */
    PowerManager();

    /**
     * desctructor
     */
    //~PowerManager() {}

    /**
     * Enable/Disable the usermod
     */
    inline void enable(bool enable) { enabled = enable; }

    /**
     * Get usermod enabled/disabled state
     */
    inline bool isEnabled() { return enabled; }

    /**
     * getId() allows you to optionally give your V2 usermod an unique ID (please define it in const.h!).
     * This could be used in the future for the system to determine whether your usermod is installed.
     */
    inline uint16_t getId() override { return USERMOD_ID_POWERMANAGER; }

    /**
     * switch relay on/off
     */
    void switchRelay(uint8_t relay, bool mode);

    /**
     * toggle relay
     */
    inline void toggleRelay(uint8_t relay) {
      switchRelay(relay, !_relay[relay].state);
    }

    /**
     * setup() is called once at boot. WiFi is not yet connected at this point.
     * You can use it to initialize variables, sensors or similar.
     */
    void setup() override;

    /**
     * connected() is called every time the WiFi is (re)connected
     * Use it to initialize network interfaces
     */
    inline void connected() override { InitHtmlAPIHandle(); }

    /**
     * loop() is called continuously. Here you can check for events, read sensors, etc.
     */
    void loop() override;

    /**
     * called after effects are rendered, just before strip.show();
     * paints segments in a power-on blackout window black
     */
    void handleOverlayDraw() override;

    /**
     * streams the JavaScript that injects the segment-card "Power relays" menu into the
     * main web UI (served at /um.js; the UI runs it after every state render)
     */
#ifdef WLED_ENABLE_UM_UI_INJECT // WLED base includes the usermod web-UI injection mechanism
    void addUIInjectCode(Print &dest) override;
#endif

#ifndef WLED_DISABLE_MQTT
    bool onMqttMessage(char* topic, char* payload) override;
    void onMqttConnect(bool sessionPresent) override;
#endif

    /**
     * handleButton() can be used to override default button behaviour. Returning true
     * will prevent button working in a default way.
     * Replicating button.cpp
     */
    bool handleButton(uint8_t b) override;

    /**
     * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
     */
    void addToJsonInfo(JsonObject &root) override;

    /**
     * addToJsonState() can be used to add custom entries to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void addToJsonState(JsonObject &root) override;

    /**
     * readFromJsonState() can be used to receive data clients send to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void readFromJsonState(JsonObject &root) override;

    /**
     * provide the changeable values
     */
    void addToConfig(JsonObject &root) override;

    void appendConfigData() override;

    /**
     * restore the changeable values
     * readFromConfig() is called before setup() to populate properties from values stored in cfg.json
     * 
     * The function should return true if configuration was successfully loaded or false if there was no configuration.
     */
    bool readFromConfig(JsonObject &root) override;
};


// class implementation

void PowerManager::publishMqtt(int relay) {
#ifndef WLED_DISABLE_MQTT
  //Check if MQTT Connected, otherwise it will crash the 8266
  if (WLED_MQTT_CONNECTED){
    char subuf[64];
    sprintf_P(subuf, PSTR("%s/relay/%d"), mqttDeviceTopic, relay);
    mqtt->publish(subuf, 0, false, _relay[relay].state ? "on" : "off");
  }
#endif
}

/**
 * switch off the strip if the delay has elapsed 
 */
void PowerManager::handleOffTimer() {
  unsigned long now = millis();
  bool activeRelays = false;
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (_relay[i].active && _switchTimerStart > 0 && now - _switchTimerStart > (unsigned long)(offMode ? _relay[i].delayOff : _relay[i].delayOn)*1000) {
      if (!_relay[i].external) switchRelay(i, !offMode);
      _relay[i].active = false;
    } else if (periodicBroadcastSec && now - lastBroadcast > (periodicBroadcastSec*1000)) {
      if (_relay[i].pin>=0) publishMqtt(i);
    }
    activeRelays = activeRelays || _relay[i].active;
  }
  if (!activeRelays) _switchTimerStart = 0;
  if (periodicBroadcastSec && now - lastBroadcast > (periodicBroadcastSec*1000)) lastBroadcast = now;
}

/**
 * HTTP API handler
 * borrowed from:
 * https://github.com/gsieben/WLED/blob/master/usermods/GeoGab-Relays/usermod_GeoGab.h
 */
#define GEOGABVERSION "0.1.3"
void PowerManager::InitHtmlAPIHandle() {  // https://github.com/me-no-dev/ESPAsyncWebServer
  DEBUG_PRINTLN(F("Relays: Initialize HTML API"));

  server.on(F("/relays"), HTTP_GET, [this](AsyncWebServerRequest *request) {
    DEBUG_PRINTLN(F("Relays: HTML API"));
    String janswer;
    String error = "";
    //int params = request->params();
    janswer = F("{\"NoOfRelays\":");
    janswer += String(POWERMANAGER_MAX_RELAYS) + ",";

    if (getActiveRelayCount()) {
      // Commands
      if (request->hasParam(FPSTR(_switch))) {
        /**** Switch ****/
        AsyncWebParameter* p = request->getParam(FPSTR(_switch));
        // Get Values
        for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
          int value = getValue(p->value(), ',', i);
          if (value==-1) {
            error = F("There must be as many arguments as relays");
          } else {
            // Switch
            if (_relay[i].external) switchRelay(i, (bool)value);
          }
        }
      } else if (request->hasParam(FPSTR(_toggle))) {
        /**** Toggle ****/
        AsyncWebParameter* p = request->getParam(FPSTR(_toggle));
        // Get Values
        for (int i=0;i<POWERMANAGER_MAX_RELAYS;i++) {
          int value = getValue(p->value(), ',', i);
          if (value==-1) {
            error = F("There must be as many arguments as relays");
          } else {
            // Toggle
            if (value && _relay[i].external) toggleRelay(i);
          }
        }
      } else {
        error = F("No valid command found");
      }
    } else {
      error = F("No active relays");
    }

    // Status response
    char sbuf[16];
    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
      sprintf_P(sbuf, PSTR("\"%d\":%d,"), i, (_relay[i].pin<0 ? -1 : (int)_relay[i].state));
      janswer += sbuf;
    }
    janswer += F("\"error\":\"");
    janswer += error;
    janswer += F("\",");
    janswer += F("\"SW Version\":\"");
    janswer += String(F(GEOGABVERSION));
    janswer += F("\"}");
    request->send(200, "application/json", janswer);
  });
}

int PowerManager::getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length()-1;

  for(int i=0; i<=maxIndex && found<=index; i++){
    if(data.charAt(i)==separator || i==maxIndex){
        found++;
        strIndex[0] = strIndex[1]+1;
        strIndex[1] = (i == maxIndex) ? i+1 : i;
    }
  }
  return found>index ? data.substring(strIndex[0], strIndex[1]).toInt() : -1;
}

//Write a byte to the IO expander
byte PowerManager::IOexpanderWrite(byte address, byte _data ) {
  Wire.beginTransmission(address);
  Wire.write(_data);
  return Wire.endTransmission(); 
}

//Read a byte from the IO expander
byte PowerManager::IOexpanderRead(int address) {
  byte _data = 0;
  Wire.requestFrom(address, 1);
  if (Wire.available()) {
    _data = Wire.read();
  }
  return _data;
}

//Write a byte to an AW9523 register
byte PowerManager::awRegWrite8(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(expanderAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission();
}

//Write two consecutive AW9523 registers (low byte to reg, high byte to reg+1) in one transaction
byte PowerManager::awRegWrite16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(expanderAddr);
  Wire.write(reg);
  Wire.write((uint8_t)(value & 0xFF));
  Wire.write((uint8_t)(value >> 8));
  return Wire.endTransmission();
}

//Read a byte from an AW9523 register, -1 on I2C error
int16_t PowerManager::awRegRead8(uint8_t reg) {
  Wire.beginTransmission(expanderAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1; // repeated start
  if (Wire.requestFrom(expanderAddr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

/**
 * initialise AW9523: verify chip ID, then configure only the ports used by relays
 * (GPIO mode, output direction, interrupts masked) without disturbing other ports.
 * state = desired output level per port (invert already applied), used = ports owned by relays
 */
void PowerManager::initAW9523(uint16_t state, uint16_t used) {
  awFound = (awRegRead8(AW9523_REG_ID) == AW9523_ID_VALUE);
  if (!awFound) {
    DEBUG_PRINTLN(F("AW9523 not found."));
    return;
  }

  // merge relay states into shadow of output registers so unrelated ports keep their level
  int16_t lo = awRegRead8(AW9523_REG_OUT0);
  int16_t hi = awRegRead8(AW9523_REG_OUT0+1);
  awOutputState = (lo < 0 || hi < 0) ? 0 : ((uint16_t)hi << 8) | (uint8_t)lo;
  awOutputState = (awOutputState & ~used) | (state & used);
  awRegWrite16(AW9523_REG_OUT0, awOutputState); // set levels before flipping direction to avoid glitches

  // P0 port drive mode; keep LED current bits, reserved bits must be written 0
  int16_t gcr = awRegRead8(AW9523_REG_GCR);
  awRegWrite8(AW9523_REG_GCR, ((gcr < 0 ? 0 : gcr) & 0x03) | (awP0PushPull ? AW9523_GCR_GPOMD : 0));

  for (int p = 0; p < 2; p++) { // p=0: P0_x registers, p=1: P1_x registers
    uint8_t mask = used >> (8*p);
    if (!mask) continue;
    int16_t v;
    v = awRegRead8(AW9523_REG_MODE0+p); if (v >= 0) awRegWrite8(AW9523_REG_MODE0+p, v | mask);  // GPIO mode
    v = awRegRead8(AW9523_REG_CFG0+p);  if (v >= 0) awRegWrite8(AW9523_REG_CFG0+p, v & ~mask);  // output direction
    v = awRegRead8(AW9523_REG_INT0+p);  if (v >= 0) awRegWrite8(AW9523_REG_INT0+p, v | mask);   // interrupts off
  }
  DEBUG_PRINTLN(F("AW9523 inited."));
}


// public methods

PowerManager::PowerManager()
  : _switchTimerStart(0)
  , enabled(POWERMANAGER_ENABLED)
  , initDone(false)
  , expanderType(POWERMANAGER_EXPANDER)
  , expanderAddr(POWERMANAGER_EXPANDER_ADDR)
  , awP0PushPull(AW9523_P0_PUSHPULL)
  , awFound(false)
  , awOutputState(0)
  , boPreMs(POWERMANAGER_BLACK_PRE_MS)
  , boPostMs(POWERMANAGER_BLACK_POST_MS)
  , stabilizeSec(POWERMANAGER_STABILIZE)
  , minOffMs(POWERMANAGER_MIN_OFF_MS)
  , masterEnabled(POWERMANAGER_MASTER)
  , masterMainSync(POWERMANAGER_MASTER_MAIN_SYNC)
  , _autoMainOff(false)
  , takeOverRelays(POWERMANAGER_TAKEOVER)
  , HAautodiscovery(POWERMANAGER_HA_DISCOVERY)
  , periodicBroadcastSec(60)
  , lastBroadcast(0)
{
  const int8_t defPins[] = {POWERMANAGER_PINS};
  const int8_t relayDelays[] = {POWERMANAGER_DELAYS};
  const bool relayExternals[] = {POWERMANAGER_EXTERNALS};
  const bool relayInverts[] = {POWERMANAGER_INVERTS};
  const int8_t relaySegments[] = {POWERMANAGER_SEGMENTS};
  const char* const relayNames[] = {POWERMANAGER_NAMES};

  for (size_t i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    _relay[i].pin      = i < COUNT_OF(defPins) ? defPins[i] : -1;
    _relay[i].delayOn  = i < COUNT_OF(relayDelays) ? relayDelays[i] : 0;
    _relay[i].delayOff = i < COUNT_OF(relayDelays) ? relayDelays[i] : 0;
    _relay[i].invert   = i < COUNT_OF(relayInverts) ? relayInverts[i] : false;
    _relay[i].active   = false;
    _relay[i].state    = false;
    _relay[i].external = i < COUNT_OF(relayExternals) ? relayExternals[i] : false;
    _relay[i].button   = -1;
    _relay[i].segment  = i < COUNT_OF(relaySegments) ? relaySegments[i] : -1;
    _relay[i].segSeen  = false;
    _relay[i].boPhase  = BO_NONE;
    _pendingSince[i]   = 0;
    _boStart[i]        = 0;
    _onAt[i]           = 0;
    _offAt[i]          = 0;
    strlcpy(_relay[i].name, i < COUNT_OF(relayNames) ? relayNames[i] : "", POWERMANAGER_NAME_LEN);
  }
  // relay 0 is the dedicated Master AC relay slot; only it may follow all segments
  if (masterEnabled) {
    _relay[0].segment = POWERMANAGER_SEG_ANY;
    if (_relay[0].delayOff == 0) _relay[0].delayOff = 5; // default PSU anti-cycling hold (config overrides)
  }
  for (size_t i=1; i<POWERMANAGER_MAX_RELAYS; i++)
    if (_relay[i].segment == POWERMANAGER_SEG_ANY) _relay[i].segment = -1;
}

/**
 * switch relay on/off
 */
void PowerManager::switchRelay(uint8_t relay, bool mode) {
  if (relay>=POWERMANAGER_MAX_RELAYS || _relay[relay].pin<0) return;
  if (_relay[relay].state != mode) { // timestamps drive the stabilization and minimum off-time windows
    if (mode) _onAt[relay]  = millis();
    else      _offAt[relay] = millis();
  }
  _relay[relay].state = mode;
  if (expanderType == EXPANDER_PCF8574 && _relay[relay].pin >= 100) {
    // we need to send all outputs at the same time
    uint8_t state = 0;
    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
      if (_relay[i].pin < 100) continue;
      uint8_t pin = _relay[i].pin - 100;
      state |= (_relay[i].invert ? !_relay[i].state : _relay[i].state) << pin; // fill relay states for all pins
    }
    IOexpanderWrite(expanderAddr, state);
    DEBUG_PRINT(F("Writing to PCF8574: ")); DEBUG_PRINTLN(state);
  } else if (expanderType == EXPANDER_AW9523 && _relay[relay].pin >= 100) {
    // shadow register lets us switch a single port without touching the others
    uint8_t port = _relay[relay].pin - 100;
    bitWrite(awOutputState, port, _relay[relay].invert ? !mode : mode);
    awRegWrite16(AW9523_REG_OUT0, awOutputState);
    DEBUG_PRINT(F("Writing to AW9523: ")); DEBUG_PRINTLN(awOutputState);
  } else if (_relay[relay].pin < 100) {
    pinMode(_relay[relay].pin, OUTPUT);
    digitalWrite(_relay[relay].pin, _relay[relay].invert ? !_relay[relay].state : _relay[relay].state);
  } else return;
  publishMqtt(relay);
}

uint8_t PowerManager::getActiveRelayCount() {
  uint8_t count = 0;
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) if (_relay[i].pin>=0) count++;
  return count;
}

/**
 * couple/decouple a relay to a segment (-1 = none, 0..n = segment id).
 * persist=false applies the link without writing cfg.json - used by preset-carried links
 * ("save":false) so switching presets does not wear flash; the configured links then
 * remain the boot baseline. The Master AC relay role (segment 99) is configured via
 * settings only, not via this API.
 */
void PowerManager::setSegmentLink(uint8_t relay, int seg, bool persist) {
  if (relay >= POWERMANAGER_MAX_RELAYS) return;
  if (relay == 0 && masterEnabled) return; // Master AC relay is not re-linkable
  if (seg < -1 || seg >= (int)strip.getMaxSegments()) seg = -1;
  if (_relay[relay].segment == seg) return;
  _relay[relay].segment = seg;
  _relay[relay].segSeen = false;
  _relay[relay].boPhase = BO_NONE;
  _pendingSince[relay] = 0;
  if (seg >= 0) _relay[relay].external = false; // coupled relays are exclusively segment-driven
  if (persist) configNeedsWrite = true; // persist to cfg.json (written from main loop)
}


//Functions called by WLED

#ifndef WLED_DISABLE_MQTT
/**
 * handling of MQTT message
 * topic only contains stripped topic (part after /wled/MAC)
 * topic should look like: /relay/X/command; where X is relay number, 0 based
 */
bool PowerManager::onMqttMessage(char* topic, char* payload) {
  if (strlen(topic) > 8 && strncmp_P(topic, PSTR("/relay/"), 7) == 0) {
    char *numEnd;
    uint8_t relay = strtoul(topic+7, &numEnd, 10);
    // relay number may have more than one digit; "/command" must directly follow it
    if (numEnd != topic+7 && strcmp_P(numEnd, _Command) == 0 && relay<POWERMANAGER_MAX_RELAYS) {
      String action = payload;
      if (action == "on") {
        if (_relay[relay].external) switchRelay(relay, true);
        return true;
      } else if (action == "off") {
        if (_relay[relay].external) switchRelay(relay, false);
        return true;
      } else if (action == FPSTR(_toggle)) {
        if (_relay[relay].external) toggleRelay(relay);
        return true;
      }
    }
  }
  return false;
}

/**
 * subscribe to MQTT topic for controlling relays
 */
void PowerManager::onMqttConnect(bool sessionPresent) {
  //(re)subscribe to required topics
  char subuf[64];
  if (mqttDeviceTopic[0] != 0) {
    strcpy(subuf, mqttDeviceTopic);
    strcat_P(subuf, PSTR("/relay/#"));
    mqtt->subscribe(subuf, 0);
    if (HAautodiscovery) publishHomeAssistantAutodiscovery();
    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
      if (_relay[i].pin<0) continue;
      publishMqtt(i); //publish current state
    }
  }
}

void PowerManager::publishHomeAssistantAutodiscovery() {
  for (int i = 0; i < POWERMANAGER_MAX_RELAYS; i++) {
    char uid[24], json_str[1024], buf[128];
    size_t payload_size;
    sprintf_P(uid, PSTR("%s_sw%d"), escapedMac.c_str(), i);

    if (_relay[i].pin >= 0 && _relay[i].external) {
      StaticJsonDocument<1024> json;
      if (_relay[i].name[0]) sprintf_P(buf, PSTR("%s %s"), serverDescription, _relay[i].name); //max length: 33 + 1 + 32 = 66
      else sprintf_P(buf, PSTR("%s Switch %d"), serverDescription, i); //max length: 33 + 8 + 3 = 44
      json[F("name")] = buf;

      sprintf_P(buf, PSTR("%s/relay/%d"), mqttDeviceTopic, i); //max length: 33 + 7 + 3 = 43
      json["~"] = buf;
      strcat_P(buf, _Command);
      mqtt->subscribe(buf, 0);

      json[F("stat_t")]  = "~";
      json[F("cmd_t")]   = F("~/command");
      json[F("pl_off")]  = "off";
      json[F("pl_on")]   = "on";
      json[F("uniq_id")] = uid;

      strcpy(buf, mqttDeviceTopic); //max length: 33 + 7 = 40
      strcat_P(buf, PSTR("/status"));
      json[F("avty_t")]       = buf;
      json[F("pl_avail")]     = F("online");
      json[F("pl_not_avail")] = F("offline");
      //TODO: dev
      payload_size = serializeJson(json, json_str);
    } else {
      //Unpublish disabled or internal relays
      json_str[0]  = 0;
      payload_size = 0;
    }
    sprintf_P(buf, PSTR("homeassistant/switch/%s/config"), uid);
    mqtt->publish(buf, 0, true, json_str, payload_size);
  }
}
#endif

/**
 * setup() is called once at boot. WiFi is not yet connected at this point.
 * You can use it to initialize variables, sensors or similar.
 */
void PowerManager::setup() {
  // pins retrieved from cfg.json (readFromConfig()) prior to running setup()
  // if we want an I2C port expander the I2C pins need to be valid
  if (i2c_sda<0 || i2c_scl<0) expanderType = EXPANDER_NONE;

  uint16_t state = 0; // desired expander output levels (invert applied)
  uint16_t used  = 0; // expander ports used by relays
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (isCoupled(i)) _relay[i].external = false; // coupled relays are exclusively segment-driven
    // coupled relays boot in Off state; handleSegmentCoupling() syncs them once segments are up
    if (_relay[i].pin >= 100) {
      uint8_t port = _relay[i].pin - 100;
      if (expanderType == EXPANDER_NONE || port >= expanderPortCount()) {
        _relay[i].pin = -1; // no expander configured or port out of range
        continue;
      }
      if (!_relay[i].external && !isCoupled(i)) _relay[i].state = takeOverRelays ? false : !offMode;
      state |= (uint16_t)(_relay[i].invert ? !_relay[i].state : _relay[i].state) << port;
      used  |= (uint16_t)1 << port;
    } else if (_relay[i].pin>=0) {
      // UM_MultiRelay is the closest core PinOwner; the enum cannot be extended by external usermods
      if (PinManager::allocatePin(_relay[i].pin,true, PinOwner::UM_MultiRelay)) {
        if (!_relay[i].external && !isCoupled(i)) _relay[i].state = takeOverRelays ? false : !offMode;
        switchRelay(i, _relay[i].state);
        _relay[i].active = false;
      } else {
        _relay[i].pin = -1;  // allocation failed
      }
    }
  }
  if (expanderType == EXPANDER_PCF8574) {
    IOexpanderWrite(expanderAddr, (uint8_t)state);  // init expander (set all outputs)
    DEBUG_PRINTLN(F("PCF8574 inited."));
  } else if (expanderType == EXPANDER_AW9523) {
    initAW9523(state, used);
  }
  _oldMode = offMode;
  initDone = true;
}

/**
 * loop() is called continuously. Here you can check for events, read sensors, etc.
 */
void PowerManager::loop() {
  static unsigned long lastUpdate = 0;
  yield();
  if (!enabled) return;
  handleBlackout(); // ms-precision power-on sequencing for coupled relays (cheap, runs every pass)
  if (strip.isUpdating() && millis() - lastUpdate < 100) return;

  if (millis() - lastUpdate < 100) return;  // update only 10 times/s
  lastUpdate = millis();

  //set relay when LEDs turn on
  if (_oldMode != offMode) {
    _oldMode = offMode;
    _switchTimerStart = millis();
    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
      // with "take over all relays", unconfigured relays do not follow main power (stay off)
      if ((_relay[i].pin>=0) && !_relay[i].external && !isCoupled(i) && !takeOverRelays) _relay[i].active = true;
    }
  }

  handleOffTimer();
  handleSegmentCoupling();
}

// --------------------------------------------------------------------------------------------
// Segment coupling & power sequencing
//
// A relay with `segment` >= 0 is exclusively segment-driven: on while its segment is on (or,
// for POWERMANAGER_SEG_ANY master relays, while any segment is on) and global power is on.
//
// Power-off: segment off -> wait for its fade-out to finish -> wait delay-off-s -> port off
//            (whichever ends later; toggling the segment back on cancels the pending cut).
// Power-on:  segment on -> delay-on-s -> min-off-ms since the last cut -> PSU stabilization
//            of master relays -> blackout sequence, advanced per loop pass by handleBlackout():
//   BO_PRE:  handleOverlayDraw() paints the segment black every frame, port still off
//   BO_POST: port energised while the incoming data is black; stays black over the power ramp
//            (a master relay additionally holds all-black for stabilize-s so the PSU settles)
//   BO_FADE: transitions are force-completed so a stale one (e.g. armed by a global power
//            toggle, with a non-black "from" state) cannot corrupt the restart; one serviced
//            frame later restartSegmentFade() fades the segment up from black
// --------------------------------------------------------------------------------------------

/**
 * is any segment the relay is coupled to still running a transition (fade)?
 */
bool PowerManager::isSegFading(uint8_t r) {
  bool fading = false;
  forOwnedSegments(r, [&fading](Segment &seg) { fading |= seg.isInTransition(); });
  return fading;
}

/**
 * force any running transition on the relay's segment(s) to complete (duration 0);
 * it is destroyed on the next serviced frame
 */
void PowerManager::cancelSegTransitions(uint8_t r) {
  forOwnedSegments(r, [](Segment &seg) { seg.startTransition(0); });
}

// restart a segment's fade from black: with `on` temporarily false, startTransition()
// re-captures a running fade-up's "from" brightness as ~0 and restarts its timer, or
// starts a fresh transition from 0 if there is none
static void fadeFromBlack(Segment &seg) {
  seg.on = false;
  seg.startTransition(strip.getTransition(), blendingStyle != TRANSITION_FADE);
  seg.on = true;
}

/**
 * restart the fade-up of the relay's segment(s) from black. WLED starts the fade the moment
 * a segment is switched on, so by the time delay-on-s and the blackout have passed it may
 * have partly or fully elapsed and the LEDs would snap on - restarting it here makes the
 * full fade visible once power is ready.
 */
void PowerManager::restartSegmentFade(uint8_t r) {
  bool restarted = false;
  forOwnedSegments(r, [&restarted](Segment &seg) {
    if (seg.on) { fadeFromBlack(seg); restarted = true; }
  });
  if (!restarted) return;
  if (strip.getTransition()) {
    // handleTransitions() force-ends ALL segment transitions (setTransitionMode(false)) when the
    // global transition window - started at the original on-click - expires. Re-open it so it
    // outlives the restarted fade, otherwise the fade's tail gets chopped (visible brightness
    // pop). If a global brightness ramp is in flight (main power on), re-anchor it at its current
    // level first: resetting the timer alone rewinds briT and causes a visible dip. This is the
    // same re-anchoring stateUpdated() does for changes arriving mid-transition.
    briOld = briT;
    transitionStartTime = millis();
    transitionActive = true;
  }
  strip.trigger(); // rendering may have gone idle during a long hold (PSU stabilization)
}

/**
 * is an "any segment" master relay (PSU trigger) either still off (its own power-on pending)
 * or within the stabilization window? Dependent segment-coupled relays postpone their power-on
 * until this clears, giving the power supply time to come up and settle before sections draw
 * load. Only applies when a stabilization time is configured.
 */
bool PowerManager::masterHoldsOn() {
  if (stabilizeSec == 0) return false;
  unsigned long now = millis();
  for (int m=0; m<POWERMANAGER_MAX_RELAYS; m++) {
    if (_relay[m].pin < 0 || _relay[m].segment != POWERMANAGER_SEG_ANY) continue;
    if (!_relay[m].state) return true; // master not on yet (its own power-on sequence is pending)
    if (now - _onAt[m] < (uint32_t)stabilizeSec * 1000) return true; // still stabilizing
  }
  return false;
}

/**
 * is any specific-segment coupled relay still switched on? An "any segment" master must be
 * the last to cut power: the sections it feeds may have longer off-delays than the master.
 */
bool PowerManager::dependentsStillOn() {
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (_relay[i].pin >= 0 && isCoupled(i) && _relay[i].segment != POWERMANAGER_SEG_ANY && _relay[i].state) return true;
  }
  return false;
}

/**
 * enter BO_FADE: force-complete stale transitions now, restart the fade one frame later
 */
void PowerManager::beginFadePhase(uint8_t r) {
  _relay[r].boPhase = BO_FADE;
  _boStart[r] = millis();
  cancelSegTransitions(r);
  strip.trigger(); // the next serviced frame destroys the cancelled transition(s)
}

/**
 * 10x/s tick: drive coupled relays towards their segment's on/off state (see section comment)
 */
void PowerManager::handleSegmentCoupling() {
  unsigned long now = millis();

  // main power sync, on-half: we switched main power off when the master cut - a segment
  // being switched on brings it back (turning main power on manually just clears the flag)
  if (_autoMainOff) {
    if (bri > 0) _autoMainOff = false;
    else {
      bool anyOn = false;
      for (unsigned s = 0; s < strip.getSegmentsNum() && !anyOn; s++) {
        const Segment &seg = strip.getSegment(s);
        anyOn = seg.isActive() && seg.on;
      }
      if (anyOn) {
        _autoMainOff = false;
        toggleOnOff(); // on (restores briLast); the master then powers up through the normal sequence
        stateUpdated(CALL_MODE_DIRECT_CHANGE);
      }
    }
  }

  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (_relay[i].pin < 0 || !isCoupled(i)) continue;

    bool segOn = false;
    if (_relay[i].segment == POWERMANAGER_SEG_ANY) {
      forOwnedSegments(i, [&segOn](Segment &seg) { segOn |= seg.on; });
    } else if ((uint8_t)_relay[i].segment < strip.getSegmentsNum() && strip.getSegment(_relay[i].segment).isActive()) {
      _relay[i].segSeen = true;
      segOn = strip.getSegment(_relay[i].segment).on;
    } else if (_relay[i].segSeen) {
      // linked segment was deleted at runtime: cut power and drop the link so a new segment
      // reusing this id does not inherit it. segSeen guards against boot races: segments may
      // not exist yet during the first loop passes, links must survive that.
      if (_relay[i].state) switchRelay(i, false);
      setSegmentLink(i, -1);
      continue;
    }

    bool target = segOn && !offMode;
    if (target == _relay[i].state) {
      _pendingSince[i] = 0; // in sync; also cancels a pending delayed switch
      if (!target && _relay[i].boPhase == BO_PRE) _relay[i].boPhase = BO_NONE; // segment went off again before power-on
      continue;
    }

    if (target) { // segment on, relay still off
      if (_relay[i].boPhase != BO_NONE) continue;                    // power-on sequence already running (handleBlackout())
      // PSU first: while a master relay is still powering up or stabilizing, keep the delay-on
      // timer unarmed so delay-on-s starts counting only once the PSU is ready (sequential).
      // With the master already up and past its window, only delay-on-s applies.
      if (_relay[i].segment != POWERMANAGER_SEG_ANY && masterHoldsOn()) { _pendingSince[i] = 0; continue; }
      if (_pendingSince[i] == 0) _pendingSince[i] = now ? now : 1;   // 0 means "nothing pending"
      if (now - _pendingSince[i] < (uint32_t)_relay[i].delayOn * 1000) continue;
      if (now - _offAt[i] < minOffMs) continue; // let the LEDs discharge after a cut (prevents white flash)
      _pendingSince[i] = 0;
      if (boPreMs || boPostMs) {
        _relay[i].boPhase = BO_PRE; // handleOverlayDraw() paints the segment black from here on
        _boStart[i] = now;
        strip.trigger();            // make sure frames are being rendered/sent
      } else {                      // blackout disabled: switch instantly
        switchRelay(i, true);
        if (_relay[i].delayOn) beginFadePhase(i); // the fade elapsed while waiting - restart it
      }
    } else {      // segment off, relay still on
      if (_pendingSince[i] == 0) _pendingSince[i] = now ? now : 1;
      // master relay: hold while any dependent section relay is still on - the PSU must be the
      // last to cut. Its delay-off then counts from the moment the last section switched off.
      if (_relay[i].segment == POWERMANAGER_SEG_ANY && dependentsStillOn()) {
        _pendingSince[i] = now ? now : 1;
        continue;
      }
      unsigned long elapsed = now - _pendingSince[i];
      // let a running fade-out finish before cutting power (bounded in case transition state lingers)
      if (isSegFading(i) && elapsed < strip.getTransition() + POWERMANAGER_FADE_GRACE_MS) continue;
      if (elapsed >= (uint32_t)_relay[i].delayOff * 1000) {
        _pendingSince[i] = 0;
        _relay[i].boPhase = BO_NONE; // abandon a power-on sequence still in a late phase
        switchRelay(i, false);
        // main power sync, off-half: the master just cut because every segment was switched
        // off (bri > 0 rules out a main power press as the cause) - reflect it on main power
        if (_relay[i].segment == POWERMANAGER_SEG_ANY && masterMainSync && bri > 0) {
          toggleOnOff(); // off (preserves briLast)
          stateUpdated(CALL_MODE_DIRECT_CHANGE);
          _autoMainOff = true;
        }
      }
    }
  }
}

/**
 * power-on blackout state machine (see section comment). Called every loop pass, unlike the
 * 100ms coupling tick, so the phase timing is ms-accurate.
 */
void PowerManager::handleBlackout() {
  unsigned long now = millis();
  // evaluate BO_FADE readiness for ALL relays before any fade is restarted: a restart creates a
  // fresh transition on the segment, which a later relay in this loop (sharing the segment, or
  // an "any segment" master) would mistake for the stale transition it is waiting on - stalling
  // against it until the safety timeout and staggering the fades instead of starting them in
  // parallel. A duplicate restart of a same-pass fresh transition is a no-op.
  uint16_t fadeReady = 0; // bitmask, POWERMANAGER_MAX_RELAYS is capped at 16
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (_relay[i].boPhase == BO_FADE && (!isSegFading(i) || now - _boStart[i] > BO_FADE_TIMEOUT_MS))
      fadeReady |= (uint16_t)1 << i;
  }
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    switch (_relay[i].boPhase) {
      case BO_PRE: // black frames streaming, port still off
        if (now - _boStart[i] >= boPreMs) {
          switchRelay(i, true); // LEDs power up while the incoming data is black
          _relay[i].boPhase = BO_POST;
          _boStart[i] = now;
        }
        break;
      case BO_POST: { // port on, stay black over the power ramp
        uint32_t hold = boPostMs;
        // master (any-segment) relay: extend the all-black hold by the PSU stabilization time
        if (_relay[i].segment == POWERMANAGER_SEG_ANY) hold += (uint32_t)stabilizeSec * 1000;
        if (now - _boStart[i] >= hold) beginFadePhase(i);
        break;
      }
      case BO_FADE: // cancelled stale transition is being destroyed, still black
        if ((fadeReady >> i) & 1) {
          _relay[i].boPhase = BO_NONE;
          restartSegmentFade(i); // the LEDs have power now - fade up from black
        }
        break;
    }
  }
}

/**
 * paint the segments of relays in a power-on blackout window black (called for every frame,
 * after effects are rendered and just before strip.show())
 */
void PowerManager::handleOverlayDraw() {
  if (!enabled) return;
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (_relay[i].boPhase == BO_NONE || _relay[i].pin < 0) continue;
    if (_relay[i].segment == POWERMANAGER_SEG_ANY) { // master relay: black out the whole strip
      unsigned len = strip.getLengthTotal();
      for (unsigned p = 0; p < len; p++) strip.setPixelColor(p, 0);
      continue;
    }
    if ((uint8_t)_relay[i].segment >= strip.getSegmentsNum()) continue;
    const Segment &seg = strip.getSegment(_relay[i].segment);
    if (!seg.isActive()) continue;
#ifndef WLED_DISABLE_2D
    if (strip.isMatrix && seg.is2D()) {
      for (int y = seg.startY; y < seg.stopY; y++)
        for (int x = seg.start; x < seg.stop; x++) strip.setPixelColorXY(x, y, 0);
      continue;
    }
#endif
    for (unsigned p = seg.start; p < seg.stop; p++) strip.setPixelColor(p, 0);
  }
}

#ifdef WLED_ENABLE_UM_UI_INJECT
/**
 * Segment-card "Power relays" menu, injected into the main UI via the /um.js mechanism
 * (concept by @blazoncek). This runs as the body of umInject(s) after every state render,
 * so the menu survives segment card re-renders; `s` is the freshly applied state object,
 * carrying our relay list from addToJsonState(). Menu open/close state persists in
 * window.pmOpen across renders. When the usermod is disabled, s.PowerManager is absent
 * and any previously injected menus are removed. On WLED bases without the injection
 * mechanism this block compiles out and relays are linked on the settings page instead.
 */
void PowerManager::addUIInjectCode(Print &dest) {
  dest.print(F(
    "let pmE=t=>d.createElement(t);" // cE() only exists on the settings pages, not in the main UI
    "let pmR=[];"
    "if(s&&s.PowerManager)pmR=(Array.isArray(s.PowerManager.relays)?s.PowerManager.relays:[s.PowerManager])"
      ".filter(r=>r.seg!==undefined&&r.seg!=99);"
    "window.pmOpen=window.pmOpen||{};"
    "d.querySelectorAll('#segcont .segin').forEach(sc=>{"
      "let old=sc.querySelector('.pmrow');if(old)old.remove();"
      "old=sc.querySelector('.pmlist');if(old)old.remove();"
      "if(!pmR.length)return;"
      "let i=parseInt(sc.id.replace('seg',''));if(isNaN(i))return;"
  ));
  dest.print(F(
      "let lnk=pmR.filter(r=>r.seg==i).map(r=>r.name||('Relay '+r.relay)).join(', ');"
      "let hd=pmE('div');hd.className='check revchkl pmrow';hd.style.cursor='pointer';"
      "hd.title='Link power relays: their output is cut when this segment is off';"
      "hd.textContent='Power relays: '+(lnk||'none');"
      "let ic=pmE('i');ic.className='icons e-icon'+(pmOpen[i]?' exp':'');"
      "ic.style.cssText='position:absolute;left:0;top:3px;transition:transform .3s;';"
      "ic.innerHTML='&#xe395;';hd.appendChild(ic);"
      "let ls=pmE('div');ls.className='pmlist'+(pmOpen[i]?'':' hide');ls.style.marginLeft='16px';"
      "hd.onclick=()=>{pmOpen[i]=!pmOpen[i];ls.classList.toggle('hide',!pmOpen[i]);ic.classList.toggle('exp',pmOpen[i]);};"
  ));
  dest.print(F(
      "pmR.forEach(r=>{"
        "let lb=pmE('label');lb.className='check revchkl';"
        "lb.append(r.name||('Relay '+r.relay));"
        "let sp;"
        "if(r.seg==i){sp=pmE('span');sp.style.cssText='color:var(--c-g);font-size:smaller;';"
          "sp.textContent=' (this segment)';lb.appendChild(sp);}"
        "else if(r.seg>=0){let os=(s.seg||[]).find(q=>q.id==r.seg);sp=pmE('span');"
          "sp.style.cssText='color:var(--c-d);font-size:smaller;';"
          "sp.textContent=' ('+(os&&os.n?os.n:'Segment '+r.seg)+')';lb.appendChild(sp);}"
        "let cb=pmE('input');cb.type='checkbox';cb.checked=(r.seg==i);"
        "cb.onchange=()=>{requestJson({PowerManager:{relay:r.relay,seg:cb.checked?i:-1}});};"
        "let cm=pmE('span');cm.className='checkmark';"
        "lb.appendChild(cb);lb.appendChild(cm);ls.appendChild(lb);"
      "});"
      "let dl=sc.querySelector('div.del');"
      "if(dl){sc.insertBefore(hd,dl);sc.insertBefore(ls,dl);}else{sc.appendChild(hd);sc.appendChild(ls);}"
    "});"
  ));
}
#endif // WLED_ENABLE_UM_UI_INJECT

/**
 * handleButton() can be used to override default button behaviour. Returning true
 * will prevent button working in a default way.
 * Replicating button.cpp
 */
bool PowerManager::handleButton(uint8_t b) {
  yield();
  if (!enabled
    || buttons[b].type == BTN_TYPE_NONE
    || buttons[b].type == BTN_TYPE_RESERVED
    || buttons[b].type == BTN_TYPE_PIR_SENSOR
    || buttons[b].type == BTN_TYPE_ANALOG
    || buttons[b].type == BTN_TYPE_ANALOG_INVERTED) {
    return false;
  }

  bool handled = false;
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (_relay[i].button == b && _relay[i].external) {
      handled = true;
    }
  }
  if (!handled) return false;

  unsigned long now = millis();

  //button is not momentary, but switch. This is only suitable on pins whose on-boot state does not matter (NOT gpio0)
  if (buttons[b].type == BTN_TYPE_SWITCH) {
    //handleSwitch(b);
    if (buttons[b].pressedBefore != isButtonPressed(b)) {
      buttons[b].pressedTime = now;
      buttons[b].pressedBefore = !buttons[b].pressedBefore;
    }

    if (buttons[b].longPressed == buttons[b].pressedBefore) return handled;
      
    if (now - buttons[b].pressedTime > WLED_DEBOUNCE_THRESHOLD) { //fire edge event only after 50ms without change (debounce)
      for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
        if (_relay[i].button == b && !isCoupled(i)) {
          switchRelay(i, buttons[b].pressedBefore);
          buttons[b].longPressed = buttons[b].pressedBefore; //save the last "long term" switch state
        }
      }
    }
    return handled;
  }

  //momentary button logic
  if (isButtonPressed(b)) { //pressed

    if (!buttons[b].pressedBefore) buttons[b].pressedTime = now;
    buttons[b].pressedBefore = true;

    if (now - buttons[b].pressedTime > 600) { //long press
      //longPressAction(b); //not exposed
      //handled = false; //use if you want to pass to default behaviour
      buttons[b].longPressed = true;
    }

  } else if (!isButtonPressed(b) && buttons[b].pressedBefore) { //released

    long dur = now - buttons[b].pressedTime;
    if (dur < WLED_DEBOUNCE_THRESHOLD) {
      buttons[b].pressedBefore = false;
      return handled;
    } //too short "press", debounce
    bool doublePress = buttons[b].waitTime; //did we have short press before?
    buttons[b].waitTime = 0;

    if (!buttons[b].longPressed) { //short press
      // if this is second release within 350ms it is a double press (buttonWaitTime!=0)
      if (doublePress) {
        //doublePressAction(b); //not exposed
        //handled = false; //use if you want to pass to default behaviour
      } else  {
        buttons[b].waitTime = now;
      }
    }
    buttons[b].pressedBefore = false;
    buttons[b].longPressed = false;
  }
  // if 350ms elapsed since last press/release it is a short press
  if (buttons[b].waitTime && now - buttons[b].waitTime > 350 && !buttons[b].pressedBefore) {
    buttons[b].waitTime = 0;
    //shortPressAction(b); //not exposed
    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
      if (_relay[i].button == b && !isCoupled(i)) {
        toggleRelay(i);
      }
    }
  }
  return handled;
}

/**
 * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
 */
void PowerManager::addToJsonInfo(JsonObject &root) {
  if (enabled) {
    JsonObject user = root["u"];
    if (user.isNull())
      user = root.createNestedObject("u");

    // visual separator above the PowerManager block (info keys/values render as raw HTML;
    // the hr is stretched from the key cell across the empty value cell)
    user.createNestedArray(F("<hr style=\"width:200%\">")).add(F(""));

    JsonArray infoArr = user.createNestedArray(FPSTR(_name)); //name
    infoArr.add(String(getActiveRelayCount()));
    infoArr.add(F(" relays"));

    if (expanderType == EXPANDER_AW9523) {
      JsonArray awArr = user.createNestedArray(F("AW9523"));
      awArr.add(awFound ? F("found @") : F("not found @"));
      awArr.add(String(F(" 0x")) + String(expanderAddr, HEX));
    }

    String uiDomString;
    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
      if (_relay[i].pin<0 || !_relay[i].external) continue;
      uiDomString = F("Relay "); uiDomString += i;
      if (_relay[i].name[0]) { uiDomString += F(" ("); uiDomString += _relay[i].name; uiDomString += ')'; }
      infoArr = user.createNestedArray(uiDomString); // timer value

      uiDomString = F("<button class=\"btn btn-xs\" onclick=\"requestJson({");
      uiDomString += FPSTR(_name);
      uiDomString += F(":{");
      uiDomString += FPSTR(_relay_str);
      uiDomString += F(":");
      uiDomString += i;
      uiDomString += F(",on:");
      uiDomString += _relay[i].state ? "false" : "true";
      uiDomString += F("}});\">");
      uiDomString += F("<i class=\"icons ");
      uiDomString += _relay[i].state ? "on" : "off";
      uiDomString += F("\">&#xe08f;</i></button>");
      infoArr.add(uiDomString);
    }

    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
      if (_relay[i].pin<0 || !isCoupled(i)) continue;
      uiDomString = F("Relay "); uiDomString += i;
      if (_relay[i].name[0]) { uiDomString += F(" ("); uiDomString += _relay[i].name; uiDomString += ')'; }
      infoArr = user.createNestedArray(uiDomString);
      if (_relay[i].segment == POWERMANAGER_SEG_ANY) uiDomString = F("follows any segment");
      else {
        uiDomString = F("follows ");
        uint8_t sid = _relay[i].segment;
        // show the segment's custom name when it has one
        if (sid < strip.getSegmentsNum() && strip.getSegment(sid).name && strip.getSegment(sid).name[0]) {
          uiDomString += strip.getSegment(sid).name;
        } else {
          uiDomString += F("segment "); uiDomString += sid;
        }
      }
      uiDomString += _relay[i].state ? F(" (on)") : F(" (off)");
      infoArr.add(uiDomString);
    }
  }
}

/**
 * addToJsonState() can be used to add custom entries to the /json/state part of the JSON API (state object).
 * Values in the state object may be modified by connected clients
 */
void PowerManager::addToJsonState(JsonObject &root) {
  if (!initDone || !enabled) return;  // prevent crash on boot applyPreset()
  JsonObject multiRelay = root[FPSTR(_name)];
  if (multiRelay.isNull()) {
    multiRelay = root.createNestedObject(FPSTR(_name));
  }
  #if POWERMANAGER_MAX_RELAYS > 1
  JsonArray rel_arr = multiRelay.createNestedArray(F("relays"));
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    if (_relay[i].pin < 0) continue;
    JsonObject relay = rel_arr.createNestedObject();
    relay[FPSTR(_relay_str)] = i;
    relay["state"] = _relay[i].state;
    relay["seg"]   = _relay[i].segment;
    if (_relay[i].name[0]) relay["name"] = (const char*)_relay[i].name;
  }
  #else
  multiRelay[FPSTR(_relay_str)] = 0;
  multiRelay["state"] = _relay[0].state;
  multiRelay["seg"]   = _relay[0].segment;
  if (_relay[0].name[0]) multiRelay["name"] = (const char*)_relay[0].name;
  #endif
}

/**
 * readFromJsonState() can be used to receive data clients send to the /json/state part of the JSON API (state object).
 * Values in the state object may be modified by connected clients
 */
void PowerManager::readFromJsonState(JsonObject &root) {
  if (!initDone || !enabled) return;  // prevent crash on boot applyPreset()
  JsonObject usermod = root[FPSTR(_name)];
  if (!usermod.isNull()) {
    if (usermod[FPSTR(_relay_str)].is<int>() && usermod[FPSTR(_relay_str)].as<int>()>=0) {
      uint8_t rly = usermod[FPSTR(_relay_str)].as<int>();
      if (rly < POWERMANAGER_MAX_RELAYS) {
        if (usermod["seg"].is<int>()) setSegmentLink(rly, usermod["seg"].as<int>(), usermod["save"] | true);
        if (usermod["on"].is<bool>()) {
          if (!isCoupled(rly)) switchRelay(rly, usermod["on"].as<bool>());
        } else if (usermod["on"].is<const char*>() && usermod["on"].as<const char*>()[0] == 't') {
          if (!isCoupled(rly)) toggleRelay(rly);
        }
      }
    }
  } else if (root[FPSTR(_name)].is<JsonArray>()) {
    JsonArray relays = root[FPSTR(_name)].as<JsonArray>();
    for (JsonVariant r : relays) {
      if (r[FPSTR(_relay_str)].is<int>() && r[FPSTR(_relay_str)].as<int>()>=0) {
        uint8_t rly = r[FPSTR(_relay_str)].as<int>();
        if (rly >= POWERMANAGER_MAX_RELAYS) continue;
        if (r["seg"].is<int>()) setSegmentLink(rly, r["seg"].as<int>(), r["save"] | true);
        if (r["on"].is<bool>()) {
          if (!isCoupled(rly)) switchRelay(rly, r["on"].as<bool>());
        } else if (r["on"].is<const char*>() && r["on"].as<const char*>()[0] == 't') {
          if (!isCoupled(rly)) toggleRelay(rly);
        }
      }
    }
  }
}

/**
 * provide the changeable values
 */
void PowerManager::addToConfig(JsonObject &root) {
  JsonObject top = root.createNestedObject(FPSTR(_name));

  top[FPSTR(_enabled)] = enabled;
  top[FPSTR(_takeOver)] = takeOverRelays;
  top[FPSTR(_expander)] = expanderType;
  top[FPSTR(_expanderAddr)] = expanderAddr;
  top[FPSTR(_pushPull)] = awP0PushPull;
  top[FPSTR(_broadcast)] = periodicBroadcastSec;
  top[FPSTR(_HAautodiscovery)] = HAautodiscovery;
  top[FPSTR(_blackPre)]  = boPreMs;
  top[FPSTR(_blackPost)] = boPostMs;
  top[FPSTR(_stabilize_str)] = stabilizeSec;
  top[FPSTR(_minOff)] = minOffMs;
  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    String parName = FPSTR(_relay_str); parName += '-'; parName += i;
    JsonObject relay = top.createNestedObject(parName);
    if (i == 0) { // relay 0 = Master AC relay slot
      relay[FPSTR(_master)]   = masterEnabled;
      relay[FPSTR(_mainSync)] = masterMainSync;
    }
    relay["name"]             = (const char*)_relay[i].name;
    relay["pin"]              = _relay[i].pin;
    relay[FPSTR(_activeHigh)] = _relay[i].invert;
    relay[FPSTR(_delayOn_str)]  = _relay[i].delayOn;
    relay[FPSTR(_delayOff_str)] = _relay[i].delayOff;
    relay[FPSTR(_external)]   = _relay[i].external;
    relay[FPSTR(_button)]     = _relay[i].button;
    relay[FPSTR(_segment_str)] = _relay[i].segment;
  }
  DEBUG_PRINTLN(F("PowerManager config saved."));
}

void PowerManager::appendConfigData() {
  oappend(F("dd=addDropdown('PowerManager','expander');"));
  oappend(F("addOption(dd,'None',0);"));
  oappend(F("addOption(dd,'PCF8574',1);"));
  oappend(F("addOption(dd,'AW9523',2);"));
  oappend(F("addInfo('PowerManager:broadcast-sec',1,'(MQTT message)');"));
  oappend(F("mrT=d.getElementsByName('PowerManager:take-over-relays');if(mrT.length>1){"
      "if(mrT[0].previousSibling)mrT[0].previousSibling.nodeValue=' Take over all relays ';"
      "mrT[1].insertAdjacentHTML('afterend',\"<div style='font-size:smaller;'>unconfigured relays (no segment link, not external) stay off instead of following main power</div>\");}"));
  // anti-flash blackout explanation on its own line below the second field
  oappend(F("mrB=d.getElementsByName('PowerManager:black-post-ms');if(mrB.length>1)mrB[1].insertAdjacentHTML('afterend',\"<div style='font-size:smaller;'>anti-flash: black frames sent to a coupled segment before (pre) and after (post) its power port switches on; both 0 = disabled</div>\");"));
  // segment coupling explanation on its own line below the first relay's segment field (avoids wrapping)
  oappend(F("mrQ=d.getElementsByName('PowerManager:relay-0:segment');if(mrQ.length>1)mrQ[1].insertAdjacentHTML('afterend',\"<div id='mrQI' style='font-size:smaller;'>-1 = off &middot; 0-31 = follow that segment</div>\");"));
  // PSU stabilization explanation below its (global) field
  oappend(F("mrY=d.getElementsByName('PowerManager:stabilize-s');if(mrY.length>1)mrY[1].insertAdjacentHTML('afterend',\"<div style='font-size:smaller;'>PSU stabilization: after the Master AC relay powers on, the other relays wait this long; a relay's own Delay On S is added on top (staggered power-on)</div>\");"));
  oappend(F("mrO=d.getElementsByName('PowerManager:min-off-ms');if(mrO.length>1)mrO[1].insertAdjacentHTML('afterend',\"<div style='font-size:smaller;'>minimum port off-time before re-energising, lets LEDs discharge (prevents white flash on rapid toggling)</div>\");"));
  // per relay row: grey out External for coupled relays (exclusively segment-driven).
  // Relay 0 is the dedicated "Master AC relay" slot: its Master checkbox renames the heading,
  // hides the segment row (driven to 99/-1 behind the scenes) and badges it as the PSU master.
  oappend(F("for(let r=0;;r++){"
      "let sg=d.getElementsByName('PowerManager:relay-'+r+':segment'),ex=d.getElementsByName('PowerManager:relay-'+r+':external');"
      "if(sg.length<2||ex.length<2)break;"
      "let fx=()=>{let c=parseInt(sg[1].value)>=0;ex[1].disabled=c;if(c)ex[1].checked=false;};"
      "sg[1].addEventListener('input',fx);"
      // relabel truthfully ('active-high' stores the invert flag, 'external' = externally controlled)
      // and join both checkboxes on one line with a small divider
      "let a=mrW('relay-'+r+':active-high'),e2=mrW('relay-'+r+':external');"
      "if(a)a.firstChild.nodeValue=' Active Low ';"
      "if(e2)e2.firstChild.nodeValue=' External control ';"
      "if(a&&e2){if(a.lastChild&&a.lastChild.nodeName=='BR')a.removeChild(a.lastChild);"
      "let dl=cE('span');dl.textContent='|';dl.style.cssText='opacity:.5;margin:0 8px;';"
      "a.after(dl);dl.after(e2);}"
      "if(r==0){"
        "let mk=d.getElementsByName('PowerManager:relay-0:master'),w=mrW('relay-0:segment'),q=gId('mrQI'),hu;"
        "d.querySelectorAll('#um u').forEach(u=>{if(u.textContent=='Relay 0')hu=u;});"
        "if(mk.length>1){"
          "let mw=mrW('relay-0:master');if(mw)mw.firstChild.nodeValue=' Enabled ';"
          "let ws=mrW('relay-0:main-sync');"
          "if(ws){ws.firstChild.nodeValue=' Sync main power ';"
            "let dv=cE('div');dv.style.fontSize='smaller';"
            "dv.innerHTML='all segments off &rarr; main power off; a segment turns it back on';"
            "ws.insertBefore(dv,ws.lastChild);}"
          "let fm=()=>{let on=mk[1].checked;"
            "if(hu){hu.textContent=on?'Master AC relay':'Relay 0';hu.classList.toggle('warn',on);}"
            "if(w)w.style.display=on?'none':'';if(q)q.style.display=on?'none':'';"
            "if(ws)ws.style.display=on?'':'none';"
            "if(on)sg[1].value=99;else if(parseInt(sg[1].value)==99)sg[1].value=-1;"
            "fx();};"
          "mk[1].addEventListener('change',fm);fm();"
          // convenience default when enabling the master by hand: 5s PSU anti-cycling off-delay
          "mk[1].addEventListener('change',()=>{if(mk[1].checked){let df=d.getElementsByName('PowerManager:relay-0:delay-off-s');if(df.length>1&&parseInt(df[1].value)==0)df[1].value=5;}});"
        "}"
      "}else{sg[1].min=-1;sg[1].max=63;}"
      "fx();}"));
  // wrap a field row (label text node + hidden & visible input + line break) in a span so it can be shown/hidden
  oappend(F("function mrW(n){let e=d.getElementsByName('PowerManager:'+n);if(e.length<2)return null;let s=cE('span'),l=e[0].previousSibling,r=e[1].nextSibling;e[0].parentNode.insertBefore(s,l);s.append(l,e[0],e[1]);if(r&&r.nodeName=='BR')s.append(r);return s;}"));
  oappend(F("mrA=mrW('expander-address');mrP=mrW('AW9523-pushpull');"));
  // section dividers + bold headers (the usermod's own h3 title stays the biggest)
  oappend(F("mrH=(el,t)=>{if(el)el.insertAdjacentHTML('beforebegin','<hr class=\"sml\"><p style=\"font-weight:bold;font-size:105%;margin:8px 0 4px;\">'+t+'</p>');};"));
  oappend(F("mrH(mrW('expander'),'Expander');"));
  oappend(F("mrH(mrW('broadcast-sec'),'Broadcast');"));
  oappend(F("mrH(mrW('black-pre-ms'),'Power sequencing');"));
  oappend(F("if(mrP){mrP.firstChild.nodeValue=' AW9523 Push-Pull ';mrP.insertBefore(Object.assign(cE('div'),{innerHTML:'P0_x drive mode, off = open-drain'}),mrP.lastChild).style.fontSize='smaller';}"));
  oappend(F("mrI=cE('div');mrI.style.fontSize='smaller';if(mrA)mrA.insertBefore(mrI,mrA.lastChild);"));
  // hide address & push-pull rows unless the matching expander is selected; show that expander's valid addresses
  oappend(F("mrS=d.getElementsByName('PowerManager:expander')[1];"));
  oappend(F("function mrU(){let v=mrS.value;if(mrA)mrA.style.display=v==0?'none':'';if(mrP)mrP.style.display=v==2?'':'none';mrI.innerHTML=v==2?'decimal: 88-91 (0x58-0x5B), default 88':'decimal: 32-39 (0x20-0x27), PCF8574A 56-63 (0x38-0x3F), default 32';}"));
  oappend(F("if(mrS){mrS.addEventListener('change',mrU);mrU();}"));
  // named pin dropdown entries for expander ports; only offered when an expander is configured
  // (change of expander type requires save & reload of this page)
  if (expanderType == EXPANDER_AW9523) {
    oappend(F("d.extra.push({'PowerManager':{pin:[['P0_0',100],['P0_1',101],['P0_2',102],['P0_3',103],['P0_4',104],['P0_5',105],['P0_6',106],['P0_7',107],['P1_0',108],['P1_1',109],['P1_2',110],['P1_3',111],['P1_4',112],['P1_5',113],['P1_6',114],['P1_7',115]]}});"));
  } else if (expanderType == EXPANDER_PCF8574) {
    oappend(F("d.extra.push({'PowerManager':{pin:[['P0',100],['P1',101],['P2',102],['P3',103],['P4',104],['P5',105],['P6',106],['P7',107]]}});"));
  }
}

/**
 * restore the changeable values
 * readFromConfig() is called before setup() to populate properties from values stored in cfg.json
 * 
 * The function should return true if configuration was successfully loaded or false if there was no configuration.
 */
bool PowerManager::readFromConfig(JsonObject &root) {
  int8_t oldPin[POWERMANAGER_MAX_RELAYS];

  JsonObject top = root[FPSTR(_name)];
  if (top.isNull()) {
    // migrate settings saved by the built-in multi_relay usermod this one grew out of
    // (all keys are compatible; the config is re-saved under the new section name)
    top = root[FPSTR(_legacyName)];
    if (!top.isNull()) {
      DEBUG_PRINTLN(F("PowerManager: migrating MultiRelay config."));
      configNeedsWrite = true;
    }
  }
  if (top.isNull()) {
    DEBUG_PRINT(FPSTR(_name));
    DEBUG_PRINTLN(F(": No config found. (Using defaults.)"));
    return false;
  }

  //bool configComplete = !top.isNull();
  //configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
  enabled = top[FPSTR(_enabled)] | enabled;
  if (!top[FPSTR(_expander)].isNull()) {
    expanderType = top[FPSTR(_expander)] | expanderType;
    expanderAddr = top[FPSTR(_expanderAddr)] | expanderAddr;
  } else if (!top[FPSTR(_pcf8574)].isNull()) {
    // migrate legacy (pre-AW9523) config keys
    if (top[FPSTR(_pcf8574)] | false) expanderType = EXPANDER_PCF8574;
    expanderAddr = top[FPSTR(_pcfAddress)] | expanderAddr;
  }
  awP0PushPull = top[FPSTR(_pushPull)] | awP0PushPull;
  if (expanderType > EXPANDER_AW9523) expanderType = EXPANDER_NONE;
  // fix I2C address if it does not match the selected expander (e.g. after changing type)
  if (expanderType == EXPANDER_AW9523 && (expanderAddr < 0x58 || expanderAddr > 0x5B)) expanderAddr = AW9523_ADDRESS;
  if (expanderType == EXPANDER_PCF8574 && expanderAddr >= 0x58) expanderAddr = PCF8574_ADDRESS;
  // if I2C is not globally initialised just ignore
  if (i2c_sda<0 || i2c_scl<0) expanderType = EXPANDER_NONE;
  periodicBroadcastSec = top[FPSTR(_broadcast)] | periodicBroadcastSec;
  periodicBroadcastSec = min(900,max(0,(int)periodicBroadcastSec));
  HAautodiscovery = top[FPSTR(_HAautodiscovery)] | HAautodiscovery;
  takeOverRelays = top[FPSTR(_takeOver)] | takeOverRelays;
  boPreMs  = top[FPSTR(_blackPre)]  | boPreMs;
  boPostMs = top[FPSTR(_blackPost)] | boPostMs;
  boPreMs  = min(500,max(0,(int)boPreMs));
  boPostMs = min(500,max(0,(int)boPostMs));
  stabilizeSec = top[FPSTR(_stabilize_str)] | stabilizeSec;
  stabilizeSec = min(60,max(0,(int)stabilizeSec));
  minOffMs = top[FPSTR(_minOff)] | minOffMs;
  minOffMs = min(10000,max(0,(int)minOffMs));

  for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++) {
    String parName = FPSTR(_relay_str); parName += '-'; parName += i;
    oldPin[i]          = _relay[i].pin;
    _relay[i].pin      = top[parName]["pin"] | _relay[i].pin;
    _relay[i].invert   = top[parName][FPSTR(_activeHigh)] | _relay[i].invert;
    _relay[i].external = top[parName][FPSTR(_external)]   | _relay[i].external;
    if (!top[parName][FPSTR(_delay_str)].isNull()) { // migrate legacy single delay (applied to both directions)
      _relay[i].delayOn = _relay[i].delayOff = top[parName][FPSTR(_delay_str)].as<int>();
    }
    _relay[i].delayOn  = top[parName][FPSTR(_delayOn_str)]  | _relay[i].delayOn;
    _relay[i].delayOff = top[parName][FPSTR(_delayOff_str)] | _relay[i].delayOff;
    _relay[i].button   = top[parName][FPSTR(_button)]     | _relay[i].button;
    _relay[i].delayOn  = min(600,max(0,(int)_relay[i].delayOn));  // bounds checking max 10min
    _relay[i].delayOff = min(600,max(0,(int)_relay[i].delayOff));
    if (i == 0) {
      masterEnabled  = top[parName][FPSTR(_master)]   | masterEnabled;
      masterMainSync = top[parName][FPSTR(_mainSync)] | masterMainSync;
    }
    int seg = top[parName][FPSTR(_segment_str)] | (int)_relay[i].segment;
    if (seg < -1 || (seg >= (int)strip.getMaxSegments() && seg != POWERMANAGER_SEG_ANY)) seg = -1;
    if (i > 0 && seg == POWERMANAGER_SEG_ANY) seg = -1; // only the Master AC relay (relay 0) may follow all segments
    _relay[i].segment  = seg;
    if (top[parName]["name"].is<const char*>()) strlcpy(_relay[i].name, top[parName]["name"].as<const char*>(), POWERMANAGER_NAME_LEN);
  }
  // relay 0 is the dedicated Master AC relay slot
  if (masterEnabled) _relay[0].segment = POWERMANAGER_SEG_ANY;
  else if (_relay[0].segment == POWERMANAGER_SEG_ANY) _relay[0].segment = -1;

  DEBUG_PRINT(FPSTR(_name));
  if (!initDone) {
    // reading config prior to setup()
    DEBUG_PRINTLN(F(" config loaded."));
  } else {
    // deallocate all pins 1st
    for (int i=0; i<POWERMANAGER_MAX_RELAYS; i++)
      if (oldPin[i]>=0 && oldPin[i]<100) {
        PinManager::deallocatePin(oldPin[i], PinOwner::UM_MultiRelay);
      }
    // allocate new pins
    setup();
    DEBUG_PRINTLN(F(" config (re)loaded."));
  }
  // use "return !top["newestParameter"].isNull();" when updating Usermod with new features
  return !top[FPSTR(_takeOver)].isNull();
}

// strings to reduce flash memory usage (used more than twice)
const char PowerManager::_name[]            PROGMEM = "PowerManager";
const char PowerManager::_legacyName[]      PROGMEM = "MultiRelay";       // config section of the multi_relay ancestor (migration)
const char PowerManager::_enabled[]         PROGMEM = "enabled";
const char PowerManager::_relay_str[]       PROGMEM = "relay";
const char PowerManager::_delay_str[]       PROGMEM = "delay-s";
const char PowerManager::_activeHigh[]      PROGMEM = "active-high";
const char PowerManager::_external[]        PROGMEM = "external";
const char PowerManager::_button[]          PROGMEM = "button";
const char PowerManager::_broadcast[]       PROGMEM = "broadcast-sec";
const char PowerManager::_HAautodiscovery[] PROGMEM = "HA-autodiscovery";
const char PowerManager::_pcf8574[]         PROGMEM = "use-PCF8574";      // legacy config key (read for migration only)
const char PowerManager::_pcfAddress[]      PROGMEM = "PCF8574-address";  // legacy config key (read for migration only)
const char PowerManager::_expander[]        PROGMEM = "expander";
const char PowerManager::_expanderAddr[]    PROGMEM = "expander-address";
const char PowerManager::_pushPull[]        PROGMEM = "AW9523-pushpull";
const char PowerManager::_switch[]          PROGMEM = "switch";
const char PowerManager::_toggle[]          PROGMEM = "toggle";
const char PowerManager::_Command[]         PROGMEM = "/command";
const char PowerManager::_segment_str[]     PROGMEM = "segment";
const char PowerManager::_delayOn_str[]     PROGMEM = "delay-on-s";
const char PowerManager::_delayOff_str[]    PROGMEM = "delay-off-s";
const char PowerManager::_stabilize_str[]   PROGMEM = "stabilize-s";
const char PowerManager::_blackPre[]        PROGMEM = "black-pre-ms";
const char PowerManager::_blackPost[]       PROGMEM = "black-post-ms";
const char PowerManager::_minOff[]          PROGMEM = "min-off-ms";
const char PowerManager::_master[]          PROGMEM = "master";
const char PowerManager::_mainSync[]        PROGMEM = "main-sync";
const char PowerManager::_takeOver[]        PROGMEM = "take-over-relays";


static PowerManager powerManager;
REGISTER_USERMOD(powerManager);