// #include "Arduino.h"

#include <WiFiManager.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <JsonStreamingParser.h>
#include <PNGdec.h>
#include <StreamString.h>
#include "rc_client.h"

#include <WiFi.h>
#include <HTTPClient.h>

#include "SPI.h"
#include <TFT_eSPI.h>

#include "HardwareSerial.h"

// Include LittleFS
#include <FS.h>
#include "LittleFS.h"
#define FileSys LittleFS

#include <time.h>

#define DEBUG 0

#define EEPROM_SIZE 2048
#define EEPROM_ID_1 142
#define EEPROM_ID_2 208

#define MUX 3
#define MUX_ENABLE_BUS HIGH
#define MUX_DISABLE_BUS LOW

#define FORMAT_LITTLEFS_IF_FAILED true

// send serial data into chunks with a delay
#define CHUNK_SIZE 32
#define TX_DELAY_MS 5

/** 
 * defines to use a lambda function to shrink  the JSON response with the list of achievements
 * remember: you need to deploy the lambda and inform its URL
 */

 #define ENABLE_SHRINK_LAMBDA 0 // 0 - disable / 1 - enable
 #define SHRINK_LAMBDA_URL "https://xxxxxxxxxx.execute-api.us-east-1.amazonaws.com/default/NES_RA_ADAPTER?"

/*
 * FIFO for achievements the user won
 */

#define FIFO_SIZE 5

typedef struct
{
  uint32_t id;
  String title;
  String url;
} achievements_t;

typedef struct
{
  achievements_t buffer[FIFO_SIZE];
  int head;
  int tail;
  int count;
} FIFO_t;

// #define MUX_ENABLE_BUS LOW
// #define MUX_DISABLE_BUS HIGH

#define MAX_IMAGE_WIDTH 240 // Adjust for your images
PNG png;                    // PNG decoder instance

int16_t xpos = 0;
int16_t ypos = 0;

TFT_eSPI tft = TFT_eSPI(); // Invoke custom library

String base_url = "https://retroachievements.org/dorequest.php?";
String ra_user_token = "";

int state = 255; // TODO: change to enum

String buffer = "";
String md5 = "";
StaticJsonDocument<512> jsonDoc;

/*
 * network client
 */

NetworkClientSecure client;
NetworkClient clientDebug;
HTTPClient https;

/*

CRC MD5

*/

String getMD5(String crc, bool firstBank)
{
  const char *filePath = "/games.txt";

  File file = LittleFS.open(filePath, "r");
  if (!file)
  {
    Serial.println("Error opening file");
    return "";
  }

  String line;
  while (file.available())
  {
    line = file.readStringUntil('\n'); // Lê uma linha do arquivo
    int indexAfterComma = line.indexOf(',');
    int indexAfterEqualSign = line.indexOf('=');

    if (indexAfterComma != -1 && indexAfterEqualSign != -1)
    {
      String crc1 = line.substring(0, indexAfterComma);
      String crc2 = line.substring(indexAfterComma + 1, indexAfterEqualSign);
      String md5 = line.substring(indexAfterEqualSign + 1);

      if ( (firstBank && crc1.equalsIgnoreCase(crc)) || (!firstBank &&crc2.equalsIgnoreCase(crc)))
      {
        file.close();
        return md5; // Retorna o MD5 se um dos CRCs for encontrado
      }
    }
  }

  file.close();
  return ""; // Retorna string vazia se não encontrar
}

/*
 * json
 */

void remove_space_new_lines_in_json(String &json)
{
  bool insideQuotes = false;
  int writeIndex = 0;

  for (int readIndex = 0; readIndex < json.length(); readIndex++)
  {
    char c = json[readIndex];

    if (c == '"')
    {
      if (json[readIndex - 1] != '\\')
        insideQuotes = !insideQuotes;
    }

    if (insideQuotes || (c != ' ' && c != '\n' && c != '\r' && c != '\t'))
    {
      json[writeIndex++] = c;
    }
  }

  json.remove(writeIndex); // Reduz o tamanho da string sem cópias extras
}

void remove_not_nested_json_field(String &json, const String &field_to_remove)
{
  remove_space_new_lines_in_json(json);

  bool insideQuotes = false;
  bool insideArray = false;
  bool skipField = false;
  int fieldLen = field_to_remove.length();
  int readIndex = 0, writeIndex = 0, skipInit = 0;

  while (readIndex < json.length())
  {
    char c = json[readIndex];

    if (c == '"')
    {
      // if (json[readIndex - 1] != '\\')
      insideQuotes = !insideQuotes;
    }

    if (c == '[' && skipField)
    {
      insideArray = true;
    }
    if (c == ']' && skipField)
    {
      insideArray = false;
    }

    if (insideQuotes && json.substring(readIndex + 1, readIndex + 1 + fieldLen) == field_to_remove && json[readIndex + fieldLen + 1] == '"')
    {
      skipField = true;
      skipInit = readIndex;
    }

    if (!skipField)
    {
      json[writeIndex++] = c;
    }

    if (skipField && json[readIndex + 1] == '}')
    {
      skipField = false; // Fim do campo a ser removido
      if (json[skipInit - 1] == ',')
      {
        writeIndex--; // Remove a vírgula que sobrou
      }
    }
    else if (skipField && json[readIndex] == ',' && (insideArray == false && insideQuotes == false))
    {
      skipField = false; // Fim do campo a ser removido
    }

    readIndex++;
  }

  json.remove(writeIndex);
}

