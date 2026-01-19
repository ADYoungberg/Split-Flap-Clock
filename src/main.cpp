#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <sntp.h>
#include <AccelStepper.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <Update.h>

// ==========================================
//              HARDWARE CONFIG
// ==========================================

// --- LEDs ---
#define LED_STATUS_PIN 2    
#define LED_COLON_PIN 4      // Blinking Colon (PWM Dimmable)
#define LED_AMPM_PIN 18      // AM/PM Indicator
#define LED_AUX_PIN 23       // AUX Light

// --- PWM Channels ---
const int PWM_FREQ = 5000;
const int PWM_RES = 8;
const int PWM_CH_STATUS = 0;
const int PWM_CH_COLON = 1; 
const int PWM_CH_AMPM = 2; 
const int PWM_CH_AUX = 3;    

// --- Steppers (Positive = Forward) ---
AccelStepper stepperHours(AccelStepper::FULL4WIRE, 26, 33, 25, 32);
AccelStepper stepperMinutes(AccelStepper::FULL4WIRE, 27, 12, 14, 13);

// --- Sensors ---
const int hallEffectSensorHoursPin = 35;
const int hallEffectSensorMinutesPin = 34;

const int WDT_TIMEOUT = 30; 

// Calibration Globals
int stepsPerRevolution = 2048; 
float stepsPerUnit = 2048.0 / 60.0; 
int stepsPerRevolutionHours = 2048; 
float stepsPerUnitHours = 2048.0 / 60.0;
const int DEFAULT_STEPS = 2048;
const int MIN_VALID_STEPS = 2040; // ~tolerance
const int MAX_VALID_STEPS = 2056;

// ==========================================
//              GLOBAL STATE
// ==========================================

Preferences preferences;
WebServer server(80);

// --- Settings ---
bool is12Hour = false;
String timeZoneString = "EST5EDT,M3.2.0,M11.1.0"; 

bool powerSaverEnabled = false; 
int motorMaxSpeed = 1000;       
int sensorSensitivity = 50; 
unsigned long lastMotorMoveTime = 0; 

bool nightModeEnabled = false;
int nightStartHour = 22;
int nightEndHour = 7;

// --- Date Display Settings ---
bool dateDisplayEnabled = false;
int dateIntervalMinutes = 5;  // How often to show date
int dateDurationSeconds = 5;  // How long to show date
unsigned long lastDateShowTime = 0;
bool isShowingDate = false;

// Auto Home Settings
int autoHomeIntervalHours = 0; // 0 = Disabled
time_t lastHomeTime = 0;

int baselineHours = 0;
int baselineMinutes = 0;
String calibrationStatus = "Idle"; 
int calibrationProgress = 0;       

// --- LED State ---
bool ledStatusEnabled = true; int ledStatusBrightness = 255;
bool ledColonEnabled = true; int ledColonBrightness = 255; 
bool ledAmPmEnabled = true; int ledAmPmBrightness = 255;
bool ledAuxEnabled = true; int ledAuxBrightness = 255; 

int currentDisplayedHour = -1;
int currentDisplayedMinute = -1;
bool manualMode = false;
int manualHourTarget = 0;
int manualMinuteTarget = 0;
bool isCalibrating = false;
bool isWifiSetup = false;
unsigned long lastWifiCheck = 0;
unsigned long lastLogicLoop = 0; // For loop throttling

struct HomingState { bool isHomed; };
HomingState homeStateHours = {false};
HomingState homeStateMinutes = {false};

// ==========================================
//             LED CONTROLLER
// ==========================================
class LedController {
  private:
    int pin; int channel; unsigned long lastToggle; bool state; bool usePWM;
  public:
    void begin(int p, int ch, bool pwm) {
      pin = p; channel = ch; usePWM = pwm;
      if(usePWM) {
        ledcSetup(channel, PWM_FREQ, PWM_RES);
        ledcAttachPin(pin, channel);
      } else {
        pinMode(pin, OUTPUT);
      }
      state = false; lastToggle = 0;
    }
    
    void update(bool enabled, int brightness, int interval) {
      if (!enabled) { forceOff(); return; }
      if (millis() - lastToggle >= interval) {
        lastToggle = millis();
        state = !state;
        if(usePWM) ledcWrite(channel, state ? brightness : 0);
        else digitalWrite(pin, state ? HIGH : LOW);
      }
    }

    void forceOff() { 
        if(usePWM) ledcWrite(channel, 0); 
        else digitalWrite(pin, LOW); 
    }
    
    void forceOn(int brightness) { 
        if(usePWM) ledcWrite(channel, brightness);
        else digitalWrite(pin, HIGH);
    }
};

LedController ledStatus; 
LedController ledColon; 
LedController ledAmPm;  
LedController ledAux; 

