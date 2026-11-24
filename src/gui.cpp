#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>

constexpr int SPS = 10;
constexpr int VIEW_SAMPLES = 8000;
constexpr int WATERFALL_ROWS = 120;
constexpr int WATERFALL_COLS = 250;
constexpr double SAMPLE_RATE = 1e6;
constexpr float BASE_STEP_AT_2X = 15.0f;

static std::mutex log_mutex;
static std::deque<std::string> debug_log;

static void AddLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    debug_log.push_back(msg);
    if (debug_log.size() > 100) debug_log.pop_front();
}

struct SdrControlState {
    std::atomic<bool> sdr_running{false};
    std::atomic<bool> tx_enabled{true};
    std::atomic<bool> tx_conv_enabled{true};
    std::atomic<bool> tx_rebuild_dirty{false};
    std::atomic<int> mode{2};
    std::atomic<float> rx_gain{30.0f};
    std::atomic<float> tx_gain{-15.0f};
    std::atomic<bool> gain_dirty{false};
    std::atomic<double> rx_freq_hz{800e6};
    std::atomic<double> tx_freq_hz{800e6};
    std::atomic<bool> freq_dirty{false};
    std::atomic<bool> device_connected{false};

    std::mutex cfg_mutex;
    char device_uri[128] = "usb:";
    char tx_message[128] = "Hello Pluto!";
};

static SdrControlState g_ctrl;
static std::mutex iq_mutex;
static std::vector<float> live_I(VIEW_SAMPLES, 0.0f);
static std::vector<float> live_Q(VIEW_SAMPLES, 0.0f);
static std::atomic<bool> has_new_live_frame{false};

static std::vector<int16_t> BuildBpskBuffer(const std::string& msg, size_t mtu, bool apply_tx_conv) {
    std::vector<int16_t> buff(2 * mtu, 0);
    buff[0] = (int16_t)0xffff; buff[1] = (int16_t)0xffff;
    buff[10] = (int16_t)0xffff; buff[11] = (int16_t)0xffff;

    size_t max_bits = (mtu / 2 - 6) / SPS;
    size_t n_bits = std::min(msg.size() * 8, max_bits);

    size_t sample_idx = 6;
    for (size_t b = 0; b < n_bits; ++b) {
        char c = msg[b / 8];
        int bit = (c >> (7 - (b % 8))) & 1;
        int16_t amp = (bit == 0) ? 1500 : -1500;

        if (apply_tx_conv) {
            for (int s = 0; s < SPS && sample_idx < mtu / 2; ++s, ++sample_idx) {
                buff[2 * sample_idx]     = (int16_t)(amp << 4);
                buff[2 * sample_idx + 1] = 0;
            }
        } else {
            if (sample_idx < mtu / 2) {
                buff[2 * sample_idx]     = (int16_t)(amp << 4);
                buff[2 * sample_idx + 1] = 0;
                sample_idx++;
            }
            for (int s = 1; s < SPS && sample_idx < mtu / 2; ++s, ++sample_idx) {
                buff[2 * sample_idx]     = 0;
                buff[2 * sample_idx + 1] = 0;
            }
        }
    }
    return buff;
}