void clean_json_field_str_value(String &json, const String &field_to_remove)
{
  remove_space_new_lines_in_json(json);

  bool insideQuotes = false;
  bool insideArray = false;
  bool skipField = false;
  int fieldLen = field_to_remove.length();
  int readIndex = 0, writeIndex = 0, skipInit = 0;
  bool removeNextStr = false;
  while (readIndex < json.length())
  {
    char c = json[readIndex];

    if (c == '"')
    {
      if (json[readIndex - 1] != '\\')
      {
        insideQuotes = !insideQuotes;
        if (insideQuotes && removeNextStr)
        {
          skipField = true;
          json[writeIndex++] = '"';
        }
        if (!insideQuotes && removeNextStr && readIndex > skipInit)
        {
          removeNextStr = false;
          skipField = false;
        }
      }
    }

    if (insideQuotes && json.substring(readIndex + 1, readIndex + 1 + fieldLen) == field_to_remove && json[readIndex + fieldLen + 1] == '"')
    {
      removeNextStr = true;
      skipInit = readIndex + fieldLen + 2;
    }

    if (!skipField)
    {
      json[writeIndex++] = c;
    }
    readIndex++;
  }

  json.remove(writeIndex);
}

/*
 * png
 */

File pngfile;

void *pngOpen(const char *filename, int32_t *size)
{
  Serial.printf("Attempting to open %s\n", filename);
  pngfile = FileSys.open(filename, "r");
  *size = pngfile.size();
  return &pngfile;
}

void pngClose(void *handle)
{
  File pngfile = *((File *)handle);
  if (pngfile)
    pngfile.close();
}

int32_t pngRead(PNGFILE *page, uint8_t *buffer, int32_t length)
{
  if (!pngfile)
    return 0;
  page = page; // Avoid warning
  return pngfile.read(buffer, length);
}

int32_t pngSeek(PNGFILE *page, int32_t position)
{
  if (!pngfile)
    return 0;
  page = page; // Avoid warning
  return pngfile.seek(position);
}

/*
 * buzzer
 */

#define NOTE_CS6 1109
#define NOTE_D6 1175
#define NOTE_D5 587
#define NOTE_C4 262
#define NOTE_G4 392
#define NOTE_G5 784
#define NOTE_A5 880
#define NOTE_B5 988
#define NOTE_E6 1319
#define NOTE_G6 1568
#define NOTE_C7 2093
#define NOTE_D7 2349
#define NOTE_E7 2637
#define NOTE_F7 2794
#define NOTE_G7 3136

int snd_notes_achievement_unlocked[] = {
    NOTE_D5, NOTE_D5, NOTE_CS6, NOTE_D6};

int snd_velocity_achievement_unlocked[] = {
    52,
    37,
    83,
    74,
};

int snd_notes_duration_achievement_unlocked[] = {
    2,
    4,
    3,
    12,
};

#define SOUND_PIN 10

/*
 * rcheevos
 */

typedef struct
{
  rc_client_server_callback_t callback;
  void *callback_data;
} async_callback_data;

rc_client_t *g_client = NULL;
static void *g_callback_userdata = &g_client; /* dummy data */
char rcheevos_userdata[16];
async_callback_data async_data;

uint16_t *unique_memory_addresses = NULL;
uint16_t unique_memory_addresses_count = 0;
uint8_t *memory_data = NULL;

String gameName = "not identified";
String gameId = "0";
bool comebackToTitleScreen = false;

bool alreadyShowedTitleScreen = false;

long comebackToTitleScreenTS;

WiFiManager wm;

// time that reset button should be pressed to kick off reset operation
#define RESET_PRESSED_TIME 5000L

// pin to the reset button
#define RESET_PIN 8

#define ENABLE_RESET 0

void fifo_init(FIFO_t *fifo)
{
  fifo->head = 0;
  fifo->tail = 0;
  fifo->count = 0;
}

bool fifo_is_empty(FIFO_t *fifo)
{
  return fifo->count == 0;
}

bool fifo_is_full(FIFO_t *fifo)
{
  return fifo->count == FIFO_SIZE;
}

bool fifo_enqueue(FIFO_t *fifo, achievements_t value)
{
  if (fifo_is_full(fifo))
  {
    return false; // FIFO cheia
  }
  fifo->buffer[fifo->tail] = value;
  fifo->tail = (fifo->tail + 1) % FIFO_SIZE;
  fifo->count++;
  return true;
}

bool fifo_dequeue(FIFO_t *fifo, achievements_t *value)
{
  if (fifo_is_empty(fifo))
  {
    return false; // FIFO vazia
  }
  *value = fifo->buffer[fifo->head];
  fifo->head = (fifo->head + 1) % FIFO_SIZE;
  fifo->count--;
  return true;
}

