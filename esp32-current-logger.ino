#include <Preferences.h>
#include <WiFi.h>
#include "time.h"
#include "sntp.h"
#include "ESPAsyncWebServer.h"
#include <vector>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "esp_random.h"
#include "esp_adc_cal.h"

#include "file_operations.h"

#define SERIAL_BUFFER_SIZE 48 // maximum command length
#define SERIAL_COMMAND_DELIM ' '
#define SERIAL_COMMAND_END '\n'

/* Preference Key Strings */
#define PREF_WIFI_SSID "WIFI_SSID"
#define PREF_WIFI_PASS "WIFI_PASS"
#define PREF_MEASURE_INTERVAL "INTERVAL"
#define PREF_CURRENT_CHECKPOINT "CURRENT"
#define PREF_CHARGE_CHECKPOINT "CHARGE"
#define PREF_CURRENT_RUN "CURRUN"
#define PREF_CAL_OFF "CAL_OFF"
#define PREF_CAL_MUL "CAL_MUL"

// There will always be some degree of noise and depending on the sensitivity of the current sensor,
// this can become quite a large amount driving up the "charge counter" even when no current runs through the
// sensor.
// This defines a threshold below which the current will be forced set to zero
#define MINIMUM_CURRENT 0.2 // Amps

// GPIO35 is the pin where the voltage will be read at.
// Connect the current sensor's output pin to that.
#define ANALOG_READ_PIN 35

Preferences pref;
size_t serial_buffer_index = 0;
char serialBuffer[SERIAL_BUFFER_SIZE + 1];

const char wifi_descriptions[][7] = { "SSID: ", "Pass: " };
const char wifi_pref_names[][10] = { PREF_WIFI_SSID, PREF_WIFI_PASS };
const char* ntpServer = "pool.ntp.org";

const char* init_error_msg = "<!DOCTYPE html><html><body><h1>SD-Card initialization error.</h1></body></html>";

// time how often the current will be sampled
// the time after which a graph-point appears on the web interface is set by the "setmes" command
const int inter_measurement_interval = 1000; // every second
uint32_t graph_measurement_interval; // "setmes"
uint32_t inter_counter = 0;

AsyncWebServer server(80);
bool connected = false;
bool card_status; // False if the SD Card could not be initialized

char* wifi_ssid;
char* wifi_pass;

// Those will hold all necessary values to keept track of current and total charge
char* current_run;
double current = 0.0;
double charge = 0.0;

// No component is ideal, so many current sensors don't have their Zero point exactly at the
// center of VCC, furthermore the esp32's ADC is not perfect either
// you can either set 
float calib_offset = 0.0f; // Offset in Ampere (before calib_mul is applied)
float calib_mul = 0.0f; // Multiplier to the final current result

// Those are used when you start a calibration using one of the "startcalib" commands
uint8_t do_calib_variable = 0; // indicator of what is being calibrated (see further down for more info)
bool do_calib = false; // if calibration is in progress
int32_t do_calib_counter = 0; // counter for how many measurements have been taken
size_t do_calib_size = 0; // if counter >= do_calib_size => calibration finished
float do_calib_value = 0.0;
float do_calib_expected_value = 0.0;

bool time_initialized = false;
bool clock_active = true;
volatile bool make_measurement = false;
volatile uint32_t ntpTime = 0;
volatile uint64_t ntpArrivedAt = 0;

hw_timer_t* m_timer = NULL;
void IRAM_ATTR onTimer()
{
  make_measurement = true;
}

// This will return a unix timestamp (seconds)
uint32_t timestamp()
{
  return ntpTime + (uint32_t)((esp_timer_get_time() - ntpArrivedAt) / 1000000);
}

// Called whenever a NTP-Time-Update arrives
void time_available(struct timeval* t)
{
  ntpArrivedAt = esp_timer_get_time();
  ntpTime = t->tv_sec;
  time_initialized = true;
  Serial.print("NTP-Time-Update: ");
  Serial.println(ntpTime);
}

