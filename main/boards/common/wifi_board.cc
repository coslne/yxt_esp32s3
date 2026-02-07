#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http.h>
#include <esp_mqtt.h>
#include <esp_udp.h>
#include <tcp_transport.h>
#include <tls_transport.h>
#include <web_socket.h>
#include <esp_log.h>

#include <wifi_manager.h>
#include <ssid_manager.h>
#include <esp_sntp.h>
#include <time.h>

static const char *TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    auto& wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsInitialized()) {
        WifiManagerConfig config;
        config.ssid_prefix = "XiaoTun";
        config.language = Lang::CODE;
        wifi_manager.Initialize(config);
    }
    wifi_manager.StartConfigAp();

    // 显示 WiFi 配置 AP 的 SSID 和 Web 服务器 URL
    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += wifi_manager.GetApSsid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += wifi_manager.GetApWebUrl();
    hint += "\n\n";
    
    // 播报配置 WiFi 的提示
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "", Lang::Sounds::P3_WIFICONFIG);
    
    // Wait forever until reset after configuration
    while (true) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void WifiBoard::StartNetwork() {
    // User can press BOOT button while starting to enter WiFi configuration mode
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    // If no WiFi SSID is configured, enter WiFi configuration mode
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "XiaoTun";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    wifi_manager.SetEventCallback([this](WifiEvent event) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification;
        switch (event) {
            case WifiEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                break;
            case WifiEvent::Connecting:
                notification = Lang::Strings::CONNECT_TO;
                notification += "..."; // Ssid not easily available in event arg
                display->ShowNotification(notification.c_str(), 30000);
                break;
            case WifiEvent::Connected:
                notification = Lang::Strings::CONNECTED_TO;
                notification += WifiManager::GetInstance().GetSsid();
                display->ShowNotification(notification.c_str(), 30000);
                break;
            default:
                break;
        }
    });
    
    wifi_manager.StartStation();

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    // Block for 60 seconds manually
    int timeout_ticks = 60 * 1000 / 100;
    while (timeout_ticks > 0) {
        if (wifi_manager.IsConnected()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_ticks--;
    }

    if (!wifi_manager.IsConnected()) {
        wifi_manager.StopStation();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    // Initialize SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_init();

    // Set timezone to China Standard Time
    setenv("TZ", "CST-8", 1);
    tzset();
}

Http* WifiBoard::CreateHttp() {
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    std::string url = CONFIG_WEBSOCKET_URL;
    if (url.find("wss://") == 0) {
        return new WebSocket(new TlsTransport());
    } else {
        return new WebSocket(new TcpTransport());
    }
#endif
    return nullptr;
}

Mqtt* WifiBoard::CreateMqtt() {
    return new EspMqtt();
}

Udp* WifiBoard::CreateUdp() {
    return new EspUdp();
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsConnected()) {
        return FONT_AWESOME_WIFI_OFF;
    }
    int rssi = wifi_manager.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_manager = WifiManager::GetInstance();
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    if (!wifi_config_mode_) {
        board_json += "\"ssid\":\"" + wifi_manager.GetSsid() + "\",";
        board_json += "\"rssi\":" + std::to_string(wifi_manager.GetRssi()) + ",";
        board_json += "\"channel\":" + std::to_string(wifi_manager.GetChannel()) + ",";
        board_json += "\"ip\":\"" + wifi_manager.GetIpAddress() + "\",";
    }
    board_json += "\"mac\":\"" + SystemInfo::GetMacAddress() + "\"}";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_manager = WifiManager::GetInstance();
    wifi_manager.SetPowerSaveLevel(enabled ? WifiPowerSaveLevel::BALANCED : WifiPowerSaveLevel::PERFORMANCE);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}
