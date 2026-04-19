#include <FastLED.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <jpeg_decoder.h>

// ESP-IDF Low-Level I2S
#include <driver/i2s_std.h>
#include <driver/gpio.h>

// ESP8266Audio Libraries
#include "AudioFileSourceSD.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioLogger.h"
#include "AudioOutput.h" // We MUST use Base and rewrite the driver completely for IDF v5

// -------------------- Pins --------------------
#define SPI_MOSI 11
#define SPI_SCLK 12
#define SPI_MISO 13
#define SD_CS    10

#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 7

// -------------------- ST7789 Display Pins (separate SPI host) --------------------
#define TFT_SCLK 18
#define TFT_MOSI 17
#define TFT_CS   16
#define TFT_DC   15
#define TFT_RST  4
#define TFT_BL   21
#define TFT_WIDTH 240
#define TFT_HEIGHT 280
#define TFT_X_OFFSET 0
#define TFT_Y_OFFSET 20
#define TFT_SPI_HZ 40000000

// -------------------- LED Pins ----------------
#define LED_PIN  40
#define NUM_LEDS 24

// -------------------- WiFi --------------------
const char* ssid = "IIIT-Guest";
const char* password = "f6s68VHJ89mC";

// -------------------- Globals --------------------
WebServer server(80);
Preferences prefs;

TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t videoDecodeTaskHandle = NULL;
TaskHandle_t displayFlushTaskHandle = NULL;

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

struct VideoPacket {
    uint32_t ptsMs;
    uint32_t dataLen;
    bool keyFrame;
    uint16_t color565;
    uint8_t frameIndex;
};

QueueHandle_t videoDisplayQueue = NULL;
volatile bool videoPipelineActive = false;
volatile bool displayReady = false;
volatile uint32_t videoTargetFps = 15;
volatile uint32_t videoFramesDecoded = 0;
volatile uint32_t videoFramesDisplayed = 0;
volatile uint32_t videoFramesDropped = 0;
volatile int32_t videoAvDriftMs = 0;
volatile uint32_t videoStartMs = 0;
volatile bool videoUseFileStream = false;
volatile bool videoUseRawFrameStream = false;
volatile bool videoUseMjpegStream = false;
volatile bool videoStreamEof = false;
uint32_t videoStreamHeaderSize = 0;

File videoStreamFile;
File videoRawFile;
File videoMjpegFile;
uint32_t videoRawHeaderSize = 0;
uint16_t videoRawWidth = TFT_WIDTH;
uint16_t videoRawHeight = TFT_HEIGHT;
uint16_t videoRawFps = 15;
size_t videoRawFrameSize = 0;
uint8_t *videoFrameBuffers[2] = {NULL, NULL};
uint8_t *videoMjpegFrameData = NULL;
size_t videoMjpegFrameCapacity = 0;
uint8_t *videoMjpegDecodeBuffer = NULL;
size_t videoMjpegDecodeBufferSize = 0;
volatile bool videoMjpegLastReadEof = false;

#define MJPEG_MAX_FRAME_BYTES (300U * 1024U)
#define MJPEG_DECODE_MAX_PIXELS (320U * 320U)
#define MJPEG_READ_CHUNK_BYTES 8192
#define MJPEG_DEFAULT_FPS 12
#define RGB_RAW_FIXED_FPS 15
#define MJPEG_SWAP_RGB565_BYTES 1
#define MJPEG_FLIP_X 0
#define MJPEG_FLIP_Y 0
#define MJPEG_ROTATE_90_CW 1
#define MJPEG_FILL_SCREEN_CROP 1
#define VIDEO_LAG_SKIP_THRESHOLD_MS 120
#define VIDEO_LAG_CRITICAL_MS 260

volatile bool videoCompanionAudioActive = false;
bool videoCompanionAudioLoop = false;
String videoCompanionAudioFile = "";
volatile uint32_t videoCompanionAudioStartMs = 0;
volatile bool videoSdReadInProgress = false;
uint8_t *videoCompanionMp3Data = NULL;
size_t videoCompanionMp3Size = 0;
volatile bool videoCompanionInRam = false;

#define VIDEO_COMPANION_MP3_MAX_BYTES (5U * 1024U * 1024U)

enum VideoStartState : uint8_t {
    VIDEO_START_IDLE = 0,
    VIDEO_START_OPENING_STREAM = 1,
    VIDEO_START_OPENING_AUDIO = 2,
    VIDEO_START_READY = 3,
    VIDEO_START_FAILED = 4,
};

volatile uint32_t videoSessionId = 0;
volatile uint8_t videoStartState = VIDEO_START_IDLE;

static const BaseType_t CORE_VIDEO = 1;
static const BaseType_t CORE_SERVICE = 0;
static const UBaseType_t PRIO_WEB = 1;
static const UBaseType_t PRIO_LED = 2;
static const UBaseType_t PRIO_VIDEO = 5;
static const UBaseType_t PRIO_AUDIO = 4;
static const TickType_t WEB_DELAY_NORMAL = pdMS_TO_TICKS(10);
static const TickType_t WEB_DELAY_VIDEO = pdMS_TO_TICKS(30);
static const TickType_t LED_DELAY_NORMAL = pdMS_TO_TICKS(30);
static const TickType_t LED_DELAY_VIDEO = pdMS_TO_TICKS(50);

SPIClass *tftSpi = NULL;
SemaphoreHandle_t tftMutex = NULL;

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
void taskVideoDecode(void *pvParameters);
void taskDisplayFlush(void *pvParameters);
void initDisplayVideoScaffold();
bool initSt7789Panel();
void st7789WriteCommand(uint8_t command, const uint8_t *data, size_t dataLen);
void st7789SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void st7789FillColor(uint16_t color);
void st7789DrawFrameRGB565(const uint8_t *frameData, uint16_t width, uint16_t height);
bool _startVideoPlayback(const String& filename, bool loopRequested);
bool _openVideoColorStream(const String& filename);
bool _readNextVideoPacket(VideoPacket &packet);
void _closeVideoColorStream();
bool _openRawFrameStream(const String& filename);
bool _readRawFrameToBuffer(uint8_t frameIndex);
bool _skipRawFrames(uint32_t frameCount);
void _closeRawFrameStream();
bool _openMjpegStream(const String& filename);
bool _readMjpegFrameBytes(size_t *outFrameLen);
bool _readMjpegFrameToBuffer(uint8_t frameIndex);
bool _skipMjpegFrames(uint32_t frameCount);
void _closeMjpegStream();
void _stopAudioDecoderOnly();
void _tryStartVideoCompanionAudio(const String& videoFilename, bool loopRequested);
void updateAudioVolumeAndBalance();
bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte = 0, bool updateMediaState = true);
bool _seekAudioToSeconds(uint32_t positionSec);
bool _enqueueAudioCommand(const AudioCommandMessage &cmd, uint32_t timeoutMs = 50);
uint32_t _videoPlaybackClockMs();
void _videoSdReadBegin();
void _videoSdReadEnd();
void _releaseCompanionAudioRam();
bool _loadFileToPsram(const String& filename, uint8_t **outData, size_t *outSize);
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

    _closeVideoColorStream();
    _closeRawFrameStream();
    _closeMjpegStream();
    videoUseFileStream = false;
    videoUseRawFrameStream = false;
    videoUseMjpegStream = false;
    videoStreamEof = false;

    if (sdMutex) {
        xSemaphoreGive(sdMutex);
    }

    _releaseCompanionAudioRam();

    currentFilename = "";
    loopEnabled = false;
    videoCompanionAudioActive = false;
    videoCompanionAudioLoop = false;
    videoCompanionAudioFile = "";
    videoCompanionAudioStartMs = 0;
    videoStartState = VIDEO_START_IDLE;
    isPaused = false;
}

