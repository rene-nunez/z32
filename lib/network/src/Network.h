#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

#include <esp_now.h>

#define WIFI_CHANNEL 1

#define ROLE_HOST 0
#define ROLE_CLIENT 1

constexpr uint8_t MAC_HOST[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr uint8_t MAC_CLIENT[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

class Network {
  public:
    static bool begin(uint8_t role);
    static bool send(const uint8_t* data, size_t len);
    static void on_receive(std::function<void(const uint8_t*, int)> callback);
    static uint8_t role() { return local_role; }

  private:
    static uint8_t local_role;
    static uint8_t remote_mac[6];
    static std::function<void(const uint8_t*, int)> rx_callback;

    static bool add_peer();
    static void on_sent(const uint8_t* mac, esp_now_send_status_t status);
    static void on_received(const uint8_t* mac, const uint8_t* data, int len);
};