FIFO_t achievements_fifo;

void play_attention_sound()
{
  delay(35);
  tone(SOUND_PIN, NOTE_G4, 35);
  delay(35);
  tone(SOUND_PIN, NOTE_G5, 35);
  delay(35);
  tone(SOUND_PIN, NOTE_G6, 35);
  delay(35);
  noTone(SOUND_PIN);
  tone(SOUND_PIN, NOTE_G4, 35);
  delay(35);
  tone(SOUND_PIN, NOTE_G5, 35);
  delay(35);
  tone(SOUND_PIN, NOTE_G6, 35);
  delay(35);
  noTone(SOUND_PIN);
}

void play_success_sound()
{

  delay(130);
  tone(SOUND_PIN, NOTE_E6, 125);
  delay(130);
  tone(SOUND_PIN, NOTE_G6, 125);
  delay(130);
  tone(SOUND_PIN, NOTE_E7, 125);
  delay(130);
  tone(SOUND_PIN, NOTE_C7, 125);
  delay(130);
  tone(SOUND_PIN, NOTE_D7, 125);
  delay(130);
  tone(SOUND_PIN, NOTE_G7, 125);
  delay(125);
  noTone(SOUND_PIN);
}

void play_error_sound()
{
  delay(100);
  tone(SOUND_PIN, NOTE_G4);
  delay(250);
  tone(SOUND_PIN, NOTE_C4);
  delay(500);
  noTone(SOUND_PIN);
}

void play_sound_achievement_unlocked()
{
  for (int thisNote = 0; thisNote < 4; thisNote++)
  {

    int noteDuration = snd_notes_duration_achievement_unlocked[thisNote] * 16;
    tone(SOUND_PIN, snd_notes_achievement_unlocked[thisNote], snd_velocity_achievement_unlocked[thisNote]);
    delay(noteDuration);
    // stop the tone playing:
    noTone(SOUND_PIN);
  }
}

void print_line(String text, int line, int line_status)
{
  print_line(text, line, line_status, 0);
}

void print_line(String text, int line, int line_status, int delta)
{
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
  tft.setCursor(20, 90 + line * 22, 2);
  tft.println("                                 ");
  tft.setCursor(46 + delta, 90 + line * 22, 2);
  if (text.length() == 0)
  {
    return;
  }
  tft.println(text);
  if (line_status == 2)
  {
    tft.fillCircle(32, 98 + line * 22, 7, TFT_RED);
  }
  else if (line_status == 1)
  {
    tft.fillCircle(32, 98 + line * 22, 7, TFT_YELLOW);
  }
  else if (line_status == 0)
  {
    tft.fillCircle(32, 98 + line * 22, 7, TFT_GREEN);
  }
}

void beginEEPROM(bool force)
{
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(0) != EEPROM_ID_1 || EEPROM.read(1) != EEPROM_ID_2 || force == true)
  {
    EEPROM.write(0, EEPROM_ID_1);
    EEPROM.write(1, EEPROM_ID_2);
    for (int i = 2; i < EEPROM_SIZE; i++)
    {
      EEPROM.write(i, 0);
    }
    EEPROM.commit();
    Serial.print("eeprom initialized");
  }
}

bool isConfigured()
{
  return EEPROM.read(2) == 1;
}

// custom paragraph and parameters to the configuration page
const char head[] = "<style>#l,#i,#z{text-align:center}#i,#z{margin:15px auto}button{background-color:#0000FF;}#l{margin:0 auto;width:100%; font-size: 28px;}p{margin-bottom:-5px}[type='checkbox']{height: 20px;width: 20px;}</style><script>var w=window,d=document,e=\"password\";function iB(a,b,c){a.insertBefore(b,c)}function gE(a){return d.getElementById(a)}function cE(a){return d.createElement(a)};\"http://192.168.1.1/\"==w.location.href&&(w.location.href+=\"wifi\");</script>\0";
const char html_p1[] = "<p id='z' style='width: 80%;'>Enter the RetroAchievements credentials below:</p>\0";
const char html_p2[] = "<p>&#8226; RetroAchievements user name: </p>\0";
const char html_p3[] = "<p>&#8226; RetroAchievements user password: </p>\0";

const char html_s[] = "<script>gE(\"s\").required=!0;l=cE(\"div\");l.innerHTML=\"NES RetroAchievements Adapter\",l.id=\"l\";m=d.body.childNodes[0];iB(m,l,m.childNodes[0]);p=cE(\"p\");p.id=\"i\",p.innerHTML=\"Choose the network you want to connect with:\",iB(m,p,m.childNodes[1]);</script>\0";

WiFiManagerParameter custom_p1(html_p1);
WiFiManagerParameter custom_p2(html_p2);
WiFiManagerParameter custom_param_1("un", NULL, "", 24, " required autocomplete='off'");
WiFiManagerParameter custom_p3(html_p3);
WiFiManagerParameter custom_param_2("up", NULL, "", 14, " type='password' required");
WiFiManagerParameter custom_s(html_s);