void _stopAudioDecoderOnly() {
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

    _releaseCompanionAudioRam();
}

void _releaseCompanionAudioRam() {
    if (videoCompanionMp3Data) {
        free(videoCompanionMp3Data);
        videoCompanionMp3Data = NULL;
    }
    videoCompanionMp3Size = 0;
    videoCompanionInRam = false;
}

bool _loadFileToPsram(const String& filename, uint8_t **outData, size_t *outSize) {
    if (!outData || !outSize || !sdMutex) return false;
    *outData = NULL;
    *outSize = 0;

    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(350))) {
        return false;
    }

    bool ok = false;
    do {
        File src = SD.open(filename, FILE_READ);
        if (!src) break;

        size_t sz = src.size();
        if (sz == 0 || sz > VIDEO_COMPANION_MP3_MAX_BYTES) {
            src.close();
            break;
        }

        uint8_t *buffer = (uint8_t*)ps_malloc(sz);
        if (!buffer) {
            src.close();
            break;
        }

        size_t offset = 0;
        while (offset < sz) {
            size_t toRead = min((size_t)2048, sz - offset);
            int n = src.read(buffer + offset, toRead);
            if (n <= 0) {
                break;
            }
            offset += (size_t)n;

            if ((offset & 0x7FFFU) == 0U) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        src.close();

        if (offset != sz) {
            free(buffer);
            break;
        }

        *outData = buffer;
        *outSize = sz;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte, bool updateMediaState) {
    if (updateMediaState) {
        _stopAudioSafely();
    } else {
        _stopAudioDecoderOnly();
    }

    if (!updateMediaState) {
        bool success = false;
        do {
            Serial.printf("Opening companion audio file (PSRAM): %s\n", filename.c_str());

            String fnameLower = filename;
            fnameLower.toLowerCase();
            if (!fnameLower.endsWith(".mp3")) {
                Serial.println("Unsupported companion audio format");
                break;
            }

            if (!_loadFileToPsram(filename, &videoCompanionMp3Data, &videoCompanionMp3Size)) {
                Serial.println("Failed to preload companion MP3 to PSRAM");
                break;
            }

            fileSource = new AudioFileSourcePROGMEM(videoCompanionMp3Data, (int32_t)videoCompanionMp3Size);
            if (!fileSource) {
                Serial.println("Failed to create PROGMEM source");
                break;
            }

            id3Source = new AudioFileSourceID3(fileSource);
            mp3 = new AudioGeneratorMP3();
            if (!id3Source || !mp3) {
                Serial.println("Failed to create companion decoder pipeline");
                break;
            }

            if (!mp3->begin(id3Source, out)) {
                Serial.println("Failed to start companion MP3 decoder");
                break;
            }

            videoCompanionInRam = true;
            videoCompanionAudioActive = true;
            videoCompanionAudioLoop = loopRequested;
            videoCompanionAudioFile = filename;
            videoCompanionAudioStartMs = millis();
            isPaused = false;
            success = true;
            Serial.println("Companion MP3 started from PSRAM");
        } while (false);

        if (!success) {
            if (mp3) { delete mp3; mp3 = NULL; }
            if (id3Source) { delete id3Source; id3Source = NULL; }
            if (fileSource) { delete fileSource; fileSource = NULL; }
            _releaseCompanionAudioRam();
            videoCompanionAudioActive = false;
            videoCompanionAudioLoop = false;
            videoCompanionAudioFile = "";
            videoCompanionAudioStartMs = 0;
            isPaused = false;
        }

        return success;
    }

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

        if (updateMediaState) {
            currentMediaType = MEDIA_AUDIO;
            currentFilename = filename;
            loopEnabled = loopRequested;
        } else {
            videoCompanionAudioActive = true;
            videoCompanionAudioLoop = loopRequested;
            videoCompanionAudioFile = filename;
            videoCompanionAudioStartMs = millis();
        }
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
        if (updateMediaState) {
            currentMediaType = MEDIA_NONE;
            currentFilename = "";
            loopEnabled = false;
        } else {
            videoCompanionAudioActive = false;
            videoCompanionAudioLoop = false;
            videoCompanionAudioFile = "";
            videoCompanionAudioStartMs = 0;
        }
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

uint32_t _videoPlaybackClockMs() {
    if (videoCompanionAudioActive && videoCompanionAudioStartMs != 0) {
        return millis() - videoCompanionAudioStartMs;
    }
    return millis() - videoStartMs;
}

void _videoSdReadBegin() {
    videoSdReadInProgress = true;
}

void _videoSdReadEnd() {
    videoSdReadInProgress = false;
}

// -------------------- Tasks --------------------
void taskWebServer(void *pvParameters) {
    while (true) {
        server.handleClient();
        TickType_t delayTicks = WEB_DELAY_NORMAL;
        if (currentMediaType == MEDIA_VIDEO && !isUploading) {
            delayTicks = WEB_DELAY_VIDEO;
        }
        vTaskDelay(delayTicks);
    }
}

void taskAudio(void *pvParameters) {
    uint32_t lastVideoAudioPumpMs = 0;
    while (true) {
        if (audioAbortRequested) {
            if (currentMediaType == MEDIA_VIDEO || videoStartState == VIDEO_START_OPENING_STREAM || videoStartState == VIDEO_START_OPENING_AUDIO) {
                audioAbortRequested = false;
            } else if (mp3) {
                _stopAudioSafely();
                audioAbortRequested = false;
            } else {
                audioAbortRequested = false;
            }
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
        if (!isPaused && mp3 && mp3->isRunning()) {
            bool playing = false;

            bool isCompanionDuringVideo = (currentMediaType == MEDIA_VIDEO && videoCompanionAudioActive);

            if (isCompanionDuringVideo && videoCompanionInRam) {
                uint32_t now = millis();
                if ((now - lastVideoAudioPumpMs) < 3U) {
                    playing = true;
                } else if (mp3) {
                    playing = mp3->loop();
                    lastVideoAudioPumpMs = now;
                } else {
                    playing = true;
                }
            } else if (isCompanionDuringVideo) {
                if (videoSdReadInProgress) {
                    playing = true;
                } else if (mp3 && xSemaphoreTake(sdMutex, 0)) {
                    playing = mp3->loop();
                    xSemaphoreGive(sdMutex);
                } else {
                    playing = true;
                }
            } else if (mp3 && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10))) {
                playing = mp3->loop();
                xSemaphoreGive(sdMutex);
            } else {
                playing = true;
            }
            
            if (!playing) {
                if (currentMediaType == MEDIA_AUDIO) {
                    String finishedTrack = currentFilename;
                    bool shouldLoop = loopEnabled;
                    _stopAudioSafely();

                    if (shouldLoop && finishedTrack.length() > 0) {
                        Serial.println("Restarting loop...");
                        _startAudioPlayback(finishedTrack, true, 0, true);
                    } else {
                        Serial.println("Playback finished naturally");
                    }
                } else if (videoCompanionAudioActive) {
                    String finishedTrack = videoCompanionAudioFile;
                    bool shouldLoop = videoCompanionAudioLoop;
                    _stopAudioDecoderOnly();

                    if (shouldLoop && finishedTrack.length() > 0) {
                        _startAudioPlayback(finishedTrack, true, 0, false);
                    } else {
                        videoCompanionAudioActive = false;
                        videoCompanionAudioLoop = false;
                        videoCompanionAudioFile = "";
                    }
                }
            }
        }
        TickType_t audioDelay = (currentMediaType == MEDIA_VIDEO && videoCompanionAudioActive) ? pdMS_TO_TICKS(5) : pdMS_TO_TICKS(2);
        vTaskDelay(audioDelay);
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
        TickType_t ledDelay = (currentMediaType == MEDIA_VIDEO) ? LED_DELAY_VIDEO : LED_DELAY_NORMAL;
        vTaskDelay(ledDelay);
    }
}

void initDisplayVideoScaffold() {
    pinMode(TFT_CS, OUTPUT);
    pinMode(TFT_DC, OUTPUT);
    pinMode(TFT_RST, OUTPUT);
    pinMode(TFT_BL, OUTPUT);

    digitalWrite(TFT_CS, HIGH);
    digitalWrite(TFT_DC, HIGH);
    digitalWrite(TFT_RST, HIGH);
    digitalWrite(TFT_BL, HIGH);

    tftMutex = xSemaphoreCreateMutex();
    tftSpi = new SPIClass(HSPI);
    if (tftSpi) {
        tftSpi->begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
        displayReady = initSt7789Panel();
    }

    videoRawFrameSize = (size_t)TFT_WIDTH * (size_t)TFT_HEIGHT * 2U;
    videoFrameBuffers[0] = (uint8_t*)ps_malloc(videoRawFrameSize);
    videoFrameBuffers[1] = (uint8_t*)ps_malloc(videoRawFrameSize);
    if (!videoFrameBuffers[0] || !videoFrameBuffers[1]) {
        if (videoFrameBuffers[0]) free(videoFrameBuffers[0]);
        if (videoFrameBuffers[1]) free(videoFrameBuffers[1]);
        videoFrameBuffers[0] = NULL;
        videoFrameBuffers[1] = NULL;
        Serial.println("WARN: Raw frame buffers unavailable; .v16 playback disabled.");
    }

    videoMjpegFrameCapacity = MJPEG_MAX_FRAME_BYTES;
    videoMjpegFrameData = (uint8_t*)ps_malloc(videoMjpegFrameCapacity);
    videoMjpegDecodeBufferSize = max(videoRawFrameSize, (size_t)MJPEG_DECODE_MAX_PIXELS * 2U);
    videoMjpegDecodeBuffer = (uint8_t*)ps_malloc(videoMjpegDecodeBufferSize);
    if (!videoMjpegFrameData || !videoMjpegDecodeBuffer) {
        if (videoMjpegFrameData) free(videoMjpegFrameData);
        if (videoMjpegDecodeBuffer) free(videoMjpegDecodeBuffer);
        videoMjpegFrameData = NULL;
        videoMjpegDecodeBuffer = NULL;
        videoMjpegFrameCapacity = 0;
        videoMjpegDecodeBufferSize = 0;
        Serial.println("WARN: MJPEG buffers unavailable; .mjpg/.mjpeg playback disabled.");
    }

    videoDisplayQueue = xQueueCreate(6, sizeof(VideoPacket));

    if (!videoDisplayQueue) {
        Serial.println("WARN: Video queues allocation failed; video pipeline disabled.");
        videoPipelineActive = false;
        return;
    }

    videoPipelineActive = true;
    if (displayReady) {
        st7789FillColor(0xF800);
        delay(120);
        st7789FillColor(0x07E0);
        delay(120);
        st7789FillColor(0x001F);
        delay(120);
        st7789FillColor(0x0000);
        Serial.println("ST7789 panel init OK (RGB test shown)");
    } else {
        Serial.println("WARN: ST7789 panel init failed");
    }
    Serial.println("Video scaffold initialized (decode/display active).");
}

void st7789WriteCommand(uint8_t command, const uint8_t *data, size_t dataLen) {
    if (!tftSpi) return;
    if (tftMutex && xSemaphoreTake(tftMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    tftSpi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);
    digitalWrite(TFT_DC, LOW);
    tftSpi->transfer(command);
    if (data && dataLen > 0) {
        digitalWrite(TFT_DC, HIGH);
        tftSpi->writeBytes(data, (uint32_t)dataLen);
    }
    digitalWrite(TFT_CS, HIGH);
    tftSpi->endTransaction();

    if (tftMutex) xSemaphoreGive(tftMutex);
}

void st7789SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += TFT_X_OFFSET;
    x1 += TFT_X_OFFSET;
    y0 += TFT_Y_OFFSET;
    y1 += TFT_Y_OFFSET;

    uint8_t colData[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    uint8_t rowData[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };

    st7789WriteCommand(0x2A, colData, sizeof(colData));
    st7789WriteCommand(0x2B, rowData, sizeof(rowData));
    st7789WriteCommand(0x2C, NULL, 0);
}

void st7789FillColor(uint16_t color) {
    if (!tftSpi || !displayReady) return;
    if (tftMutex && xSemaphoreTake(tftMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;

    uint8_t lineBuffer[TFT_WIDTH * 2];
    for (int x = 0; x < TFT_WIDTH; x++) {
        lineBuffer[(x * 2)] = (uint8_t)(color >> 8);
        lineBuffer[(x * 2) + 1] = (uint8_t)(color & 0xFF);
    }

    tftSpi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);

    uint16_t x0 = TFT_X_OFFSET;
    uint16_t x1 = TFT_X_OFFSET + TFT_WIDTH - 1;
    uint16_t y0 = TFT_Y_OFFSET;
    uint16_t y1 = TFT_Y_OFFSET + TFT_HEIGHT - 1;

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2A);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(x0 >> 8));
    tftSpi->transfer((uint8_t)(x0 & 0xFF));
    tftSpi->transfer((uint8_t)(x1 >> 8));
    tftSpi->transfer((uint8_t)(x1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2B);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(y0 >> 8));
    tftSpi->transfer((uint8_t)(y0 & 0xFF));
    tftSpi->transfer((uint8_t)(y1 >> 8));
    tftSpi->transfer((uint8_t)(y1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2C);
    digitalWrite(TFT_DC, HIGH);

    for (int y = 0; y < TFT_HEIGHT; y++) {
        tftSpi->writeBytes(lineBuffer, (uint32_t)sizeof(lineBuffer));
    }

    digitalWrite(TFT_CS, HIGH);
    tftSpi->endTransaction();

    if (tftMutex) xSemaphoreGive(tftMutex);
}

void st7789DrawFrameRGB565(const uint8_t *frameData, uint16_t width, uint16_t height) {
    if (!tftSpi || !displayReady || !frameData) return;
    if (width != TFT_WIDTH || height != TFT_HEIGHT) return;
    if (tftMutex && xSemaphoreTake(tftMutex, pdMS_TO_TICKS(40)) != pdTRUE) return;

    tftSpi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);

    uint16_t x0 = TFT_X_OFFSET;
    uint16_t x1 = TFT_X_OFFSET + width - 1;
    uint16_t y0 = TFT_Y_OFFSET;
    uint16_t y1 = TFT_Y_OFFSET + height - 1;

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2A);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(x0 >> 8));
    tftSpi->transfer((uint8_t)(x0 & 0xFF));
    tftSpi->transfer((uint8_t)(x1 >> 8));
    tftSpi->transfer((uint8_t)(x1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2B);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(y0 >> 8));
    tftSpi->transfer((uint8_t)(y0 & 0xFF));
    tftSpi->transfer((uint8_t)(y1 >> 8));
    tftSpi->transfer((uint8_t)(y1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2C);
    digitalWrite(TFT_DC, HIGH);

    size_t totalBytes = (size_t)width * (size_t)height * 2U;
    const uint8_t *ptr = frameData;
    while (totalBytes > 0) {
        uint32_t chunk = (totalBytes > 4096U) ? 4096U : (uint32_t)totalBytes;
        tftSpi->writeBytes(ptr, chunk);
        ptr += chunk;
        totalBytes -= chunk;
    }

    digitalWrite(TFT_CS, HIGH);
    tftSpi->endTransaction();

    if (tftMutex) xSemaphoreGive(tftMutex);
}

