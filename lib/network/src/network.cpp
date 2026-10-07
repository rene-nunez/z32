#include <Arduino.h>
#include <cstring>
#include <esp_wifi.h>
#include <WiFi.h>

#include "network.h"

uint8_t network::_role = 0xFF;
uint8_t network::_peer_mac[6] = {0};
std::function<void(const uint8_t*, int)> network::_rx_callback;

bool network::begin(uint8_t role) {
  _role = role;

  const uint8_t* own_mac = (role == ROLE_HOST) ? MAC_HOST : MAC_CLIENT;
  const uint8_t* peer_mac = (role == ROLE_HOST) ? MAC_CLIENT : MAC_HOST;
  memcpy(_peer_mac, peer_mac, sizeof(_peer_mac));

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

  esp_now_register_send_cb(_on_sent);
  esp_now_register_recv_cb(_on_received);

  if (!_add_peer()) {
    Serial.println("[network] peer add failed, resetting...");
    return false;
  }

  // role is reported once by game at boot ([game] ready role=...)
  return true;
}

bool network::send(const uint8_t* data, size_t len) {
  return esp_now_send(_peer_mac, data, len) == ESP_OK;
}

void network::on_receive(std::function<void(const uint8_t*, int)> callback) {
  _rx_callback = callback;
}

bool network::_add_peer() {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, _peer_mac, sizeof(peer.peer_addr));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void network::_on_sent(const uint8_t* mac, esp_now_send_status_t status) {
  (void)mac;
  (void)status; // single drops are routine at ~30Hz; a dead peer surfaces via quiet-drop
}

void network::_on_received(const uint8_t* mac, const uint8_t* data, int len) {
  if (_rx_callback) {
    _rx_callback(data, len);
  }
}
