#include <FastLED.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>

// ESP-IDF Low-Level I2S
#include <driver/i2s_std.h>
#include <driver/gpio.h>

// ESP8266Audio Libraries
#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h" // We MUST use Base and rewrite the driver completely for IDF v5

// -------------------- Pins --------------------
#define SPI_MOSI 11
#define SPI_SCLK 12
#define SPI_MISO 13
#define SD_CS    10

#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 7

// -------------------- LED Pins ----------------
#define LED_PIN  38
#define NUM_LEDS 24

// -------------------- WiFi --------------------
const char* ssid = "IIIT-Guest";
const char* password = "f6s68VHJ89mC";

// -------------------- Globals --------------------
WebServer server(80);
Preferences prefs;

TaskHandle_t audioTaskHandle = NULL;

// -------------------- LED Globals -------------
CRGB leds[NUM_LEDS];
String ledMode = "off";
String ledColorHex = "#000000";
uint8_t ledBrightness = 100;
bool systemPowerOn = true;

uint8_t speakerVolumePct = 75; // Right channel (Speaker)
uint8_t exciterVolumePct = 50; // Left channel (Exciter)

bool loopEnabled = false;
String currentFilename = "";
bool isPaused = false;
volatile bool isUploading = false;

enum MediaType { MEDIA_NONE, MEDIA_AUDIO, MEDIA_VIDEO };
MediaType currentMediaType = MEDIA_NONE;

enum AudioCommand {
    CMD_IDLE,
    CMD_PLAY,
    CMD_STOP,
    CMD_SEEK
};
struct AudioCommandMessage {
    AudioCommand cmd;
    char filename[192];
    bool loopRequested;
    uint32_t positionSec;
};

QueueHandle_t audioCmdQueue = NULL;
volatile bool audioAbortRequested = false;

extern SemaphoreHandle_t sdMutex;

// -------------------- Custom Audio Output --------------------
// This cleanly splits a single I2S DIN wire into two independent volume-controlled channels
class AudioOutputHaptic : public AudioOutput {
private:
    int _bclk, _lrc, _dout;
    int _sampleRate = 44100;
    i2s_chan_handle_t tx_chan; // IDF v5 channel handle
public:
    float leftVol = 0.5f;
    float rightVol = 0.75f;
    
    AudioOutputHaptic(int bclk, int lrc, int dout) : AudioOutput() {
        _bclk = bclk;
        _lrc = lrc;
        _dout = dout;
        tx_chan = NULL;
    }

    virtual ~AudioOutputHaptic() {
        stop();
    }