// Standard function to convert the 12-Bit ADC value into a voltage (millivolts) using
// builtin esp32 functions
uint32_t adc_voltage(int16_t raw_value)
{
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_characterize(ADC_UNIT_2, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  return esp_adc_cal_raw_to_voltage(raw_value, &adc_chars);
}

// appends a csv-entry to the current_run, if one is initialized
// also saves the current and charge values to preferences so that 
// in the event of an unexpected shutdown
// the values can be recovered
void save_graph_point()
{
  if(!current_run) return;
  uint32_t now = timestamp();
  pref.putDouble(PREF_CURRENT_CHECKPOINT, current);
  pref.putDouble(PREF_CHARGE_CHECKPOINT, charge);
  
  char* buffer = new char[11 + 1 + 16 + 1 + 16 + 1 + 1];
  sprintf(buffer, "%u;%.5f;%.5f\n", now, current, charge);
  write_file(SD, current_run, buffer, true);
  free(buffer);
}

// creates a new run-csv-file and resets current and charge
// runs are in the format /runs/<unix-timestamp>_<random 32-bit hex identifier>.csv
void new_run()
{
  clock_active = false;
  if(current_run) free(current_run);
  current_run = new char[6 + 11 + 1 + 8 + 4 + 1];
  uint32_t now = timestamp();
  sprintf(current_run, "/runs/%u_%08x.csv", now, esp_random());
  Serial.print(F("New run: "));
  Serial.println(current_run);
  pref.putString(PREF_CURRENT_RUN, current_run); // save path to new run, so it will still be active after a reboot
  current = 0;
  charge = 0;
  write_file(SD, current_run, "", false); // create file
  save_graph_point();
  clock_active = true;
}

void webserver_handle_reset(AsyncWebServerRequest *request)
{
  new_run();
  request->send(SD, "/reset.html", "text/html");
}

void webserver_handle_view(AsyncWebServerRequest *request)
{
  // Retrieve get parameter "run"
  size_t paramssz = request->params();
  char* runValue;
  size_t i;
  for(i = 0; i < paramssz; i++)
  {
    AsyncWebParameter* p = request->getParam(i);
    if(p->name().equals("run"))
    {
      size_t l = p->value().length();
      runValue = new char[l + 1];
      p->value().toCharArray(runValue, l + 1);
      break;
    }
  }

  // Serve file
  request->send(SD, "/run.html", String(), false, [runValue](const String& var){ // Template processor
    if(card_status && var == "FILECONTENT")
    {
      size_t runvaluesz = strlen(runValue);
      char path[6 + 4 + runvaluesz + 1];
      strcpy(path, "/runs/");
      strcpy(path + 6, runValue);
      strcpy(path + 6 + runvaluesz, ".csv");
      size_t fsz = file_size(SD, path);
      if (fsz == 0)
      {
        return String(F("File does not exist/read error."));
      }
      char* buf = new char[fsz + 1];
      read_file(SD, path, buf);
      for(size_t i = 0; i < fsz; i++)
        if(buf[i] == '\n')
          buf[i] = '|';
      String returnee = String(buf);
      free(buf);
      return returnee;
    }
    return String();
  });

  // if the for loop on the top did not run through completele, 
  // i.e. the buffer for the get parameter was allocated
  if(i != paramssz)
    free(runValue);
}

void webserver_handle_index(AsyncWebServerRequest *request)
{
  if (card_status)
  {
    request->send(SD, "/index.html", String(), false, [](const String& var){
      if(var == "RUNNAME")
      {
        return String(current_run);
      }
      if(var == "CURRENT")
      {
        char buffer[16];
        sprintf(buffer, "%.5f", current);
        return String(buffer);
      }
      if(var == "CHARGE")
      {
        char buffer[16];
        sprintf(buffer, "%.5f", charge);
        return String(buffer);
      }
      if(var == "RUNLIST")
      {
        char* file_list_buffer;
        bool ldirsuccess = list_dir_output(SD, "/runs", &file_list_buffer); // requires us to free the buffer afterwards
        if(ldirsuccess)
        {
          String returnee = String(file_list_buffer);
          free(file_list_buffer);
          return returnee;
        }
        else
        {
          return String(F("File does not exist/read error."));
        }
      }
      return String();
    });
  } else
  {
    request->send(200, "text/html", init_error_msg);
  }
}

// This will be called every inter_measurement_interval milliseconds
void update_charge_current()
{
  const int count = 20;
  float analog = 0;
  // Some averaging of the measured analog values
  for(int i = 0; i < count; i++)
  {
    analog += adc_voltage(analogRead(ANALOG_READ_PIN));
    delay(40);
  }
  analog /= count;

  // Calculate the raw, non-calibrated current value
  float tmp = ((analog / 1000.0f) - 1.65f);

  // Calibration
  if(do_calib)
  {
    do_calib_value += tmp;
    do_calib_counter++;
    if(do_calib_counter >= do_calib_size)
    {
      do_calib = false;
      do_calib_counter = 0;
      float calc = do_calib_value / do_calib_size;
      switch(do_calib_variable)
      {
        case 1:
        {
          calib_offset = do_calib_expected_value - calc;
          pref.putFloat(PREF_CAL_OFF, calib_offset);
        } break;
        case 2:
        {
          calib_mul = do_calib_expected_value / (calc + calib_offset);
          pref.putFloat(PREF_CAL_MUL, calib_mul);
        } break;
      }
      do_calib_value = 0.0;
      Serial.println(F("Finished calibration."));
    }
  }

  // Set current and increment charge
  current = ((tmp + calib_offset) * calib_mul);
  if(abs(current) < MINIMUM_CURRENT)
    current = 0;
  charge += abs(current) * (inter_measurement_interval / 1000.0);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Current Monitor");
  if (!SD.begin()) {
    card_status = false;
    Serial.println("SD-Card initialization error.");
  }
  else
  {
    card_status = true;
  }

  pinMode(ANALOG_READ_PIN, INPUT);
  analogReadResolution(12);

  pref.begin("currentmeter", false);

  // Read preferences
  String ssid = pref.getString(PREF_WIFI_SSID);
  String pass = pref.getString(PREF_WIFI_PASS);
  graph_measurement_interval = pref.getULong(PREF_MEASURE_INTERVAL); // ULong is uint32_t
  current = pref.getDouble(PREF_CURRENT_CHECKPOINT);
  charge = pref.getDouble(PREF_CHARGE_CHECKPOINT);
  calib_offset = pref.getFloat(PREF_CAL_OFF);
  calib_mul = pref.getFloat(PREF_CAL_MUL);
  String currun = pref.getString(PREF_CURRENT_RUN);
  current_run = (char*)malloc(sizeof(char) * (currun.length() + 1));
  memset(current_run, 0, sizeof(char) * (currun.length() + 1));
  currun.toCharArray(current_run, currun.length() + 1);

  wifi_ssid = (char*)malloc(sizeof(char) * (ssid.length() + 1));
  wifi_pass = (char*)malloc(sizeof(char) * (pass.length() + 1));

  memset(wifi_ssid, 0, (ssid.length() + 1) * sizeof(char));
  memset(wifi_pass, 0, (pass.length() + 1) * sizeof(char));

  ssid.toCharArray(wifi_ssid, ssid.length() + 1);
  pass.toCharArray(wifi_pass, pass.length() + 1);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);

  // Setup NTP-Polling every minute
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_set_time_sync_notification_cb(time_available);
  sntp_servermode_dhcp(1);
  sntp_set_sync_interval(60000); // ms
  sntp_setservername(0, ntpServer);
  sntp_init();
  Serial.println("Waiting for NTP-Time-Update...");

  // Setup timer for measurements
  // Prescaler 8000 => 80MHz clock / 8000 = 10000 Hz (100µS per timer overflow)
  m_timer = timerBegin(0, 8000, true);
  timerAttachInterrupt(m_timer, &onTimer, true);
  // call every inter_measurement_interval * 10 overflows
  // (inter_measurement_interval is in milliseconds)
  timerAlarmWrite(m_timer, inter_measurement_interval * 10, true);
  timerAlarmEnable(m_timer);
}

