/*
 * Usermod for detecting people entering/leaving a staircase and switching the
 * staircase on/off.
 *
 * Edit the Animated_Staircase_config.h file to compile this usermod for your
 * specific configuration.
 *
 * See the accompanying README.md file for more info.
 */
#include "wled.h"
#ifdef STAIRCASE_INCLUDE_LUX
#include <BH1750_v2.h>
#endif

class Animated_Staircase_Wipe : public Usermod
{
private:
  /* configuration (available in API and stored in flash) */
  bool enabled = false;              // Enable this usermod
  int8_t topPIRorTriggerPin = -1;    // disabled
  int8_t bottomPIRorTriggerPin = -1; // disabled
  int8_t topEchoPin = -1;            // disabled
  int8_t bottomEchoPin = -1;         // disabled
  bool useUSSensorTop = false;       // using PIR or UltraSound sensor?
  bool useUSSensorBottom = false;    // using PIR or UltraSound sensor?
  unsigned int topMaxDist = 50;      // default maximum measured distance in cm, top
  unsigned int bottomMaxDist = 50;   // default maximum measured distance in cm, bottom
  bool togglePower = false;          // toggle power on/off with staircase on/off
#ifdef STAIRCASE_INCLUDE_LUX
  Usermod_BH1750 *bh1750UserMod;
  float daylightLuxThreshold = 100.0; // threshold for daylight lux, above this the staircase will not turn on
#endif

  /* runtime variables */
  bool initDone = false;

  // state of the wipe
  // 0: inactive 1: wiping 2: solid
#define WIPE_STATE_INACTIVE 0
#define WIPE_STATE_WIPING 1
#define WIPE_STATE_SOLID 2
#define WIPE_STATE_WIPING_OFF 3
#define WIPE_STATE_COMPLETE 4
  int8_t wipeState = WIPE_STATE_INACTIVE;
  unsigned long timeStaticStart = 0;
  // User setting for the wipe state
#define WIPE_OFF 0
#define WIPE_UP 1
#define WIPE_DOWN 2
  uint16_t userWipeVar = WIPE_OFF;
  // How long the lights will stay on after the last sensor trigger, in seconds
  uint16_t userWipeOffTime = 20;
  uint16_t previousWipeVar = 0;
  uint8_t userEffectSpeed = 225;

  // Time between checking of the sensors
  const unsigned int scanDelay = 100;

  // Lights on or off.
  // Flipping this will start a transition.
  bool on = false;

  // Swipe direction for current transition
#define SWIPE_UP true
#define SWIPE_DOWN false
  bool swipe = SWIPE_UP;

  // Indicates which Sensor was seen last (to determine
  // the direction when swiping off)
#define LOWER false
#define UPPER true
  bool lastSensor = LOWER;

  // Time of the last transition action
  unsigned long lastTime = 0;

  // Time of the last sensor check
  unsigned long lastScanTime = 0;

  // Last time the lights were switched on or off
  unsigned long lastSwitchTime = 0;

  // These values are used by the API to read the
  // last sensor state, or trigger a sensor
  // through the API
  bool topSensorRead = false;
  bool topSensorWrite = false;
  bool bottomSensorRead = false;
  bool bottomSensorWrite = false;
  bool topSensorState = false;
  bool bottomSensorState = false;

  // strings to reduce flash memory usage (used more than twice)
  static const char _name[];
  static const char _enabled[];
  static const char _useTopUltrasoundSensor[];
  static const char _topPIRorTrigger_pin[];
  static const char _topEcho_pin[];
  static const char _useBottomUltrasoundSensor[];
  static const char _bottomPIRorTrigger_pin[];
  static const char _bottomEcho_pin[];
  static const char _topEchoCm[];
  static const char _bottomEchoCm[];
  static const char _togglePower[];
  static const char _onTime[];
#ifdef STAIRCASE_INCLUDE_LUX
  static const char _daylightLuxThreshold[];
#endif

  void publishMqtt(bool bottom, const char *state)
  {
#ifndef WLED_DISABLE_MQTT
    // Check if MQTT Connected, otherwise it will crash the 8266
    if (WLED_MQTT_CONNECTED)
    {
      char subuf[64];

      const char *topicSuffix = bottom ? "bot" : "top";
      sprintf_P(subuf, PSTR("%s/motion/%s"), mqttDeviceTopic, topicSuffix);
      mqtt->publish(subuf, 0, false, state);
    }
#endif
  }