static void SdrWorkerThread() {
    while (true) {
        if (!g_ctrl.sdr_running.load()) {
            g_ctrl.device_connected.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::string uri_str, msg_copy;
        {
            std::lock_guard<std::mutex> lock(g_ctrl.cfg_mutex);
            uri_str = g_ctrl.device_uri;
            msg_copy = g_ctrl.tx_message;
        }

        uri_str.erase(0, uri_str.find_first_not_of(" \t\r\n"));
        size_t last_ch = uri_str.find_last_not_of(" \t\r\n");
        if (last_ch != std::string::npos) uri_str.erase(last_ch + 1);
        if (uri_str.empty()) uri_str = "usb:";
        if (uri_str.find(':') == std::string::npos) uri_str = "ip:" + uri_str;

        AddLog("[SDR] Подключение к " + uri_str + "...");

        SoapySDRKwargs args = {};
        SoapySDRKwargs_set(&args, "driver", "plutosdr");
        SoapySDRKwargs_set(&args, "uri", uri_str.c_str());
        SoapySDRKwargs_set(&args, "direct", "1");
        SoapySDRKwargs_set(&args, "timestamp_every", "1920");
        SoapySDRKwargs_set(&args, "loopback", "0");

        SoapySDRDevice* sdr = SoapySDRDevice_make(&args);
        SoapySDRKwargs_clear(&args);

        if (!sdr) {
            AddLog(std::string("[ОШИБКА] Плата недоступна: ") + SoapySDRDevice_lastError());
            g_ctrl.sdr_running.store(false);
            g_ctrl.device_connected.store(false);
            continue;
        }

        g_ctrl.device_connected.store(true);
        AddLog("[SDR] Устройство подключено (" + uri_str + ").");

        int current_mode = g_ctrl.mode.load();
        bool show_rx = (current_mode == 0 || current_mode == 2);
        bool do_tx   = (current_mode == 1 || current_mode == 2);
        bool need_rx_stream = show_rx || do_tx;

        size_t channels[] = {0};
        SoapySDRStream* rxStream = nullptr;
        SoapySDRStream* txStream = nullptr;

        if (need_rx_stream) {
            SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, SAMPLE_RATE);
            SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, g_ctrl.rx_freq_hz.load(), nullptr);
            SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, g_ctrl.rx_gain.load());
            rxStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, SOAPY_SDR_CS16, channels, 1, nullptr);
            if (rxStream) SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0);
        }

        if (do_tx) {
            SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_TX, 0, SAMPLE_RATE);
            SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_TX, 0, g_ctrl.tx_freq_hz.load(), nullptr);
            SoapySDRDevice_setGain(sdr, SOAPY_SDR_TX, 0, g_ctrl.tx_gain.load());
            txStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_TX, SOAPY_SDR_CS16, channels, 1, nullptr);
            if (txStream) SoapySDRDevice_activateStream(sdr, txStream, 0, 0, 0);
        }

        size_t rx_mtu = rxStream ? SoapySDRDevice_getStreamMTU(sdr, rxStream) : 1920;
        size_t tx_mtu = txStream ? SoapySDRDevice_getStreamMTU(sdr, txStream) : 1920;

        std::vector<int16_t> rx_buff(2 * rx_mtu, 0);
        std::vector<int16_t> tx_buff = BuildBpskBuffer(msg_copy, tx_mtu, g_ctrl.tx_conv_enabled.load());

        std::vector<float> ring_I, ring_Q;
        ring_I.reserve(VIEW_SAMPLES * 2);
        ring_Q.reserve(VIEW_SAMPLES * 2);

        long long last_time = 0;
        bool tx_err_logged = false;

        while (g_ctrl.sdr_running.load()) {
            if (g_ctrl.gain_dirty.exchange(false)) {
                if (need_rx_stream) SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, g_ctrl.rx_gain.load());
                if (do_tx) SoapySDRDevice_setGain(sdr, SOAPY_SDR_TX, 0, g_ctrl.tx_gain.load());
            }

            if (g_ctrl.freq_dirty.exchange(false)) {
                if (need_rx_stream) SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, g_ctrl.rx_freq_hz.load(), nullptr);
                if (do_tx) SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_TX, 0, g_ctrl.tx_freq_hz.load(), nullptr);
            }

            if (g_ctrl.tx_rebuild_dirty.exchange(false)) {
                std::lock_guard<std::mutex> lock(g_ctrl.cfg_mutex);
                tx_buff = BuildBpskBuffer(g_ctrl.tx_message, tx_mtu, g_ctrl.tx_conv_enabled.load());
            }

            if (need_rx_stream && rxStream) {
                void* buffs[] = {rx_buff.data()};
                int flags = 0;
                long long timeNs = 0;
                int sr = SoapySDRDevice_readStream(sdr, rxStream, buffs, rx_mtu, &flags, &timeNs, 200000);
                if (sr > 0) {
                    last_time = timeNs;
                    if (show_rx) {
                        for (int i = 0; i < sr; ++i) {
                            ring_I.push_back((float)rx_buff[2 * i]);
                            ring_Q.push_back((float)rx_buff[2 * i + 1]);
                        }
                        if ((int)ring_I.size() >= VIEW_SAMPLES) {
                            std::lock_guard<std::mutex> lock(iq_mutex);
                            std::copy(ring_I.end() - VIEW_SAMPLES, ring_I.end(), live_I.begin());
                            std::copy(ring_Q.end() - VIEW_SAMPLES, ring_Q.end(), live_Q.begin());
                            has_new_live_frame.store(true);
                            ring_I.clear();
                            ring_Q.clear();
                        }
                    }
                }
            }

            if (do_tx && txStream && g_ctrl.tx_enabled.load() && last_time > 0) {
                long long tx_time = last_time + 4000000LL;
                for (size_t i = 0; i < 8; ++i) {
                    uint8_t b = (tx_time >> (i * 8)) & 0xff;
                    tx_buff[2 + i] = (int16_t)(b << 4);
                }
                void* tx_buffs[] = {tx_buff.data()};
                int flags_tx = SOAPY_SDR_HAS_TIME;
                int st = SoapySDRDevice_writeStream(sdr, txStream, (const void* const*)tx_buffs, tx_mtu, &flags_tx, tx_time, 200000);
                if (st < 0 && !tx_err_logged) {
                    AddLog("[TX ОШИБКА] writeStream код: " + std::to_string(st));
                    tx_err_logged = true;
                }
            }
        }

        if (rxStream) {
            SoapySDRDevice_deactivateStream(sdr, rxStream, 0, 0);
            SoapySDRDevice_closeStream(sdr, rxStream);
        }
        if (txStream) {
            SoapySDRDevice_deactivateStream(sdr, txStream, 0, 0);
            SoapySDRDevice_closeStream(sdr, txStream);
        }
        SoapySDRDevice_unmake(sdr);
        g_ctrl.device_connected.store(false);
        AddLog("[SDR] Устройство остановлено.");
    }
}

static void ApplyMatchedFilter(const std::vector<float>& in_I, const std::vector<float>& in_Q,
                               std::vector<float>& out_I, std::vector<float>& out_Q, int sps) {
    size_t n = in_I.size();
    out_I.assign(n, 0.0f);
    out_Q.assign(n, 0.0f);
    float norm = 1.0f / (float)sps;

    for (size_t i = 0; i < n; ++i) {
        float si = 0.0f, sq = 0.0f;
        for (int k = 0; k < sps; ++k) {
            if (i >= (size_t)k) {
                si += in_I[i - k];
                sq += in_Q[i - k];
            }
        }
        out_I[i] = si * norm;
        out_Q[i] = sq * norm;
    }
}

