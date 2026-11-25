#include "Handler.h"

bool Handler::begin(uint8_t role) {
  if (!Network::begin(role)) {
    return false;
  }

  Network::on_receive([this](const uint8_t* data, int len) {
    _dispatch(data, len);
  });

  return true;
}

bool Handler::send(const void* payload, size_t len) {
  return Network::send(static_cast<const uint8_t*>(payload), len);
}

void Handler::on_message(msg_type type, std::function<void(const uint8_t*, size_t)> callback) {
  if (_handler_count >= MAX_HANDLERS) {
    return;
  }

  _handlers[_handler_count].type = type;
  _handlers[_handler_count].callback = callback;
  ++_handler_count;
}

uint8_t Handler::role() const {
  return Network::role();
}

void Handler::_dispatch(const uint8_t* data, int len) {
  if (len < 1) {
    return;
  }

  msg_type type = static_cast<msg_type>(data[0]);

  for (size_t i = 0; i < _handler_count; ++i) {
    if (_handlers[i].type == type) {
      _handlers[i].callback(data, static_cast<size_t>(len));
      return;
    }
  }
}
