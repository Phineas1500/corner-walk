// Session creation uses esp_http_client with normal certificate validation.
// The SDK's optional warmed api.openai.com socket does not target our broker.
#include "https_transport.hpp"
namespace live_https {
bool TlsTransport::begin() { return false; }
int TlsTransport::poll() { return -1; }
bool TlsTransport::healthy() { return false; }
void TlsTransport::close() {}
esp_transport_handle_t TlsTransport::release() { return nullptr; }
esp_transport_handle_t wrap(esp_transport_handle_t h) {
  if (h) esp_transport_destroy(h);
  return nullptr;
}
}