static inline void InterpolateFarrow(const std::vector<float>& xI, const std::vector<float>& xQ,
                                     float pos, float& outI, float& outQ) {
    int n = (int)std::floor(pos);
    float mu = pos - (float)n;
    int max_idx = (int)xI.size() - 1;

    int i_m1 = std::clamp(n - 1, 0, max_idx);
    int i_0  = std::clamp(n,     0, max_idx);
    int i_p1 = std::clamp(n + 1, 0, max_idx);
    int i_p2 = std::clamp(n + 2, 0, max_idx);

    float v2_I = 0.5f * (xI[i_p2] - xI[i_p1] - xI[i_0] + xI[i_m1]);
    float v1_I = 0.5f * (-xI[i_p2] + 3.0f * xI[i_p1] - xI[i_0] - xI[i_m1]);
    outI = (v2_I * mu + v1_I) * mu + xI[i_0];

    float v2_Q = 0.5f * (xQ[i_p2] - xQ[i_p1] - xQ[i_0] + xQ[i_m1]);
    float v1_Q = 0.5f * (-xQ[i_p2] + 3.0f * xQ[i_p1] - xQ[i_0] - xQ[i_m1]);
    outQ = (v2_Q * mu + v1_Q) * mu + xQ[i_0];
}

struct GardnerSyncTelemetry {
    float locked_phase = 0.0f;
    float mean_ted_error = 0.0f;
    bool is_locked = false;
};

static GardnerSyncTelemetry RunGardnerSymbolSync(const std::vector<float>& proc_I,
                                                 const std::vector<float>& proc_Q,
                                                 std::vector<float>& out_sym_I,
                                                 std::vector<float>& out_sym_Q,
                                                 int sps) {
    out_sym_I.clear();
    out_sym_Q.clear();
    GardnerSyncTelemetry tel;

    int n = (int)proc_I.size();
    if (n < sps * 4) return tel;

    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        peak = std::max(peak, std::sqrt(proc_I[i] * proc_I[i] + proc_Q[i] * proc_Q[i]));
    }
    float power_gate = (peak * 0.25f) * (peak * 0.25f);
    if (peak < 15.0f) return tel;

    const float Kp = 0.40f;
    const float Ki = 0.02f;
    static float integrator = 0.0f;
    static float phase_offset = 0.0f;
    float pos = (float)sps + phase_offset;

    double err_acc = 0.0;
    int err_cnt = 0;

    while (pos < (float)(n - 3)) {
        float curr_I, curr_Q, mid_I, mid_Q, prev_I, prev_Q;
        InterpolateFarrow(proc_I, proc_Q, pos, curr_I, curr_Q);
        InterpolateFarrow(proc_I, proc_Q, pos - 0.5f * (float)sps, mid_I, mid_Q);
        InterpolateFarrow(proc_I, proc_Q, pos - 1.0f * (float)sps, prev_I, prev_Q);

        out_sym_I.push_back(curr_I);
        out_sym_Q.push_back(curr_Q);

        float p_avg = 0.5f * (curr_I * curr_I + curr_Q * curr_Q + prev_I * prev_I + prev_Q * prev_Q);
        float delta = 0.0f;
        if (p_avg > power_gate) {
            float ted_raw = (curr_I - prev_I) * mid_I + (curr_Q - prev_Q) * mid_Q;
            float err_norm = std::clamp(ted_raw / (p_avg + 1e-6f), -1.0f, 1.0f);
            integrator = std::clamp(integrator + Ki * err_norm, -0.5f, 0.5f);
            delta = Kp * err_norm + integrator;
            err_acc += std::abs(err_norm);
            err_cnt++;
        }
        pos += (float)sps - delta;
    }

    float rem = std::fmod(pos, (float)sps);
    if (rem < 0.0f) rem += (float)sps;
    phase_offset = rem;
    tel.locked_phase = rem;
    tel.mean_ted_error = (err_cnt > 0) ? (float)(err_acc / err_cnt) : 0.0f;
    tel.is_locked = (err_cnt > 15 && tel.mean_ted_error < 0.18f);
    return tel;
}

static void RebuildWaterfallFromPcm(const std::vector<int16_t>& pcm_data, int current_offset,
                                    float gain_lin, float wf_max_scale, float wf_gamma,
                                    std::vector<float>& waterfall_grid) {
    int total_complex = (int)(pcm_data.size() / 2);
    if (total_complex <= VIEW_SAMPLES) return;

    int step = VIEW_SAMPLES / WATERFALL_COLS;
    int row_stride = 60;

    for (int r = 0; r < WATERFALL_ROWS; ++r) {
        int row_offset = current_offset - r * row_stride;
        if (row_offset < 0) {
            std::fill(waterfall_grid.begin() + r * WATERFALL_COLS,
                      waterfall_grid.begin() + (r + 1) * WATERFALL_COLS, 0.0f);
            continue;
        }
        row_offset = std::min(row_offset, total_complex - VIEW_SAMPLES - 1);

        for (int c = 0; c < WATERFALL_COLS; ++c) {
            float acc = 0.0f;
            int base_idx = row_offset + c * step;
            for (int k = 0; k < step; ++k) {
                float vi = (float)pcm_data[2 * (base_idx + k)] * gain_lin;
                float vq = (float)pcm_data[2 * (base_idx + k) + 1] * gain_lin;
                acc += std::sqrt(vi * vi + vq * vq);
            }
            float avg = acc / (float)step;
            float norm_val = std::clamp(avg / std::max(wf_max_scale, 1.0f), 0.0f, 1.0f);
            waterfall_grid[r * WATERFALL_COLS + c] = std::pow(norm_val, wf_gamma);
        }
    }
}

