# 1 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
# 2 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 3 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 4 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 5 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 6 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 7 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 8 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2

// ESP-IDF Low-Level I2S
# 11 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2


// ESP8266Audio Libraries
# 15 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 16 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 17 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2
# 18 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 2

// -------------------- Pins --------------------
# 29 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
// -------------------- WiFi --------------------
const char* ssid = "IIIT-Guest";
const char* password = "f6s68VHJ89mC";

// -------------------- Globals --------------------
WebServer server(80);
Preferences prefs;

TaskHandle_t audioTaskHandle = 
# 37 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                              __null
# 37 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                                  ;

uint8_t speakerVolumePct = 75; // Right channel (Speaker)
uint8_t exciterVolumePct = 50; // Left channel (Exciter)

bool loopEnabled = false;
String currentFilename = "";
bool isPaused = false;

enum MediaType { MEDIA_NONE, MEDIA_AUDIO, MEDIA_VIDEO };
MediaType currentMediaType = MEDIA_NONE;

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
        tx_chan = 
# 64 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                 __null
# 64 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                     ;
    }

    virtual ~AudioOutputHaptic() {
        stop();
    }

    virtual bool SetRate(int hz) override {
        if (hz == _sampleRate) return true;
        _sampleRate = hz;
        if (tx_chan) {
            i2s_channel_disable(tx_chan);
            i2s_std_clk_config_t clk_cfg = { .sample_rate_hz = (uint32_t)_sampleRate, .clk_src = I2S_CLK_SRC_DEFAULT, .ext_clk_freq_hz = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8, };
            i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
            i2s_channel_enable(tx_chan);
        }
        return true;
    }

    // Base class does not define SetBitsPerSample/SetChannels so we drop 'override' or omit
    bool SetBitsPerSample(int bits) { return true; } // Forced to 16-bit
    bool SetChannels(int channels) { return true; } // Forced to Stereo DIN

    virtual bool begin() override {
        // 1. Allocate an I2S TX channel (IDF v5)
        i2s_chan_config_t chan_cfg = { .id = I2S_NUM_AUTO, .role = I2S_ROLE_MASTER, .dma_desc_num = 6, .dma_frame_num = 240, .auto_clear_after_cb = false, .auto_clear_before_cb = false, .allow_pd = false, .intr_priority = 0, };
        chan_cfg.auto_clear = true; // Auto clear TX buffer
        esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, 
# 91 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                                                            __null
# 91 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                                                                );
        if (err != 0 /*!< esp_err_t value indicating success (no error) */) {
            Serial0.println("I2S channel allocation failed");
            return false;
        }

        // 2. Configure for Standard I2S Mode
        i2s_std_config_t std_cfg = {
            .clk_cfg = { .sample_rate_hz = (uint32_t)_sampleRate, .clk_src = I2S_CLK_SRC_DEFAULT, .ext_clk_freq_hz = 0, .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8, },
            .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true, .left_align = true, .big_endian = false, .bit_order_lsb = false },
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC /*!< Used in i2s_gpio_config_t for signals which are not used */,
                .bclk = (gpio_num_t)_bclk,
                .ws = (gpio_num_t)_lrc,
                .dout = (gpio_num_t)_dout,
                .din = GPIO_NUM_NC /*!< Used in i2s_gpio_config_t for signals which are not used */,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
        if (err != 0 /*!< esp_err_t value indicating success (no error) */) {
            Serial0.println("I2S std mode initialization failed");
            return false;
        }

        // 3. Enable the channel
        err = i2s_channel_enable(tx_chan);
        if (err != 0 /*!< esp_err_t value indicating success (no error) */) {
            Serial0.println("I2S channel enable failed");
            return false;
        }

        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!tx_chan) return false;

        int16_t scaled[2];
        // Average L&R to ensure mono source, then scale independently
        int32_t mono = (sample[0] + sample[1]) / 2;

        // Apply independent volumes. Left=Exciter, Right=Speaker
        scaled[0] = max(-32768, min(32767, (int)(mono * leftVol)));
        scaled[1] = max(-32768, min(32767, (int)(mono * rightVol)));

        size_t bytes_written = 0;
        // Non-blocking-ish write keeps control path responsive during rapid API commands
        esp_err_t err = i2s_channel_write(tx_chan, &scaled, 4, &bytes_written, 0);
        return err == 0 /*!< esp_err_t value indicating success (no error) */;
    }

    virtual uint16_t ConsumeSamples(int16_t *samples, uint16_t count) override {
        // We override this to process blocks via our math pipeline
        for (uint16_t i=0; i<count; i++) {
            ConsumeSample(samples + i*2);
        }
        return count;
    }

    virtual bool stop() override {
        if (tx_chan) {
            i2s_channel_disable(tx_chan);
            i2s_del_channel(tx_chan);
            tx_chan = 
# 160 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                     __null
# 160 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                         ;
        }
        return true;
    }
};