bool initSt7789Panel() {
    if (!tftSpi) return false;

    digitalWrite(TFT_RST, LOW);
    delay(20);
    digitalWrite(TFT_RST, HIGH);
    delay(120);

    st7789WriteCommand(0x01, NULL, 0); // SWRESET
    delay(120);
    st7789WriteCommand(0x11, NULL, 0); // SLPOUT
    delay(120);

    uint8_t colmod = 0x55; // RGB565
    st7789WriteCommand(0x3A, &colmod, 1);

    uint8_t madctl = 0x00;
    st7789WriteCommand(0x36, &madctl, 1);

    uint8_t porch[5] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    st7789WriteCommand(0xB2, porch, sizeof(porch));

    uint8_t gate = 0x35;
    st7789WriteCommand(0xB7, &gate, 1);

    uint8_t vcom = 0x19;
    st7789WriteCommand(0xBB, &vcom, 1);

    uint8_t lcm = 0x2C;
    st7789WriteCommand(0xC0, &lcm, 1);

    uint8_t vdvVrh[2] = {0x01, 0xFF};
    st7789WriteCommand(0xC2, vdvVrh, sizeof(vdvVrh));

    uint8_t vrhs = 0x12;
    st7789WriteCommand(0xC3, &vrhs, 1);

    uint8_t vdvs = 0x20;
    st7789WriteCommand(0xC4, &vdvs, 1);

    uint8_t frctrl2 = 0x0F;
    st7789WriteCommand(0xC6, &frctrl2, 1);

    uint8_t pwctrl1 = 0xA4;
    st7789WriteCommand(0xD0, &pwctrl1, 1);

    st7789WriteCommand(0x21, NULL, 0); // INVON
    st7789WriteCommand(0x13, NULL, 0); // NORON
    st7789WriteCommand(0x29, NULL, 0); // DISPON
    delay(20);

    digitalWrite(TFT_BL, HIGH);
    return true;
}