static void DrawPolarGrid(float max_r) {
    const int segments = 96;
    float radii[4] = { max_r * 0.25f, max_r * 0.50f, max_r * 0.75f, max_r };

    ImPlotSpec ring_spec;
    ring_spec.LineColor = ImVec4(0.35f, 0.45f, 0.55f, 0.35f);
    ring_spec.LineWeight = 1.0f;

    for (int r_idx = 0; r_idx < 4; ++r_idx) {
        float r = radii[r_idx];
        std::vector<float> cx(segments + 1), cy(segments + 1);
        for (int i = 0; i <= segments; ++i) {
            float theta = 2.0f * 3.14159265f * (float)i / (float)segments;
            cx[i] = r * std::cos(theta);
            cy[i] = r * std::sin(theta);
        }
        ImPlot::PlotLine("##polar_ring", cx.data(), cy.data(), segments + 1, ring_spec);
    }

    float diag = max_r * 0.7071f;
    float d1_x[2] = { -diag, diag }, d1_y[2] = { -diag, diag };
    float d2_x[2] = { -diag, diag }, d2_y[2] = { diag, -diag };

    ImPlotSpec diag_spec;
    diag_spec.LineColor = ImVec4(0.35f, 0.45f, 0.55f, 0.30f);
    diag_spec.LineWeight = 1.0f;

    ImPlot::PlotLine("##polar_diag1", d1_x, d1_y, 2, diag_spec);
    ImPlot::PlotLine("##polar_diag2", d2_x, d2_y, 2, diag_spec);
}

static bool SliderFloatDoubleClick(const char* label, float* v, float v_min, float v_max, const char* fmt) {
    static std::string editing_id;
    bool changed = false;

    if (editing_id == label) {
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat(label, v, 0.0f, 0.0f, fmt, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            *v = std::clamp(*v, v_min, v_max);
            editing_id.clear();
            changed = true;
        } else if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
            *v = std::clamp(*v, v_min, v_max);
            editing_id.clear();
            changed = true;
        }
    } else {
        changed = ImGui::SliderFloat(label, v, v_min, v_max, fmt);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) editing_id = label;
    }
    return changed;
}

