// Raw ESP-NOW transport; no message logic

#include <Arduino.h>
#include <cstring>
#include <esp_wifi.h>
#include <WiFi.h>

#include "Network.h"

uint8_t Network::local_role = 0xFF;
uint8_t Network::remote_mac[6] = {0};
std::function<void(const uint8_t*, int)> Network::rx_callback;

bool Network::begin(uint8_t role) {
  local_role = role;

  const uint8_t* own_mac = (role == ROLE_HOST) ? MAC_HOST : MAC_CLIENT;
  const uint8_t* peer_mac = (role == ROLE_HOST) ? MAC_CLIENT : MAC_HOST;
  memcpy(remote_mac, peer_mac, sizeof(remote_mac));

  WiFi.mode(WIFI_STA);
  delay(100);

  if (esp_wifi_set_mac(WIFI_IF_STA, own_mac) != ESP_OK) {
    Serial.println("[network] mac set failed, resetting...");
    return false;
  }

  if (esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("[network] channel set failed, resetting...");
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[network] esp_now init failed, resetting...");
    return false;
  }

  esp_now_register_send_cb(on_sent);
  esp_now_register_recv_cb(on_received);

  if (!add_peer()) {
    Serial.println("[network] peer add failed, resetting...");
    return false;
  }

  return true;
}

bool Network::send(const uint8_t* data, size_t len) {
  return esp_now_send(remote_mac, data, len) == ESP_OK;
}

void Network::on_receive(std::function<void(const uint8_t*, int)> callback) {
  rx_callback = callback;
}

bool Network::add_peer() {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, remote_mac, sizeof(peer.peer_addr));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void Network::on_sent(const uint8_t* mac, esp_now_send_status_t status) {
  (void)mac;
  (void)status;
}

void Network::on_received(const uint8_t* mac, const uint8_t* data, int len) {
  if (rx_callback) {
    rx_callback(data, len);
  }
}