bool _startVideoPlayback(const String& filename, bool loopRequested) {
    _stopAudioSafely();

    videoStartState = VIDEO_START_OPENING_STREAM;
    videoSessionId++;

    videoFramesDecoded = 0;
    videoFramesDisplayed = 0;
    videoFramesDropped = 0;
    videoAvDriftMs = 0;
    videoStartMs = millis();
    videoUseFileStream = false;
    videoUseRawFrameStream = false;
    videoUseMjpegStream = false;
    videoStreamEof = false;

    String lower = filename;
    lower.toLowerCase();
    if (lower.endsWith(".v16") || lower.endsWith(".rgb")) {
        videoUseRawFrameStream = _openRawFrameStream(filename);
        if (!videoUseRawFrameStream) {
            Serial.println("Video raw stream open failed.");
            videoStartState = VIDEO_START_FAILED;
            return false;
        }
    } else if (lower.endsWith(".mjpg") || lower.endsWith(".mjpeg")) {
        videoUseMjpegStream = _openMjpegStream(filename);
        if (!videoUseMjpegStream) {
            Serial.println("Video MJPEG stream open failed.");
            videoStartState = VIDEO_START_FAILED;
            return false;
        }
    } else if (lower.endsWith(".vid")) {
        videoUseFileStream = _openVideoColorStream(filename);
        if (!videoUseFileStream) {
            Serial.println("Video .vid stream open failed.");
            videoStartState = VIDEO_START_FAILED;
            return false;
        }
    } else {
        Serial.println("Unsupported video extension.");
        videoStartState = VIDEO_START_FAILED;
        return false;
    }

    if (videoDisplayQueue) xQueueReset(videoDisplayQueue);

    currentFilename = filename;
    loopEnabled = loopRequested;
    isPaused = false;

    videoStartState = VIDEO_START_OPENING_AUDIO;
    _tryStartVideoCompanionAudio(filename, loopRequested);
    if (videoCompanionAudioActive && videoCompanionAudioStartMs != 0) {
        videoStartMs = videoCompanionAudioStartMs;
    } else {
        videoStartMs = millis();
    }

    currentMediaType = MEDIA_VIDEO;
    videoStartState = VIDEO_START_READY;

    const char* modeStr = "synthetic";
    if (videoUseRawFrameStream) modeStr = "raw-frame";
    else if (videoUseMjpegStream) modeStr = "mjpeg";
    else if (videoUseFileStream) modeStr = "color-stream";
    Serial.printf("Video playback started (%s).\n", modeStr);
    return true;
}

bool _openVideoColorStream(const String& filename) {
    if (!sdMutex) return false;

    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300))) {
        return false;
    }

    bool ok = false;
    do {
        if (videoStreamFile) {
            videoStreamFile.close();
        }

        videoStreamFile = SD.open(filename, FILE_READ);
        if (!videoStreamFile) {
            break;
        }

        uint8_t magic[4] = {0};
        if (videoStreamFile.read(magic, sizeof(magic)) != (int)sizeof(magic)) {
            videoStreamFile.close();
            break;
        }

        if (!(magic[0] == 'V' && magic[1] == 'C' && magic[2] == 'L' && magic[3] == 'R')) {
            videoStreamFile.close();
            break;
        }

        videoStreamHeaderSize = 4;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _readNextVideoPacket(VideoPacket &packet) {
    if (!sdMutex || !videoStreamFile) return false;
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100))) {
        _videoSdReadEnd();
        return false;
    }

    uint8_t buffer[6] = {0};
    int readLen = videoStreamFile.read(buffer, sizeof(buffer));
    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();

    if (readLen != (int)sizeof(buffer)) {
        return false;
    }

    packet.ptsMs = (uint32_t)buffer[0] |
                   ((uint32_t)buffer[1] << 8) |
                   ((uint32_t)buffer[2] << 16) |
                   ((uint32_t)buffer[3] << 24);
    packet.color565 = (uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8);
    packet.dataLen = videoFramesDecoded;
    packet.keyFrame = ((videoFramesDecoded % 30U) == 0U);
    return true;
}

