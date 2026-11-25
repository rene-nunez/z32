#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

#include <Protocol.h>

class Handler {
  public:
    bool begin(uint8_t role);
    bool send(const void* payload, size_t len);
    void on_message(msg_type type, std::function<void(const uint8_t*, size_t)> callback);
    uint8_t role() const;

  private:
    static constexpr size_t MAX_HANDLERS = 8;

    struct handler_entry {
      msg_type type = msg_type::heartbeat;
      std::function<void(const uint8_t*, size_t)> callback;
    };

    handler_entry handlers[MAX_HANDLERS] = {};
    size_t handler_count = 0;

    void dispatch(const uint8_t* data, int len);
};