AudioFileSourceSD *fileSource = 
# 166 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                               __null
# 166 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                                   ;
AudioFileSourceID3 *id3Source = 
# 167 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                               __null
# 167 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                                   ;
AudioGeneratorMP3 *mp3 = 
# 168 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                        __null
# 168 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                            ;
AudioOutputHaptic *out = 
# 169 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                        __null
# 169 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                            ;

// -------------------- Forward Declarations & IPC --------------------
void handleApiFiles();
void handleApiPlay();
void handleApiStop();
void handleApiPause();
void handleApiSeek();
void handleApiCurrent();
void handleApiVolumeSpeaker();
void handleApiVolumeExciter();
void updateAudioVolumeAndBalance();
bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte = 0);
bool _seekAudioToSeconds(uint32_t positionSec);

SemaphoreHandle_t sdMutex = 
# 184 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                           __null
# 184 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                               ;

// -------------------- Loop (Core 1) --------------------
void _stopAudioSafely() {
    isPaused = true; // Stop play loop entry before deleting memory
    currentMediaType = MEDIA_NONE;

    if (sdMutex) {
        xQueueSemaphoreTake( ( sdMutex ), ( ( TickType_t ) 0xffffffffUL ) );
    }

    if (mp3) {
        if (mp3->isRunning()) mp3->stop();
        delete mp3;
        mp3 = 
# 198 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
             __null
# 198 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                 ;
    }
    if (id3Source) {
        delete id3Source;
        id3Source = 
# 202 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                   __null
# 202 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                       ;
    }
    if (fileSource) {
        if (fileSource->isOpen()) fileSource->close();
        delete fileSource;
        fileSource = 
# 207 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                    __null
# 207 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                        ;
    }

    if (sdMutex) {
        xQueueGenericSend( ( QueueHandle_t ) ( sdMutex ), 
# 211 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
       __null
# 211 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
       , ( ( TickType_t ) 0U ), ( ( BaseType_t ) 0 ) );
    }

    currentFilename = "";
    loopEnabled = false;
    isPaused = false;
}

bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte) {
    _stopAudioSafely();

    xQueueSemaphoreTake( ( sdMutex ), ( ( TickType_t ) 0xffffffffUL ) );

    bool success = false;
    do {
        Serial0.printf("Checking if file exists: %s\n", filename.c_str());
        if (!SD.exists(filename)) {
            Serial0.println("Play requested but file not found: " + filename);
            break;
        }

        String fnameLower = filename;
        fnameLower.toLowerCase();
        if (!fnameLower.endsWith(".mp3")) {
            Serial0.println("Unsupported media for Stage 1: " + filename);
            break;
        }

        fileSource = new AudioFileSourceSD(filename.c_str());
        if (!fileSource || !fileSource->isOpen()) {
            Serial0.println("Failed to open media file");
            break;
        }

        if (startByte > 0) {
            uint32_t fileSize = fileSource->getSize();
            uint32_t clamped = startByte;
            if (fileSize > 0 && clamped >= fileSize) {
                clamped = fileSize - 1;
            }
            fileSource->seek(clamped, 
# 251 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3
                                     0
# 251 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                                             );
        }

        id3Source = new AudioFileSourceID3(fileSource);
        mp3 = new AudioGeneratorMP3();
        if (!id3Source || !mp3) {
            Serial0.println("Failed to create decoder pipeline");
            break;
        }

        if (!mp3->begin(id3Source, out)) {
            Serial0.println("Failed to start MP3 decoder");
            break;
        }

        currentMediaType = MEDIA_AUDIO;
        currentFilename = filename;
        loopEnabled = loopRequested;
        isPaused = false;
        Serial0.println("Starting MP3 Audio Decoder...");
        success = true;
    } while (false);

    if (!success) {
        if (mp3) { delete mp3; mp3 = 
# 275 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                                    __null
# 275 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                                        ; }
        if (id3Source) { delete id3Source; id3Source = 
# 276 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                                                      __null
# 276 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                                                          ; }
        if (fileSource) {
            if (fileSource->isOpen()) fileSource->close();
            delete fileSource;
            fileSource = 
# 280 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
                        __null
# 280 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
                            ;
        }
        currentMediaType = MEDIA_NONE;
        currentFilename = "";
        loopEnabled = false;
        isPaused = false;
    }

    xQueueGenericSend( ( QueueHandle_t ) ( sdMutex ), 
# 288 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
   __null
# 288 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
   , ( ( TickType_t ) 0U ), ( ( BaseType_t ) 0 ) );
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

// -------------------- Setup --------------------
void setup() {
    Serial0.begin(115200);
    delay(1000);
    Serial0.println("\n--- ESP32 Haptic Core (ESP8266Audio Version) ---");

    // Load preferences
    prefs.begin("settings", false);
    speakerVolumePct = prefs.getUInt("spk_vol", 75);
    exciterVolumePct = prefs.getUInt("exc_vol", 50);

    // Initialize SPI and SD
    SPI.begin(12, 13, 11);
    SPI.setFrequency(4000000); // 4MHz for stable SD reading with ESP8266Audio

    if (!SD.begin(10)) {
        Serial0.println("SD Card initialization failed!");
    } else {
        Serial0.println("SD Card initialized.");
    }

    sdMutex = xQueueCreateMutex( ( ( uint8_t ) 1U ) );

    // Initialize custom ESP-IDF Direct DMA Audio Pipeline
    out = new AudioOutputHaptic(5, 6, 7);

    updateAudioVolumeAndBalance();

    // Connect WiFi
    WiFi.begin(ssid, password);
    Serial0.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial0.print(".");
    }
    Serial0.println("\nWiFi connected.");
    Serial0.println(WiFi.localIP());

    // Setup routes
    server.on("/api/media/files", HTTP_GET, handleApiFiles);
    server.on("/api/media/play", HTTP_POST, handleApiPlay);
    server.on("/api/media/stop", HTTP_POST, handleApiStop);
    server.on("/api/media/pause", HTTP_POST, handleApiPause);
    server.on("/api/media/seek", HTTP_POST, handleApiSeek);
    server.on("/api/media/current", HTTP_GET, handleApiCurrent);
    server.on("/api/volume/speaker", HTTP_POST, handleApiVolumeSpeaker);
    server.on("/api/volume/exciter", HTTP_POST, handleApiVolumeExciter);

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

    // HTTP and playback run in same loop/core for deterministic behavior
}

// -------------------- Loop (Core 1) --------------------
// (Moved _stopAudioSafely up above setup)

void loop() {
    // Serve HTTP in same core/context as playback to avoid cross-core races
    server.handleClient();

    // Audio Playback Pump safely wrapped in sdMutex to prevent SPI collisions
    if (!isPaused && currentMediaType == MEDIA_AUDIO && mp3 && mp3->isRunning()) {
        bool playing = false;

        // Use a safe pointer check before calling methods on mp3
        if (mp3 && xQueueSemaphoreTake( ( sdMutex ), ( 0 ) )) {
            playing = mp3->loop();
            xQueueGenericSend( ( QueueHandle_t ) ( sdMutex ), 
# 383 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
           __null
# 383 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
           , ( ( TickType_t ) 0U ), ( ( BaseType_t ) 0 ) );
        } else {
            playing = true; // Pretend it played just this loop to not stop abruptly
        }

        if (!playing) {
            String finishedTrack = currentFilename;
            bool shouldLoop = loopEnabled;
            _stopAudioSafely();

            if (shouldLoop && finishedTrack.length() > 0) {
                Serial0.println("Restarting loop...");
                _startAudioPlayback(finishedTrack, true, 0);
            } else {
                Serial0.println("Playback finished naturally");
            }
        }
    }

    // Relax Core 1 watchdogs (critical for smooth system operation)
    vTaskDelay(1);
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

    if (xQueueSemaphoreTake( ( sdMutex ), ( ( TickType_t ) 0xffffffffUL ) )) {
        File root = SD.open("/");
        if (!root) {
            xQueueGenericSend( ( QueueHandle_t ) ( sdMutex ), 
# 422 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
           __null
# 422 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
           , ( ( TickType_t ) 0U ), ( ( BaseType_t ) 0 ) );
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
        xQueueGenericSend( ( QueueHandle_t ) ( sdMutex ), 
# 447 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
       __null
# 447 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
       , ( ( TickType_t ) 0U ), ( ( BaseType_t ) 0 ) );
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

    Serial0.println("Received /api/media/play");

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

    Serial0.printf("AUDIO_CMD_PLAY: %s loop=%s\n", filename.c_str(), requestedLoop ? "true" : "false");
    bool ok = _startAudioPlayback(filename, requestedLoop, 0);
    if (!ok) {
        server.send(404, "application/json", "{\"error\":\"Failed to start playback\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"Playback configured\",\"filename\":\"" + filename + "\"}");
}

void handleApiStop() {
    Serial0.println("AUDIO_CMD_STOP");
    _stopAudioSafely();
    server.send(200, "application/json", "{\"status\":\"Playback stopped\"}");
}

void handleApiPause() {
    Serial0.println("AUDIO_CMD_TOGGLE_PAUSE");
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

    Serial0.printf("AUDIO_CMD_SEEK: %d sec\n", positionSec);
    if (!_seekAudioToSeconds((uint32_t)positionSec)) {
        server.send(409, "application/json", "{\"error\":\"Seek failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"Seek completed\"}");
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
        if (xQueueSemaphoreTake( ( sdMutex ), ( ( ( TickType_t ) ( ( ( TickType_t ) ( 20 ) * ( TickType_t ) 1000 ) / ( TickType_t ) 1000U ) ) ) ) == ( ( BaseType_t ) 1 )) {
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
            xQueueGenericSend( ( QueueHandle_t ) ( sdMutex ), 
# 557 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino" 3 4
           __null
# 557 "/home/chandrashekar/Arduino/esp32_haptic/esp32_haptic.ino"
           , ( ( TickType_t ) 0U ), ( ( BaseType_t ) 0 ) );
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