// Command Processor
void process_command(char* command, size_t size)
{
  size_t command_length = 0;
  for (; command_length < size; command_length++)
    if (command[command_length] == SERIAL_COMMAND_DELIM
        || command[command_length] == SERIAL_COMMAND_END)
      break;

  if (strncmp(command, "help", command_length) == 0)
  {
    Serial.println(F("--- HELP"));
    Serial.println(F("help | Displays this help"));
    Serial.println(F("wifi <SSID> <pwd> | Sets WIFI SSID and password"));
    Serial.println(F("printwifi | Display Wifi settings"));
    Serial.println(F("wifistatus | Display Wifi status"));
    Serial.println(F("setmes <interval> | Set Graph measurement interval (in seconds)"));
    Serial.println(F("getmes | Display Graph measurement interval"));
    Serial.println(F("setoff <voltage> | Set current sensor output offset"));
    Serial.println(F("setmul <factor> | Set current sensor output coefficient"));
    Serial.println(F("getcalib | Display current sensor calibration values (offset and coefficient)"));
    Serial.println(F("startcalib1 <count> <aref> | Calibrate Offset (aref is applied current)"));
    Serial.println(F("startcalib2 <count> <aref> | Calibrate Coefficient (aref is applied current)"));
    Serial.println();
  } else if (strncmp(command, "wifi", command_length) == 0)
  {
    size_t seg_length;
    size_t start = command_length + 1;
    Serial.println(F("Changing WIFI Settings to (Requires restart to apply): "));
    for (int i = 0; i < 2; i++)
    {
      for (seg_length = 0; seg_length + start < size; seg_length++)
        if (command[seg_length + start] == SERIAL_COMMAND_DELIM
            || command[seg_length + start] == SERIAL_COMMAND_END)
          break;

      Serial.print(wifi_descriptions[i]);
      for (size_t k = 0; k < seg_length; k++)
        Serial.print(command[k + start]);
      command[seg_length + start] = '\0';
      pref.putString(wifi_pref_names[i], command + start);
      Serial.println();

      start += seg_length + 1;
    }
    Serial.println();
  } else if (strncmp(command, "printwifi", command_length) == 0)
  {
    Serial.println(F("Current WIFI Settings: "));
    Serial.print(wifi_descriptions[0]);
    Serial.println(pref.getString(PREF_WIFI_SSID));
    Serial.print(wifi_descriptions[1]);
    Serial.println(pref.getString(PREF_WIFI_PASS));
    Serial.println();
  }
  else if (strncmp(command, "wifistatus", command_length) == 0)
  {
    Serial.print(F("Current WIFI status: "));
    switch (WiFi.status())
    {
      case WL_CONNECTED:
        Serial.print("Connected");
        break;
      case WL_CONNECT_FAILED:
        Serial.print("Connection Failed");
        break;
      case WL_CONNECTION_LOST:
        Serial.print("Connection Lost");
        break;
      case WL_DISCONNECTED:
        Serial.print("Disconnected");
        break;
      case WL_NO_SHIELD:
        Serial.print("Not supported by device");
        break;
      case WL_NO_SSID_AVAIL:
        Serial.print("SSID not available");
        break;
      default:
        Serial.print("Other Error");
        break;
    }
    Serial.println();
    Serial.print(F("IP-Address: "));
    Serial.println(WiFi.localIP());
  } else if (strncmp(command, "getmes", command_length) == 0)
  {
    Serial.println();
    Serial.print(F("Measurement interval: "));
    Serial.print(graph_measurement_interval);
    Serial.println(F(" seconds"));
  } else if (strncmp(command, "setmes", command_length) == 0)
  {
    // This code for converting the command argument to a number is used very often
    // should be put into its own function
    size_t seg_length;
    size_t start = command_length + 1;
    for (seg_length = 0; seg_length + start < size; seg_length++)
      if (command[seg_length + start] == SERIAL_COMMAND_DELIM
          || command[seg_length + start] == SERIAL_COMMAND_END)
        break;
    command[seg_length + start] = '\0';
    String str(command + start);
    uint32_t val = str.toInt();
    graph_measurement_interval = val;
    pref.putULong(PREF_MEASURE_INTERVAL, val);

    Serial.println();
    Serial.print(F("Changed measurement interval to "));
    Serial.print(val);
    Serial.println(F(" seconds."));
  }
  else if (strncmp(command, "setoff", command_length) == 0)
  {
    size_t seg_length;
    size_t start = command_length + 1;
    for (seg_length = 0; seg_length + start < size; seg_length++)
      if (command[seg_length + start] == SERIAL_COMMAND_DELIM
          || command[seg_length + start] == SERIAL_COMMAND_END)
        break;
    command[seg_length + start] = '\0';
    String str(command + start);
    float val = str.toFloat();
    calib_offset = val;
    pref.putFloat(PREF_CAL_OFF, val);

    Serial.println();
    Serial.print(F("Changed offset to "));
    Serial.print(val);
    Serial.println(F(" A."));
  }
  else if (strncmp(command, "setmul", command_length) == 0)
  {
    size_t seg_length;
    size_t start = command_length + 1;
    for (seg_length = 0; seg_length + start < size; seg_length++)
      if (command[seg_length + start] == SERIAL_COMMAND_DELIM
          || command[seg_length + start] == SERIAL_COMMAND_END)
        break;
    command[seg_length + start] = '\0';
    String str(command + start);
    float val = str.toFloat();
    calib_mul = val;
    pref.putFloat(PREF_CAL_MUL, val);

    Serial.println();
    Serial.print(F("Changed coefficient to "));
    Serial.println(val);
  } else if (strncmp(command, "getcalib", command_length) == 0)
  {
    Serial.println();
    Serial.print(F("Offset: "));
    Serial.print(calib_offset);
    Serial.print(F(" A; Coefficient: "));
    Serial.println(calib_mul);
  } else if (strncmp(command, "startcalib", command_length - 1) == 0)
  {
    if(do_calib)
    {
      Serial.println(F("Already calibrating!"));
      return;
    }

    char var = command[command_length - 1];
    
    size_t seg_length;
    size_t start = command_length + 1;
    Serial.println(F("Starting calibration. Make sure to keep current constant!"));
    float values[2];
    for (int i = 0; i < 2; i++)
    {
      for (seg_length = 0; seg_length + start < size; seg_length++)
        if (command[seg_length + start] == SERIAL_COMMAND_DELIM
            || command[seg_length + start] == SERIAL_COMMAND_END)
          break;

      command[seg_length + start] = '\0';

      values[i] = String(command + start).toFloat();

      start += seg_length + 1;
    }
    size_t steps = (size_t)values[0];
    float reference = values[1];
    
    Serial.printf("Calibrating with %u steps and a current reference of %.2f A.\n", steps, reference);
    do_calib_expected_value = reference;
    do_calib_size = steps;
    if(var == '1')
    {
      do_calib_variable = 1;
      do_calib = true;
    } else if(var == '2')
    {
      do_calib_variable = 2;
      do_calib = true;
    }
    else
    {
      Serial.println(F("Calibration variable not found."));
    }
  }
  else
  {
    Serial.println(F("Command not found"));
    Serial.println();
  }
}

