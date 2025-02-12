#include <switch.h>
#include <cassert>
#include <iostream>
#include <sstream>

//TMP!!
#include <fstream>

#include "audio_pipe.hpp"
#include "base64.hpp"

/* discard incoming text messages over the socket that are longer than this */
#define MAX_RECV_BUF_SIZE (65 * 1024 * 10)
#define RECV_BUF_REALLOC_SIZE (8 * 1024)

// lets send audio every 60ms
// at 24k samples/sec, with 20 ms packetization, we send 480 samples every 20 ms
// each sample is 2 bytes, so 960 bytes every 20 ms
// 960 * 3 = 2880 bytes every 60 ms
#define MIN_AUDIO_BYTES (2880)

using namespace openai_s2s;

namespace {
  static const char *requestedTcpKeepaliveSecs = std::getenv("MOD_AUDIO_FORK_TCP_KEEPALIVE_SECS");
  static int nTcpKeepaliveSecs = requestedTcpKeepaliveSecs ? ::atoi(requestedTcpKeepaliveSecs) : 55;

  //static std::ofstream audio_file("/tmp/raw_audio_data.bin", std::ios::binary | std::ios::app);

  // Function to write raw audio data to file
  /*
  void writeRawAudioToFile(const uint8_t* buffer, size_t datalen) {
      if (audio_file.is_open()) {
          audio_file.write(reinterpret_cast<const char*>(buffer), datalen);
      } else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to open raw_audio_data.bin for writing.\n");
      }
  }
  */

}