void _closeVideoColorStream() {
    if (videoStreamFile) {
        videoStreamFile.close();
    }
}

bool _openRawFrameStream(const String& filename) {
    if (!sdMutex || !videoFrameBuffers[0] || !videoFrameBuffers[1]) return false;
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300))) return false;

    bool ok = false;
    do {
        if (videoRawFile) {
            videoRawFile.close();
        }

        videoRawFile = SD.open(filename, FILE_READ);
        if (!videoRawFile) break;

        String lower = filename;
        lower.toLowerCase();
        if (lower.endsWith(".rgb")) {
            videoRawWidth = TFT_WIDTH;
            videoRawHeight = TFT_HEIGHT;
            videoRawFps = RGB_RAW_FIXED_FPS;
            videoRawFrameSize = (size_t)videoRawWidth * (size_t)videoRawHeight * 2U;
            videoRawHeaderSize = 0;
            videoTargetFps = videoRawFps;
            ok = true;
            break;
        }

        uint8_t header[10] = {0};
        if (videoRawFile.read(header, sizeof(header)) != (int)sizeof(header)) {
            videoRawFile.close();
            break;
        }

        if (!(header[0] == 'V' && header[1] == '1' && header[2] == '6' && header[3] == 'F')) {
            videoRawFile.close();
            break;
        }

        videoRawWidth = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
        videoRawHeight = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
        videoRawFps = (uint16_t)header[8] | ((uint16_t)header[9] << 8);

        if (videoRawWidth != TFT_WIDTH || videoRawHeight != TFT_HEIGHT || videoRawFps == 0) {
            videoRawFile.close();
            break;
        }

        videoRawFrameSize = (size_t)videoRawWidth * (size_t)videoRawHeight * 2U;
        videoRawHeaderSize = sizeof(header);
        videoTargetFps = videoRawFps;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _readRawFrameToBuffer(uint8_t frameIndex) {
    if (!sdMutex || !videoRawFile || frameIndex > 1 || !videoFrameBuffers[frameIndex]) return false;
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(150))) {
        _videoSdReadEnd();
        return false;
    }

    int readLen = videoRawFile.read(videoFrameBuffers[frameIndex], videoRawFrameSize);
    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();
    return readLen == (int)videoRawFrameSize;
}

bool _skipRawFrames(uint32_t frameCount) {
    if (!sdMutex || !videoRawFile || frameCount == 0 || videoRawFrameSize == 0) return false;
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(80))) {
        _videoSdReadEnd();
        return false;
    }

    uint64_t skipBytes = (uint64_t)videoRawFrameSize * (uint64_t)frameCount;
    uint64_t curPos = (uint64_t)videoRawFile.position();
    uint64_t fileSize = (uint64_t)videoRawFile.size();
    uint64_t newPos = curPos + skipBytes;
    if (newPos > fileSize) {
        newPos = fileSize;
    }

    bool ok = videoRawFile.seek((uint32_t)newPos, SeekSet);
    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();
    return ok;
}

void _closeRawFrameStream() {
    if (videoRawFile) {
        videoRawFile.close();
    }
}

static esp_jpeg_image_scale_t _selectJpegScale(uint16_t width, uint16_t height) {
    size_t maxPixels = videoMjpegDecodeBufferSize / 2U;
    if (maxPixels == 0) {
        return JPEG_IMAGE_SCALE_1_8;
    }

    for (int s = 0; s <= 3; s++) {
        uint16_t scaledW = width;
        uint16_t scaledH = height;
        for (int i = 0; i < s; i++) {
            scaledW = (uint16_t)((scaledW + 1U) / 2U);
            scaledH = (uint16_t)((scaledH + 1U) / 2U);
        }
        size_t pixels = (size_t)scaledW * (size_t)scaledH;
        if (pixels <= maxPixels) {
            return (esp_jpeg_image_scale_t)s;
        }
    }

    return JPEG_IMAGE_SCALE_1_8;
}

static bool _decodeJpegToFrameBuffer(const uint8_t *jpegData, size_t jpegSize, uint8_t frameIndex) {
    if (!jpegData || jpegSize < 4 || frameIndex > 1) return false;
    if (!videoFrameBuffers[frameIndex] || !videoMjpegDecodeBuffer) return false;

    esp_jpeg_image_cfg_t probeCfg = {};
    probeCfg.indata = (uint8_t*)jpegData;
    probeCfg.indata_size = jpegSize;
    probeCfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    probeCfg.out_scale = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t probeOut = {};
    if (esp_jpeg_get_image_info(&probeCfg, &probeOut) != ESP_OK || probeOut.width == 0 || probeOut.height == 0) {
        return false;
    }

    esp_jpeg_image_scale_t scale = _selectJpegScale(probeOut.width, probeOut.height);

    esp_jpeg_image_cfg_t decodeCfg = {};
    decodeCfg.indata = (uint8_t*)jpegData;
    decodeCfg.indata_size = jpegSize;
    decodeCfg.outbuf = videoMjpegDecodeBuffer;
    decodeCfg.outbuf_size = videoMjpegDecodeBufferSize;
    decodeCfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    decodeCfg.out_scale = scale;
    decodeCfg.flags.swap_color_bytes = 0;

    esp_jpeg_image_output_t decodeOut = {};
    if (esp_jpeg_decode(&decodeCfg, &decodeOut) != ESP_OK || decodeOut.width == 0 || decodeOut.height == 0) {
        return false;
    }

    uint8_t *target = videoFrameBuffers[frameIndex];
    memset(target, 0x00, videoRawFrameSize);

    uint16_t drawWidth = decodeOut.width;
    uint16_t drawHeight = decodeOut.height;
    if (MJPEG_ROTATE_90_CW) {
        drawWidth = decodeOut.height;
        drawHeight = decodeOut.width;
    }

    uint16_t dstX = 0;
    uint16_t dstY = 0;
    uint16_t outW = drawWidth;
    uint16_t outH = drawHeight;

    uint16_t cropX = 0;
    uint16_t cropY = 0;
    uint16_t cropW = drawWidth;
    uint16_t cropH = drawHeight;

    if (MJPEG_FILL_SCREEN_CROP) {
        outW = TFT_WIDTH;
        outH = TFT_HEIGHT;

        uint32_t srcMul = (uint32_t)drawWidth * (uint32_t)TFT_HEIGHT;
        uint32_t dstMul = (uint32_t)TFT_WIDTH * (uint32_t)drawHeight;
        if (srcMul > dstMul) {
            cropW = (uint16_t)(((uint32_t)drawHeight * (uint32_t)TFT_WIDTH) / (uint32_t)TFT_HEIGHT);
            cropX = (uint16_t)((drawWidth - cropW) / 2U);
        } else if (srcMul < dstMul) {
            cropH = (uint16_t)(((uint32_t)drawWidth * (uint32_t)TFT_HEIGHT) / (uint32_t)TFT_WIDTH);
            cropY = (uint16_t)((drawHeight - cropH) / 2U);
        }
    } else {
        if (outW > TFT_WIDTH || outH > TFT_HEIGHT) {
            return false;
        }
        dstX = (uint16_t)((TFT_WIDTH - outW) / 2U);
        dstY = (uint16_t)((TFT_HEIGHT - outH) / 2U);
    }

    for (uint16_t y = 0; y < outH; y++) {
        for (uint16_t x = 0; x < outW; x++) {
            uint16_t srcX;
            uint16_t srcY;

            uint16_t tx = x;
            uint16_t ty = y;
            if (MJPEG_FILL_SCREEN_CROP) {
                tx = (uint16_t)(cropX + ((uint32_t)x * (uint32_t)cropW) / (uint32_t)outW);
                ty = (uint16_t)(cropY + ((uint32_t)y * (uint32_t)cropH) / (uint32_t)outH);
            }

            if (MJPEG_ROTATE_90_CW) {
                srcX = ty;
                srcY = (uint16_t)(decodeOut.height - 1U - tx);
            } else {
                srcX = tx;
                srcY = ty;
            }

            if (MJPEG_FLIP_X) {
                srcX = (uint16_t)(decodeOut.width - 1U - srcX);
            }
            if (MJPEG_FLIP_Y) {
                srcY = (uint16_t)(decodeOut.height - 1U - srcY);
            }

            size_t srcIndex = ((size_t)srcY * (size_t)decodeOut.width + (size_t)srcX) * 2U;
            uint8_t lo = videoMjpegDecodeBuffer[srcIndex];
            uint8_t hi = videoMjpegDecodeBuffer[srcIndex + 1U];

            size_t dstIndex = ((size_t)(dstY + y) * (size_t)TFT_WIDTH + (size_t)(dstX + x)) * 2U;
            if (MJPEG_SWAP_RGB565_BYTES) {
                target[dstIndex] = hi;
                target[dstIndex + 1U] = lo;
            } else {
                target[dstIndex] = lo;
                target[dstIndex + 1U] = hi;
            }
        }
    }

    return true;
}