    virtual bool SetRate(int hz) override {
        if (hz == _sampleRate) return true;
        _sampleRate = hz;
        if (tx_chan) {
            i2s_channel_disable(tx_chan);
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)_sampleRate);
            i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
            i2s_channel_enable(tx_chan);
        }
        return true;
    }
    
    // Base class does not define SetBitsPerSample/SetChannels so we drop 'override' or omit
    bool SetBitsPerSample(int bits) { return true; } // Forced to 16-bit
    bool SetChannels(int channels) { return true; }  // Forced to Stereo DIN

    virtual bool begin() override {
        // 1. Allocate an I2S TX channel (IDF v5)
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true; // Auto clear TX buffer
        chan_cfg.dma_desc_num = 12;    // Deep buffer for dropout prevention
        chan_cfg.dma_frame_num = 1000; // ~270ms of audio buffer
        esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
        if (err != ESP_OK) {
            Serial.println("I2S channel allocation failed");
            return false;
        }

        // 2. Configure for Standard I2S Mode
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)_sampleRate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)_bclk,
                .ws = (gpio_num_t)_lrc,
                .dout = (gpio_num_t)_dout,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
        if (err != ESP_OK) {
            Serial.println("I2S std mode initialization failed");
            return false;
        }

        // 3. Enable the channel
        err = i2s_channel_enable(tx_chan);
        if (err != ESP_OK) {
            Serial.println("I2S channel enable failed");
            return false;
        }

        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!tx_chan) return false;
        if (audioAbortRequested) return false;
        
        int16_t scaled[2];
        // Average L&R to ensure mono source, then scale independently
        int32_t mono = (sample[0] + sample[1]) / 2;
        
        // Apply independent volumes. Left=Exciter, Right=Speaker
        scaled[0] = max(-32768, min(32767, (int)(mono * leftVol)));
        scaled[1] = max(-32768, min(32767, (int)(mono * rightVol)));
        
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_chan, &scaled, 4, &bytes_written, pdMS_TO_TICKS(20));
        return err == ESP_OK;
    }
    
    virtual uint16_t ConsumeSamples(int16_t *samples, uint16_t count) override {
        if (!tx_chan) return 0;
        if (audioAbortRequested) return 0;
        
        const uint16_t CHUNK = 128;
        int16_t scaled[CHUNK * 2];
        uint16_t processed = 0;
        
        while (processed < count) {
            if (audioAbortRequested) return processed;
            uint16_t toProcess = min((int)(count - processed), (int)CHUNK);
            for (uint16_t i = 0; i < toProcess; i++) {
                int16_t l = samples[(processed + i) * 2];
                int16_t r = samples[(processed + i) * 2 + 1];
                int32_t mono = (l + r) / 2;
                scaled[i * 2]     = max(-32768, min(32767, (int)(mono * leftVol)));
                scaled[i * 2 + 1] = max(-32768, min(32767, (int)(mono * rightVol)));
            }
            size_t bytes_written = 0;
            esp_err_t err = i2s_channel_write(tx_chan, scaled, toProcess * 4, &bytes_written, pdMS_TO_TICKS(20));
            if (err != ESP_OK) {
                return processed;
            }
            processed += toProcess;
        }
        return count;
    }

    virtual bool stop() override {
        if (tx_chan) {
            i2s_channel_disable(tx_chan);
            i2s_del_channel(tx_chan);
            tx_chan = NULL;
        }
        return true;
    }
};

AudioFileSource *fileSource = NULL;
AudioFileSourceID3 *id3Source = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputHaptic *out = NULL;

// -------------------- Forward Declarations & IPC --------------------
void handleApiFiles();
void handleApiPlay();
void handleApiStop();
void handleApiPause();
void handleApiSeek();
void handleApiCurrent();
void handleApiVolumeSpeaker();
void handleApiVolumeExciter();
void handleApiLedMode();
void handleApiLedColor();
void handleApiLedBrightness();
void handleApiStatus();
void handleApiPowerToggle();
void handleApiBattery();
void handleApiMediaDelete();
void handleApiMediaUpload();
void handleCorsOptions();
void updateAudioVolumeAndBalance();
bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte = 0);
bool _seekAudioToSeconds(uint32_t positionSec);
bool _enqueueAudioCommand(const AudioCommandMessage &cmd, uint32_t timeoutMs = 50);
extern size_t uploadBytesWritten;
extern bool uploadHadWriteError;

SemaphoreHandle_t sdMutex = NULL;

// -------------------- Loop (Core 1) --------------------
void _stopAudioSafely() {
    isPaused = true; // Stop play loop entry before deleting memory
    currentMediaType = MEDIA_NONE;

    if (sdMutex) {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
    }

    if (mp3) {
        if (mp3->isRunning()) mp3->stop();
        delete mp3;
        mp3 = NULL;
    }
    if (id3Source) {
        delete id3Source;
        id3Source = NULL;
    }
    if (fileSource) {
        if (fileSource->isOpen()) fileSource->close();
        delete fileSource;
        fileSource = NULL;
    }

    if (sdMutex) {
        xSemaphoreGive(sdMutex);
    }

    currentFilename = "";
    loopEnabled = false;
    isPaused = false;
}

bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte) {
    _stopAudioSafely();

    xSemaphoreTake(sdMutex, portMAX_DELAY);

    bool success = false;
    do {
        Serial.printf("Opening media file: %s\n", filename.c_str());

        String fnameLower = filename;
        fnameLower.toLowerCase();
        if (!fnameLower.endsWith(".mp3")) {
            Serial.println("Unsupported media for Stage 1: " + filename);
            break;
        }

        fileSource = new AudioFileSourceSD(filename.c_str());
        if (!fileSource || !fileSource->isOpen()) {
            Serial.println("Failed to open media file");
            break;
        }

        if (startByte > 0) {
            uint32_t fileSize = fileSource->getSize();
            uint32_t clamped = startByte;
            if (fileSize > 0 && clamped >= fileSize) {
                clamped = fileSize - 1;
            }
            fileSource->seek(clamped, SEEK_SET);
        }

        id3Source = new AudioFileSourceID3(fileSource);
        mp3 = new AudioGeneratorMP3();
        if (!id3Source || !mp3) {
            Serial.println("Failed to create decoder pipeline");
            break;
        }

        Serial.println("Starting decoder begin()...");
        if (!mp3->begin(id3Source, out)) {
            Serial.println("Failed to start MP3 decoder");
            break;
        }

        currentMediaType = MEDIA_AUDIO;
        currentFilename = filename;
        loopEnabled = loopRequested;
        isPaused = false;
        Serial.println("Starting MP3 Audio Decoder...");
        success = true;
    } while (false);

    if (!success) {
        if (mp3) { delete mp3; mp3 = NULL; }
        if (id3Source) { delete id3Source; id3Source = NULL; }
        if (fileSource) {
            if (fileSource->isOpen()) fileSource->close();
            delete fileSource;
            fileSource = NULL;
        }
        currentMediaType = MEDIA_NONE;
        currentFilename = "";
        loopEnabled = false;
        isPaused = false;
    }

    xSemaphoreGive(sdMutex);
    return success;
}

bool _seekAudioToSeconds(uint32_t positionSec) {
    if (currentMediaType != MEDIA_AUDIO || currentFilename.length() == 0) {
        return false;
    }

    const uint32_t assumedBitrateKbps = 128;
    uint64_t targetByte = (uint64_t)positionSec * assumedBitrateKbps * 1000ULL / 8ULL;
    return _startAudioPlayback(currentFilename, loopEnabled, (uint32_t)targetByte);
}