void configureWifiManager()
{
  wm.setBreakAfterConfig(true);
  wm.setCaptivePortalEnable(true);
  wm.setMinimumSignalQuality(40);
  wm.setConnectTimeout(30);
  wm.addParameter(&custom_p1);
  wm.addParameter(&custom_p2);
  wm.addParameter(&custom_param_1);
  wm.addParameter(&custom_p3);
  wm.addParameter(&custom_param_2);
  wm.addParameter(&custom_s);
  wm.setCustomHeadElement(head);
  wm.setDarkMode(true);
  wm.setAPStaticIPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
}

void clean_screen_text()
{
  print_line("", 0, 0);
  print_line("", 1, 0);
  print_line("", 2, 0);
  print_line("", 3, 0);
  print_line("", 4, 0);
}

String try_login_RA(String ra_user, String ra_pass)
{
  String response = "";
  DeserializationError error;
  int httpCode;
  client.setInsecure();
  String loginPath = "r=login&u=@USER@&p=@PASS@";
  loginPath.replace("@USER@", ra_user);
  loginPath.replace("@PASS@", ra_pass);
  https.begin(client, base_url + loginPath);

  const char *headerKeys[] = {"date"};                                       // The only header I care about.
  const size_t headerKeysCount = sizeof(headerKeys) / sizeof(headerKeys[0]); // Returns 1
  https.collectHeaders(headerKeys, headerKeysCount);

  httpCode = https.GET();
  // TODO use this timestamp to control temp files on littleFS
  String dateHeader = https.header("date");
  Serial.println("Header Date: " + dateHeader);

  if (httpCode != HTTP_CODE_OK)
  {
    state = 253; // error - login failed
    Serial0.print("ERROR=253-LOGIN_FAILED\r\n");
    Serial.print("ERROR=253-LOGIN_FAILED\r\n");
    return String("null");
  }
  response = https.getString();

  https.end();

  error = deserializeJson(jsonDoc, response);

  if (error)
  {
    state = 252; // error - json parse login failed
    Serial0.print("ERROR=252-JSON_PARSE_ERROR\r\n");
    Serial.print("ERROR=252-JSON_PARSE_ERROR\r\n");
    return String("null");
  }

  String ra_token(jsonDoc["Token"]);
  return ra_token;
}

void save_configuration_info_eeprom(String ra_user, String ra_pass)
{
  int user_len = ra_user.length();
  int pass_len = ra_pass.length();
  EEPROM.write(2, 1);
  EEPROM.write(3, user_len);
  for (int i = 0; i < user_len; i++)
  {
    EEPROM.write(4 + i, ra_user[i]);
  }
  EEPROM.write(4 + user_len, pass_len);
  for (int i = 0; i < pass_len; i++)
  {
    EEPROM.write(5 + user_len + i, ra_pass[i]);
  }
  EEPROM.write(6 + user_len + pass_len, 0);
  EEPROM.commit();
}

String read_ra_user_from_eeprom()
{
  int len = EEPROM.read(3);
  String ra_user = "";
  for (int i = 0; i < len; i++)
  {
    ra_user += (char)EEPROM.read(4 + i);
  }
  return ra_user;
}

String read_ra_pass_from_eeprom()
{
  int ra_user_len = EEPROM.read(3);
  int len = EEPROM.read(4 + ra_user_len);
  String ra_pass = "";
  for (int i = 0; i < len; i++)
  {
    ra_pass += (char)EEPROM.read(5 + ra_user_len + i);
  }
  return ra_pass;
}

// handle the reset
void handle_reset()
{
  if (ENABLE_RESET == 0)
  {
    return;
  }
  unsigned long start = millis();
  while (digitalRead(RESET_PIN) == LOW && (millis() - start) < 10000)
  {
    yield();
    print_line("Resetting in progress...", 0, 1);
  }
  if ((millis() - start) >= RESET_PRESSED_TIME)
  {

    Serial.print("reset");
    beginEEPROM(true);
    print_line("Reset successful!", 1, 0);
    print_line("Reboot in 5 seconds...", 2, 0);
    delay(5000);
    ESP.restart();
  }
  print_line("Reset aborted!", 0, 1);
}