bool _openMjpegStream(const String& filename) {
    if (!sdMutex || !videoFrameBuffers[0] || !videoFrameBuffers[1]) return false;
    if (!videoMjpegFrameData || !videoMjpegDecodeBuffer || videoMjpegFrameCapacity == 0) return false;
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300))) return false;

    bool ok = false;
    do {
        if (videoMjpegFile) {
            videoMjpegFile.close();
        }

        videoMjpegFile = SD.open(filename, FILE_READ);
        if (!videoMjpegFile) break;

        videoMjpegLastReadEof = false;
        videoTargetFps = MJPEG_DEFAULT_FPS;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _readMjpegFrameToBuffer(uint8_t frameIndex) {
    if (!sdMutex || !videoMjpegFile || frameIndex > 1) return false;
    if (!videoMjpegFrameData || videoMjpegFrameCapacity == 0) return false;

    size_t frameLen = 0;
    if (!_readMjpegFrameBytes(&frameLen)) {
        return false;
    }

    if (frameLen < 4 || frameLen > videoMjpegFrameCapacity) {
        return false;
    }

    return _decodeJpegToFrameBuffer(videoMjpegFrameData, frameLen, frameIndex);
}

bool _readMjpegFrameBytes(size_t *outFrameLen) {
    if (!outFrameLen) return false;
    *outFrameLen = 0;
    if (!sdMutex || !videoMjpegFile || !videoMjpegFrameData || videoMjpegFrameCapacity == 0) return false;

    uint8_t prev = 0;
    bool foundSoi = false;
    bool foundEoi = false;
    bool overflow = false;
    size_t frameLen = 0;
    videoMjpegLastReadEof = false;

    uint8_t chunk[MJPEG_READ_CHUNK_BYTES];
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(60))) {
        _videoSdReadEnd();
        return false;
    }

    while (!foundEoi) {
        int readLen = videoMjpegFile.read(chunk, sizeof(chunk));
        if (readLen <= 0) {
            videoMjpegLastReadEof = true;
            break;
        }

        for (int i = 0; i < readLen; i++) {
            uint8_t b = chunk[i];
            if (!foundSoi) {
                if (prev == 0xFF && b == 0xD8) {
                    if (videoMjpegFrameCapacity < 2) {
                        overflow = true;
                        break;
                    }
                    videoMjpegFrameData[0] = 0xFF;
                    videoMjpegFrameData[1] = 0xD8;
                    frameLen = 2;
                    foundSoi = true;
                }
            } else {
                if (frameLen < videoMjpegFrameCapacity) {
                    videoMjpegFrameData[frameLen++] = b;
                } else {
                    overflow = true;
                    break;
                }

                if (prev == 0xFF && b == 0xD9) {
                    foundEoi = true;
                    break;
                }
            }
            prev = b;
        }

        if (overflow) break;
    }

    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();

    if (!foundSoi || !foundEoi || overflow || frameLen < 4) {
        return false;
    }

    *outFrameLen = frameLen;
    return true;
}

bool _skipMjpegFrames(uint32_t frameCount) {
    if (frameCount == 0) return true;
    for (uint32_t i = 0; i < frameCount; i++) {
        size_t discardedLen = 0;
        if (!_readMjpegFrameBytes(&discardedLen)) {
            return false;
        }
    }
    return true;
}

void _closeMjpegStream() {
    if (videoMjpegFile) {
        videoMjpegFile.close();
    }
    videoMjpegLastReadEof = false;
}

void _tryStartVideoCompanionAudio(const String& videoFilename, bool loopRequested) {
    String lower = videoFilename;
    lower.toLowerCase();
    if (!(lower.endsWith(".mjpg") || lower.endsWith(".mjpeg") || lower.endsWith(".v16") || lower.endsWith(".vid") || lower.endsWith(".rgb"))) {
        videoCompanionAudioActive = false;
        videoCompanionAudioLoop = false;
        videoCompanionAudioFile = "";
        videoCompanionAudioStartMs = 0;
        return;
    }

    String audioFile = videoFilename;
    int dot = audioFile.lastIndexOf('.');
    if (dot < 0) return;
    audioFile = audioFile.substring(0, dot) + ".mp3";

    bool exists = false;
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        exists = SD.exists(audioFile);
        xSemaphoreGive(sdMutex);
    }

    if (!exists) {
        videoCompanionAudioActive = false;
        videoCompanionAudioLoop = false;
        videoCompanionAudioFile = "";
        videoCompanionAudioStartMs = 0;
        Serial.println("No companion .mp3 found for video");
        return;
    }

    if (_startAudioPlayback(audioFile, loopRequested, 0, false)) {
        Serial.printf("Companion audio started: %s\n", audioFile.c_str());
    } else {
        videoCompanionAudioActive = false;
        videoCompanionAudioLoop = false;
        videoCompanionAudioFile = "";
        videoCompanionAudioStartMs = 0;
        Serial.printf("Companion audio failed: %s\n", audioFile.c_str());
    }
}