bool _enqueueAudioCommand(const AudioCommandMessage &cmd, uint32_t timeoutMs) {
    if (!audioCmdQueue) return false;
    return xQueueSend(audioCmdQueue, &cmd, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// -------------------- Tasks --------------------
void taskWebServer(void *pvParameters) {
    while (true) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void taskAudio(void *pvParameters) {
    while (true) {
        if (audioAbortRequested && currentMediaType == MEDIA_AUDIO) {
            _stopAudioSafely();
            audioAbortRequested = false;
        }

        AudioCommandMessage cmdMsg;
        while (xQueueReceive(audioCmdQueue, &cmdMsg, 0) == pdTRUE) {
            if (cmdMsg.cmd == CMD_PLAY) {
                audioAbortRequested = false;
                _startAudioPlayback(String(cmdMsg.filename), cmdMsg.loopRequested, 0);
            } else if (cmdMsg.cmd == CMD_STOP) {
                audioAbortRequested = true;
                _stopAudioSafely();
                audioAbortRequested = false;
            } else if (cmdMsg.cmd == CMD_SEEK) {
                audioAbortRequested = false;
                _seekAudioToSeconds(cmdMsg.positionSec);
            }
        }

        if (isUploading) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Audio Playback Pump safely wrapped in sdMutex to prevent SPI collisions
        if (!isPaused && currentMediaType == MEDIA_AUDIO && mp3 && mp3->isRunning()) {
            bool playing = false;
            
            // Use a safe pointer check before calling methods on mp3
            if (mp3 && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10))) {  
                playing = mp3->loop();
                xSemaphoreGive(sdMutex);
            } else {
                playing = true; // Pretend it played just this loop to not stop abruptly
            }
            
            if (!playing) {
                String finishedTrack = currentFilename;
                bool shouldLoop = loopEnabled;
                _stopAudioSafely();

                if (shouldLoop && finishedTrack.length() > 0) {
                    Serial.println("Restarting loop...");
                    _startAudioPlayback(finishedTrack, true, 0);
                } else {
                    Serial.println("Playback finished naturally");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2)); // Relax watchdogs and yield CPU to let WebServer grab the mutex if needed
    }
}

void taskLEDs(void *pvParameters) {
    uint8_t hue = 0;
    while (true) {
        if (isUploading) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!systemPowerOn) {
            FastLED.clear();
            FastLED.show();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (ledMode == "off") {
            FastLED.clear();
            FastLED.show();
        } else if (ledMode == "manual") {
            long number = strtol(&ledColorHex[1], NULL, 16);
            int r = number >> 16;
            int g = number >> 8 & 0xFF;
            int b = number & 0xFF;
            fill_solid(leds, NUM_LEDS, CRGB(r, g, b));
            FastLED.show();
        } else if (ledMode == "random") {
            fill_rainbow(leds, NUM_LEDS, hue, 255/NUM_LEDS);
            FastLED.show();
            hue++;
        } else if (ledMode == "frequency" || ledMode == "oled_sync") {
             // Placeholder for audio visualizer sync
             fill_solid(leds, NUM_LEDS, CRGB::Blue);
             FastLED.show();
        }
        vTaskDelay(pdMS_TO_TICKS(30)); // ~33 fps
    }
}

// -------------------- Tasks Setup --------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- ESP32 Haptic Core (ESP8266Audio Version) ---");

    // Load preferences
    prefs.begin("settings", false);
    speakerVolumePct = prefs.getUInt("spk_vol", 75);
    exciterVolumePct = prefs.getUInt("exc_vol", 50);

    // Initialize LED system
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    ledMode = prefs.getString("led_mode", "random");
    ledBrightness = prefs.getUInt("led_bright", 100);
    ledColorHex = prefs.getString("led_color", "#FF0000");
    FastLED.setBrightness(ledBrightness);

    // Initialize SPI and SD
    SPI.begin(SPI_SCLK, SPI_MISO, SPI_MOSI);
    SPI.setFrequency(4000000); // 4MHz for stable SD reading with ESP8266Audio
    
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card initialization failed!");
    } else {
        Serial.println("SD Card initialized.");
    }

    sdMutex = xSemaphoreCreateMutex();
    audioCmdQueue = xQueueCreate(8, sizeof(AudioCommandMessage));
    if (!audioCmdQueue) {
        Serial.println("ERROR: Failed to create audio command queue");
    }

    // Initialize custom ESP-IDF Direct DMA Audio Pipeline
    out = new AudioOutputHaptic(I2S_BCLK, I2S_LRC, I2S_DOUT);
    
    updateAudioVolumeAndBalance();

    // Connect WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected.");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi connection failed! Proceeding anyway...");
    }

    // Setup routes
    server.on("/api/media/files", HTTP_GET, handleApiFiles);
    server.on("/api/media/files", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/play", HTTP_POST, handleApiPlay);
    server.on("/api/media/play", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/stop", HTTP_POST, handleApiStop);
    server.on("/api/media/stop", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/pause", HTTP_POST, handleApiPause);
    server.on("/api/media/pause", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/seek", HTTP_POST, handleApiSeek);
    server.on("/api/media/seek", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/current", HTTP_GET, handleApiCurrent);
    server.on("/api/media/current", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/volume/speaker", HTTP_POST, handleApiVolumeSpeaker);
    server.on("/api/volume/speaker", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/volume/exciter", HTTP_POST, handleApiVolumeExciter);
    server.on("/api/volume/exciter", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/delete", HTTP_DELETE, handleApiMediaDelete);
    server.on("/api/media/delete", HTTP_POST, handleApiMediaDelete);
    server.on("/api/media/delete", HTTP_OPTIONS, handleCorsOptions);
    // Note: Upload multipart route isn't easily done in server.on without huge callbacks. Using standard for now.
    server.on("/api/media/upload", HTTP_POST, [](){
        if (uploadHadWriteError) {
            server.send(500, "application/json", "{\"error\":\"Upload failed during SD write\"}");
        } else {
            server.send(200, "application/json", String("{\"status\":\"Upload done\",\"written\":") + uploadBytesWritten + "}");
        }
    }, handleApiMediaUpload);
    server.on("/api/media/upload", HTTP_OPTIONS, handleCorsOptions);

    server.on("/api/led/mode", HTTP_POST, handleApiLedMode);
    server.on("/api/led/mode", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/led/color", HTTP_POST, handleApiLedColor);
    server.on("/api/led/color", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/led/brightness", HTTP_POST, handleApiLedBrightness);
    server.on("/api/led/brightness", HTTP_OPTIONS, handleCorsOptions);
    
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/status", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/power/toggle", HTTP_POST, handleApiPowerToggle);
    server.on("/api/power/toggle", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/battery", HTTP_GET, handleApiBattery);
    server.on("/api/battery", HTTP_OPTIONS, handleCorsOptions);
    
    // Explicit OPTIONS handler for CORS preflight (Allows Swagger UI to work)
    server.onNotFound([]() {
        if (server.method() == HTTP_OPTIONS) {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
            server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
            server.send(204);
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    });

    // Enable CORS
    server.enableCORS(true);
    server.begin();

    // Create proper FreeRTOS Tasks
    xTaskCreatePinnedToCore(taskWebServer, "WebServerTask", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(taskAudio, "AudioTask", 16384, NULL, 2, &audioTaskHandle, 1);
    xTaskCreatePinnedToCore(taskLEDs, "LEDTask", 4096, NULL, 1, NULL, 0);

}
//                 }
//             }
//         }
//         vTaskDelay(1);
//     }
// }

// -------------------- Loop (Core 1) --------------------
// (Moved _stopAudioSafely up above setup)

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void handleCorsOptions() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Accept");
    server.sendHeader("Access-Control-Allow-Private-Network", "true");
    server.send(204);
}

// -------------------- Volume Logic --------------------
void updateAudioVolumeAndBalance() {
    if (out) {
        out->leftVol = (float)exciterVolumePct / 100.0f;
        out->rightVol = (float)speakerVolumePct / 100.0f;
    }
}

// -------------------- API Handlers (Core 0) --------------------
void handleApiFiles() {
    DynamicJsonDocument doc(4096);
    JsonArray files = doc.createNestedArray("files");

    if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
        File root = SD.open("/");
        if (!root) {
            xSemaphoreGive(sdMutex);
            server.send(500, "application/json", "{\"error\":\"Failed to open directory\"}");
            return;
        }

        File file = root.openNextFile();
        while (file) {
            if (!file.isDirectory()) {
                JsonObject f = files.createNestedObject();
                f["filename"] = String("/") + file.name();
                f["size"] = file.size();
                
                String name = String(file.name());
                name.toLowerCase();
                if (name.endsWith(".mp3")) {
                    f["type"] = "audio";
                } else {
                    f["type"] = "unknown";
                }
            }
            File nextFile = root.openNextFile();
            file.close();
            file = nextFile;
        }
        root.close();
        xSemaphoreGive(sdMutex);
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiPlay() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }

    Serial.println("Received /api/media/play");

    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String filename = doc["filename"] | "";
    if (filename.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Missing filename\"}");
        return;
    }

    bool requestedLoop = doc["loop"] | false;
    if (!filename.startsWith("/")) {
        filename = "/" + filename;
    }

    Serial.printf("AUDIO_CMD_PLAY: %s loop=%s\n", filename.c_str(), requestedLoop ? "true" : "false");

    audioAbortRequested = true;
    if (audioCmdQueue) {
        xQueueReset(audioCmdQueue);
    }

    AudioCommandMessage cmd = {};
    cmd.cmd = CMD_PLAY;
    cmd.loopRequested = requestedLoop;
    strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);

    if (!_enqueueAudioCommand(cmd, 100)) {
        server.send(503, "application/json", "{\"error\":\"Audio command queue busy\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"Playback configured\",\"filename\":\"" + filename + "\"}");
}

void handleApiStop() {
    Serial.println("AUDIO_CMD_STOP");

    audioAbortRequested = true;
    if (audioCmdQueue) {
        xQueueReset(audioCmdQueue);
    }

    AudioCommandMessage cmd = {};
    cmd.cmd = CMD_STOP;
    if (!_enqueueAudioCommand(cmd, 100)) {
        server.send(503, "application/json", "{\"error\":\"Audio command queue busy\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"Playback stopped\"}");
}

void handleApiPause() {
    Serial.println("AUDIO_CMD_TOGGLE_PAUSE");
    if (currentMediaType == MEDIA_AUDIO && mp3) {
        isPaused = !isPaused;
    }
    server.send(200, "application/json", String("{\"status\":\"Pause/Resume toggled\",\"paused\":") + (isPaused ? "true}" : "false}"));
}

void handleApiSeek() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    int positionSec = doc["position"] | -1;
    if (positionSec < 0) {
        server.send(400, "application/json", "{\"error\":\"position must be >= 0\"}");
        return;
    }

    Serial.printf("AUDIO_CMD_SEEK: %d sec\n", positionSec);

    audioAbortRequested = true;

    AudioCommandMessage cmd = {};
    cmd.cmd = CMD_SEEK;
    cmd.positionSec = (uint32_t)positionSec;
    if (!_enqueueAudioCommand(cmd, 100)) {
        server.send(503, "application/json", "{\"error\":\"Audio command queue busy\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"Seek requested\"}");
}

void handleApiCurrent() {
    DynamicJsonDocument doc(512);
    
    doc["playing"] = (!isPaused && currentMediaType == MEDIA_AUDIO && currentFilename != "");
    doc["paused"] = isPaused;
    doc["filename"] = currentFilename;
    doc["loop"] = loopEnabled;

    String typeStr = "none";
    if (currentMediaType == MEDIA_AUDIO) typeStr = "audio";
    if (currentMediaType == MEDIA_VIDEO) typeStr = "video";
    doc["type"] = typeStr;

    if (currentMediaType == MEDIA_AUDIO) {
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (fileSource && fileSource->isOpen()) {
                uint32_t pos = fileSource->getPos();
                uint32_t size = fileSource->getSize();
                doc["position_bytes"] = pos;
                doc["size_bytes"] = size;
                doc["position"] = (uint32_t)((uint64_t)pos * 8ULL / 128000ULL);
                doc["duration"] = (uint32_t)((uint64_t)size * 8ULL / 128000ULL);
            }
            if (id3Source) {
                doc["id3_parsed"] = true; // Ready for album arts down the line
            }
            xSemaphoreGive(sdMutex);
        }
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiVolumeSpeaker() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    int level = doc["level"] | -1;
    if (level < 0 || level > 100) {
        server.send(400, "application/json", "{\"error\":\"level must be 0..100\"}");
        return;
    }

    speakerVolumePct = (uint8_t)level;
    
    prefs.putUInt("spk_vol", speakerVolumePct);
    updateAudioVolumeAndBalance();
    
    server.send(200, "application/json", "{\"status\":\"Speaker volume set\"}");
}

void handleApiVolumeExciter() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    int level = doc["level"] | -1;
    if (level < 0 || level > 100) {
        server.send(400, "application/json", "{\"error\":\"level must be 0..100\"}");
        return;
    }

    exciterVolumePct = (uint8_t)level;

    prefs.putUInt("exc_vol", exciterVolumePct);
    updateAudioVolumeAndBalance();
    
    server.send(200, "application/json", "{\"status\":\"Exciter volume set\"}");
}

void handleApiLedMode() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    String mode = doc["mode"] | "off";
    if (mode == "random" || mode == "frequency" || mode == "oled_sync" || mode == "manual" || mode == "off") {
        ledMode = mode;
        prefs.putString("led_mode", ledMode);
        server.send(200, "application/json", "{\"status\":\"LED mode set\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"Invalid mode\"}");
    }
}

void handleApiLedColor() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    ledColorHex = doc["color"] | ledColorHex;
    if (doc.containsKey("brightness")) {
        ledBrightness = doc["brightness"];
        FastLED.setBrightness(ledBrightness);
        prefs.putUInt("led_bright", ledBrightness);
    }
    prefs.putString("led_color", ledColorHex);
    ledMode = "manual";
    prefs.putString("led_mode", "manual");
    
    server.send(200, "application/json", "{\"status\":\"LED color set\"}");
}

void handleApiLedBrightness() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    ledBrightness = doc["brightness"] | ledBrightness;
    FastLED.setBrightness(ledBrightness);
    prefs.putUInt("led_bright", ledBrightness);
    
    server.send(200, "application/json", "{\"status\":\"LED brightness set\"}");
}

void handleApiStatus() {
    DynamicJsonDocument doc(1024);
    
    doc["power"] = systemPowerOn ? "on" : "off";
    
    JsonObject bat = doc.createNestedObject("battery");
    bat["percent"] = 100; // Placeholder
    bat["voltage"] = 4.2;
    bat["charging"] = false;
    bat["full"] = true;
    
    JsonObject pb = doc.createNestedObject("playback");
    pb["playing"] = (!isPaused && currentMediaType != MEDIA_NONE && currentFilename != "");
    pb["paused"] = isPaused;
    pb["filename"] = currentFilename;
    pb["loop"] = loopEnabled;
    
    JsonObject l = doc.createNestedObject("led");
    l["mode"] = ledMode;
    l["brightness"] = ledBrightness;
    l["color"] = ledColorHex;
    
    JsonObject v = doc.createNestedObject("volume");
    v["speaker"] = speakerVolumePct;
    v["exciter"] = exciterVolumePct;
    
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiPowerToggle() {
    systemPowerOn = !systemPowerOn;
    if (!systemPowerOn) {
        AudioCommandMessage cmd = {};
        cmd.cmd = CMD_STOP;
        _enqueueAudioCommand(cmd, 50);
    }
    server.send(200, "application/json", String("{\"state\":\"") + (systemPowerOn ? "on" : "off") + "\"}");
}

void handleApiBattery() {
    // Mocked for MVP
    server.send(200, "application/json", "{\"percent\":100,\"voltage\":4.2,\"charging\":false,\"full\":true}");
}

void handleApiMediaDelete() {
    if (!server.hasArg("filename")) {
        server.send(400, "application/json", "{\"error\":\"Missing filename query parameter\"}");
        return;
    }
    String fname = server.arg("filename");
    if (!fname.startsWith("/")) fname = "/" + fname;
    
    if (fname == currentFilename && !isPaused && currentMediaType != MEDIA_NONE) {
        server.send(409, "application/json", "{\"error\":\"File is currently playing\"}");
        return;
    }
    
    if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
        if (SD.remove(fname)) {
            server.send(200, "application/json", "{\"status\":\"File deleted\"}");
        } else {
            server.send(404, "application/json", "{\"error\":\"File not found or locked\"}");
        }
        xSemaphoreGive(sdMutex);
    }
}

File uploadFile;
uint8_t *uploadBuffer = NULL;
size_t uploadBufferIndex = 0;
size_t actualBufferSize = 0;
size_t uploadBytesWritten = 0;
bool uploadHadWriteError = false;
const size_t DESIRED_BUFFER_SIZE = 8 * 1024; // 8KB Internal RAM Buffer (Safer for SPI FATFS)

void _flushUploadBuffer() {
    if (uploadFile && uploadBuffer && uploadBufferIndex > 0) {
        if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
            size_t requested = uploadBufferIndex;
            size_t written = uploadFile.write(uploadBuffer, requested);

            if (written != requested) {
                // One retry for transient SPI/SD timing hiccups
                vTaskDelay(pdMS_TO_TICKS(2));
                size_t retryWritten = uploadFile.write(uploadBuffer + written, requested - written);
                written += retryWritten;
            }

            if (written != requested) {
                uploadHadWriteError = true;
            }
            uploadBytesWritten += written;
            xSemaphoreGive(sdMutex);
        }
        uploadBufferIndex = 0;
        // Yield momentarily to let watchdog reset
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void handleApiMediaUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        // Hard-stop playback before upload to guarantee exclusive SD access
        audioAbortRequested = true;
        if (audioCmdQueue) {
            xQueueReset(audioCmdQueue);
        }
        _stopAudioSafely();
        isUploading = true;

        // Turn off LEDs immediately to prevent FastLED interrupt starvation during WiFi/SD operation
        FastLED.clear();
        FastLED.show();
        
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        
        Serial.printf("Upload Start: %s\n", filename.c_str());
        uploadBytesWritten = 0;
        uploadHadWriteError = false;
        
        if (!uploadBuffer) {
            // Allocate safely in standard 8-bit internal memory (avoid PSRAM DMA issues)
            uploadBuffer = (uint8_t*)malloc(DESIRED_BUFFER_SIZE);
            if (uploadBuffer) {
                actualBufferSize = DESIRED_BUFFER_SIZE;
                Serial.println("Allocated 8KB internal RAM buffer for SPI safe upload.");
            } else {
                actualBufferSize = 1024;
                uploadBuffer = (uint8_t*)malloc(actualBufferSize);
                Serial.println("Fallback to 1KB internal RAM buffer.");
            }
        }
        uploadBufferIndex = 0;
        
        if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
            // Remove existing file if it exists to overwrite cleanly
            if (SD.exists(filename)) {
                SD.remove(filename);
            }
            uploadFile = SD.open(filename, FILE_WRITE);
            if (!uploadFile) {
                Serial.println("Failed to open SD file for writing!");
            }
            xSemaphoreGive(sdMutex);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            if (uploadBuffer) {
                size_t remaining = upload.currentSize;
                uint8_t* ptr = upload.buf;
                
                while (remaining > 0) {
                    size_t spaceLeft = actualBufferSize - uploadBufferIndex;
                    size_t toCopy = (remaining < spaceLeft) ? remaining : spaceLeft;
                    
                    memcpy(&uploadBuffer[uploadBufferIndex], ptr, toCopy);
                    uploadBufferIndex += toCopy;
                    ptr += toCopy;
                    remaining -= toCopy;
                    
                    if (uploadBufferIndex >= actualBufferSize) {
                        _flushUploadBuffer();
                    }
                }
            } else {
                // Complete fallback if even malloc failed
                if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
                    size_t written = uploadFile.write(upload.buf, upload.currentSize);
                    if (written != upload.currentSize) {
                        Serial.println("SD Write failed or partial write!");
                    }
                    xSemaphoreGive(sdMutex);
                }
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            _flushUploadBuffer();
            if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
                uploadFile.flush();
                uploadFile.close();
                xSemaphoreGive(sdMutex);
                if (uploadHadWriteError) {
                    Serial.printf("Upload End With Errors: %s, expected=%u written=%u\n", upload.filename.c_str(), upload.totalSize, uploadBytesWritten);
                } else {
                    Serial.printf("Upload End: %s, %u bytes\n", upload.filename.c_str(), uploadBytesWritten);
                }
            }
        }
        if (uploadBuffer) {
            free(uploadBuffer);
            uploadBuffer = NULL;
        }
        isUploading = false;
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) {
            if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
                uploadFile.close();
                xSemaphoreGive(sdMutex);
                Serial.println("Upload Aborted by client!");
                
                // Delete the incomplete file
                String filename = upload.filename;
                if (!filename.startsWith("/")) filename = "/" + filename;
                SD.remove(filename);
            }
        }
        if (uploadBuffer) {
            free(uploadBuffer);
            uploadBuffer = NULL;
        }
        uploadHadWriteError = true;
        isUploading = false;
    }
}