  void publishTimeMqtt(bool bottom, const char *state)
  {
#ifndef WLED_DISABLE_MQTT
    // Check if MQTT Connected, otherwise it will crash the 8266
    if (WLED_MQTT_CONNECTED)
    {
      char subuf[64];

      const char *topicSuffix = bottom ? "bot" : "top";
      sprintf_P(subuf, PSTR("%s/motion_time/%s"), mqttDeviceTopic, topicSuffix);
      mqtt->publish(subuf, 0, false, state);
    }
#endif
  }

  /*
   * Detects if an object is within ultrasound range.
   * signalPin: The pin where the pulse is sent
   * echoPin:   The pin where the echo is received
   * maxTimeUs: Detection timeout in microseconds. If an echo is
   *            received within this time, an object is detected
   *            and the function will return true.
   *
   * The speed of sound is 343 meters per second at 20 degrees Celsius.
   * Since the sound has to travel back and forth, the detection
   * distance for the sensor in cm is (0.0343 * maxTimeUs) / 2.
   *
   * For practical reasons, here are some useful distances:
   *
   * Distance =	maxtime
   *     5 cm =  292 uS
   *    10 cm =  583 uS
   *    20 cm = 1166 uS
   *    30 cm = 1749 uS
   *    50 cm = 2915 uS
   *   100 cm = 5831 uS
   */
  bool ultrasoundRead(int8_t signalPin, int8_t echoPin, unsigned int maxTimeUs)
  {
    if (signalPin < 0 || echoPin < 0)
      return false;
    digitalWrite(signalPin, LOW);
    delayMicroseconds(2);
    digitalWrite(signalPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(signalPin, LOW);
    return pulseIn(echoPin, HIGH, maxTimeUs) > 0;
  }

  bool checkSensors()
  {
    bool sensorChanged = false;
    unsigned long millisCount = millis();

#ifdef STAIRCASE_INCLUDE_LUX
    // check lux
    if (bh1750UserMod != nullptr)
    {
      float currentLux = bh1750UserMod->getIlluminance();
      if (currentLux > daylightLuxThreshold)
      {
        // If its too light, reset the state vars and don't turn on the lights
        bottomSensorWrite = false;
        topSensorWrite = false;
        return false;
      }
    }
#endif

    if (
        (!on && (millisCount - lastScanTime) > scanDelay) || (on && (millisCount - lastScanTime) > scanDelay * 10) // check less often when on
    )
    {
      lastScanTime = millis();

      bottomSensorRead = bottomSensorWrite ||
                         (!useUSSensorBottom ? (bottomPIRorTriggerPin < 0 ? false : digitalRead(bottomPIRorTriggerPin)) : ultrasoundRead(bottomPIRorTriggerPin, bottomEchoPin, bottomMaxDist * 59) // cm to us
                         );
      topSensorRead = topSensorWrite ||
                      (!useUSSensorTop ? (topPIRorTriggerPin < 0 ? false : digitalRead(topPIRorTriggerPin)) : ultrasoundRead(topPIRorTriggerPin, topEchoPin, topMaxDist * 59) // cm to us
                      );

      if (bottomSensorRead != bottomSensorState)
      {
        bottomSensorState = bottomSensorRead; // change previous state
        sensorChanged = true;
        publishMqtt(true, bottomSensorState ? "on" : "off");
        if (bottomSensorState)
        {
          userWipeVar = WIPE_UP;
          lastSensor = LOWER;
        }
        DEBUG_PRINTLN(F("Bottom sensor changed."));
      }

      if (topSensorRead != topSensorState)
      {
        topSensorState = topSensorRead; // change previous state
        sensorChanged = true;
        publishMqtt(false, topSensorState ? "on" : "off");
        if (topSensorState)
        {
          userWipeVar = WIPE_DOWN;
          lastSensor = UPPER;
        }
        DEBUG_PRINTLN(F("Top sensor changed."));
      }

      // Values read, reset the flags for next API call
      topSensorWrite = false;
      bottomSensorWrite = false;

      if (topSensorRead != bottomSensorRead)
      {
        lastSwitchTime = millis();

        if (on)
        {
          // lastSensor = topSensorRead;
          timeStaticStart = millis();
          publishTimeMqtt(lastSensor, String(timeStaticStart).c_str());
        }
        else
        {
          on = true;
        }
      }
    }
    return sensorChanged;
  }

  // send sensor values to JSON API
  void writeSensorsToJson(JsonObject &staircase)
  {
    staircase[F("top-sensor")] = topSensorRead;
    staircase[F("bottom-sensor")] = bottomSensorRead;

#ifdef STAIRCASE_INCLUDE_LUX
    float lux;
    if (bh1750UserMod != nullptr)
    {
      lux = bh1750UserMod->getIlluminance();
    }
    staircase[F("lux")] = lux;
#endif
  }

  // allow overrides from JSON API
  void readSensorsFromJson(JsonObject &staircase)
  {
    bottomSensorWrite = bottomSensorState || (staircase[F("bottom-sensor")].as<bool>());
    topSensorWrite = topSensorState || (staircase[F("top-sensor")].as<bool>());
  }

  void enable(bool enable)
  {
    if (enable)
    {
      DEBUG_PRINTLN(F("Animated Staircase enabled."));

      if (!useUSSensorBottom)
        pinMode(bottomPIRorTriggerPin, INPUT_PULLDOWN);
      else
      {
        pinMode(bottomPIRorTriggerPin, OUTPUT);
        pinMode(bottomEchoPin, INPUT);
      }

      if (!useUSSensorTop)
        pinMode(topPIRorTriggerPin, INPUT_PULLDOWN);
      else
      {
        pinMode(topPIRorTriggerPin, OUTPUT);
        pinMode(topEchoPin, INPUT);
      }
    }
    enabled = enable;
  }

  void updateWipe()
  {
    // userVar0 (U0 in HTTP API):
    // has to be set to 1 if movement is detected on the PIR that is the same side of the staircase as the ESP8266
    // has to be set to 2 if movement is detected on the PIR that is the opposite side
    // can be set to 0 if no movement is detected. Otherwise LEDs will turn off after a configurable timeout (userVar1 seconds)
    // 1 = up
    // 2 = down
    // wipe state:
    // 0: inactive
    // 1: wipe on
    // 2: static/hold light on
    // 3: wipe off
    // 4: wiping completed, turn off

    if (userWipeVar > WIPE_OFF)
    {
      // If the current primary and secondary colors are black, dont bother with the wipe
      if (colPri[0] == 0 && colPri[1] == 0 && colPri[2] == 0 && colPri[3] == 0 && colSec[0] == 0 && colSec[1] == 0 && colSec[2] == 0 && colSec[3] == 0)
      {
        userWipeVar = WIPE_OFF;
        return;
      }

      previousWipeVar = userWipeVar;

      if (wipeState == WIPE_STATE_INACTIVE) // Start wipe
      {
        startWipe();
        wipeState = WIPE_STATE_WIPING;
      }
      else if (wipeState == WIPE_STATE_WIPING) // perform wipe on
      {
        uint32_t cycleTime = 360 + (255 - userEffectSpeed) * 75; // this is how long one wipe takes (minus 25 ms to make sure we switch in time)
        if (millis() + strip.timebase > (cycleTime - 25))
        { // wipe complete
          effectCurrent = FX_MODE_STATIC;
          timeStaticStart = millis();
          colorUpdated(CALL_MODE_NOTIFICATION);
          wipeState = WIPE_STATE_SOLID;
        }
      }
      else if (wipeState == WIPE_STATE_SOLID) // hold light on / static
      {
        if (userWipeOffTime > 0) // if userWipeOffTime is not set, the light will stay on until second PIR or external command is triggered
        {
          if (millis() - timeStaticStart > userWipeOffTime * 1000)
            wipeState = WIPE_STATE_WIPING_OFF;
        }
      }
      else if (wipeState == WIPE_STATE_WIPING_OFF) // switch to wipe off
      {
#ifdef STAIRCASE_WIPE_OFF
        effectCurrent = FX_MODE_COLOR_WIPE;
        strip.timebase = 360 + (255 - userEffectSpeed) * 75 - millis(); // make sure wipe starts fully lit
        colorUpdated(CALL_MODE_NOTIFICATION);
        wipeState = WIPE_STATE_COMPLETE;
#else
        turnOff();
#endif
      }
      else // wiping off
      {
        if (millis() + strip.timebase > (725 + (255 - userEffectSpeed) * 150))
          turnOff(); // wipe complete
      }
    }
    else // user is setting wipe off
    {
      wipeState = WIPE_STATE_INACTIVE; // reset for next time
      if (previousWipeVar)
      {
#ifdef STAIRCASE_WIPE_OFF
        userWipeVar = previousWipeVar;
        wipeState = WIPE_STATE_WIPING_OFF;
#else
        turnOff();
#endif
      }
      previousWipeVar = WIPE_OFF;
    }
  }

public:
  void setup()
  {
#ifdef STAIRCASE_INCLUDE_LUX
    bh1750UserMod = (Usermod_BH1750 *)UsermodManager::lookup(USERMOD_ID_BH1750);
#endif
    // standardize invalid pin numbers to -1
    if (topPIRorTriggerPin < 0)
      topPIRorTriggerPin = -1;
    if (topEchoPin < 0)
      topEchoPin = -1;
    if (bottomPIRorTriggerPin < 0)
      bottomPIRorTriggerPin = -1;
    if (bottomEchoPin < 0)
      bottomEchoPin = -1;
    // allocate pins
    PinManagerPinType pins[4] = {
        {topPIRorTriggerPin, useUSSensorTop},
        {topEchoPin, false},
        {bottomPIRorTriggerPin, useUSSensorBottom},
        {bottomEchoPin, false},
    };
    // NOTE: this *WILL* return TRUE if all the pins are set to -1.
    //       this is *BY DESIGN*.
    if (!PinManager::allocateMultiplePins(pins, 4, PinOwner::UM_AnimatedStaircase))
    {
      topPIRorTriggerPin = -1;
      topEchoPin = -1;
      bottomPIRorTriggerPin = -1;
      bottomEchoPin = -1;
      enabled = false;
    }
    enable(enabled);
    initDone = true;
  }

  void loop()
  {
    if (!enabled || strip.isUpdating())
      return;

    checkSensors();
    updateWipe();
  }

  uint16_t getId() { return USERMOD_ID_ANIMATED_STAIRCASE; }

#ifndef WLED_DISABLE_MQTT
  /**
   * handling of MQTT message
   * topic only contains stripped topic (part after /wled/MAC)
   * topic should look like: /wipe with amessage of [up|down]
   */
  bool onMqttMessage(char *topic, char *payload)
  {
    if (strlen(topic) == 5 && strncmp_P(topic, PSTR("/wipe"), 5) == 0)
    {
      String action = payload;
      if (action == "up")
      {
        bottomSensorWrite = true;
        return true;
      }
      else if (action == "down")
      {
        topSensorWrite = true;
        return true;
      }
      else if (action == "on")
      {
        enable(true);
        return true;
      }
      else if (action == "off")
      {
        enable(false);
        return true;
      }
    }
    return false;
  }

  /**
   * subscribe to MQTT topic for controlling usermod
   */
  void onMqttConnect(bool sessionPresent)
  {
    //(re)subscribe to required topics
    char subuf[64];
    if (mqttDeviceTopic[0] != 0)
    {
      strcpy(subuf, mqttDeviceTopic);
      strcat_P(subuf, PSTR("/wipe"));
      mqtt->subscribe(subuf, 0);
    }
  }
#endif

  void addToJsonState(JsonObject &root)
  {
    JsonObject staircase = root[FPSTR(_name)];
    if (staircase.isNull())
    {
      staircase = root.createNestedObject(FPSTR(_name));
    }
    writeSensorsToJson(staircase);
    DEBUG_PRINTLN(F("Staircase sensor state exposed in API."));
  }

  /*
   * Reads configuration settings from the json API.
   * See void addToJsonState(JsonObject& root)
   */
  void readFromJsonState(JsonObject &root)
  {
    if (!initDone)
      return; // prevent crash on boot applyPreset()
    bool en = enabled;
    JsonObject staircase = root[FPSTR(_name)];
    if (!staircase.isNull())
    {
      if (staircase[FPSTR(_enabled)].is<bool>())
      {
        en = staircase[FPSTR(_enabled)].as<bool>();
      }
      else
      {
        String str = staircase[FPSTR(_enabled)]; // checkbox -> off or on
        en = (bool)(str != "off");               // off is guaranteed to be present
      }
      if (en != enabled)
        enable(en);
      readSensorsFromJson(staircase);
      DEBUG_PRINTLN(F("Staircase sensor state read from API."));
      // This allows users to trigger a wipe via the state instead of the sensors - not sure we actually need it
      userWipeVar = root["userWipe"] | userWipeVar; // if "userWipe" key exists in JSON, update, else keep old value
    }
  }

  /*
   * Writes the configuration to internal flash memory.
   */
  void addToConfig(JsonObject &root)
  {
    JsonObject staircase = root[FPSTR(_name)];
    if (staircase.isNull())
    {
      staircase = root.createNestedObject(FPSTR(_name));
    }
    staircase[FPSTR(_enabled)] = enabled;
    staircase[FPSTR(_useTopUltrasoundSensor)] = useUSSensorTop;
    staircase[FPSTR(_topPIRorTrigger_pin)] = topPIRorTriggerPin;
    staircase[FPSTR(_topEcho_pin)] = useUSSensorTop ? topEchoPin : -1;
    staircase[FPSTR(_useBottomUltrasoundSensor)] = useUSSensorBottom;
    staircase[FPSTR(_bottomPIRorTrigger_pin)] = bottomPIRorTriggerPin;
    staircase[FPSTR(_bottomEcho_pin)] = useUSSensorBottom ? bottomEchoPin : -1;
    staircase[FPSTR(_topEchoCm)] = topMaxDist;
    staircase[FPSTR(_bottomEchoCm)] = bottomMaxDist;
    staircase[FPSTR(_togglePower)] = togglePower;
    staircase[FPSTR(_onTime)] = userWipeOffTime;
#ifdef STAIRCASE_INCLUDE_LUX
    staircase[FPSTR(_daylightLuxThreshold)] = daylightLuxThreshold;
#endif
    DEBUG_PRINTLN(F("Staircase config saved."));
  }

  /*
   * Reads the configuration to internal flash memory before setup() is called.
   *
   * The function should return true if configuration was successfully loaded or false if there was no configuration.
   */
  bool readFromConfig(JsonObject &root)
  {
    bool oldUseUSSensorTop = useUSSensorTop;
    bool oldUseUSSensorBottom = useUSSensorBottom;
    int8_t oldTopAPin = topPIRorTriggerPin;
    int8_t oldTopBPin = topEchoPin;
    int8_t oldBottomAPin = bottomPIRorTriggerPin;
    int8_t oldBottomBPin = bottomEchoPin;

    JsonObject top = root[FPSTR(_name)];
    if (top.isNull())
    {
      DEBUG_PRINT(FPSTR(_name));
      DEBUG_PRINTLN(F(": No config found. (Using defaults.)"));
      return false;
    }

    enabled = top[FPSTR(_enabled)] | enabled;

    useUSSensorTop = top[FPSTR(_useTopUltrasoundSensor)] | useUSSensorTop;
    topPIRorTriggerPin = top[FPSTR(_topPIRorTrigger_pin)] | topPIRorTriggerPin;
    topEchoPin = top[FPSTR(_topEcho_pin)] | topEchoPin;

    useUSSensorBottom = top[FPSTR(_useBottomUltrasoundSensor)] | useUSSensorBottom;
    bottomPIRorTriggerPin = top[FPSTR(_bottomPIRorTrigger_pin)] | bottomPIRorTriggerPin;
    bottomEchoPin = top[FPSTR(_bottomEcho_pin)] | bottomEchoPin;

    topMaxDist = top[FPSTR(_topEchoCm)] | topMaxDist;
    topMaxDist = min(150, max(30, (int)topMaxDist)); // max distance ~1.5m (a lag of 9ms may be expected)
    bottomMaxDist = top[FPSTR(_bottomEchoCm)] | bottomMaxDist;
    bottomMaxDist = min(150, max(30, (int)bottomMaxDist)); // max distance ~1.5m (a lag of 9ms may be expected)

    togglePower = top[FPSTR(_togglePower)] | togglePower; // staircase toggles power on/off

    userWipeOffTime = top[FPSTR(_onTime)] | userWipeOffTime; // staircase toggles power on/off

#ifdef STAIRCASE_INCLUDE_LUX
    daylightLuxThreshold = top[FPSTR(_daylightLuxThreshold)] | daylightLuxThreshold; // threshold for daylight lux, above this the staircase will not turn on
#endif

    DEBUG_PRINT(FPSTR(_name));
    if (!initDone)
    {
      // first run: reading from cfg.json
      DEBUG_PRINTLN(F(" config loaded."));
    }
    else
    {
      // changing parameters from settings page
      DEBUG_PRINTLN(F(" config (re)loaded."));
      bool changed = false;
      if ((oldUseUSSensorTop != useUSSensorTop) ||
          (oldUseUSSensorBottom != useUSSensorBottom) ||
          (oldTopAPin != topPIRorTriggerPin) ||
          (oldTopBPin != topEchoPin) ||
          (oldBottomAPin != bottomPIRorTriggerPin) ||
          (oldBottomBPin != bottomEchoPin))
      {
        changed = true;
        PinManager::deallocatePin(oldTopAPin, PinOwner::UM_AnimatedStaircase);
        PinManager::deallocatePin(oldTopBPin, PinOwner::UM_AnimatedStaircase);
        PinManager::deallocatePin(oldBottomAPin, PinOwner::UM_AnimatedStaircase);
        PinManager::deallocatePin(oldBottomBPin, PinOwner::UM_AnimatedStaircase);
      }
      if (changed)
        setup();
    }
    // use "return !top["newestParameter"].isNull();" when updating Usermod with new features
    // return !top[FPSTR(_togglePower)].isNull();
    return !top[FPSTR(_onTime)].isNull();
  }

  /*
   * Shows the delay between steps and power-off time in the "info"
   * tab of the web-UI.
   */
  void addToJsonInfo(JsonObject &root)
  {
    JsonObject user = root["u"];
    if (user.isNull())
    {
      user = root.createNestedObject("u");
    }

    JsonArray infoArr = user.createNestedArray(FPSTR(_name)); // name

    String uiDomString = F("<button class=\"btn btn-xs\" onclick=\"requestJson({");
    uiDomString += FPSTR(_name);
    uiDomString += F(":{");
    uiDomString += FPSTR(_enabled);
    uiDomString += enabled ? F(":false}});\">") : F(":true}});\">");
    uiDomString += F("<i class=\"icons ");
    uiDomString += enabled ? "on" : "off";
    uiDomString += F("\">&#xe08f;</i></button>");
    infoArr.add(uiDomString);
  }

  void turnOff()
  {
    jsonTransitionOnce = true;
#ifdef STAIRCASE_WIPE_OFF
    strip.setTransition(0); // turn off immediately after wipe completed
#else
    strip.setTransition(4000); // fade out slowly
#endif
    bri = 0;
    stateUpdated(CALL_MODE_NOTIFICATION);
    wipeState = WIPE_STATE_INACTIVE;
    userWipeVar = WIPE_OFF;
    previousWipeVar = WIPE_OFF;
    on = false;
  }

  void startWipe()
  {
    bri = briLast; // turn on
    jsonTransitionOnce = true;
    strip.setTransition(0); // no transition
    effectCurrent = FX_MODE_COLOR_WIPE;
    strip.resetTimebase();         // make sure wipe starts from beginning
    effectSpeed = userEffectSpeed; // set wipe speed

    // set wipe direction
    Segment &seg = strip.getSegment(0);
    bool doReverse = (userWipeVar == WIPE_DOWN);
    seg.setOption(1, doReverse);

    colorUpdated(CALL_MODE_NOTIFICATION);
  }
};

// strings to reduce flash memory usage (used more than twice)
const char Animated_Staircase_Wipe::_name[] PROGMEM = "staircase";
const char Animated_Staircase_Wipe::_enabled[] PROGMEM = "enabled";
const char Animated_Staircase_Wipe::_useTopUltrasoundSensor[] PROGMEM = "useTopUltrasoundSensor";
const char Animated_Staircase_Wipe::_topPIRorTrigger_pin[] PROGMEM = "topPIRorTrigger_pin";
const char Animated_Staircase_Wipe::_topEcho_pin[] PROGMEM = "topEcho_pin";
const char Animated_Staircase_Wipe::_useBottomUltrasoundSensor[] PROGMEM = "useBottomUltrasoundSensor";
const char Animated_Staircase_Wipe::_bottomPIRorTrigger_pin[] PROGMEM = "bottomPIRorTrigger_pin";
const char Animated_Staircase_Wipe::_bottomEcho_pin[] PROGMEM = "bottomEcho_pin";
const char Animated_Staircase_Wipe::_topEchoCm[] PROGMEM = "top-dist-cm";
const char Animated_Staircase_Wipe::_bottomEchoCm[] PROGMEM = "bottom-dist-cm";
const char Animated_Staircase_Wipe::_togglePower[] PROGMEM = "toggle-on-off";
const char Animated_Staircase_Wipe::_onTime[] PROGMEM = "on-time";
#ifdef STAIRCASE_INCLUDE_LUX
const char Animated_Staircase_Wipe::_daylightLuxThreshold[] PROGMEM = "daylight-lux-threshold";
#endif

static Animated_Staircase_Wipe animated_staircase_wipe;
REGISTER_USERMOD(animated_staircase_wipe);
