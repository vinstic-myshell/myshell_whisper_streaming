#include <websocketpp/config/asio_no_tls.hpp>

#include "common.h"
#include "streaming_common.h"
#include "whisper.h"
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <websocketpp/server.hpp>
#include "json.hpp"
using json = nlohmann::json;

typedef websocketpp::server<websocketpp::config::asio> server;

using websocketpp::connection_hdl;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

// pull out the type of messages sent by our config
typedef server::message_ptr message_ptr;

class streaming_server {
public:
  streaming_server(std::string model_path) {
    m_server.init_asio();

    m_server.set_open_handler(bind(&streaming_server::on_open, this, ::_1));
    m_server.set_close_handler(bind(&streaming_server::on_close, this, ::_1));
    m_server.set_message_handler(
        bind(&streaming_server::on_message, this, ::_1, ::_2));
    m_server.set_access_channels(websocketpp::log::elevel::none);
    m_server.clear_access_channels(websocketpp::log::alevel::none);

    std::ifstream file(model_path, std::ifstream::binary);
    if (!file) {
      std::cerr << "Error opening the file" << std::endl;
      std::cout << "Error opening the file" << std::endl;
      exit(1);
    }
    file.seekg(0, file.end);
    std::streamsize file_size = file.tellg();
    file.seekg(0, file.beg);

    this->model_buffer.resize(file_size);
    if (!file.read(this->model_buffer.data(), file_size)) {
      std::cerr << "Error reading the file" << std::endl;
      std::cout << "Error reading the file" << std::endl;
      exit(1);
    }
    std::cout << "model size: " << this->model_buffer.size() / 1024 / 1024
              << " MB" << std::endl;
  }

  ~streaming_server() {
    m_server.stop_listening();
    m_server.stop();
  }

  whisper_full_params default_whisper_params() {

    whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = true;
    wparams.translate = false;
    wparams.single_segment = true;
    wparams.max_tokens = 32;
    wparams.language = "en";
    wparams.n_threads =
        std::min(4, (int32_t)std::thread::hardware_concurrency());
    ;

    wparams.audio_ctx = 0;
    wparams.speed_up = false;

    // disable temperature fallback
    // wparams.temperature_inc  = -1.0f;
    wparams.temperature_inc = 0.0f;

    wparams.prompt_tokens = nullptr;
    wparams.prompt_n_tokens = 0;
    return wparams;
  }

  void on_open(connection_hdl hdl) {
    std::shared_ptr<Session> s = std::make_shared<Session>();
    s->ctx = whisper_init_from_buffer(this->model_buffer.data(),
                                      this->model_buffer.size());
    this->m_connections[hdl] = s;
  }

  void on_close(connection_hdl hdl) { m_connections.erase(hdl); }

  void on_message(connection_hdl hdl, server::message_ptr msg) {
    auto s = this->m_connections[hdl];
    // check for a special command to instruct the server to stop listening so
    // it can be cleanly exited.
    if (msg->get_opcode() == 1) {
      // TEXT
    }
    if (msg->get_opcode() == 2) {
      // BINARY
    }
    // Convert the binary payload to a float32 array
    std::vector<float> pcmf32_new(msg->get_payload().size() / sizeof(float));
    std::memcpy(pcmf32_new.data(), msg->get_payload().data(),
                msg->get_payload().size());

    // Get the size of the array and prepare the response
    const int n_samples_new = pcmf32_new.size();
    const int n_samples_take = std::min(
        (int)s->pcmf32_old.size(),
        std::max(0, s->n_samples_keep + s->n_samples_len - n_samples_new));
    s->pcmf32.resize(n_samples_new + n_samples_take);

    for (int i = 0; i < n_samples_take; i++) {
      s->pcmf32[i] = s->pcmf32_old[s->pcmf32_old.size() - n_samples_take + i];
    }

    memcpy(s->pcmf32.data() + n_samples_take, pcmf32_new.data(),
           n_samples_new * sizeof(float));

    s->pcmf32_old = s->pcmf32;

    whisper_full_params wparams = this->default_whisper_params();
    if (whisper_full(s->ctx, wparams, s->pcmf32.data(), s->pcmf32.size()) !=
        0) {
      this->m_server.send(hdl, "error", websocketpp::frame::opcode::text);
    }

    // step3: collect result
    const int n_segments = whisper_full_n_segments(s->ctx);
    std::stringstream ss;
    for (int i = 0; i < n_segments; ++i) {
      const char *text = whisper_full_get_segment_text(s->ctx, i);
      ss << text;
    }
    std::string iter_result = ss.str();

    // for (const auto &r : whisper_result) {
    //   response.add_result(r);
    // }
    std::vector<std::string> result_response(s->whisper_result.begin(),s->whisper_result.end());
    result_response.push_back(iter_result);
    ++s->n_iter;
    if (s->n_iter % s->n_newline == 0) {
      // keep part of the audio for next iteration to try to mitigate word
      // boundary issues
      s->pcmf32_old = std::vector<float>(s->pcmf32.end() - s->n_samples_keep,
                                         s->pcmf32.end());
      s->whisper_result.push_back(iter_result);
    }


    json j;
    j["result"] = result_response;
    j["is_talking"] = true;
    try {
      this->m_server.send(hdl, j.dump(), websocketpp::frame::opcode::text);
    } catch (websocketpp::exception const &e) {
      std::cout << "Echo failed because: "
                << "(" << e.what() << ")" << std::endl;
    }
  }

  void run(uint16_t port) {
    m_server.set_reuse_addr(true);
    m_server.listen(port);
    m_server.start_accept();
    m_server.run();
  }

private:
  typedef std::map<connection_hdl, std::shared_ptr<Session>,
                   std::owner_less<connection_hdl>>
      con_list;
  std::vector<char> model_buffer;
  server m_server;
  con_list m_connections;
};

int main(int argc, char **argv) {
  whisper_streaming_params params;

  if (!whisper_streaming_params_parse(argc, argv, params)) {
    return 1;
  }
  // Create a server endpoint
  streaming_server server(params.model);

  try {
    server.run(9002);
  } catch (websocketpp::exception const &e) {
    std::cout << e.what() << std::endl;
  } catch (...) {
    std::cout << "other exception" << std::endl;
  }
}