// Fetch a file from the URL given and save it in LittleFS
// Return 1 if a web fetch was needed or 0 if file already exists
bool download_file_to_littleFS(String url, String filename)
{

  // If it exists then no need to fetch it
  if (LittleFS.exists(filename) == true)
  {
    Serial.print("Found " + filename + " in LittleFS\n");
    return 0;
  }

  Serial.print("Downloading " + filename + " from " + url + "\n");

  // Check WiFi connection
  if ((WiFi.status() == WL_CONNECTED))
  {

    HTTPClient http;
    client.setInsecure();
    http.begin(client, url);

    // Start connection and send HTTP header
    int httpCode = http.GET();
    if (httpCode == 200)
    {
      fs::File f = LittleFS.open(filename, "w+");
      if (!f)
      {
        Serial.print("file open failed\n");
        return 0;
      }
            
      // File found at server
      if (httpCode == HTTP_CODE_OK)
      {

        // Get length of document (is -1 when Server sends no Content-Length header)
        int total = http.getSize();
        int len = total;

        // Create buffer for read
        uint8_t buff[128] = {0};

        // Get tcp stream
        WiFiClient *stream = http.getStreamPtr();

        // Read all data from server
        while (http.connected() && (len > 0 || len == -1))
        {
          // Get available data size
          size_t size = stream->available();

          if (size)
          {
            // Read up to 128 bytes
            int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));

            // Write it to file
            f.write(buff, c);

            // Calculate remaining bytes
            if (len > 0)
            {
              len -= c;
            }
          }
          yield();
        }
      }
      f.close();
    }
    else
    {
      Serial.printf("download failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
  return 1; // File was fetched from web
}

void setup()
{
  // memset(str_buffer, 0, STRING_BUFFER_SIZE);
  // config mux pin
  pinMode(MUX, OUTPUT);
  digitalWrite(MUX, MUX_DISABLE_BUS); // isolate the cartridge

  // config reset pin
  pinMode(RESET_PIN, INPUT);

  beginEEPROM(false);

  fifo_init(&achievements_fifo);

  configureWifiManager();

  tft.begin();
  Serial.begin(9600);
  Serial0.setTimeout(2); // 2ms timeout for Serial0
  Serial0.begin(115200);
  delay(250);

  // Initialise LittleFS
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED))
  {
    Serial.print("LittleFS initialisation failed!");
    while (1)
      yield(); // Stay here twiddling thumbs waiting
  }

  Serial0.print("VERSION=0.8\r\n");
  Serial0.print("RESET\r\n");
  delay(250);

  tft.fillScreen(TFT_YELLOW);
  tft.setTextColor(TFT_BLACK, TFT_YELLOW, true);

  tft.setCursor(10, 10, 4);
  tft.setTextSize(1);
  tft.println("RetroAchievements");
  tft.setCursor(140, 40, 4);
  tft.println("Adapter");

  tft.fillRoundRect(16, 76, 208, 128, 15, TFT_BLUE);
  tft.fillRoundRect(20, 80, 200, 120, 12, TFT_BLACK);

  tft.setCursor(75, 220, 2);
  tft.println("by Odelot & GH");

  handle_reset();

  int wifi_configuration_tries = 0;
  bool wifi_configured = false;
  bool configured = isConfigured();
  if (!configured)
  {
    while (!configured)
    {
      if (wifi_configuration_tries == 0 && !wifi_configured)
      {
        print_line(" Configure the Adapter:", 0, 1);
        print_line("Connect to its wifi network", 1, -1, -16);
        print_line("named \"SNES_RA_ADAPTER\",", 2, -1, -16);
        print_line("password: 12345678, and ", 3, -1, -16);
        print_line("then open http://192.186.1.1", 4, -1, -16);
        play_attention_sound();
      }
      else if (wifi_configuration_tries > 0 && !wifi_configured)
      {
        print_line("Could not connect to wifi", 0, 2);
        print_line("network!", 1, -1, 16);
        print_line("check it and try again", 3, -1, 16);
        play_error_sound();
      }
      else if (wifi_configured)
      {
        print_line("Could not log in", 0, 2);
        print_line("RetroAchievements!", 1, -1, 16);
        print_line("check the credentials", 3, -1, 16);
        print_line("and try again", 4, -1, 16);
        play_error_sound();
      }
      if (wm.startConfigPortal("SNES_RA_ADAPTER", "12345678"))
      {
        Serial.print("connected...yeey :)");
        wifi_configured = true;
        clean_screen_text();
        print_line("Wifi OK!", 0, 0);
        String temp_ra_user = custom_param_1.getValue();
        String temp_ra_pass = custom_param_2.getValue();
        ra_user_token = try_login_RA(temp_ra_user, temp_ra_pass);
        Serial.print(ra_user_token);
        if (ra_user_token.compareTo("null") != 0)
        {
          print_line("Logged in RA!", 1, 0);
          play_success_sound();
          Serial.println("saving configuration info in eeprom");
          save_configuration_info_eeprom(temp_ra_user, temp_ra_pass);
        }
        else
        {
          Serial.print("could not log in RA");
          clean_screen_text();
        }
      }
      else
      {
        Serial.print("not connected...booo :(");
        clean_screen_text();
        wifi_configuration_tries += 1;
      }
      configured = isConfigured();
    }
  }
  else
  {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_15dBm);
    print_line("Connecting to Wifi...", 0, 1);
    if (WiFi.status() != WL_CONNECTED)
    {
      WiFi.begin();
      while (WiFi.status() != WL_CONNECTED)
        yield();
    }
    print_line("Wifi OK!", 0, 0); // TODO: implement timeout
    String ra_user = read_ra_user_from_eeprom();
    String ra_pass = read_ra_pass_from_eeprom();
    print_line("Logging in RA...", 1, 1);
    ra_user_token = try_login_RA(ra_user, ra_pass);
    if (ra_user_token.compareTo("null") != 0)
    {
      print_line("Logged in RA!", 1, 0);
      play_success_sound();
    }
    else
    {
      print_line("Could not log in RA!", 1, 2);
      print_line("Consider reset the adapter", 3, -1, 16);
      play_error_sound();
    }
  }
  char token_and_user[512];
  sprintf(token_and_user, "TOKEN_AND_USER=%s,%s\r\n", ra_user_token.c_str(), read_ra_user_from_eeprom().c_str());
  Serial0.print(token_and_user);
  Serial.print(token_and_user);          // DEBUG
  Serial.print("TFT_INIT\r\n"); // DEBUG
  delay(250);
  state = 0;
  Serial.print("setup end");
  // pico sends a random char by Serial or maybe it is a esp32c3 supermini bug
  if (Serial0.available() > 0)
  {
    Serial0.readString();
  }
}