int AudioPipe::lws_callback(struct lws *wsi, 
  enum lws_callback_reasons reason,
  void *user, void *in, size_t len) {

  struct AudioPipe::lws_per_vhost_data *vhd = 
    (struct AudioPipe::lws_per_vhost_data *) lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));

  struct lws_vhost* vhost = lws_get_vhost(wsi);
  AudioPipe ** ppAp = (AudioPipe **) user;

  switch (reason) {
    case LWS_CALLBACK_PROTOCOL_INIT:
      vhd = (struct AudioPipe::lws_per_vhost_data *) lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi), lws_get_protocol(wsi), sizeof(struct AudioPipe::lws_per_vhost_data));
      vhd->context = lws_get_context(wsi);
      vhd->protocol = lws_get_protocol(wsi);
      vhd->vhost = lws_get_vhost(wsi);
      break;

    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
      {
        AudioPipe* ap = findPendingConnect(wsi);
        if (ap) {
          std::string apiKey = ap->getApiKey();
          if (apiKey.length() > 0) {
            unsigned char **p = (unsigned char **)in, *end = (*p) + len;

            // Use stringstream to construct the Authorization header
            std::stringstream authHeader;
            authHeader << "Bearer " << apiKey;

            // Convert stringstream content to std::string
            std::string authHeaderStr = authHeader.str();

            // Add the Authorization header using lws_add_http_header_by_name
            const char *authHeaderName = "Authorization:";
            if (lws_add_http_header_by_name(wsi, (const unsigned char *)authHeaderName,
                                            (const unsigned char *)authHeaderStr.c_str(),
                                            authHeaderStr.length(), p, end)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: failed to add Authorization header\n");
                return -1;
            }

            // Add the custom OpenAI-Beta header
            const char *header_name = "OpenAI-Beta:";
            const char *header_value = "realtime=v1";
            if (lws_add_http_header_by_name(wsi, (const unsigned char *)header_name,
                                            (const unsigned char *)header_value,
                                            strlen(header_value), p, end)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: failed to add OpenAI-Beta header\n");
                return -1;
            }
          }
        }
      }
      break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      processPendingConnects(vhd);
      processPendingDisconnects(vhd);
      processPendingWrites();
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);
        int rc = lws_http_client_http_response(wsi);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR: %s, response status %d\n", in ? (char *)in : "(null)", rc); 
        if (ap) {
          ap->m_state = LWS_CLIENT_FAILED;
          ap->m_callback(ap->m_uuid.c_str(),  ap->m_bugname.c_str(), openai_s2s::AudioPipe::CONNECT_FAIL, (char *) in);
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR unable to find wsi %p..\n", wsi); 
        }
      }      
      break;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);

        if (ap) {
          *ppAp = ap;
          ap->m_vhd = vhd;
          ap->m_state = LWS_CLIENT_CONNECTED;
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), openai_s2s::AudioPipe::CONNECT_SUCCESS, NULL);
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_ESTABLISHED %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
        }
      }      
      break;
    case LWS_CALLBACK_CLIENT_CLOSED:
      {
        AudioPipe* ap = *ppAp;

        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CLOSED %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
          return 0;
        }
        if (ap->m_state == LWS_CLIENT_DISCONNECTING) {
          // closed by us

          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s socket closed by us\n", ap->m_uuid.c_str());
          ap->m_callback(ap->m_uuid.c_str(),  ap->m_bugname.c_str(), openai_s2s::AudioPipe::CONNECTION_CLOSED_GRACEFULLY, NULL);
        }
        else if (ap->m_state == LWS_CLIENT_CONNECTED) {
          // closed by far end
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s socket closed by far end\n", ap->m_uuid.c_str());
          ap->m_callback(ap->m_uuid.c_str(),  ap->m_bugname.c_str(), openai_s2s::AudioPipe::CONNECTION_DROPPED, NULL);
        }
        ap->m_state = LWS_CLIENT_DISCONNECTED;
    
        //NB: after receiving any of the events above, any holder of a 
        //pointer or reference to this object must treat is as no longer valid

        *ppAp = NULL;
        delete ap;
      }
      break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
      {
        AudioPipe* ap = *ppAp;

        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
          return 0;
        }

        if (lws_frame_is_binary(wsi)) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE received binary frame, discarding.\n");
          return 0;
        }

        if (lws_is_first_fragment(wsi)) {
          // allocate a buffer for the entire chunk of memory needed
          assert(nullptr == ap->m_recv_buf);
          ap->m_recv_buf_len = len + lws_remaining_packet_payload(wsi);
          ap->m_recv_buf = (uint8_t*) malloc(ap->m_recv_buf_len);
          ap->m_recv_buf_ptr = ap->m_recv_buf;
        }

        size_t write_offset = ap->m_recv_buf_ptr - ap->m_recv_buf;
        size_t remaining_space = ap->m_recv_buf_len - write_offset;
        if (remaining_space < len) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE buffer realloc needed.\n");
          size_t newlen = ap->m_recv_buf_len + RECV_BUF_REALLOC_SIZE;
          if (newlen > MAX_RECV_BUF_SIZE) {
            free(ap->m_recv_buf);
            ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
            ap->m_recv_buf_len = 0;
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE max buffer exceeded, truncating message.\n");
          }
          else {
            ap->m_recv_buf = (uint8_t*) realloc(ap->m_recv_buf, newlen);
            if (nullptr != ap->m_recv_buf) {
              ap->m_recv_buf_len = newlen;
              ap->m_recv_buf_ptr = ap->m_recv_buf + write_offset;
            }
          }
        }

        if (nullptr != ap->m_recv_buf) {
          if (len > 0) {
            memcpy(ap->m_recv_buf_ptr, in, len);
            ap->m_recv_buf_ptr += len;
          }
          if (lws_is_final_fragment(wsi)) {
            if (nullptr != ap->m_recv_buf) {
              std::string msg((char *)ap->m_recv_buf, ap->m_recv_buf_ptr - ap->m_recv_buf);
              ap->m_callback(ap->m_uuid.c_str(),  ap->m_bugname.c_str(), openai_s2s::AudioPipe::MESSAGE, msg.c_str());
              if (nullptr != ap->m_recv_buf) free(ap->m_recv_buf);
            }
            ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
            ap->m_recv_buf_len = 0;
          }
        }
      }
      break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
      {
        AudioPipe* ap = *ppAp;

        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
          return 0;
        }

        // check for text frames to send
        {
          std::lock_guard<std::mutex> lk(ap->m_text_mutex);
          if (ap->m_metadata.length() > 0) {
            uint8_t buf[ap->m_metadata.length() + LWS_PRE];
            memcpy(buf + LWS_PRE, ap->m_metadata.c_str(), ap->m_metadata.length());
            int n = ap->m_metadata.length();
            int m = lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
            ap->m_metadata.clear();
            if (m < n) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE %s ERROR sending text frames %d %d\n", ap->m_uuid.c_str(), m, n); 
              return -1;
            }

            // there may be audio data, but only one write per writeable event
            // get it next time
            lws_callback_on_writable(wsi);

            return 0;
          }
        }

        if (ap->m_state == LWS_CLIENT_DISCONNECTING) {
          lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
          return -1;
        }

        // check for audio packets
        {
          std::lock_guard<std::mutex> lk(ap->m_audio_mutex);

          if (ap->m_audio_buffer_write_offset > LWS_PRE) {
            size_t datalen = ap->m_audio_buffer_write_offset - LWS_PRE;
      
            if (datalen >= MIN_AUDIO_BYTES) {
              std::ostringstream oss;

              //TMP!!
              //writeRawAudioToFile(ap->m_audio_buffer + LWS_PRE, datalen);

              oss << "{\"type\":\"input_audio_buffer.append\",\"audio\":\"" << drachtio::base64_encode((unsigned char const *) ap->m_audio_buffer + LWS_PRE, datalen) << "\"}";
              std::string result = oss.str();
              uint8_t buf[result.length() + LWS_PRE];
              memcpy(buf + LWS_PRE,result.c_str(), result.length());
              int n = result.length();
              int m = lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
              if (m < n) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE attemped to send %lu bytes only sent %d wsi %p..\n", 
                  n, m, wsi); 
              }
              ap->m_audio_buffer_write_offset = LWS_PRE;
            }
          }
        }

        return 0;
      }
      break;

    default:
      break;
  }
  return lws_callback_http_dummy(wsi, reason, user, in, len);
}


