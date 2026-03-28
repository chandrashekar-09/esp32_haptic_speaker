#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>

// ESP-IDF Low-Level I2S
#include <driver/i2s.h>

// ESP8266Audio Libraries
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h" // Base class only

// -------------------- Pins --------------------
#define SPI_MOSI 11
#define SPI_SCLK 12
#define SPI_MISO 13
#define SD_CS    10

#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 7

// -------------------- WiFi --------------------
const char* ssid = "IIIT-Guest";
const char* password = "f6s68VHJ89mC";

// -------------------- Globals --------------------
WebServer server(80);
Preferences prefs;

uint8_t speakerVolumePct = 75; // Right channel (Speaker)
uint8_t exciterVolumePct = 50; // Left channel (Exciter)

bool loopEnabled = false;
String currentFilename = "";
bool isPaused = false;

// -------------------- Custom Audio Output --------------------
// This cleanly splits a single I2S DIN wire into two independent volume-controlled channels
class AudioOutputHaptic : public AudioOutput {
private:
    int _bclk, _lrc, _dout;
    int _sampleRate = 44100;
public:
    float leftVol = 0.5f;
    float rightVol = 0.75f;
    
    AudioOutputHaptic(int bclk, int lrc, int dout) : AudioOutput() {
        _bclk = bclk;
        _lrc = lrc;
        _dout = dout;
    }

    virtual ~AudioOutputHaptic() {
        stop();
    }

    virtual bool SetRate(int hz) override {
        _sampleRate = hz;
        i2s_set_sample_rates(I2S_NUM_0, _sampleRate);
        return true;
    }
    
    virtual bool SetBitsPerSample(int bits) { return true; } // Forced to 16-bit
    virtual bool SetChannels(int channels) override { return true; }  // Forced to Stereo DIN

    virtual bool begin() override {
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = _sampleRate,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            // Deep DMA Buffers: Absolutely critical for smooth audio and future video playback
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 8,
            .dma_buf_len = 512,
            .use_apll = false,
            .tx_desc_auto_clear = true
        };
        
        i2s_pin_config_t pin_config = {
            .bck_io_num = _bclk,
            .ws_io_num = _lrc,
            .data_out_num = _dout,
            .data_in_num = I2S_PIN_NO_CHANGE
        };
        
        i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
        i2s_set_pin(I2S_NUM_0, &pin_config);
        i2s_zero_dma_buffer(I2S_NUM_0);
        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        int16_t scaled[2];
        // Average L&R to ensure mono source, then scale independently
        int32_t mono = (sample[0] + sample[1]) / 2;
        
        // Apply independent volumes. Left=Exciter, Right=Speaker
        scaled[0] = max(-32768, min(32767, (int)(mono * leftVol)));
        scaled[1] = max(-32768, min(32767, (int)(mono * rightVol)));
        
        size_t bytes_written = 0;
        // Blast perfectly scaled 16-bit frames straight to hardware DMA
        i2s_write(I2S_NUM_0, &scaled, 4, &bytes_written, portMAX_DELAY);
        return true;
    }
    
    virtual uint16_t ConsumeSamples(int16_t *samples, uint16_t count) override {
        // We override this to process blocks, but actually just pass them down our math pipeline
        for (uint16_t i=0; i<count; i++) {
            ConsumeSample(samples + i*2);
        }
        return count;
    }

    virtual bool stop() override {
        i2s_driver_uninstall(I2S_NUM_0);
        return true;
    }
};

AudioFileSourceSD *fileSource = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputHaptic *out = NULL;

// -------------------- Forward Declarations & IPC --------------------
void handleApiFiles();
void handleApiPlay();
void handleApiStop();
void handleApiPause();
void handleApiCurrent();
void handleApiVolumeSpeaker();
void handleApiVolumeExciter();
void updateAudioVolumeAndBalance();

SemaphoreHandle_t sdMutex = NULL;
String pendingPlayFilename = "";
bool pendingPlayFlag = false;
bool pendingStopFlag = false;
bool pendingPauseFlag = false;
SemaphoreHandle_t audioMutex = NULL;

// -------------------- Tasks --------------------
void webServerTask(void *pvParameters) {
    while (true) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// -------------------- Setup --------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- ESP32 Haptic Core (ESP8266Audio Version) ---");

    // Load preferences
    prefs.begin("settings", false);
    speakerVolumePct = prefs.getUInt("spk_vol", 75);
    exciterVolumePct = prefs.getUInt("exc_vol", 50);

    // Initialize SPI and SD
    SPI.begin(SPI_SCLK, SPI_MISO, SPI_MOSI);
    SPI.setFrequency(4000000); // 4MHz for stable SD reading with ESP8266Audio
    
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card initialization failed!");
    } else {
        Serial.println("SD Card initialized.");
    }

    audioMutex = xSemaphoreCreateMutex();
    sdMutex = xSemaphoreCreateMutex();

    // Initialize custom ESP-IDF Direct DMA Audio Pipeline
    out = new AudioOutputHaptic(I2S_BCLK, I2S_LRC, I2S_DOUT);
    
    updateAudioVolumeAndBalance();

    // Connect WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected.");
    Serial.println(WiFi.localIP());

    // Setup routes
    server.on("/api/media/files", HTTP_GET, handleApiFiles);
    server.on("/api/media/play", HTTP_POST, handleApiPlay);
    server.on("/api/media/stop", HTTP_POST, handleApiStop);
    server.on("/api/media/pause", HTTP_POST, handleApiPause);
    server.on("/api/media/current", HTTP_GET, handleApiCurrent);
    server.on("/api/volume/speaker", HTTP_POST, handleApiVolumeSpeaker);
    server.on("/api/volume/exciter", HTTP_POST, handleApiVolumeExciter);
    
    // Enable CORS
    server.enableCORS(true);
    server.begin();

    // Pin WebServer task to Core 0
    xTaskCreatePinnedToCore(
        webServerTask, 
        "WebServerTask", 
        8192, 
        NULL, 
        1, 
        NULL, 
        0 // Core 0
    );
}