static bool SliderIntDoubleClick(const char* label, int* v, int v_min, int v_max) {
    static std::string editing_id;
    bool changed = false;

    if (editing_id == label) {
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputInt(label, v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            *v = std::clamp(*v, v_min, v_max);
            editing_id.clear();
            changed = true;
        } else if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
            *v = std::clamp(*v, v_min, v_max);
            editing_id.clear();
            changed = true;
        }
    } else {
        changed = ImGui::SliderInt(label, v, v_min, v_max);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) editing_id = label;
    }
    return changed;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return -1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window* window = SDL_CreateWindow(
        "ADALM-Pluto SDR | Панель управления и телеметрии",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1620, 940,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    const char* font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
    };
    bool font_loaded = false;
    for (const char* fp : font_paths) {
        FILE* tf = std::fopen(fp, "rb");
        if (tf) {
            std::fclose(tf);
            io.Fonts->AddFontFromFileTTF(fp, 15.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
            font_loaded = true;
            break;
        }
    }
    if (!font_loaded) io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.ItemSpacing = ImVec2(10.0f, 7.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 1.00f);
    style.Colors[ImGuiCol_ChildBg]  = ImVec4(0.09f, 0.11f, 0.14f, 1.00f);

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::thread sdr_thread(SdrWorkerThread);
    sdr_thread.detach();

    AddLog("[СИСТЕМА] Готово. По умолчанию выбран живой эфир SDR.");

    std::vector<int16_t> pcm_data;
    bool use_pcm_file = false;

    std::vector<float> time_I(VIEW_SAMPLES, 0.0f), time_Q(VIEW_SAMPLES, 0.0f), time_Mag(VIEW_SAMPLES, 0.0f);
    std::vector<float> proc_I(VIEW_SAMPLES, 0.0f), proc_Q(VIEW_SAMPLES, 0.0f);
    std::vector<float> sym_I, sym_Q;
    std::vector<float> waterfall_grid(WATERFALL_ROWS * WATERFALL_COLS, 0.0f);

    float rx_freq_mhz = 800.0f;
    float tx_freq_mhz = 800.0f;
    float rx_gain_db = 30.0f;
    float tx_power_db = -15.0f;
    int manual_strobe_phase = 4;
    bool auto_symbol_sync = true;
    bool use_rx_matched_filter = true;
    bool use_tx_conv_filter = true;
    bool pause_stream = false;

    float playback_speed_x = 0.50f;
    double exact_offset = 272000.0;
    int buffer_offset = 272000;
    int ui_mode = 2;

    const char* palette_names[] = {
        "Viridis (Фиолетовый -> Зеленый -> Желтый)",
        "Plasma (Синий -> Розовый -> Желтый)",
        "Jet (Синий -> Голубой -> Желтый -> Красный)",
        "Hot (Черный -> Красный -> Оранжевый -> Белый)",
        "Cool (Голубой -> Пурпурный)",
        "Greys (Белый -> Серый -> Черный)"
    };
    const ImPlotColormap palette_ids[] = {
        ImPlotColormap_Viridis, ImPlotColormap_Plasma, ImPlotColormap_Jet,
        ImPlotColormap_Hot, ImPlotColormap_Cool, ImPlotColormap_Greys
    };
    int selected_palette = 0;
    float wf_gamma = 0.85f;
    float wf_max_scale = 1800.0f;

    float peak_mag = 0.0f;
    float mean_power_db = -120.0f;
    GardnerSyncTelemetry sync_tel;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space)) {
            pause_stream = !pause_stream;
        }

        bool frame_updated = false;
        bool user_scrubbed = false;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("RootDashboard", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::BeginChild("TopNavbar", ImVec2(0, 46), true);
        ImGui::TextColored(ImVec4(0.35f, 0.78f, 1.0f, 1.0f), "ADALM-PLUTO SDR // ПАНЕЛЬ ПРИЕМОПЕРЕДАТЧИКА");
        ImGui::SameLine(400);
        if (use_pcm_file) {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f),
                               pause_stream ? "[PCM ФАЙЛ: ПАУЗА]" : "[PCM ФАЙЛ: %.2fx]", playback_speed_x);
        } else if (g_ctrl.device_connected.load()) {
            ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f),
                               g_ctrl.tx_enabled.load()
                                   ? "[SDR: RX %.2f МГц | TX %.2f МГц]"
                                   : "[SDR: RX %.2f МГц (TX ПАУЗА)]",
                               rx_freq_mhz, tx_freq_mhz);
        } else {
            ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.75f, 1.0f), "[ЖИВОЙ ЭФИР SDR: ОЖИДАНИЕ]");
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 260);
        if (ImGui::Button(pause_stream ? "Продолжить" : "Пауза", ImVec2(115, 28))) pause_stream = !pause_stream;
        ImGui::SameLine();
        if (ImGui::Button("Очистить WF", ImVec2(115, 28))) std::fill(waterfall_grid.begin(), waterfall_grid.end(), 0.0f);
        ImGui::EndChild();

        ImGui::Spacing();

        ImGui::BeginChild("LeftSidebar", ImVec2(420, 0), true);

        ImGui::TextColored(ImVec4(0.45f, 0.80f, 1.0f, 1.0f), "ИСТОЧНИК СИГНАЛА И УПРАВЛЕНИЕ SDR");
        ImGui::Separator();

        if (ImGui::Button("Живой эфир SDR", ImVec2(190, 30))) {
            use_pcm_file = false;
            std::fill(time_I.begin(), time_I.end(), 0.0f);
            std::fill(time_Q.begin(), time_Q.end(), 0.0f);
            std::fill(waterfall_grid.begin(), waterfall_grid.end(), 0.0f);
            AddLog("[ИСТОЧНИК] Выбран живой эфир ADALM-Pluto.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Читать из PCM файла", ImVec2(195, 30))) {
            g_ctrl.sdr_running.store(false);
            FILE* f = std::fopen("txdata.pcm", "rb");
            if (!f) f = std::fopen("build/txdata.pcm", "rb");
            if (f) {
                std::fseek(f, 0, SEEK_END);
                long file_bytes = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (file_bytes > 0) {
                    size_t count = (size_t)file_bytes / sizeof(int16_t);
                    pcm_data.resize(count);
                    size_t read_items = std::fread(pcm_data.data(), sizeof(int16_t), count, f);
                    pcm_data.resize(read_items);
                    use_pcm_file = true;
                    buffer_offset = std::min(272000, std::max(0, (int)(read_items / 2) - VIEW_SAMPLES - 1));
                    exact_offset = (double)buffer_offset;
                    user_scrubbed = true;
                    AddLog("[ФАЙЛ] Загружен txdata.pcm (" + std::to_string(read_items / 2) + " отсчетов).");
                }
                std::fclose(f);
            } else {
                AddLog("[ОШИБКА] Файл txdata.pcm не найден!");
            }
        }

        ImGui::Spacing();

        const char* modes[] = { "Прием (RX)", "Передача (TX)", "Дуплекс (TX + RX)" };
        if (ImGui::Combo("Режим платы", &ui_mode, modes, IM_ARRAYSIZE(modes))) {
            g_ctrl.mode.store(ui_mode);
        }

        {
            std::lock_guard<std::mutex> lock(g_ctrl.cfg_mutex);
            ImGui::SetNextItemWidth(220);
            ImGui::InputText("##uri_input", g_ctrl.device_uri, sizeof(g_ctrl.device_uri));
            ImGui::SameLine();
            if (ImGui::SmallButton("usb:")) std::snprintf(g_ctrl.device_uri, sizeof(g_ctrl.device_uri), "usb:");
            ImGui::SameLine();
            if (ImGui::SmallButton("192.168.2.1")) std::snprintf(g_ctrl.device_uri, sizeof(g_ctrl.device_uri), "ip:192.168.2.1");
            ImGui::SameLine();
            ImGui::Text("Адрес");

            if (ImGui::InputText("Текст для TX", g_ctrl.tx_message, sizeof(g_ctrl.tx_message))) {
                g_ctrl.tx_rebuild_dirty.store(true);
            }
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
        if (ImGui::Button("Запустить SDR", ImVec2(190, 32))) {
            use_pcm_file = false;
            g_ctrl.mode.store(ui_mode);
            g_ctrl.rx_freq_hz.store((double)rx_freq_mhz * 1e6);
            g_ctrl.tx_freq_hz.store((double)tx_freq_mhz * 1e6);
            g_ctrl.tx_enabled.store(true);
            g_ctrl.sdr_running.store(true);
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("Остановить всё (SDR)", ImVec2(195, 32))) {
            g_ctrl.sdr_running.store(false);
        }
        ImGui::PopStyleColor();

        if (g_ctrl.tx_enabled.load()) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.35f, 0.12f, 1.0f));
            if (ImGui::Button("Остановить передачу (TX)", ImVec2(-1, 28))) {
                g_ctrl.tx_enabled.store(false);
                AddLog("[TX] Передача остановлена.");
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.65f, 1.0f));
            if (ImGui::Button("Возобновить передачу (TX)", ImVec2(-1, 28))) {
                g_ctrl.tx_enabled.store(true);
                AddLog("[TX] Передача возобновлена.");
            }
            ImGui::PopStyleColor();
        }

        if (use_pcm_file && (int)(pcm_data.size() / 2) > VIEW_SAMPLES) {
            int max_off = (int)(pcm_data.size() / 2) - VIEW_SAMPLES - 1;
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "НАВИГАЦИЯ И СКОРОСТЬ PCM (2x клик = ввод)");
            ImGui::Separator();

            SliderFloatDoubleClick("Скорость (x)", &playback_speed_x, 0.05f, 2.00f, "%.2fx");

            if (SliderIntDoubleClick("Весь файл", &buffer_offset, 0, max_off)) {
                exact_offset = (double)buffer_offset;
                user_scrubbed = true;
            }

            int pkt_min = std::min(268000, max_off);
            int pkt_max = std::min(286000, max_off);
            if (pkt_max > pkt_min) {
                int local_pos = std::clamp(buffer_offset, pkt_min, pkt_max);
                if (SliderIntDoubleClick("Зона пакета", &local_pos, pkt_min, pkt_max)) {
                    buffer_offset = local_pos;
                    exact_offset = (double)buffer_offset;
                    user_scrubbed = true;
                }
            }

            if (ImGui::SmallButton("В начало")) { buffer_offset = 0; exact_offset = 0.0; user_scrubbed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("-500")) { buffer_offset = std::max(0, buffer_offset - 500); exact_offset = (double)buffer_offset; user_scrubbed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("К пакету (272k)")) { buffer_offset = std::min(272000, max_off); exact_offset = (double)buffer_offset; user_scrubbed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("+500")) { buffer_offset = std::min(max_off, buffer_offset + 500); exact_offset = (double)buffer_offset; user_scrubbed = true; }
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 1.0f, 1.0f), "ЧАСТОТА, УСИЛЕНИЕ И МОЩНОСТЬ (2x клик = ввод)");
        ImGui::Separator();

        if (SliderFloatDoubleClick("Частота RX (МГц)", &rx_freq_mhz, 70.0f, 6000.0f, "%.3f МГц")) {
            g_ctrl.rx_freq_hz.store((double)rx_freq_mhz * 1e6);
            g_ctrl.freq_dirty.store(true);
            AddLog("[RF] Частота RX -> " + std::to_string(rx_freq_mhz) + " МГц");
        }
        if (SliderFloatDoubleClick("Частота TX (МГц)", &tx_freq_mhz, 70.0f, 6000.0f, "%.3f МГц")) {
            g_ctrl.tx_freq_hz.store((double)tx_freq_mhz * 1e6);
            g_ctrl.freq_dirty.store(true);
            AddLog("[RF] Частота TX -> " + std::to_string(tx_freq_mhz) + " МГц");
        }
        if (ImGui::SmallButton("Уравнять TX = RX")) {
            tx_freq_mhz = rx_freq_mhz;
            g_ctrl.tx_freq_hz.store((double)tx_freq_mhz * 1e6);
            g_ctrl.freq_dirty.store(true);
            AddLog("[RF] Частота TX = RX (" + std::to_string(tx_freq_mhz) + " МГц)");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("800 МГц")) {
            rx_freq_mhz = 800.0f;
            tx_freq_mhz = 800.0f;
            g_ctrl.rx_freq_hz.store(800e6);
            g_ctrl.tx_freq_hz.store(800e6);
            g_ctrl.freq_dirty.store(true);
            AddLog("[RF] Сброс частот на 800.0 МГц");
        }

        if (SliderFloatDoubleClick("Усиление RX (дБ)", &rx_gain_db, 0.0f, 70.0f, "%.1f дБ")) {
            g_ctrl.rx_gain.store(rx_gain_db);
            g_ctrl.gain_dirty.store(true);
        }
        if (SliderFloatDoubleClick("Мощность TX (дБ)", &tx_power_db, -80.0f, 0.0f, "%.1f дБ")) {
            g_ctrl.tx_gain.store(tx_power_db);
            g_ctrl.gain_dirty.store(true);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 1.0f, 1.0f), "ЦОС И СИМВОЛЬНАЯ СИНХРОНИЗАЦИЯ");
        ImGui::Separator();

        if (ImGui::Checkbox("Формирующий фильтр TX (Свертка TX)", &use_tx_conv_filter)) {
            g_ctrl.tx_conv_enabled.store(use_tx_conv_filter);
            g_ctrl.tx_rebuild_dirty.store(true);
        }
        ImGui::Checkbox("Согласованный фильтр RX (Свертка RX)", &use_rx_matched_filter);
        ImGui::Checkbox("Авто-синхронизация (Gardner CDR)", &auto_symbol_sync);
        SliderIntDoubleClick("Ручная фаза (0..9)", &manual_strobe_phase, 0, SPS - 1);

        if (auto_symbol_sync) {
            ImGui::TextColored(sync_tel.is_locked ? ImVec4(0.25f, 0.90f, 0.45f, 1.0f) : ImVec4(0.85f, 0.70f, 0.25f, 1.0f),
                               "Статус CDR: %s | Фаза: %.2f | Ошибка: %.3f",
                               sync_tel.is_locked ? "ЗАХВАТ" : "ПОИСК", sync_tel.locked_phase, sync_tel.mean_ted_error);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 1.0f, 1.0f), "НАСТРОЙКИ ВОДОПАДА (WATERFALL)");
        ImGui::Separator();

        ImGui::Combo("Палитра WF", &selected_palette, palette_names, IM_ARRAYSIZE(palette_names));
        if (SliderFloatDoubleClick("Гамма WF", &wf_gamma, 0.20f, 2.50f, "%.2f") && use_pcm_file) user_scrubbed = true;
        if (SliderFloatDoubleClick("Порог шкалы WF", &wf_max_scale, 300.0f, 4000.0f, "%.0f") && use_pcm_file) user_scrubbed = true;

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "ОТЛАДЧИК И ТЕЛЕМЕТРИЯ");
        ImGui::Separator();
        ImGui::Text("Пик |z|: %.1f  |  Мощность: %.1f дБFS  |  FPS: %.0f", peak_mag, mean_power_db, io.Framerate);
        if (ImGui::SmallButton("Очистить лог")) {
            std::lock_guard<std::mutex> lock(log_mutex);
            debug_log.clear();
        }
        ImGui::BeginChild("DebugLogBox", ImVec2(0, 0), true);
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            for (const auto& entry : debug_log) ImGui::TextUnformatted(entry.c_str());
        }
        ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::EndChild();

        if (use_pcm_file) {
            int total_complex = (int)(pcm_data.size() / 2);
            if (total_complex > VIEW_SAMPLES) {
                int max_off = total_complex - VIEW_SAMPLES - 1;
                float gain_lin = std::pow(10.0f, (rx_gain_db - 30.0f) / 20.0f);

                if (!pause_stream && !user_scrubbed) {
                    exact_offset += (double)playback_speed_x * ((double)BASE_STEP_AT_2X / 2.0);
                    if (exact_offset > (double)max_off) exact_offset = 0.0;
                    int new_off = (int)exact_offset;
                    if (new_off != buffer_offset) {
                        buffer_offset = new_off;
                        frame_updated = true;
                    }
                }

                for (int i = 0; i < VIEW_SAMPLES; ++i) {
                    time_I[i] = (float)pcm_data[2 * (buffer_offset + i)] * gain_lin;
                    time_Q[i] = (float)pcm_data[2 * (buffer_offset + i) + 1] * gain_lin;
                }

                if (user_scrubbed) {
                    RebuildWaterfallFromPcm(pcm_data, buffer_offset, gain_lin, wf_max_scale, wf_gamma, waterfall_grid);
                }
            }
        } else {
            if (has_new_live_frame.exchange(false) && !pause_stream) {
                std::lock_guard<std::mutex> lock(iq_mutex);
                time_I = live_I;
                time_Q = live_Q;
                frame_updated = true;
            }
        }

        float max_m = 0.0f;
        double sum_sq = 0.0;
        for (int i = 0; i < VIEW_SAMPLES; ++i) {
            float m = std::sqrt(time_I[i] * time_I[i] + time_Q[i] * time_Q[i]);
            time_Mag[i] = m;
            if (m > max_m) max_m = m;
            sum_sq += (double)(time_I[i] * time_I[i] + time_Q[i] * time_Q[i]);
        }
        peak_mag = max_m;
        double rms = std::sqrt(sum_sq / VIEW_SAMPLES);
        mean_power_db = 20.0f * std::log10((float)(rms / 2048.0 + 1e-6));

        if (use_rx_matched_filter) {
            ApplyMatchedFilter(time_I, time_Q, proc_I, proc_Q, SPS);
        } else {
            proc_I = time_I;
            proc_Q = time_Q;
        }

        if (auto_symbol_sync) {
            sync_tel = RunGardnerSymbolSync(proc_I, proc_Q, sym_I, sym_Q, SPS);
        } else {
            sym_I.clear();
            sym_Q.clear();
            for (int i = manual_strobe_phase; i < VIEW_SAMPLES; i += SPS) {
                sym_I.push_back(proc_I[i]);
                sym_Q.push_back(proc_Q[i]);
            }
            sync_tel.is_locked = false;
            sync_tel.locked_phase = (float)manual_strobe_phase;
        }

        if (frame_updated && !pause_stream && !user_scrubbed) {
            for (int r = WATERFALL_ROWS - 1; r > 0; --r) {
                for (int c = 0; c < WATERFALL_COLS; ++c) {
                    waterfall_grid[r * WATERFALL_COLS + c] = waterfall_grid[(r - 1) * WATERFALL_COLS + c];
                }
            }
            int step = VIEW_SAMPLES / WATERFALL_COLS;
            for (int c = 0; c < WATERFALL_COLS; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < step; ++k) acc += time_Mag[c * step + k];
                float avg = acc / (float)step;
                float norm_val = std::clamp(avg / std::max(wf_max_scale, 1.0f), 0.0f, 1.0f);
                waterfall_grid[c] = std::pow(norm_val, wf_gamma);
            }
        }

        ImGui::SameLine();

        ImGui::BeginChild("MainWorkspace", ImVec2(0, 0), false);

        if (ImPlot::BeginPlot("Сигнал во времени (I — Синий, Q — Красный, |z| — Зеленый)", ImVec2(-1, 270))) {
            ImPlot::SetupAxes("Номер отсчета", "Амплитуда");
            ImPlot::SetupAxesLimits(0, VIEW_SAMPLES, -2200, 2200, ImGuiCond_Once);
            ImPlotSpec spec_i; spec_i.LineColor = ImVec4(0.20f, 0.55f, 1.00f, 1.0f);
            ImPlotSpec spec_q; spec_q.LineColor = ImVec4(0.95f, 0.25f, 0.25f, 1.0f);
            ImPlotSpec spec_m; spec_m.LineColor = ImVec4(0.20f, 0.85f, 0.40f, 1.0f);

            ImPlot::PlotLine("I (Синфазный)", time_I.data(), VIEW_SAMPLES, 1.0, 0.0, spec_i);
            ImPlot::PlotLine("Q (Квадратурный)", time_Q.data(), VIEW_SAMPLES, 1.0, 0.0, spec_q);
            ImPlot::PlotLine("Модуль |z|", time_Mag.data(), VIEW_SAMPLES, 1.0, 0.0, spec_m);
            ImPlot::EndPlot();
        }

        float half_w = ImGui::GetContentRegionAvail().x * 0.46f;

        if (ImPlot::BeginPlot("Полярные координаты (Созвездие I/Q)", ImVec2(half_w, -1), ImPlotFlags_Equal)) {
            ImPlot::SetupAxes("I (Re)", "Q (Im)");
            ImPlot::SetupAxesLimits(-2400, 2400, -2400, 2400, ImGuiCond_Once);
            DrawPolarGrid(2000.0f);

            ImPlotSpec all_spec;
            all_spec.Marker = ImPlotMarker_Circle;
            all_spec.MarkerSize = 2.0f;
            all_spec.MarkerFillColor = ImVec4(0.55f, 0.35f, 0.95f, 0.22f);
            all_spec.MarkerLineColor = ImVec4(0.55f, 0.35f, 0.95f, 0.22f);
            ImPlot::PlotScatter("Все отсчеты", proc_I.data(), proc_Q.data(), VIEW_SAMPLES, all_spec);

            ImPlotSpec sym_spec;
            sym_spec.Marker = ImPlotMarker_Circle;
            sym_spec.MarkerSize = 4.8f;
            sym_spec.MarkerFillColor = auto_symbol_sync ? ImVec4(0.20f, 0.95f, 0.45f, 0.95f)
                                                        : ImVec4(1.00f, 0.25f, 0.35f, 0.95f);
            sym_spec.MarkerLineColor = sym_spec.MarkerFillColor;
            ImPlot::PlotScatter(auto_symbol_sync ? "Синхр. символы (CDR)" : "Ручная фаза",
                                sym_I.data(), sym_Q.data(), (int)sym_I.size(), sym_spec);
            ImPlot::EndPlot();
        }

        ImGui::SameLine();

        ImPlot::PushColormap(palette_ids[selected_palette]);
        float wf_w = ImGui::GetContentRegionAvail().x - 75.0f;
        if (ImPlot::BeginPlot("Водопад (История амплитуды)", ImVec2(wf_w, -1), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("Время окна ->", "История (новые сверху)",
                              ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_Lock,
                              ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_Lock);
            ImPlot::SetupAxesLimits(0.0, 1.0, 0.0, 1.0, ImGuiCond_Always);
            ImPlot::PlotHeatmap("##wf_heatmap", waterfall_grid.data(),
                                WATERFALL_ROWS, WATERFALL_COLS, 0.0, 1.0, nullptr);
            ImPlot::EndPlot();
        }
        ImGui::SameLine();
        ImPlot::ColormapScale("##wf_scale", 0.0, wf_max_scale, ImVec2(68, -1));
        ImPlot::PopColormap();

        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    g_ctrl.sdr_running.store(false);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