void taskVideoDecode(void *pvParameters) {
    VideoPacket packet;
    uint32_t nextFrameAtMs = 0;
    bool hasPendingPacket = false;
    VideoPacket pendingPacket;
    uint8_t nextFrameIndex = 0;

    while (true) {
        if (!videoPipelineActive || isUploading || currentMediaType != MEDIA_VIDEO) {
            nextFrameAtMs = 0;
            hasPendingPacket = false;
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }

        uint32_t nowMs = millis();

        if (videoUseRawFrameStream) {
            if (nextFrameAtMs == 0) {
                nextFrameAtMs = nowMs;
            }

            uint32_t framePeriodMs = (videoTargetFps == 0) ? 66 : (1000UL / videoTargetFps);
            if (videoCompanionAudioActive && framePeriodMs > 0) {
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                uint32_t expectedPtsMs = videoFramesDecoded * framePeriodMs;
                int32_t lagMs = (int32_t)playbackClockMs - (int32_t)expectedPtsMs;
                if (lagMs > VIDEO_LAG_SKIP_THRESHOLD_MS) {
                    uint32_t framesToSkip = (uint32_t)lagMs / framePeriodMs;
                    if (framesToSkip > 0) {
                        if (framesToSkip > 4U) framesToSkip = 4U;
                        if (_skipRawFrames(framesToSkip)) {
                            videoFramesDecoded += framesToSkip;
                            videoFramesDropped += framesToSkip;
                        }
                    }
                }
            }

            if ((int32_t)(nowMs - nextFrameAtMs) >= 0) {
                if (_readRawFrameToBuffer(nextFrameIndex)) {
                    packet.ptsMs = videoFramesDecoded * framePeriodMs;
                    packet.dataLen = videoFramesDecoded;
                    packet.keyFrame = ((videoFramesDecoded % 30U) == 0U);
                    packet.color565 = 0;
                    packet.frameIndex = nextFrameIndex;

                    if (xQueueSend(videoDisplayQueue, &packet, 0) == pdTRUE) {
                        videoFramesDecoded++;
                        nextFrameIndex = (nextFrameIndex == 0) ? 1 : 0;
                    } else {
                        videoFramesDropped++;
                    }
                } else {
                    videoStreamEof = true;
                    if (loopEnabled && videoRawFile) {
                        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                                _videoSdReadBegin();
                            videoRawFile.seek(videoRawHeaderSize, SeekSet);
                            xSemaphoreGive(sdMutex);
                                _videoSdReadEnd();
                            videoStartMs = millis();
                            videoStreamEof = false;
                        }
                    } else {
                        _stopAudioSafely();
                        vTaskDelay(pdMS_TO_TICKS(25));
                        continue;
                    }
                }

                nextFrameAtMs += framePeriodMs;
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                if (videoCompanionAudioActive && (int32_t)(playbackClockMs - nextFrameAtMs) > (int32_t)VIDEO_LAG_CRITICAL_MS) {
                    nextFrameAtMs = playbackClockMs;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (videoUseMjpegStream) {
            if (nextFrameAtMs == 0) {
                nextFrameAtMs = nowMs;
            }

            uint32_t framePeriodMs = (videoTargetFps == 0) ? 66 : (1000UL / videoTargetFps);
            if (videoCompanionAudioActive && framePeriodMs > 0) {
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                uint32_t expectedPtsMs = videoFramesDecoded * framePeriodMs;
                int32_t lagMs = (int32_t)playbackClockMs - (int32_t)expectedPtsMs;
                if (lagMs > VIDEO_LAG_SKIP_THRESHOLD_MS) {
                    uint32_t framesToSkip = (uint32_t)lagMs / framePeriodMs;
                    if (framesToSkip > 0) {
                        if (framesToSkip > 3U) framesToSkip = 3U;
                        if (_skipMjpegFrames(framesToSkip)) {
                            videoFramesDecoded += framesToSkip;
                            videoFramesDropped += framesToSkip;
                        }
                    }
                }
            }

            if ((int32_t)(nowMs - nextFrameAtMs) >= 0) {
                if (videoDisplayQueue && uxQueueSpacesAvailable(videoDisplayQueue) == 0) {
                    videoFramesDropped++;
                    nextFrameAtMs = nowMs + framePeriodMs;
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }

                if (_readMjpegFrameToBuffer(nextFrameIndex)) {
                    packet.ptsMs = videoFramesDecoded * framePeriodMs;
                    packet.dataLen = videoFramesDecoded;
                    packet.keyFrame = ((videoFramesDecoded % 30U) == 0U);
                    packet.color565 = 0;
                    packet.frameIndex = nextFrameIndex;

                    if (xQueueSend(videoDisplayQueue, &packet, 0) == pdTRUE) {
                        videoFramesDecoded++;
                        nextFrameIndex = (nextFrameIndex == 0) ? 1 : 0;
                    } else {
                        videoFramesDropped++;
                    }
                } else {
                    if (videoMjpegLastReadEof) {
                        videoStreamEof = true;
                        if (loopEnabled && videoMjpegFile) {
                            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                                _videoSdReadBegin();
                                videoMjpegFile.seek(0, SeekSet);
                                xSemaphoreGive(sdMutex);
                                _videoSdReadEnd();
                                videoStartMs = millis();
                                videoStreamEof = false;
                                videoMjpegLastReadEof = false;
                            }
                        } else {
                            _stopAudioSafely();
                            vTaskDelay(pdMS_TO_TICKS(25));
                            continue;
                        }
                    } else {
                        videoFramesDropped++;
                    }
                }

                nextFrameAtMs += framePeriodMs;
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                if (videoCompanionAudioActive && (int32_t)(playbackClockMs - nextFrameAtMs) > (int32_t)VIDEO_LAG_CRITICAL_MS) {
                    nextFrameAtMs = playbackClockMs;
                } else if ((int32_t)(nowMs - nextFrameAtMs) > (int32_t)framePeriodMs) {
                    nextFrameAtMs = nowMs + framePeriodMs;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (videoUseFileStream) {
            if (!hasPendingPacket) {
                if (_readNextVideoPacket(pendingPacket)) {
                    hasPendingPacket = true;
                } else {
                    videoStreamEof = true;
                    if (loopEnabled && videoStreamFile) {
                        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            videoStreamFile.seek(videoStreamHeaderSize, SeekSet);
                            xSemaphoreGive(sdMutex);
                            videoStartMs = millis();
                            videoStreamEof = false;
                        }
                    } else {
                        _stopAudioSafely();
                        vTaskDelay(pdMS_TO_TICKS(25));
                        continue;
                    }
                }
            }

            if (hasPendingPacket) {
                uint32_t elapsedMs = millis() - videoStartMs;
                if ((int32_t)(elapsedMs - pendingPacket.ptsMs) >= 0) {
                    if (xQueueSend(videoDisplayQueue, &pendingPacket, 0) == pdTRUE) {
                        videoFramesDecoded++;
                    } else {
                        videoFramesDropped++;
                    }
                    hasPendingPacket = false;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // Phase 2 source: synthetic frame producer at target FPS.
        if (nextFrameAtMs == 0) {
            nextFrameAtMs = nowMs;
        }

        uint32_t framePeriodMs = (videoTargetFps == 0) ? 66 : (1000UL / videoTargetFps);

        if ((int32_t)(nowMs - nextFrameAtMs) >= 0) {
            packet.ptsMs = nowMs - videoStartMs;
            packet.dataLen = videoFramesDecoded;
            packet.keyFrame = ((videoFramesDecoded % 30U) == 0U);
            packet.color565 = (uint16_t)((((videoFramesDecoded * 3U) & 0x1FU) << 11) |
                                         (((videoFramesDecoded * 5U) & 0x3FU) << 5) |
                                         ((videoFramesDecoded * 7U) & 0x1FU));
            packet.frameIndex = 0;

            if (xQueueSend(videoDisplayQueue, &packet, 0) == pdTRUE) {
                videoFramesDecoded++;
            } else {
                videoFramesDropped++;
            }

            nextFrameAtMs += framePeriodMs;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void taskDisplayFlush(void *pvParameters) {
    VideoPacket packet;
    VideoPacket latestPacket;
    while (true) {
        if (!videoPipelineActive || !displayReady || isUploading || currentMediaType != MEDIA_VIDEO) {
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }

        if (xQueueReceive(videoDisplayQueue, &packet, pdMS_TO_TICKS(2)) == pdTRUE) {
            while (xQueueReceive(videoDisplayQueue, &latestPacket, 0) == pdTRUE) {
                packet = latestPacket;
                videoFramesDropped++;
            }

            if ((videoUseRawFrameStream || videoUseMjpegStream) && packet.frameIndex <= 1 && videoFrameBuffers[packet.frameIndex]) {
                st7789DrawFrameRGB565(videoFrameBuffers[packet.frameIndex], TFT_WIDTH, TFT_HEIGHT);
            } else {
                st7789FillColor(packet.color565);
            }

            uint32_t playbackClockMs = _videoPlaybackClockMs();
            videoAvDriftMs = (int32_t)playbackClockMs - (int32_t)packet.ptsMs;
            videoFramesDisplayed++;
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// -------------------- Tasks Setup --------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- ESP32 Haptic Core (ESP8266Audio Version) ---");
    audioLogger = &Serial;

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

    initDisplayVideoScaffold();

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
    xTaskCreatePinnedToCore(taskWebServer, "WebServerTask", 8192, NULL, PRIO_WEB, NULL, CORE_SERVICE);
    xTaskCreatePinnedToCore(taskAudio, "AudioTask", 16384, NULL, PRIO_AUDIO, &audioTaskHandle, CORE_SERVICE);
    xTaskCreatePinnedToCore(taskLEDs, "LEDTask", 4096, NULL, PRIO_LED, NULL, CORE_SERVICE);
    xTaskCreatePinnedToCore(taskVideoDecode, "VideoDecodeTask", 12288, NULL, PRIO_VIDEO, &videoDecodeTaskHandle, CORE_VIDEO);
    xTaskCreatePinnedToCore(taskDisplayFlush, "DisplayFlushTask", 8192, NULL, PRIO_VIDEO, &displayFlushTaskHandle, CORE_VIDEO);

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

    TickType_t waitTicks = (currentMediaType == MEDIA_VIDEO) ? pdMS_TO_TICKS(15) : portMAX_DELAY;
    if (xSemaphoreTake(sdMutex, waitTicks)) {
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
    } else {
        server.send(503, "application/json", "{\"error\":\"SD busy during playback\"}");
        return;
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

    String fnameLower = filename;
    fnameLower.toLowerCase();
    if (fnameLower.endsWith(".mjpg") || fnameLower.endsWith(".mjpeg") || fnameLower.endsWith(".vid") || fnameLower.endsWith(".v16") || fnameLower.endsWith(".rgb")) {
        audioAbortRequested = false;
        if (audioCmdQueue) {
            xQueueReset(audioCmdQueue);
        }

        if (!_startVideoPlayback(filename, requestedLoop)) {
            server.send(409, "application/json", "{\"error\":\"Failed to start video playback\"}");
            return;
        }
        server.send(200, "application/json", "{\"status\":\"Video playback configured\",\"filename\":\"" + filename + "\"}");
        return;
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

    if (currentMediaType == MEDIA_VIDEO) {
        _stopAudioSafely();
        if (videoDisplayQueue) xQueueReset(videoDisplayQueue);
        videoFramesDecoded = 0;
        videoFramesDisplayed = 0;
        videoFramesDropped = 0;
        videoAvDriftMs = 0;
        videoStartState = VIDEO_START_IDLE;
        server.send(200, "application/json", "{\"status\":\"Video playback stopped\"}");
        return;
    }

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

    JsonObject video = doc.createNestedObject("video");
    video["pipeline"] = videoPipelineActive;
    if (videoUseRawFrameStream) {
        video["mode"] = "raw-frame";
    } else if (videoUseMjpegStream) {
        video["mode"] = "mjpeg";
    } else if (videoUseFileStream) {
        video["mode"] = "color-stream";
    } else {
        video["mode"] = "synthetic";
    }
    video["targetFps"] = videoTargetFps;
    video["decodedFrames"] = videoFramesDecoded;
    video["displayedFrames"] = videoFramesDisplayed;
    video["droppedFrames"] = videoFramesDropped;
    video["avDriftMs"] = videoAvDriftMs;
    video["eof"] = videoStreamEof;
    video["companionAudio"] = videoCompanionAudioActive;
    video["sessionId"] = videoSessionId;
    video["startState"] = videoStartState;
    video["companionInRam"] = videoCompanionInRam;
    
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
    
    TickType_t waitTicks = (currentMediaType == MEDIA_VIDEO) ? pdMS_TO_TICKS(20) : portMAX_DELAY;
    if (xSemaphoreTake(sdMutex, waitTicks)) {
        if (SD.remove(fname)) {
            server.send(200, "application/json", "{\"status\":\"File deleted\"}");
        } else {
            server.send(404, "application/json", "{\"error\":\"File not found or locked\"}");
        }
        xSemaphoreGive(sdMutex);
    } else {
        server.send(503, "application/json", "{\"error\":\"SD busy during playback\"}");
    }
}

File uploadFile;
uint8_t *uploadBuffer = NULL;
size_t uploadBufferIndex = 0;
size_t actualBufferSize = 0;
size_t uploadBytesWritten = 0;
bool uploadHadWriteError = false;
const size_t DESIRED_BUFFER_SIZE = 8 * 1024; // 8KB Internal RAM Buffer (Safer for SPI FATFS)
const uint8_t UPLOAD_MAX_WRITE_RETRIES = 5;

void _flushUploadBuffer() {
    if (uploadFile && uploadBuffer && uploadBufferIndex > 0) {
        if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
            size_t requested = uploadBufferIndex;
            size_t totalWritten = 0;
            uint8_t retries = 0;

            while (totalWritten < requested) {
                size_t justWritten = uploadFile.write(uploadBuffer + totalWritten, requested - totalWritten);
                if (justWritten == 0) {
                    retries++;
                    if (retries >= UPLOAD_MAX_WRITE_RETRIES) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(2));
                    continue;
                }
                totalWritten += justWritten;
                retries = 0;
            }

            if (totalWritten != requested) {
                uploadHadWriteError = true;
            }
            uploadBytesWritten += totalWritten;
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
            if (uploadBytesWritten != upload.totalSize) {
                uploadHadWriteError = true;
            }
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
