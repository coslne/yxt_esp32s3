#include "wifi_station.h"
#include <cstring>
#include <algorithm>
#include <vector>
#include <netdb.h>
#include <arpa/inet.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_wpa2.h>
#include <esp_http_client.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT 5

WifiStation::WifiStation() {
    event_group_ = xEventGroupCreate();

    // Load configuration from NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        if (nvs_get_i8(nvs, "max_tx_power", &max_tx_power_) != ESP_OK) {
            max_tx_power_ = 0;
        }
        if (nvs_get_u8(nvs, "remember_bssid", &remember_bssid_) != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");
    
    // Stop event handling first
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    
    was_connected_ = false;
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void()> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);
    
    station_netif_ = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::WifiEventHandler, this, &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::IpEventHandler, this, &instance_got_ip_));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Timer for periodic scanning if not connected
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* this_ = (WifiStation*)arg;
            if (!this_->IsConnected()) {
                // Config scan to show hidden SSIDs
                wifi_scan_config_t scan_config = {
                    .ssid = nullptr,
                    .bssid = nullptr,
                    .channel = 0,
                    .show_hidden = true,
                    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                    .scan_time = {
                        .active = {
                            .min = 120,
                            .max = 150
                        }
                    }
                };
                esp_wifi_scan_start(&scan_config, false);
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, 
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    if (ap_num == 0) {
        ESP_LOGI(TAG, "No APs found, retry in %d seconds", scan_current_interval_microseconds_ / 1000000);
        esp_timer_start_once(timer_handle_, scan_current_interval_microseconds_);
        UpdateScanInterval();
        return;
    }

    std::vector<wifi_ap_record_t> ap_records(ap_num);
    esp_wifi_scan_get_ap_records(&ap_num, ap_records.data());
    
    // Sort by signal strength (RSSI)
    std::sort(ap_records.begin(), ap_records.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    connect_queue_.clear();

    for (const auto& ap_record : ap_records) {
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [&](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });

        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found known AP: %s (RSSI: %d, Auth: %d)", ap_record.ssid, ap_record.rssi, ap_record.authmode);
            WifiApRecord record;
            record.ssid = it->ssid;
            record.password = it->password;
            record.username = it->username;
            record.channel = ap_record.primary;
            record.authmode = ap_record.authmode;
            memcpy(record.bssid, ap_record.bssid, 6);
            
            connect_queue_.push_back(record);
        }
    }

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No matching AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000000);
        esp_timer_start_once(timer_handle_, scan_current_interval_microseconds_);
        UpdateScanInterval();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "Connection queue empty, restarting scan");
        wifi_scan_config_t scan_config = {
            .ssid = nullptr,
            .bssid = nullptr,
            .channel = 0,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = {
                .active = {
                    .min = 120,
                    .max = 150
                }
            }
        };
        esp_wifi_scan_start(&scan_config, false);
        return;
    }

    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    esp_wifi_disconnect();

    // Reset Portal Login State
    needs_portal_login_ = false;
    pending_portal_username_ = "";
    pending_portal_password_ = "";

    bool is_enterprise = false;
    
    if (!ap_record.username.empty() && 
        (ap_record.authmode == WIFI_AUTH_WPA2_ENTERPRISE || 
         ap_record.authmode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE)) {
        is_enterprise = true;
    }
    
    // Check for Portal (username provided but not WPA2 Enterprise)
    // For BUPT-portal which is OPEN but has username/password for portal
    if (!ap_record.username.empty() && !is_enterprise) {
        ESP_LOGI(TAG, "Portal Login potential: %s", ap_record.ssid.c_str());
        needs_portal_login_ = true;
        pending_portal_username_ = ap_record.username;
        pending_portal_password_ = ap_record.password;
    }

    if (is_enterprise) {
        esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)ap_record.username.c_str(), ap_record.username.length());
        esp_wifi_sta_wpa2_ent_set_username((uint8_t *)ap_record.username.c_str(), ap_record.username.length());
        esp_wifi_sta_wpa2_ent_set_password((uint8_t *)ap_record.password.c_str(), ap_record.password.length());
        esp_wifi_sta_wpa2_ent_enable();
    } else {
        esp_wifi_sta_wpa2_ent_disable();
    }

    wifi_config_t wifi_config = {};
    bzero(&wifi_config, sizeof(wifi_config));
    strlcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str(), sizeof(wifi_config.sta.ssid));
    
    if (!is_enterprise) {
        if (ap_record.authmode != WIFI_AUTH_OPEN) {
             strlcpy((char *)wifi_config.sta.password, ap_record.password.c_str(), sizeof(wifi_config.sta.password));
        }
    }
    
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    
    wifi_config.sta.listen_interval = 3;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    if (!IsConnected()) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