// static members
static const lws_retry_bo_t retry = {
    nullptr,   // retry_ms_table
    0,         // retry_ms_table_count
    0,         // conceal_count
    UINT16_MAX,         // secs_since_valid_ping
    UINT16_MAX,        // secs_since_valid_hangup
    0          // jitter_percent
};

struct lws_context *AudioPipe::context = nullptr;
std::thread AudioPipe::serviceThread;
std::mutex AudioPipe::mutex_connects;
std::mutex AudioPipe::mutex_disconnects;
std::mutex AudioPipe::mutex_writes;
std::list<AudioPipe*> AudioPipe::pendingConnects;
std::list<AudioPipe*> AudioPipe::pendingDisconnects;
std::list<AudioPipe*> AudioPipe::pendingWrites;
AudioPipe::log_emit_function AudioPipe::logger;
std::mutex AudioPipe::mapMutex;
bool AudioPipe::stopFlag;

void AudioPipe::processPendingConnects(lws_per_vhost_data *vhd) {
  std::list<AudioPipe*> connects;
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    for (auto it = pendingConnects.begin(); it != pendingConnects.end(); ++it) {
      if ((*it)->m_state == LWS_CLIENT_IDLE) {
        connects.push_back(*it);
        (*it)->m_state = LWS_CLIENT_CONNECTING;
      }
    }
  }
  for (auto it = connects.begin(); it != connects.end(); ++it) {
    AudioPipe* ap = *it;
    ap->connect_client(vhd);   
  }
}

void AudioPipe::processPendingDisconnects(lws_per_vhost_data *vhd) {
  std::list<AudioPipe*> disconnects;
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    for (auto it = pendingDisconnects.begin(); it != pendingDisconnects.end(); ++it) {
      if ((*it)->m_state == LWS_CLIENT_DISCONNECTING) disconnects.push_back(*it);
    }
    pendingDisconnects.clear();
  }
  for (auto it = disconnects.begin(); it != disconnects.end(); ++it) {
    AudioPipe* ap = *it;
    lws_callback_on_writable(ap->m_wsi); 
  }
}

void AudioPipe::processPendingWrites() {
  std::list<AudioPipe*> writes;
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    for (auto it = pendingWrites.begin(); it != pendingWrites.end(); ++it) {
       if ((*it)->m_state == LWS_CLIENT_CONNECTED) writes.push_back(*it);
    }  
    pendingWrites.clear();
  }
  for (auto it = writes.begin(); it != writes.end(); ++it) {
    AudioPipe* ap = *it;
    lws_callback_on_writable(ap->m_wsi);
  }
}

AudioPipe* AudioPipe::findAndRemovePendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);
  std::list<AudioPipe* > toRemove;

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state;

    if ((*it)->m_wsi == nullptr)
      toRemove.push_back(*it);

    if ((state == LWS_CLIENT_CONNECTING) &&
      (*it)->m_wsi == wsi) ap = *it;
  }

  for (auto it = toRemove.begin(); it != toRemove.end(); ++it)
    pendingConnects.remove(*it);

  if (ap) {
    pendingConnects.remove(ap);
  }

  return ap;
}

AudioPipe* AudioPipe::findPendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state;
    if ((state == LWS_CLIENT_CONNECTING) &&
      (*it)->m_wsi == wsi) ap = *it;
  }
  return ap;
}

void AudioPipe::addPendingConnect(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    pendingConnects.push_back(ap);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s after adding connect there are %lu pending connects\n", 
      ap->m_uuid.c_str(), pendingConnects.size());
  }
  lws_cancel_service(context);
}
void AudioPipe::addPendingDisconnect(AudioPipe* ap) {
  ap->m_state = LWS_CLIENT_DISCONNECTING;
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    pendingDisconnects.push_back(ap);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s after adding disconnect there are %lu pending disconnects\n", 
      ap->m_uuid.c_str(), pendingDisconnects.size());
  }
  lws_cancel_service(ap->m_vhd->context);
}
void AudioPipe::addPendingWrite(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    pendingWrites.push_back(ap);
  }
  lws_cancel_service(ap->m_vhd->context);
}