void show_title_screen()
{
  tft.setTextColor(TFT_BLACK, TFT_YELLOW, true);

  tft.setTextSize(1);
  tft.setTextPadding(240);
  tft.drawString("", 0, 10, 4);
  tft.drawString("", 0, 40, 4);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(gameName, 120, 40, 4);
  tft.setTextPadding(0);

  tft.fillRoundRect(20, 80, 200, 120, 12, TFT_BLACK);
  xpos = 72;
  ypos = 92;
  String fileName = "/title_" + gameId + ".png";
  int16_t rc = png.open(fileName.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw);
  if (rc == PNG_SUCCESS)
  {
    // Serial.print("Successfully opened png file"); // DEBUG
    // Serial.printf("image specs: (%d x %d), %d bpp, pixel type: %d\n", png.getWidth(), png.getHeight(), png.getBpp(), png.getPixelType()); // DEBUG
    tft.startWrite();
    uint32_t dt = millis();
    rc = png.decode(NULL, 0);
    // Serial.print(millis() - dt); // DEBUG
    // Serial.print("ms"); // DEBUG
    tft.endWrite();
    png.close();
  }
  // play_sound_achievement_unlocked();
  alreadyShowedTitleScreen = true;
}

void show_achievement(achievements_t achievement)
{
  tft.fillRoundRect(20, 80, 200, 120, 12, TFT_BLACK);
  print_line("New Achievement Unlocked!", 0, 0);
  print_line("", 1, 0);
  print_line("", 2, 0);
  print_line("", 3, 0);
  print_line(achievement.title, 4, -1);
  Serial.println(achievement.title);
  xpos = 50;
  ypos = 110;
  char fileName[64];
  sprintf(fileName, "/achievement_%d.png", achievement.id);
  download_file_to_littleFS(achievement.url, fileName);
  int16_t rc = png.open(fileName, pngOpen, pngClose, pngRead, pngSeek, pngDraw);
  if (rc == PNG_SUCCESS)
  {
    Serial.print("Successfully opened png file");
    Serial.printf("image specs: (%d x %d), %d bpp, pixel type: %d\n", png.getWidth(), png.getHeight(), png.getBpp(), png.getPixelType());
    tft.startWrite();
    uint32_t dt = millis();
    rc = png.decode(NULL, 0);
    Serial.print(millis() - dt);
    Serial.print("ms");
    tft.endWrite();
    png.close();
  }
  play_sound_achievement_unlocked();
  comebackToTitleScreen = true;
  comebackToTitleScreenTS = millis();
}

//=========================================v==========================================
//                                      pngDraw
//====================================================================================
// This next function will be called during decoding of the png file to
// render each image line to the TFT.  If you use a different TFT library
// you will need to adapt this function to suit.
// Callback function to draw pixels to the display
void pngDraw(PNGDRAW *pDraw)
{
  uint16_t lineBuffer[MAX_IMAGE_WIDTH];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  tft.pushImage(xpos, ypos + pDraw->y, pDraw->iWidth, 1, lineBuffer);
}

void decodePixel(uint16_t x, uint16_t y, uint16_t color)
{
  tft.drawPixel(x + xpos, y + ypos, color);
}

bool prefix(const char *pre, const char *str)
{
  return strncmp(pre, str, strlen(pre)) == 0;
}

uint32_t frame = 0;