// -------------------- Loop (Core 1) --------------------
void _stopAudioSafely() {
    if (mp3 && mp3->isRunning()) {
        mp3->stop();
    }
}

void loop() {
    // Process IPC commands
    if (xSemaphoreTake(audioMutex, 0) == pdTRUE) {
        if (pendingStopFlag) {
            _stopAudioSafely();
            isPaused = false;
            pendingStopFlag = false;
        }
        if (pendingPauseFlag) {
            isPaused = !isPaused;
            pendingPauseFlag = false;
        }
        if (pendingPlayFlag && pendingPlayFilename != "") {
            _stopAudioSafely();
            
            if (fileSource) { delete fileSource; fileSource = NULL; }
            if (mp3) { delete mp3; mp3 = NULL; }

            if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
                Serial.printf("Checking if file exists: %s\n", pendingPlayFilename.c_str());
                if (SD.exists(pendingPlayFilename)) {
                    Serial.println("File exists, starting playback...");
                    fileSource = new AudioFileSourceSD(pendingPlayFilename.c_str());
                    mp3 = new AudioGeneratorMP3();
                    mp3->begin(fileSource, out);
                    isPaused = false;
                } else {
                    Serial.println("Play requested but file not found: " + pendingPlayFilename);
                    currentFilename = "";
                }
                xSemaphoreGive(sdMutex);
            }
            pendingPlayFlag = false;
        }
        xSemaphoreGive(audioMutex);
    }

    // Audio Playback Pump safely wrapped in sdMutex to prevent SPI collisions
    if (!isPaused && mp3 && mp3->isRunning()) {
        bool playing = false;
        if (xSemaphoreTake(sdMutex, 0)) {  // only process if Core 0 isn't browsing files
            playing = mp3->loop();
            xSemaphoreGive(sdMutex);
        } else {
            playing = true; // Pretend it played just this loop to not stop abruptly
        }
        
        if (!playing) {
            mp3->stop();
            if (loopEnabled && currentFilename.length() > 0) {
                // Restart seamlessly
                if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
                    fileSource->close();
                    fileSource->open(currentFilename.c_str());
                    mp3->begin(fileSource, out);
                    xSemaphoreGive(sdMutex);
                }
            } else {
                currentFilename = "";
                Serial.println("Playback finished");
            }
        }
    }
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

    loopEnabled = doc["loop"] | false;
    if (!filename.startsWith("/")) {
        filename = "/" + filename;
    }
    
    currentFilename = filename;
    
    if (xSemaphoreTake(audioMutex, portMAX_DELAY)) {
        pendingPlayFilename = filename;
        pendingStopFlag = true;
        pendingPlayFlag = true;
        xSemaphoreGive(audioMutex);
    }
    
    server.send(200, "application/json", "{\"status\":\"Playback started\",\"filename\":\"" + filename + "\"}");
}

void handleApiStop() {
    currentFilename = "";
    if (xSemaphoreTake(audioMutex, portMAX_DELAY)) {
        pendingStopFlag = true;
        xSemaphoreGive(audioMutex);
    }
    server.send(200, "application/json", "{\"status\":\"Playback stopped\"}");
}

void handleApiPause() {
    if (xSemaphoreTake(audioMutex, portMAX_DELAY)) {
        pendingPauseFlag = true;
        xSemaphoreGive(audioMutex);
    }
    server.send(200, "application/json", "{\"status\":\"Pause/Resume toggled\"}");
}

void handleApiCurrent() {
    DynamicJsonDocument doc(512);
    
    doc["playing"] = (mp3 && mp3->isRunning() && !isPaused);
    doc["paused"] = isPaused;
    doc["filename"] = currentFilename;
    doc["loop"] = loopEnabled;

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
    deserializeJson(doc, server.arg("plain"));
    
    speakerVolumePct = doc["level"] | speakerVolumePct;
    if (speakerVolumePct > 100) speakerVolumePct = 100;
    
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
    deserializeJson(doc, server.arg("plain"));
    
    exciterVolumePct = doc["level"] | exciterVolumePct;
    if (exciterVolumePct > 100) exciterVolumePct = 100;

    prefs.putUInt("exc_vol", exciterVolumePct);
    updateAudioVolumeAndBalance();
    
    server.send(200, "application/json", "{\"status\":\"Exciter volume set\"}");
}