uint8_t WifiStation::GetChannel() {
    if (!IsConnected()) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.primary;
    }
    return 0;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}

void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}

void WifiStation::UpdateScanInterval() {
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handlers
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wifi Started, starting scan...");
            {
                wifi_scan_config_t scan_config = {
                    .ssid = nullptr,
                    .bssid = nullptr,
                    .channel = 0,
                    .show_hidden = true,
                    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                    .scan_time = {
                        .active = {
                            .min = 120,
                            .max = 150
                        }
                    }
                };
                esp_wifi_scan_start(&scan_config, false);
            }
            if (this_->on_scan_begin_) this_->on_scan_begin_();
            break;

        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "Scan Done");
            xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
            this_->HandleScanResult();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
            
            bool was_connected = this_->was_connected_;
            this_->was_connected_ = false;
            
            if (was_connected && this_->on_disconnected_) {
                this_->on_disconnected_();
            }

            if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
                this_->reconnect_count_++;
                ESP_LOGI(TAG, "Disconnected, retrying... (%d/%d)", this_->reconnect_count_, MAX_RECONNECT_COUNT);
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "Reconnect failed, looking for other APs or scanning...");
                if (!this_->connect_queue_.empty()) {
                    this_->StartConnect();
                } else {
                    esp_timer_start_once(this_->timer_handle_, this_->scan_current_interval_microseconds_);
                    this_->UpdateScanInterval();
                }
            }
            break;
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->was_connected_ = true;
    this_->reconnect_count_ = 0;
    this_->connect_queue_.clear();
    this_->scan_current_interval_microseconds_ = this_->scan_min_interval_microseconds_;

    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }

    if (this_->needs_portal_login_) {
        ESP_LOGI(TAG, "Initiating Portal Login for %s", this_->ssid_.c_str());
        this_->StartPortalLogin(this_->pending_portal_username_, this_->pending_portal_password_);
    }
}

// Portal Login Implementation
struct PortalLoginCtx {
    std::string username;
    std::string password;
    std::string ssid;
};