// ==========================================
//              HTML DASHBOARD
// ==========================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Split Flap Clock</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: 'Segoe UI', sans-serif; text-align: center; background-color: #f4f4f9; margin: 0; padding: 20px; color: #333; }
    .card { background: white; max-width: 650px; margin: auto; padding: 25px; border-radius: 15px; box-shadow: 0 10px 20px rgba(0,0,0,0.1); }
    h1 { color: #2c3e50; margin-bottom: 5px; }
    
    /* Flex layout for 5 columns */
    .stat-box { display: flex; justify-content: space-between; margin: 20px 0; background: #eef2f5; padding: 10px; border-radius: 10px; }
    
    /* UPDATED: Width ~19% to fit 5 items */
    .stat { width: 19%; display: flex; flex-direction: column; justify-content: center; }
    
    .stat h3 { margin: 5px 0; font-size: 11px; color: #7f8c8d; text-transform: uppercase; letter-spacing: 1px;}
    
    .time-display { font-size: 24px; font-weight: bold; color: #2c3e50; }
    .sensor-text { font-size: 13px; color: #555; line-height: 1.4; word-wrap: break-word; }
    
    .active { color: #e74c3c; font-weight: bold; }
    .inactive { color: #27ae60; font-weight: bold; }
    
    input, select, button { box-sizing: border-box; padding: 12px; margin: 8px 0; width: 100%; border: 1px solid #ddd; border-radius: 8px; font-size: 16px; }
    input[type=checkbox] { width: 20px; height: 20px; vertical-align: middle; margin: 0 10px 0 0; }
    input[type=range] { padding: 0; margin: 10px 0; }
    button { background-color: #3498db; color: white; border: none; cursor: pointer; transition: 0.3s; font-weight: 600; }
    button:hover { background-color: #2980b9; }
    .btn-green { background-color: #2ecc71; } .btn-green:hover { background-color: #27ae60; }
    .btn-orange { background-color: #f39c12; } .btn-orange:hover { background-color: #d35400; }
    .btn-danger { background-color: #e74c3c; } .btn-danger:hover { background-color: #c0392b; }
    .control-group { border-top: 2px solid #f0f0f0; padding-top: 20px; margin-top: 20px; text-align: left; }
    .row { display: flex; align-items: center; gap: 10px; margin-bottom: 10px; }
    .row input[type=number] { flex: 1; }
    label { font-weight: bold; display: block; margin-top: 10px; }
    .sub-label { font-weight: normal; font-size: 14px; color: #666; }
    
    #prog-wrap { display:none; background:#eee; height:20px; border-radius:10px; overflow:hidden; margin-top:10px;}
    #prog-bar { background:#2ecc71; height:100%; width:0%; transition:width 0.2s;}
    
    #calib-overlay { display:none; position:fixed; top:0; left:0; width:100%; height:100%; background:rgba(0,0,0,0.8); z-index:99; align-items:center; justify-content:center; flex-direction:column; color:white; }
    #calib-bar-wrap { width:80%; height:30px; background:#555; border-radius:15px; overflow:hidden; margin-top:20px; }
    #calib-bar { width:0%; height:100%; background:#f1c40f; transition:width 0.2s; }
  </style>
</head>
<body>
  
  <div id="calib-overlay">
      <h2 id="calib-text">Calibrating...</h2>
      <div id="calib-bar-wrap"><div id="calib-bar"></div></div>
      <p>Do not turn off power.</p>
  </div>

  <div class="card">
    <h1>Split Flap Clock</h1>
    
    <div class="stat-box">
      <div class="stat">
          <h3>WIFI</h3>
          <div id="wifiStats" class="sensor-text">--</div>
      </div>
      <div class="stat">
          <h3>TIME</h3>
          <div id="dispTime" class="time-display">--:--</div>
      </div>
      <div class="stat">
          <h3>DATE</h3>
          <div id="dispDate" class="time-display" style="font-size:22px">--</div>
      </div>
      <div class="stat">
          <h3>SENSORS</h3>
          <div id="sensorStats" class="sensor-text">Loading...</div>
      </div>
      <div class="stat">
          <h3>CALIBRATION</h3>
          <div id="calibStats" class="sensor-text">Loading...</div>
      </div>
    </div>

    <div class="control-group">
      <h3>Manual Control</h3>
      <div class="row">
        <input type="number" id="manualH" placeholder="HH" min="0" max="23">
        <input type="number" id="manualM" placeholder="MM" min="0" max="59">
      </div>
      <button onclick="setManual()">Move to Time</button>
      <button onclick="resumeAuto()" class="btn-green">Resume Auto Clock</button>
    </div>

    <div class="control-group">
      <h3>Configuration</h3>
      <form action="/save" method="POST">
        <label>Clock Mode</label>
        <select id="is12h" name="is12h"><option value="0">24 Hour</option><option value="1">12 Hour</option></select>

        <label>Region / Timezone</label>
        <select id="tz" name="tz">
            <option value="UTC0">Universal Time (UTC/GMT)</option>
            <optgroup label="North America">
                <option value="EST5EDT,M3.2.0,M11.1.0">Eastern Time (New York, Toronto)</option>
                <option value="CST6CDT,M3.2.0,M11.1.0">Central Time (Chicago, Mexico City)</option>
                <option value="MST7MDT,M3.2.0,M11.1.0">Mountain Time (Denver)</option>
                <option value="MST7">Mountain - No DST (Arizona)</option>
                <option value="PST8PDT,M3.2.0,M11.1.0">Pacific Time (LA, Vancouver)</option>
                <option value="AKST9AKDT,M3.2.0,M11.1.0">Alaska (Anchorage)</option>
                <option value="HST10">Hawaii (Honolulu)</option>
            </optgroup>
            <optgroup label="South America">
                <option value="<-03>3">Brazil / Argentina (Sao Paulo, Buenos Aires)</option>
                <option value="<-04>4<-03>,M9.1.6/24,M4.1.6/24">Chile (Santiago)</option>
                <option value="<-05>5">Colombia / Peru (Bogota, Lima)</option>
            </optgroup>
            <optgroup label="Europe">
                <option value="GMT0BST,M3.5.0/1,M10.5.0">UK / Ireland (London, Dublin)</option>
                <option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe (Paris, Berlin, Rome)</option>
                <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe (Athens, Helsinki)</option>
                <option value="MSK-3">Moscow (No DST)</option>
            </optgroup>
            <optgroup label="Africa & Middle East">
                <option value="WAT-1">West Africa (Lagos, Algiers)</option>
                <option value="SAST-2">South Africa (Johannesburg)</option>
                <option value="EET-2EEST,M4.5.5/0,M10.5.4/24">Egypt (Cairo)</option>
                <option value="<-03>3">Saudi Arabia (Riyadh)</option>
                <option value="<-04>4">UAE (Dubai)</option>
            </optgroup>
            <optgroup label="Asia">
                <option value="IST-5:30">India (New Delhi, Mumbai)</option>
                <option value="<-07>7">Thailand / Vietnam (Bangkok, Hanoi)</option>
                <option value="<-08>8">China / Singapore (Beijing, HK, Perth)</option>
                <option value="JST-9">Japan / Korea (Tokyo, Seoul)</option>
            </optgroup>
            <optgroup label="Oceania">
                <option value="ACST-9:30ACDT,M10.1.0,M4.1.0/3">Adelaide (South Australia)</option>
                <option value="AEST-10">Brisbane (No DST)</option>
                <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Sydney / Melbourne</option>
                <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand (Auckland)</option>
            </optgroup>
        </select>
        
        <label>Maintenance</label>
        <div class="row">
            <span class="sub-label" style="width:200px">Auto-Home Every (Hours):</span>
            <input type="number" id="homeInt" name="homeInt" min="0" max="168" placeholder="0 to Disable">
        </div>

        <label>Motor Settings</label>
        <div class="row">
            <span class="sub-label">Power Saver (Off 2s after move):</span>
            <input type="checkbox" id="pwrSav" name="pwrSav" value="1">
        </div>
        <div class="row">
             <span class="sub-label">Max Speed:</span>
             <input type="range" id="spd" name="spd" min="100" max="1200" oninput="document.getElementById('spdVal').innerText=this.value">
             <span id="spdVal" style="width:40px; text-align:right;">1000</span>
        </div>

        <label>Sensor Tuning</label>
        <div class="row">
             <span class="sub-label">Sensitivity:</span>
             <input type="range" id="sens" name="sens" min="1" max="100" oninput="document.getElementById('sensVal').innerText=this.value">
             <span id="sensVal" style="width:40px; text-align:right;">50</span>
        </div>
        <div class="row">
             <button type="button" class="btn-orange" onclick="runCalibration('sensors')">Calibrate Sensors (Home)</button>
             <button type="button" class="btn-orange" style="background:#d35400;" onclick="runCalibration('motors')">Calibrate Motors (Full)</button>
        </div>

      <label>Alternating Date Display</label>
        
        <div class="row">
            <span class="sub-label">Enable Date Display:</span>
            <input type="checkbox" id="dateEn" name="dateEn" value="1">
        </div>

        <div class="row">
             <span class="sub-label" style="width:60%">Display date every (Minutes):</span>
             <input type="number" id="dateInt" name="dateInt" min="1" max="60">
        </div>

        <div class="row">
             <span class="sub-label" style="width:60%">Display duration (Seconds):</span>
             <input type="number" id="dateDur" name="dateDur" min="2" max="60">
        </div>

        <label>Night Mode</label>
        
        <div class="row">
            <span class="sub-label">Enable Night Mode:</span>
            <input type="checkbox" id="nightEn" name="nightEn" value="1">
        </div>

        <div class="row">
            <span class="sub-label" style="width:60%">Turn OFF at Hour (0-23):</span>
            <input type="number" id="nStart" name="nStart" placeholder="22" min="0" max="23">
        </div>

        <div class="row">
            <span class="sub-label" style="width:60%">Turn ON at Hour (0-23):</span>
            <input type="number" id="nEnd" name="nEnd" placeholder="7" min="0" max="23">
        </div>

        <label>LED Settings</label>
        
        <div class="row">
            <span class="sub-label">Internal Status LED:</span>
            <input type="checkbox" id="ledS_en" name="ledS_en" value="1">
        </div>
        <input type="range" id="ledS_br" name="ledS_br" min="0" max="255">

        <div class="row">
            <span class="sub-label">Blinking Colon:</span>
            <input type="checkbox" id="ledC_en" name="ledC_en" value="1">
        </div>
        <input type="range" id="ledC_br" name="ledC_br" min="0" max="255">
        
        <div class="row">
            <span class="sub-label">Auxiliary Light (Solid):</span>
            <input type="checkbox" id="ledX_en" name="ledX_en" value="1">
        </div>
        <input type="range" id="ledX_br" name="ledX_br" min="0" max="255">

        <div class="row">
            <span class="sub-label">PM Indicator:</span>
            <input type="checkbox" id="ledA_en" name="ledA_en" value="1">
        </div>
        <input type="range" id="ledA_br" name="ledA_br" min="0" max="255">

        <button type="submit">Save Settings</button>
      </form>
    </div>

    <div class="control-group">
      <h3>System</h3>
      <div class="row">
        <button class="btn-danger" type="button" onclick="if(confirm('Restart?')) location.href='/restart'">Restart</button>
        <button class="btn-danger" type="button" onclick="if(confirm('Reset WiFi?')) location.href='/reset_wifi'">Reset WiFi</button>
      </div>

      <div style="margin-top:10px;">
        <button class="btn-danger" style="background:#c0392b;" type="button" onclick="if(confirm('RESET CALIBRATION? Use this if clock spins wildly.')) location.href='/reset_cal'">Reset Calibration</button>
      </div>

      <div style="margin-top:15px; border-top:1px solid #ddd; padding-top:15px;">
        <input type="file" id="fwFile" accept=".bin" style="display:none" onchange="uploadFirmware(this)">
        <button style="background:#8e44ad;" onclick="document.getElementById('fwFile').click()">Update Firmware</button>
        <div id="prog-wrap"><div id="prog-bar"></div></div>
        <p id="updStatus" style="font-size:12px; color:#666;"></p>
      </div>
      
      <p style="font-size:12px; text-align:center; color:#888; margin-top:20px;">Access via: http://splitflap.local</p>
    </div>
  </div>

  <script>
    function calcDiff(val) {
        let diff = ((val - 2048) / 2048) * 100;
        if(diff === 0) return '<span style="color:#27ae60; font-weight:bold;">0.00% (Default)</span>';
        let color = (diff > 0.5 || diff < -0.5) ? '#e74c3c' : '#27ae60';
        return '<span style="color:'+color+'; font-weight:bold;">' + (diff>0?'+':'') + diff.toFixed(2) + '%</span>';
    }

    function updateStatus() {
      fetch('/status').then(res => res.json()).then(data => {
        // UPDATE WIFI
        let sig = data.rssi;
        let quality = (sig >= -50) ? "Excellent" : (sig >= -60) ? "Good" : (sig >= -70) ? "Fair" : "Weak";
        let color = (sig >= -60) ? "#27ae60" : (sig >= -70) ? "#f39c12" : "#e74c3c";
        document.getElementById('wifiStats').innerHTML = '<strong>' + data.ssid + '</strong><br><span style="color:' + color + ';">' + quality + ' (' + sig + 'dBm)</span>';

        // UPDATE TIME
        document.getElementById('dispTime').innerText = (data.h<10?'0':'')+data.h + ':' + (data.m<10?'0':'')+data.m;
        
        // UPDATE DATE (NEW)
        document.getElementById('dispDate').innerText = data.date;

        // UPDATE SENSORS
        let sensHtml = 'H: ' + (data.sensH ? '<span class="active">MAG</span>' : '<span class="inactive">---</span>') + ' (' + data.baseH + ')<br>' +
                       'M: ' + (data.sensM ? '<span class="active">MAG</span>' : '<span class="inactive">---</span>') + ' (' + data.baseM + ')';
        document.getElementById('sensorStats').innerHTML = sensHtml;

        // UPDATE CALIBRATION
        let calHtml = 'H: ' + calcDiff(data.stepH) + '<br>M: ' + calcDiff(data.stepM);
        document.getElementById('calibStats').innerHTML = calHtml;

        if(!document.getElementById('tz').dataset.loaded) {
           document.getElementById('is12h').value = data.conf_12h ? "1" : "0";
           document.getElementById('tz').value = data.conf_tz; 
           document.getElementById('homeInt').value = data.conf_homeInt;
           
           document.getElementById('dateEn').checked = data.conf_dEn;
           document.getElementById('dateInt').value = data.conf_dInt;
           document.getElementById('dateDur').value = data.conf_dDur;

           document.getElementById('pwrSav').checked = data.conf_pwrSav;
           document.getElementById('spd').value = data.conf_spd;
           document.getElementById('spdVal').innerText = data.conf_spd;
           document.getElementById('sens').value = data.conf_sens;
           document.getElementById('sensVal').innerText = data.conf_sens;
           document.getElementById('nightEn').checked = data.conf_nEn;
           document.getElementById('nStart').value = data.conf_nStart;
           document.getElementById('nEnd').value = data.conf_nEnd;
           
           document.getElementById('ledS_en').checked = data.ledS_en;
           document.getElementById('ledS_br').value = data.ledS_br;
           document.getElementById('ledC_en').checked = data.ledC_en;
           document.getElementById('ledC_br').value = data.ledC_br;
           document.getElementById('ledX_en').checked = data.ledX_en;
           document.getElementById('ledX_br').value = data.ledX_br;
           document.getElementById('ledA_en').checked = data.ledA_en;
           document.getElementById('ledA_br').value = data.ledA_br;
           document.getElementById('tz').dataset.loaded = true;
        }
      });
    }

    function runCalibration(type) {
        let msg = (type === 'motors') ? 
            "Full Calibration: Will recalibrate sensors AND count motor steps. Continue?" :
            "Sensor Calibration: Will recalibrate baseline and home to 00:00. Continue?";
            
        if(!confirm(msg)) return;
        
        document.getElementById('calib-overlay').style.display = 'flex';
        fetch('/calibrate_' + type, { method: 'POST' });
        
        let pollTimer = setInterval(() => {
            fetch('/calib_status').then(r=>r.json()).then(d => {
                document.getElementById('calib-text').innerText = d.status;
                document.getElementById('calib-bar').style.width = d.progress + "%";
                if(d.progress >= 100) {
                    clearInterval(pollTimer);
                    setTimeout(() => { 
                        document.getElementById('calib-overlay').style.display = 'none'; 
                        location.reload(); 
                    }, 1000);
                }
            });
        }, 500);
    }

    function uploadFirmware(input) {
        let file = input.files[0];
        if(!file) return;
        let formData = new FormData();
        formData.append("update", file);
        document.getElementById('prog-wrap').style.display = 'block';
        document.getElementById('updStatus').innerText = "Uploading " + file.name + "...";
        let xhr = new XMLHttpRequest();
        xhr.open("POST", "/update");
        xhr.upload.addEventListener("progress", function(evt) {
            if (evt.lengthComputable) {
                let percentComplete = (evt.loaded / evt.total) * 100;
                document.getElementById('prog-bar').style.width = percentComplete + '%';
            }
        }, false);
        xhr.onload = function() {
            if (xhr.status == 200 && xhr.responseText == "OK") {
                 document.getElementById('updStatus').innerText = "Success! Rebooting...";
                 setTimeout(() => location.reload(), 5000);
            } else {
                 document.getElementById('updStatus').innerText = "Failed: " + xhr.responseText;
            }
        };
        xhr.send(formData);
    }

    function setManual() {
      let h = document.getElementById('manualH').value;
      let m = document.getElementById('manualM').value;
      fetch('/manual?h=' + h + '&m=' + m, { method: 'POST' });
    }
    function resumeAuto() { fetch('/resume', { method: 'POST' }); }
    setInterval(updateStatus, 1000);
    updateStatus();
  </script>
</body>
</html>
)rawliteral";

// ==========================================
//              CORE FUNCTIONS
// ==========================================

struct Time { int hour; int minute; bool isPm; };

Time getLocalTimeData() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return { -1, -1, false};
  int rawHour = timeinfo.tm_hour;
  bool pm = (rawHour >= 12); 
  int iHour = rawHour;
  int iMinute = timeinfo.tm_min;
  if (is12Hour) {
    iHour = (iHour % 12);
    if (iHour == 0) iHour = 12;
  }
  return {iHour, iMinute, pm};
}

bool isNightTime() {
  if (!nightModeEnabled) return false;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  int h = timeinfo.tm_hour;
  if (nightStartHour > nightEndHour) return (h >= nightStartHour || h < nightEndHour);
  else return (h >= nightStartHour && h < nightEndHour);
}

int readSensorAverage(int pin) {
    long sum = 0;
    for(int i=0; i<10; i++) {
        sum += analogRead(pin);
        delay(1);
    }
    return sum / 10;
}

// Helper to find the center of the magnet
// Returns the number of steps taken to cross the magnet width
int centerOnMagnet(AccelStepper &stepper, int sensorPin, int baseline, int threshold) {
    long startPos = stepper.currentPosition();
    bool magnetLost = false;
    
    // We are already at the 'edge' (sensor triggered). 
    // Now move slowly forward until the sensor turns OFF.
    stepper.setSpeed(200); // Slow for precision
    
    // Limit search to 150 steps to prevent infinite loop if sensor stuck
    while (!magnetLost && (stepper.currentPosition() - startPos < 150)) {
        stepper.runSpeed();
        // Check if we are back to baseline (sensor OFF)
        if (abs(readSensorAverage(sensorPin) - baseline) < threshold) {
             magnetLost = true;
        }
        esp_task_wdt_reset();
    }
    
    long endPos = stepper.currentPosition();
    long magnetWidth = endPos - startPos;
    
    // Move backward to the exact center
    long centerOffset = magnetWidth / 2;
    stepper.setCurrentPosition(magnetWidth); // Temporarily call end "width"
    stepper.runToNewPosition(centerOffset);  // Move back to "half width"
    
    // Now we are in the center. Set this as TRUE ZERO.
    stepper.setCurrentPosition(0);
    
    return (int)magnetWidth;
}

void runHomingSequence(bool measureBaseline, bool countSteps) {
  isCalibrating = true;
  homeStateHours.isHomed = false;
  homeStateMinutes.isHomed = false;
  
  stepperHours.enableOutputs();
  stepperMinutes.enableOutputs();

  // --- STAGE 1 & 2: BASELINE (Unchanged) ---
  if (measureBaseline) {
      calibrationStatus = "Clearing Sensors...";
      calibrationProgress = 5;
      server.handleClient();
      stepperHours.move(600); stepperMinutes.move(600);
      stepperHours.setMaxSpeed(600); stepperMinutes.setMaxSpeed(600);
      while(stepperHours.distanceToGo() != 0 || stepperMinutes.distanceToGo() != 0) {
          stepperHours.run(); stepperMinutes.run(); esp_task_wdt_reset();
      }
      
      calibrationStatus = "Measuring Baseline...";
      calibrationProgress = 10;
      server.handleClient();
      long sumH = 0, sumM = 0;
      for(int i=0; i<200; i++) {
          sumH += analogRead(hallEffectSensorHoursPin);
          sumM += analogRead(hallEffectSensorMinutesPin);
          delay(2);
      }
      baselineHours = sumH / 200;
      baselineMinutes = sumM / 200;
      
      preferences.begin("clock-conf", false);
      preferences.putInt("baseH", baselineHours);
      preferences.putInt("baseM", baselineMinutes);
      preferences.end();
  } else {
      if(baselineHours == 0) baselineHours = 1800; 
      if(baselineMinutes == 0) baselineMinutes = 1800;
  }

  // --- STAGE 3: FIND ZERO & CENTER (UPDATED) ---
  calibrationStatus = "Centering on Home...";
  int threshold = map(sensorSensitivity, 1, 100, 1500, 100);

  stepperHours.setMaxSpeed(600); stepperHours.setSpeed(300); 
  stepperMinutes.setMaxSpeed(600); stepperMinutes.setSpeed(300); 

  // -- 3a. Find Edges --
  while (!homeStateHours.isHomed || !homeStateMinutes.isHomed) {
    esp_task_wdt_reset(); server.handleClient(); ledStatus.forceOn(255);
    
    if (!homeStateHours.isHomed) {
      if (abs(readSensorAverage(hallEffectSensorHoursPin) - baselineHours) < threshold) { 
          stepperHours.runSpeed(); 
      } else { 
          // Edge Found! Now perform Centering
          stepperHours.stop();
          centerOnMagnet(stepperHours, hallEffectSensorHoursPin, baselineHours, threshold);
          homeStateHours.isHomed = true; 
      }
    }
    if (!homeStateMinutes.isHomed) {
      if (abs(readSensorAverage(hallEffectSensorMinutesPin) - baselineMinutes) < threshold) { 
          stepperMinutes.runSpeed(); 
      } else { 
          // Edge Found! Now perform Centering
          stepperMinutes.stop();
          centerOnMagnet(stepperMinutes, hallEffectSensorMinutesPin, baselineMinutes, threshold);
          homeStateMinutes.isHomed = true; 
      }
    }
  }

  if (!countSteps) {
      // Just finish up if we aren't calibrating the step count
      calibrationStatus = "Homed & Centered";
      calibrationProgress = 100;
      server.handleClient();
      stepperHours.setMaxSpeed(motorMaxSpeed); stepperHours.setAcceleration(1000);
      stepperMinutes.setMaxSpeed(motorMaxSpeed); stepperMinutes.setAcceleration(1000);
      currentDisplayedHour = 0; currentDisplayedMinute = 0;
      isCalibrating = false; ledStatus.forceOff();
      return; 
  }

  // --- STAGE 4: CALIBRATE MOTOR STEPS (With Safety Check) ---
   
   // --- CALIBRATE MINUTES (2 TURNS) ---
   calibrationStatus = "Counting M Steps (2 Turns)...";
   calibrationProgress = 50;
   server.handleClient();
   
   stepperMinutes.setCurrentPosition(0); 
   stepperMinutes.move(6000);     // <--- UPDATED: Enough for 2.5 turns (was 3000)
   stepperMinutes.setMaxSpeed(600);
   
   // Move away blindly past the first magnet trigger (at ~2048)
   // We wait until 3000 steps to ensure we skipped the first turn completely
   while(stepperMinutes.currentPosition() < 3000) { // <--- UPDATED: Blind zone extended
       stepperMinutes.run(); esp_task_wdt_reset();
   }
   
   bool magnetFound = false;
   float measuredStepsM = 0; // Use float for division later
   
   while(stepperMinutes.distanceToGo() != 0 && !magnetFound) {
       stepperMinutes.run(); esp_task_wdt_reset(); server.handleClient();
       
       if (abs(readSensorAverage(hallEffectSensorMinutesPin) - baselineMinutes) > threshold) {
           magnetFound = true;
           stepperMinutes.stop();
           centerOnMagnet(stepperMinutes, hallEffectSensorMinutesPin, baselineMinutes, threshold);
           
           // Divide total by 2 to get the single revolution average
           measuredStepsM = stepperMinutes.currentPosition() / 2.0; 
           
           stepperMinutes.setCurrentPosition(0); 
       }
   }
   
   // Validate Minute Steps (Check against limits)
   if(magnetFound && measuredStepsM >= MIN_VALID_STEPS && measuredStepsM <= MAX_VALID_STEPS) {
       stepsPerRevolution = (int)round(measuredStepsM); // Round to nearest whole step
       stepsPerUnit = stepsPerRevolution / 60.0;
       preferences.begin("clock-conf", false);
       preferences.putInt("stepsRev", stepsPerRevolution);
       preferences.end();
   } else {
       calibrationStatus = "Err M: " + String(measuredStepsM);
       delay(2000); 
   }

   // --- CALIBRATE HOURS (2 TURNS) ---
   calibrationStatus = "Counting H Steps (2 Turns)...";
   calibrationProgress = 80;
   server.handleClient();
   
   stepperHours.setCurrentPosition(0);
   stepperHours.move(6000);      // <--- UPDATED
   stepperHours.setMaxSpeed(600); 
   
   while(stepperHours.currentPosition() < 3000) { // <--- UPDATED
       stepperHours.run(); esp_task_wdt_reset();
   }
   
   magnetFound = false;
   float measuredStepsH = 0;
   
   while(stepperHours.distanceToGo() != 0 && !magnetFound) {
       stepperHours.run(); esp_task_wdt_reset(); server.handleClient();

       if (abs(readSensorAverage(hallEffectSensorHoursPin) - baselineHours) > threshold) {
           magnetFound = true;
           stepperHours.stop();
           centerOnMagnet(stepperHours, hallEffectSensorHoursPin, baselineHours, threshold);
           
           // Divide total by 2
           measuredStepsH = stepperHours.currentPosition() / 2.0;
           
           stepperHours.setCurrentPosition(0); 
       }
   }
   
   if(magnetFound && measuredStepsH >= MIN_VALID_STEPS && measuredStepsH <= MAX_VALID_STEPS) {
       stepsPerRevolutionHours = (int)round(measuredStepsH);
       stepsPerUnitHours = stepsPerRevolutionHours / 60.0;
       preferences.begin("clock-conf", false);
       preferences.putInt("stepsRevH", stepsPerRevolutionHours);
       preferences.end();
   } else {
       calibrationStatus = "Err H: " + String(measuredStepsH);
       delay(3000);
   }
   
   calibrationStatus = "Complete: M" + String(stepsPerRevolution) + " H" + String(stepsPerRevolutionHours);
   calibrationProgress = 100;
   server.handleClient();
   delay(3000); 
  
  stepperHours.setMaxSpeed(motorMaxSpeed); stepperHours.setAcceleration(1000); 
  stepperMinutes.setMaxSpeed(motorMaxSpeed); stepperMinutes.setAcceleration(1000); 

  currentDisplayedHour = 0; currentDisplayedMinute = 0;
  isCalibrating = false; ledStatus.forceOff(); 
}

long calculateTargetPosition(int currentVal, int nextVal, bool isHour) {
   // 1. Get the Motor and Calibration settings for this specific spool
   long currentSteps = isHour ? stepperHours.currentPosition() : stepperMinutes.currentPosition();
   int stepsRev = isHour ? stepsPerRevolutionHours : stepsPerRevolution;
   
   // 2. Determine where we are inside the current rotation (0 to ~2048)
   long currentMod = currentSteps % stepsRev;
   if (currentMod < 0) currentMod += stepsRev; // Handle negative positions safely

   // 3. Calculate the PERFECT absolute target based on the time
   //    (We use 60.0 for both because your code configures hours to scale that way)
   float perfectTarget = ((float)nextVal * (float)stepsRev) / 60.0;
   
   // 4. Round that target to the nearest "Safe" multiple of 4
   //    This aligns with the motor's magnetic detents so it won't drift when power is cut.
   long targetMod = (long)round(perfectTarget);
   long remainder = targetMod % 4;
   if (remainder != 0) {
       if (remainder >= 2) targetMod += (4 - remainder); // Round Up
       else targetMod -= remainder;                      // Round Down
   }

   // 5. Calculate the difference to move
   long diff = targetMod - currentMod;

   // 6. Handle the rollover (e.g., moving from Minute 59 to 00)
   //    If the shortest path is backwards, we add a full revolution to make it forward
   if (diff < 0) diff += stepsRev;

   return diff;
}

void blinkIpAddress() {
    IPAddress ip = WiFi.localIP();
    int lastOctet = ip[3]; String ipStr = String(lastOctet);
    Serial.print("Blinking IP Last Octet: "); Serial.println(ipStr);
    ledAmPm.forceOff(); delay(1000); 
    for (int i = 0; i < ipStr.length(); i++) {
        char c = ipStr.charAt(i); int digit = c - '0'; int blinks = digit; if (blinks == 0) blinks = 10; 
        for (int b = 0; b < blinks; b++) { ledAmPm.forceOn(255); delay(200); ledAmPm.forceOff(); delay(200); }
        delay(2000); 
    }
}

// ==========================================
//              WEB HANDLERS
// ==========================================
void handleRoot() { server.send(200, "text/html", index_html); }

void handleStatus() {
  JsonDocument doc;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
      char dateBuf[16];
      strftime(dateBuf, sizeof(dateBuf), "%b %d", &timeinfo); // Format: "Jan 18"
      doc["date"] = String(dateBuf);
  } else {
      doc["date"] = "--";
  }
  doc["ssid"] = WiFi.SSID();
  doc["rssi"] = WiFi.RSSI();
  doc["conf_dEn"] = dateDisplayEnabled;
  doc["conf_dInt"] = dateIntervalMinutes;
  doc["conf_dDur"] = dateDurationSeconds;
  doc["h"] = currentDisplayedHour; doc["m"] = currentDisplayedMinute;
  int th = map(sensorSensitivity, 1, 100, 1500, 100);
  doc["sensH"] = (abs(analogRead(hallEffectSensorHoursPin) - baselineHours) > th); 
  doc["sensM"] = (abs(analogRead(hallEffectSensorMinutesPin) - baselineMinutes) > th); 
  doc["baseH"] = baselineHours; doc["baseM"] = baselineMinutes;
  doc["stepH"] = stepsPerRevolutionHours;
  doc["stepM"] = stepsPerRevolution;
  doc["conf_12h"] = is12Hour; doc["conf_tz"] = timeZoneString;
  doc["conf_pwrSav"] = powerSaverEnabled; doc["conf_spd"] = motorMaxSpeed;
  doc["conf_sens"] = sensorSensitivity;
  doc["conf_nEn"] = nightModeEnabled; doc["conf_nStart"] = nightStartHour; doc["conf_nEnd"] = nightEndHour;
  doc["conf_homeInt"] = autoHomeIntervalHours;
  doc["ledS_en"] = ledStatusEnabled; doc["ledS_br"] = ledStatusBrightness;
  doc["ledC_en"] = ledColonEnabled; doc["ledC_br"] = ledColonBrightness;
  doc["ledX_en"] = ledAuxEnabled; doc["ledX_br"] = ledAuxBrightness; 
  doc["ledA_en"] = ledAmPmEnabled; doc["ledA_br"] = ledAmPmBrightness;
  String json; serializeJson(doc, json); server.send(200, "application/json", json);
}

void handleCalibStatus() {
  JsonDocument doc;
  doc["status"] = calibrationStatus;
  doc["progress"] = calibrationProgress;
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSave() {
  if (server.hasArg("is12h")) is12Hour = (server.arg("is12h") == "1");
  if (server.hasArg("tz")) timeZoneString = server.arg("tz");
  powerSaverEnabled = (server.hasArg("pwrSav"));
  if (server.hasArg("spd")) motorMaxSpeed = server.arg("spd").toInt();
  if (server.hasArg("sens")) sensorSensitivity = server.arg("sens").toInt();
  nightModeEnabled = (server.hasArg("nightEn")); 
  if (server.hasArg("nStart")) nightStartHour = server.arg("nStart").toInt();
  if (server.hasArg("nEnd")) nightEndHour = server.arg("nEnd").toInt();
  if (server.hasArg("homeInt")) autoHomeIntervalHours = server.arg("homeInt").toInt();
  dateDisplayEnabled = (server.hasArg("dateEn"));
  if (server.hasArg("dateInt")) dateIntervalMinutes = server.arg("dateInt").toInt();
  if (server.hasArg("dateDur")) dateDurationSeconds = server.arg("dateDur").toInt();
  ledStatusEnabled = (server.hasArg("ledS_en"));
  if (server.hasArg("ledS_br")) ledStatusBrightness = server.arg("ledS_br").toInt();
  ledColonEnabled = (server.hasArg("ledC_en"));
  if (server.hasArg("ledC_br")) ledColonBrightness = server.arg("ledC_br").toInt();
  ledAuxEnabled = (server.hasArg("ledX_en")); 
  if (server.hasArg("ledX_br")) ledAuxBrightness = server.arg("ledX_br").toInt(); 
  ledAmPmEnabled = (server.hasArg("ledA_en"));
  if (server.hasArg("ledA_br")) ledAmPmBrightness = server.arg("ledA_br").toInt();

  preferences.begin("clock-conf", false);
  preferences.putBool("12h", is12Hour); preferences.putString("tz", timeZoneString);
  preferences.putBool("idle", powerSaverEnabled); preferences.putInt("spd", motorMaxSpeed);
  preferences.putInt("sens", sensorSensitivity);
  preferences.putBool("nEn", nightModeEnabled); preferences.putInt("nSt", nightStartHour); preferences.putInt("nEd", nightEndHour);
  preferences.putInt("homeInt", autoHomeIntervalHours);
  preferences.putBool("dEn", dateDisplayEnabled);
  preferences.putInt("dInt", dateIntervalMinutes);
  preferences.putInt("dDur", dateDurationSeconds);
  preferences.putBool("lSe", ledStatusEnabled); preferences.putInt("lSb", ledStatusBrightness);
  preferences.putBool("lCe", ledColonEnabled); preferences.putInt("lCb", ledColonBrightness);
  preferences.putBool("lXe", ledAuxEnabled); preferences.putInt("lXb", ledAuxBrightness); 
  preferences.putBool("lAe", ledAmPmEnabled); preferences.putInt("lAb", ledAmPmBrightness);
  preferences.end();

  configTzTime(timeZoneString.c_str(), "pool.ntp.org", "time.nist.gov");
  stepperHours.setMaxSpeed(motorMaxSpeed); stepperMinutes.setMaxSpeed(motorMaxSpeed);
  if(!powerSaverEnabled) { stepperHours.enableOutputs(); stepperMinutes.enableOutputs(); }
  server.sendHeader("Location", "/"); server.send(303);
}

void handleManual() {
  if (server.hasArg("h") && server.hasArg("m")) {
    manualMode = true; manualHourTarget = server.arg("h").toInt(); manualMinuteTarget = server.arg("m").toInt();
  }
  server.send(200, "text/plain", "OK");
}

void handleResume() { manualMode = false; server.send(200, "text/plain", "OK"); }
void handleResetWifi() { server.send(200, "text/plain", "Resetting WiFi..."); WiFiManager wm; wm.resetSettings(); delay(1000); ESP.restart(); }
void handleRestart() { server.send(200, "text/plain", "Restarting..."); delay(1000); ESP.restart(); }
void handleResetCal() {
    preferences.begin("clock-conf", false); preferences.remove("stepsRev"); preferences.remove("stepsRevH"); preferences.end();
    server.send(200, "text/plain", "Calibration Reset. Restarting..."); delay(1000); ESP.restart();
}

// ==========================================
//              SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  esp_task_wdt_init(WDT_TIMEOUT, true); esp_task_wdt_add(NULL);

  preferences.begin("clock-conf", true);
  is12Hour = preferences.getBool("12h", false);
  timeZoneString = preferences.getString("tz", "EST5EDT,M3.2.0,M11.1.0"); 
  powerSaverEnabled = preferences.getBool("idle", false);
  motorMaxSpeed = preferences.getInt("spd", 1000);
  sensorSensitivity = preferences.getInt("sens", 50);
  nightModeEnabled = preferences.getBool("nEn", false);
  nightStartHour = preferences.getInt("nSt", 22); nightEndHour = preferences.getInt("nEd", 7);
  autoHomeIntervalHours = preferences.getInt("homeInt", 0);
  dateDisplayEnabled = preferences.getBool("dEn", false);
  dateIntervalMinutes = preferences.getInt("dInt", 5);
  dateDurationSeconds = preferences.getInt("dDur", 5);
  baselineHours = preferences.getInt("baseH", 1800); baselineMinutes = preferences.getInt("baseM", 1800);
  stepsPerRevolution = preferences.getInt("stepsRev", 2048);
  stepsPerRevolutionHours = preferences.getInt("stepsRevH", 2048);
  stepsPerUnit = stepsPerRevolution / 60.0; stepsPerUnitHours = stepsPerRevolutionHours / 60.0;
  
  ledStatusEnabled = preferences.getBool("lSe", true); ledStatusBrightness = preferences.getInt("lSb", 255);
  ledColonEnabled = preferences.getBool("lCe", true); ledColonBrightness = preferences.getInt("lCb", 255);
  ledAuxEnabled = preferences.getBool("lXe", true); ledAuxBrightness = preferences.getInt("lXb", 255); 
  ledAmPmEnabled = preferences.getBool("lAe", true); ledAmPmBrightness = preferences.getInt("lAb", 255);
  preferences.end();

  // Initialize LEDs
  ledStatus.begin(LED_STATUS_PIN, PWM_CH_STATUS, true);
  ledColon.begin(LED_COLON_PIN, PWM_CH_COLON, true); // COLON IS PWM
  ledAmPm.begin(LED_AMPM_PIN, PWM_CH_AMPM, true);
  ledAux.begin(LED_AUX_PIN, PWM_CH_AUX, true); 

  WiFiManager wm;
  wm.setAPCallback([](WiFiManager *myWiFiManager) { ledStatus.forceOn(255); });
  if (!wm.autoConnect("SplitFlapClockSetup")) { ESP.restart(); }
  blinkIpAddress(); 
  configTzTime(timeZoneString.c_str(), "pool.ntp.org", "time.nist.gov");
  
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/calib_status", handleCalibStatus); 
  server.on("/save", HTTP_POST, handleSave);
  server.on("/manual", HTTP_POST, handleManual);
  server.on("/resume", HTTP_POST, handleResume);
  server.on("/reset_wifi", handleResetWifi);
  server.on("/restart", handleRestart);
  server.on("/reset_cal", handleResetCal);
  
  // --- SPLIT CALIBRATION ENDPOINTS ---
  server.on("/calibrate_sensors", HTTP_POST, []() { 
      server.send(200, "text/plain", "OK"); 
      // measureBaseline=TRUE, countSteps=FALSE
      runHomingSequence(true, false); 
  });
  
  server.on("/calibrate_motors", HTTP_POST, []() { 
      server.send(200, "text/plain", "OK"); 
      // measureBaseline=TRUE, countSteps=TRUE
      runHomingSequence(true, true); 
  });

  server.on("/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close"); server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK"); ESP.restart();
    }, []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) { if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {} } 
      else if (upload.status == UPLOAD_FILE_WRITE) { if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {} } 
      else if (upload.status == UPLOAD_FILE_END) { if (Update.end(true)) {} }
    });

  server.begin();
  
  // Apply Acceleration on startup
  stepperHours.setAcceleration(1000);
  stepperMinutes.setAcceleration(1000);
  
  // Initial Homing: Use existing calibration (Measure=False, Count=False)
  runHomingSequence(false, false); 
  lastHomeTime = time(nullptr); // Initialize timer
}

// ==========================================
//              LOOP
// ==========================================
void loop() {
  // 1. PRIORITY: Steppers must run EVERY cycle for max speed
  stepperHours.run();
  stepperMinutes.run();

  // 2. THROTTLE: Only run WiFi, Time, and LED logic every 50ms
  // This removes the "friction" causing the motors to slow down.
  if (millis() - lastLogicLoop > 50) {
      lastLogicLoop = millis();

      server.handleClient();
      esp_task_wdt_reset();
      
      // Update LEDs
      ledColon.update(ledColonEnabled, ledColonBrightness, 500); 
      if (ledAuxEnabled) { ledAux.forceOn(ledAuxBrightness); } else { ledAux.forceOff(); }
      
      Time t = getLocalTimeData();
      if (ledAmPmEnabled && t.isPm) { ledAmPm.forceOn(ledAmPmBrightness); } else { ledAmPm.forceOff(); }

      // --- AUTO HOME LOGIC ---
      if (autoHomeIntervalHours > 0) {
          time_t now = time(nullptr);
          // Check if time is valid (> 2020) and interval has passed
          if (now > 1600000000 && (now - lastHomeTime) >= (autoHomeIntervalHours * 3600)) {
              Serial.println("Auto-Homing Triggered...");
              // Just home, do not measure baseline or count steps
              runHomingSequence(false, false); 
              lastHomeTime = time(nullptr);
              return; // Sequence is blocking, so return after
          }
      }

      // Check Night Mode
      if (isNightTime()) { 
          stepperHours.disableOutputs(); 
          stepperMinutes.disableOutputs(); 
          return; 
      }
      
      // Update Motor Targets (Clock Logic)
      if (stepperHours.distanceToGo() == 0 && stepperMinutes.distanceToGo() == 0) {
        int targetH, targetM;

        if (manualMode) { 
             targetH = manualHourTarget; targetM = manualMinuteTarget; 
        } else {
             // --- DATE DISPLAY LOGIC START ---
             unsigned long now = millis();
             // Check if we should switch TO Date mode
             if (dateDisplayEnabled && !isShowingDate && (now - lastDateShowTime > (dateIntervalMinutes * 60000))) {
                 isShowingDate = true;
                 lastDateShowTime = now;
             }
             // Check if we should switch BACK to Time mode
             if (isShowingDate && (now - lastDateShowTime > (dateDurationSeconds * 1000))) {
                 isShowingDate = false;
                 lastDateShowTime = now; // Reset timer so interval starts from now
             }
             
             struct tm timeinfo;
             if (!getLocalTime(&timeinfo)) return;

             if (isShowingDate) {
                 // Month (0-11) + 1 -> 1-12
                 targetH = timeinfo.tm_mon + 1; 
                 // Day of month (1-31)
                 targetM = timeinfo.tm_mday;   
             } else {
                 // Standard Time Logic
                 if (t.hour == -1) return;
                 targetH = t.hour; 
                 targetM = t.minute;
             }
             // --- DATE DISPLAY LOGIC END ---
        }

        bool moved = false; 
        if (targetM != currentDisplayedMinute) {
          long steps = calculateTargetPosition(currentDisplayedMinute, targetM, false);
          if (powerSaverEnabled) stepperMinutes.enableOutputs(); 
          stepperMinutes.move(steps); currentDisplayedMinute = targetM; lastMotorMoveTime = millis(); moved = true;
        }
        if (targetH != currentDisplayedHour) {
          long steps = calculateTargetPosition(currentDisplayedHour, targetH, true);
          if (powerSaverEnabled) stepperHours.enableOutputs(); 
          stepperHours.move(steps); currentDisplayedHour = targetH; lastMotorMoveTime = millis(); moved = true;
        }
      }
      
      // Disable Motors if Idle
      if (stepperHours.distanceToGo() != 0 || stepperMinutes.distanceToGo() != 0) { lastMotorMoveTime = millis(); } 
      else { if (powerSaverEnabled && (millis() - lastMotorMoveTime > 2000)) { stepperHours.disableOutputs(); stepperMinutes.disableOutputs(); } }
      
      if (powerSaverEnabled && (stepperMinutes.distanceToGo() == 0 && stepperHours.distanceToGo() == 0)) {
            // Double check before killing power
            if(millis() - lastMotorMoveTime > 2000) {
                stepperMinutes.disableOutputs(); stepperHours.disableOutputs();
            }
      }
  } // End of throttled logic
}