void loop()
{
  // reconnect if wifi got disconnected
  if (WiFi.status() != WL_CONNECTED)
  {
    WiFi.begin();
    while (WiFi.status() != WL_CONNECTED)
      yield();
  }
  long cbDelta = millis() - comebackToTitleScreenTS;
  if (comebackToTitleScreen == true && cbDelta > 15000)
  {
    comebackToTitleScreen = false;
    if (fifo_is_empty(&achievements_fifo))
    {
      show_title_screen();
    }
  }

  if (state > 200)
  {              // 200 - 254 are error states
    state = 128; // do nothing
    print_line("Cartridge not identified", 1, 2);
    print_line("Turn off the console", 4, 2);
    digitalWrite(MUX, MUX_ENABLE_BUS); // enable cartridge

    // TEST
    Serial0.print("START_WATCH\r\n");
    Serial.print("START_WATCH\r\n");
  }

  if (state == 0)
  {
    print_line("Identifying cartridge...", 1, 1);

    // digitalWrite(MUX, HIGH); // enable cartridge
    // delay(10000); // test load everdrive
    // digitalWrite(MUX, LOW); // disable cartridge
    Serial0.print("READ_CRC\r\n"); // send command to read cartridge crc
    Serial.print("READ_CRC\r\n");  // send command to read cartridge crc
    delay(250);
    state = 1;

    
    // TEST - FORCING SUPER STREET FIGHTER 2
    md5 = "7ca258cc2ae3712986017777164faf1c";

    // TEST - FORCING SUPER MARIO WORLD
    // md5 = "38bb405ba6c6714697b48fb0ad15a2a1";

    // TEST - FORCING F-ZERO
    // md5 = "15ef4c1cbe9bac8013af65c091c1de2b";

    // TEST - FORCING INTERNATIONAL SUPERSTAR SOCCER
    // md5 = "c49738f77917e22207568aa69acb20a4";

    // TEST = FORCING SUPER METROID
    // md5 = "82094ca2db9d6e78c91c88d31d7c0d06";

    // TEST - SEND MD5 TO PICO
    Serial0.print("CRC_FOUND_MD5=" + md5 + "\r\n");
    Serial.print("CRC_FOUND_MD5=" + md5 + "\r\n");
    state = 2;
  }
  if (state == 2)
  {
    digitalWrite(MUX, HIGH); // enable cartridge
    Serial0.print("START_WATCH\r\n");
    Serial.print("START_WATCH\r\n");
    state = 3;
  }

  if (fifo_is_empty(&achievements_fifo) == false && Serial.available() == 0 && comebackToTitleScreen == false) // show achievements
  {
    achievements_t achievement;
    fifo_dequeue(&achievements_fifo, &achievement);
    show_achievement(achievement);
  }

  while (Serial0.available() > 0)
  {
    int dataLenght = Serial0.available();
    buffer += Serial0.readString();
    // Serial.print(buffer);
    if (buffer.length() > 2048)
    {
      buffer = ""; // clear buffer
      Serial0.print("BUFFER_OVERFLOW\r\n");
      Serial.print("BUFFER_OVERFLOW\r\n");
    }
    if (buffer.indexOf("\r\n") > 0)
    {
      // we have a command
      String command = buffer.substring(0, buffer.indexOf("\n") + 1);
      buffer = buffer.substring(buffer.indexOf("\n") + 1);

      if (command.startsWith("REQ"))
      {
        // example of requsest
        // REQ=FF;M:POST;U:https://retroachievements.org/dorequest.php;D:r=login2&u=user&p=pass
        const char user_agent[] = "SNES_RA_ADAPTER/0.1 rcheevos/11.6";

        String request = command.substring(4);
        // Serial.println("REQ=" + request);
        int pos = request.indexOf(";");
        String request_id = request.substring(0, pos);
        request = request.substring(pos + 1);
        pos = request.indexOf(";");
        String method = request.substring(2, pos);
        request = request.substring(pos + 1);
        pos = request.indexOf(";");
        String url = request.substring(2, pos);
        String data = request.substring(pos + 3);
        // Serial.println("REQ_METHOD=" + method);
        // Serial.println("REQ_URL=" + url);
        // Serial.println("REQ_DATA=" + data);
        int httpCode;

        if (prefix("r=patch", data.c_str()) && ENABLE_SHRINK_LAMBDA == 1)
        {
          // change to a temporary aws lambda that shrinks the patch JSON
          url = SHRINK_LAMBDA_URL;
        }
        // else {
        client.setInsecure();
        https.begin(client, url);
        //}
        https.setUserAgent(user_agent);

        if (method == "POST")
        {
          https.addHeader("Content-Type", "application/x-www-form-urlencoded");
          httpCode = https.POST(data);
        }
        else if (method == "GET")
        {
          httpCode = https.GET();
        }
        if (httpCode != HTTP_CODE_OK)
        {
          // TODO: handle errors
          Serial0.print("REQ_ERROR=" + String(httpCode) + "\r\n");
          Serial.print("REQ_ERROR=" + https.errorToString(httpCode) + "\r\n");
        }
        char httpCodeStr[4];
        sprintf(httpCodeStr, "%03X", httpCode);
        String response = https.getString();

        if (prefix("r=patch", data.c_str()))
        {
          // print response size and header content size
          Serial.print("PATCH LENGTH: ");
          Serial.println(response.length());
          clean_json_field_str_value(response, "Description");
          remove_not_nested_json_field(response, "Warning");
          remove_not_nested_json_field(response, "BadgeLockedURL");
          remove_not_nested_json_field(response, "BadgeURL");
          remove_not_nested_json_field(response, "ImageIconURL");
          remove_not_nested_json_field(response, "Rarity");
          remove_not_nested_json_field(response, "RarityHardcore");
          remove_not_nested_json_field(response, "Author");
          remove_not_nested_json_field(response, "RichPresencePatch");
          Serial.print("NEW PATCH LENGTH: ");
          Serial.println(response.length());
        }
        else if (prefix("r=login", data.c_str()))
        {
          remove_not_nested_json_field(response, "AvatarUrl");
        }
        Serial0.print("RESP=");
        Serial0.print(request_id);
        Serial0.print(";");
        Serial0.print(httpCodeStr);
        Serial0.print(";");

        const char *ptr = response.c_str(); // Obtém ponteiro para o buffer da String
        uint32_t len = response.length();
        uint32_t offset = 0;

        while (offset < len)
        {
          uint32_t chunkLen = min((uint32_t)CHUNK_SIZE, (uint32_t)(len - offset));
          Serial0.write((const uint8_t *)&ptr[offset], chunkLen); // Envia bloco direto do buffer
          Serial0.flush();                                        // Garante que o bloco foi enviado
          offset += chunkLen;
          delay(TX_DELAY_MS); // Pequeno atraso para evitar congestionamento no Pico
        }
        Serial0.print("\r\n");
        Serial.print("RESP=");
        Serial.print(request_id);
        Serial.print(";");
        Serial.print(httpCodeStr);
        Serial.print(";");
        Serial.print(response);
        Serial.print("\r\n");
        https.end();
      }
      else if (command.startsWith("MUX="))
      {
        {
          int mux = command.substring(4).toInt();
          if (mux == 0)
          {
            digitalWrite(MUX, MUX_ENABLE_BUS);
          }
          else
          {
            digitalWrite(MUX, MUX_DISABLE_BUS);
          }
          Serial0.print("MUX_SET\r\n");
          Serial.print("MUX_SET\r\n");
        }
      }
      else if (command.startsWith("READ_CRC="))
      {
        if (state != 1)
        {
          Serial0.print("COMMAND_IGNORED_WRONG_STATE\r\n");
          Serial.print("COMMAND_IGNORED_WRONG_STATE\r\n");
        }
        else
        {
          String data = command.substring(9);
          data.trim();
          Serial.print("READ_CRC=" + data + "\r\n"); // DEBUG
          String beginCRC = data.substring(0, 8);
          Serial.print("BEGIN_CRC=" + beginCRC + "\r\n");
          String endCRC = data.substring(9);
          Serial.print("END_CRC=" + endCRC + "\r\n");
          md5 = getMD5(beginCRC, true);
          if (md5.length() == 0)
          {
            md5 = getMD5(endCRC, false);
          }
          if (md5.length() == 0)
          {
            Serial0.print("CRC_NOT_FOUND\r\n");
            Serial.print("CRC_NOT_FOUND\r\n");
            state = 254; // error - cartridge not found
          }
          else
          {
            Serial0.print("CRC_FOUND_MD5=" + md5 + "\r\n");
            Serial.print("CRC_FOUND_MD5=" + md5 + "\r\n");
            state = 2;
          }
        }
      }
      else if (command.startsWith("CRC_NOT_FOUND"))
      {

        state = 254;
      }
      else if (command.startsWith("A="))
      {
        // example A=123456;Cruise Control;https://media.retroachievements.org/Badge/348421.png
        Serial.print(command);
        int pos = command.indexOf(";");
        String achievements_id = command.substring(2, pos);
        command = command.substring(pos + 1);
        pos = command.indexOf(";");
        String achievements_title = command.substring(0, pos);
        command = command.substring(pos + 1);
        String achievements_image = command;
        achievements_image.trim();
        achievements_t achievement;
        achievement.id = achievements_id.toInt();
        achievement.title = achievements_title;
        achievement.url = achievements_image;
        if (fifo_enqueue(&achievements_fifo, achievement) == false)
        {
          Serial.print("FIFO_FULL\r\n");
        }
        else
        {
          Serial.print("ACHIEVEMENT_ADDED\r\n");
        }
      }
      else if (command.startsWith("GAME_INFO="))
      {
        // example GAME_INFO=1496;R.C. Pro-Am;https://media.retroachievements.org/Images/052570.png
        int pos = command.indexOf(";");
        gameId = command.substring(10, pos);
        command = command.substring(pos + 1);
        pos = command.indexOf(";");
        gameName = command.substring(0, pos);
        command = command.substring(pos + 1);
        String gameImage = command;
        gameImage.trim();
        char fileName[64];
        sprintf(fileName, "/title_%s.png", gameId.c_str());
        download_file_to_littleFS(gameImage.c_str(), fileName);

        // if game name is longer than 18 chars, add ... at the end
        if (gameName.length() > 18)
        {
          gameName = gameName.substring(0, 18 - 3) + "...";
        }

        print_line("Cartridge identified:", 1, 0);
        print_line(" * " + gameName + " * ", 2, 0);
        print_line("please RESET the console", 4, 0);
        Serial.print(command);
      }
      else if (command.startsWith("NES_RESETED"))
      {

        show_title_screen();
        // TODO play some sound?
        state = 128; // do nothing
      }
      else
      {
        Serial.print("UNKNOWN=");
        Serial.print(command);
      }
    }
  }
  yield();
}