void WifiStation::StartPortalLogin(const std::string& username, const std::string& password) {
    PortalLoginCtx* ctx = new PortalLoginCtx{username, password, ssid_};
    
    xTaskCreate([](void* arg) {
        PortalLoginCtx* ctx = static_cast<PortalLoginCtx*>(arg);
        std::string user = ctx->username;
        std::string pass = ctx->password;
        std::string current_ssid = ctx->ssid;
        delete ctx;
        
        ESP_LOGI(TAG, "PortalLoginTask: Started for %s on SSID: %s", user.c_str(), current_ssid.c_str());
        
        std::string login_url;
        bool detected = false;
        bool is_hijacked_200 = false; // Flag if we detected 200 OK interception without redirect

        // Helper to check 302 location or DNS hijacking
        auto check_redirect = [&](const char* url) -> bool {
            esp_http_client_config_t config = {};
            config.url = url; 
            config.disable_auto_redirect = true;
            config.timeout_ms = 5000;
            
            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_err_t err = esp_http_client_perform(client);
            bool found = false;
            
            if (err == ESP_OK) {
                int code = esp_http_client_get_status_code(client);
                ESP_LOGI(TAG, "Probe %s status: %d", url, code);
                
                if (code == 302 || code == 301) {
                    char *loc = nullptr;
                    if (esp_http_client_get_header(client, "Location", &loc) == ESP_OK && loc) {
                        login_url = loc;
                        ESP_LOGI(TAG, "Redirect found: %s", login_url.c_str());
                        free(loc); 
                        found = true;
                    }
                } else if (code == 200) {
                     // Method 2: Check MIUI 204 (It should be 204, if 200 it's hijacked)
                     if (strstr(url, "generate_204")) {
                        ESP_LOGI(TAG, "MIUI generate_204 returned 200. Hijacked.");
                        is_hijacked_200 = true;
                        
                        struct hostent *he = gethostbyname("connect.rom.miui.com");
                        if (he && he->h_addr_list[0]) {
                            char *ip = inet_ntoa(*(struct in_addr*)(he->h_addr_list[0]));
                            ESP_LOGI(TAG, "connect.rom.miui.com resolved to: %s", ip);
                            // Even if IP is public, 200 OK on generate_204 confirms hijacking
                            if (strncmp(ip, "10.", 3) == 0 || 
                                strncmp(ip, "192.168", 7) == 0 || 
                                strncmp(ip, "172.", 4) == 0) {
                                login_url = std::string("http://") + ip + "/login";
                                found = true;
                            }
                        }
                     }
                     // Method 1: Check Apple "Success" logic
                     else if (strstr(url, "captive.apple.com")) {
                         int content_len = esp_http_client_get_content_length(client);
                         if (content_len > 0 && content_len < 200) {
                             ESP_LOGI(TAG, "Apple Success detected.");
                         } else {
                             ESP_LOGI(TAG, "Apple probe returned large body (%d), likely portal.", content_len);
                             is_hijacked_200 = true;
                             
                             struct hostent *he = gethostbyname("captive.apple.com");
                             if (he && he->h_addr_list[0]) {
                                 char *ip = inet_ntoa(*(struct in_addr*)(he->h_addr_list[0]));
                                 ESP_LOGI(TAG, "captive.apple.com resolved to: %s", ip);
                                 if (strncmp(ip, "10.", 3) == 0 || 
                                     strncmp(ip, "192.168", 7) == 0 || 
                                     strncmp(ip, "172.", 4) == 0) {
                                      login_url = std::string("http://") + ip + "/login";
                                      found = true;
                                 }
                             }
                         }
                     }
                }
            }
            esp_http_client_cleanup(client);
            return found;
        };

        // 1. Try MIUI (China optimized)
        if (!detected) detected = check_redirect("http://connect.rom.miui.com/generate_204");
        
        // 2. Try Apple Probe
        if (!detected) detected = check_redirect("http://captive.apple.com/");

        // 3. Fallback logic: If we detected hijacking (200 OK) but couldn't get a URL via DNS/headers
        // OR if we are on BUPT-portal specifically.
        if (!detected && (is_hijacked_200 || current_ssid.find("BUPT") != std::string::npos)) {
            ESP_LOGW(TAG, "Hijacking detected or BUPT SSID match. Using fallback strategies.");
            
            // Strategy A: BUPT specific hardcoded fallback
            if (current_ssid == "BUPT-portal" || current_ssid == "BUPT-mobile") {
                ESP_LOGI(TAG, "Applying BUPT-portal hardcoded Login URL");
                login_url = "http://10.3.8.216/login";
            } 
            // Strategy B: Try Gateway IP
            else {
                esp_netif_ip_info_t ip_info;
                esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char gw_ip[16];
                    esp_ip4addr_ntoa(&ip_info.gw, gw_ip, sizeof(gw_ip));
                    ESP_LOGI(TAG, "Trying Gateway IP for portal: %s", gw_ip);
                    login_url = std::string("http://") + gw_ip + "/login"; 
                }
            }
        }
        
        if (!login_url.empty()) {
            ESP_LOGI(TAG, "Using Login URL: %s", login_url.c_str());
            
            std::string post_url = login_url;
            // Ensure we hit /login
            if (post_url.find("login") == std::string::npos) {
                  if (post_url.back() == '/') post_url += "login";
                  else post_url += "/login";
            }
            
            ESP_LOGI(TAG, "Attempting login POST to: %s", post_url.c_str());
            
            esp_http_client_config_t config = {};
            config.url = post_url.c_str();
            config.method = HTTP_METHOD_POST;
            config.timeout_ms = 8000;
            
            esp_http_client_handle_t client = esp_http_client_init(&config);
            
            esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
            esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
            
            std::string body = "user=" + user + "&pass=" + pass;
            esp_http_client_set_post_field(client, body.c_str(), body.length());
            
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Login Result Code: %d", esp_http_client_get_status_code(client));
                // Optional: Verify internet access again
            } else {
                ESP_LOGE(TAG, "Login request failed: %s", esp_err_to_name(err));
            }
            
            esp_http_client_cleanup(client);
        } else {
             ESP_LOGW(TAG, "No login URL determined. Skipping login.");
        }
        
        vTaskDelete(NULL);
    }, "portal_login", 6144, ctx, 5, NULL);
}
