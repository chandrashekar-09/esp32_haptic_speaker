#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>

// --- Configuration ---
const char* ssid = "IIIT-Guest";
const char* password = "f6s68VHJ89mC";

// ESP32-S3 SD_MMC Pins (1-Bit Mode)
#define PIN_SCLK 12
#define PIN_CMD  11  // This is your MOSI pin
#define PIN_D0   13  // This is your MISO pin

WebServer server(80);
File uploadFile;
String currentFilename = "";

// --- Handlers ---

void handleUploadResponse() {
    // Open the file to verify final size
    File file = SD_MMC.open(currentFilename, FILE_READ);
    size_t actualSize = (file) ? file.size() : 0;
    if (file) file.close();

    String jsonResponse = "{\"status\":\"success\",\"file\":\"" + currentFilename + "\",\"size_bytes\":" + String(actualSize) + "}";
    server.send(200, "application/json", jsonResponse);
    
    Serial.printf("\n[VERIFIED] File %s is %u bytes on disk.\n", currentFilename.c_str(), actualSize);
}

void handleFileUpload() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        currentFilename = upload.filename;
        if (!currentFilename.startsWith("/")) currentFilename = "/" + currentFilename;
        
        Serial.printf("\n[SD_MMC] Starting: %s\n", currentFilename.c_str());
        
        if (SD_MMC.exists(currentFilename)) {
            SD_MMC.remove(currentFilename);
        }
        
        uploadFile = SD_MMC.open(currentFilename, FILE_WRITE);
    } 
    else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            // SD_MMC is fast enough that we don't usually need retries, 
            // but we check for successful writes anyway.
            size_t written = uploadFile.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize) {
                Serial.printf("Critical Write Failure! %u/%u\n", written, upload.currentSize);
            }
        }
    } 
    else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.println("[SD_MMC] Upload Finished.");
        }
    }
}

void setup() {
    Serial.begin(115200);

    // 1. Configure SD_MMC Pins for S3
    // S3 requires setPins(clk, cmd, d0)
    if (!SD_MMC.setPins(PIN_SCLK, PIN_CMD, PIN_D0)) {
        Serial.println("Pin configuration failed!");
    }

    // 2. Mount SD_MMC in 1-Bit Mode (more stable for jumper wires)
    // The 'true' parameter enables 1-bit mode.
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("SD_MMC Mount Failed! (Check pull-up resistors on CMD/D0)");
        // If this fails, your wiring might require SD.h (SPI) instead.
        return; 
    }
    Serial.println("SD_MMC (1-Bit Mode) Ready.");

    // 3. WiFi Setup
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());

    // 4. API Routes
    server.on("/upload", HTTP_POST, handleUploadResponse, handleFileUpload);
    
    server.begin();
    Serial.println("Server listening on /upload");
}

void loop() {
    server.handleClient();
}