bool AudioPipe::lws_service_thread() {
  struct lws_context_creation_info info;
  std::thread::id this_id = std::this_thread::get_id();

  const struct lws_protocols protocols[] = {
    {
      "",
      AudioPipe::lws_callback,
      sizeof(void *),
      1024,
    },
    { NULL, NULL, 0, 0 }
  };

  memset(&info, 0, sizeof info); 
  info.port = CONTEXT_PORT_NO_LISTEN; 
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.protocols = protocols;
  info.ka_time = nTcpKeepaliveSecs;                    // tcp keep-alive timer
  info.ka_probes = 4;                   // number of times to try ka before closing connection
  info.ka_interval = 5;                 // time between ka's
  info.timeout_secs = 10;                // doc says timeout for "various processes involving network roundtrips"
  info.keepalive_timeout = 5;           // seconds to allow remote client to hold on to an idle HTTP/1.1 connection 
  info.timeout_secs_ah_idle = 10;       // secs to allow a client to hold an ah without using it
  info.retry_and_idle_policy = &retry;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::lws_service_thread creating context\n");

  context = lws_create_context(&info);
  if (!context) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread failed creating context\n"); 
    return false;
  }

  int n;
  do {
    n = lws_service(context, 0);
  } while (n >= 0 && !stopFlag);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::lws_service_thread ending\n"); 
  lws_context_destroy(context);

  return true;
}

void AudioPipe::initialize(int loglevel, log_emit_function logger) {

  //lws_set_log_level(loglevel, logger);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::initialize starting\n"); 
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag = false;
  serviceThread = std::thread(&AudioPipe::lws_service_thread);
}

bool AudioPipe::deinitialize() {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::deinitialize\n"); 
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag = true;
  if (serviceThread.joinable()) {
    serviceThread.join();
  }

  return true;
}

// instance members
AudioPipe::AudioPipe(const char* uuid, const char* bugname, const char* host, unsigned int port, const char* path,
  size_t bufLen, size_t minFreespace, const char* apiKey, notifyHandler_t callback) :
  m_uuid(uuid), m_host(host), m_port(port), m_path(path), m_bugname(bugname),
  m_audio_buffer_min_freespace(minFreespace), m_audio_buffer_max_len(bufLen), m_gracefulShutdown(false),
  m_audio_buffer_write_offset(LWS_PRE), m_recv_buf(nullptr), m_recv_buf_ptr(nullptr),
  m_state(LWS_CLIENT_IDLE), m_wsi(nullptr), m_vhd(nullptr), m_callback(callback) {

  if (apiKey) m_apiKey = apiKey;
  else m_apiKey = "";

  m_audio_buffer = new uint8_t[m_audio_buffer_max_len];
}
AudioPipe::~AudioPipe() {
  if (m_audio_buffer) delete [] m_audio_buffer;
  if (m_recv_buf) delete [] m_recv_buf;
}

void AudioPipe::connect(void) {
  addPendingConnect(this);
}

bool AudioPipe::connect_client(struct lws_per_vhost_data *vhd) {
  assert(m_audio_buffer != nullptr);
  assert(m_vhd == nullptr);
  struct lws_client_connect_info i;

  memset(&i, 0, sizeof(i));
  i.context = vhd->context;
  i.port = m_port;
  i.address = m_host.c_str();
  i.path = m_path.c_str();
  i.host = i.address;
  i.origin = i.address;
  i.ssl_connection = LCCSCF_USE_SSL;
  i.pwsi = &(m_wsi);

  m_state = LWS_CLIENT_CONNECTING;
  m_vhd = vhd;

  m_wsi = lws_client_connect_via_info(&i);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s attempting connection, path: %s, wsi is %p\n", m_uuid.c_str(), m_path.c_str(), m_wsi);

  return nullptr != m_wsi;
}

void AudioPipe::bufferForSending(const char* text) {
  if (m_state != LWS_CLIENT_CONNECTED) return;
  {
    std::lock_guard<std::mutex> lk(m_text_mutex);
    m_metadata.append(text);
  }
  addPendingWrite(this);
}

void AudioPipe::unlockAudioBuffer() {
  if (m_audio_buffer_write_offset > LWS_PRE) addPendingWrite(this);
  m_audio_mutex.unlock();
}

void AudioPipe::close() {
  if (m_state != LWS_CLIENT_CONNECTED) {
    return;
  }
  addPendingDisconnect(this);
}

void AudioPipe::do_graceful_shutdown() {
  m_gracefulShutdown = true;
  addPendingWrite(this);
}