void process_serial_input()
{
  while (Serial.available())
  {
    char c = Serial.read();
    serialBuffer[serial_buffer_index++] = c;

    if (c == SERIAL_COMMAND_END)
    {
      process_command(serialBuffer, serial_buffer_index);
      serial_buffer_index = 0;
    }

    // prevent buffer overflows
    if (serial_buffer_index >= SERIAL_BUFFER_SIZE)
    {
      Serial.println("Command too long. Try again.");
      serial_buffer_index = 0;
    }
  }
}

void loop() {
  process_serial_input(); // Commands

  // Nothing will work before the Time has been updated through NTP
  if(time_initialized)
  {
    if (!connected && WiFi.status() == WL_CONNECTED)
    {
      connected = true;
      
      server.on("/", HTTP_GET, webserver_handle_index);
      server.on("/reset", HTTP_GET, webserver_handle_reset);
      server.on("/view", HTTP_GET, webserver_handle_view);
      server.begin();
    }
  
    if (connected && WiFi.status() != WL_CONNECTED)
      connected = false;
  
    if(make_measurement)
    {
      make_measurement = false;
      update_charge_current();
      
      inter_counter++;
      uint32_t thresh = graph_measurement_interval * 1000 / inter_measurement_interval;
      if(inter_counter >= thresh)
      {
        inter_counter = 0;
        save_graph_point();
        Serial.printf("Graph-Point at: %u\n", timestamp());
      }
      Serial.printf("Measurement: current=%f; charge=%f\n", current, charge);
    }
  }
  delay(10);
}
