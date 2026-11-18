#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <complex>
#include <string>
#include <algorithm>
#include <iostream>
#include <iomanip>

constexpr int SPS = 10;
constexpr double SAMPLE_RATE = 1e6;
constexpr double CENTER_FREQ = 800e6;

static std::vector<int16_t> BuildTxBuffer(const std::string& message, size_t mtu) {
    std::vector<int16_t> buff(2 * mtu, 0);
    buff[0] = (int16_t)0xffff; buff[1] = (int16_t)0xffff;
    buff[10] = (int16_t)0xffff; buff[11] = (int16_t)0xffff;

    size_t max_bits = (mtu / 2 - 6) / SPS;
    size_t n_bits = std::min(message.size() * 8, max_bits);

    size_t sample_idx = 6;
    for (size_t b = 0; b < n_bits; ++b) {
        char c = message[b / 8];
        int bit = (c >> (7 - (b % 8))) & 1;
        int16_t amp = (bit == 0) ? 1500 : -1500;

        for (int s = 0; s < SPS; ++s, ++sample_idx) {
            buff[2 * sample_idx]     = (int16_t)(amp << 4);
            buff[2 * sample_idx + 1] = 0;
        }
    }
    return buff;
}

static std::vector<std::complex<float>> ApplyRxMatchedFilter(const std::vector<int16_t>& raw_iq, int sps) {
    size_t n_samples = raw_iq.size() / 2;
    std::vector<std::complex<float>> filtered(n_samples, {0.0f, 0.0f});
    float norm = 1.0f / (float)sps;

    for (size_t i = 0; i < n_samples; ++i) {
        float acc_i = 0.0f, acc_q = 0.0f;
        for (int k = 0; k < sps; ++k) {
            if (i >= (size_t)k) {
                acc_i += (float)raw_iq[2 * (i - k)];
                acc_q += (float)raw_iq[2 * (i - k) + 1];
            }
        }
        filtered[i] = {acc_i * norm, acc_q * norm};
    }
    return filtered;
}

static inline std::complex<float> InterpolateFarrow(const std::vector<std::complex<float>>& x, float pos) {
    int n = (int)std::floor(pos);
    float mu = pos - (float)n;
    int max_idx = (int)x.size() - 1;

    int i_m1 = std::clamp(n - 1, 0, max_idx);
    int i_0  = std::clamp(n,     0, max_idx);
    int i_p1 = std::clamp(n + 1, 0, max_idx);
    int i_p2 = std::clamp(n + 2, 0, max_idx);

    std::complex<float> v2 = 0.5f * (x[i_p2] - x[i_p1] - x[i_0] + x[i_m1]);
    std::complex<float> v1 = 0.5f * (-x[i_p2] + 3.0f * x[i_p1] - x[i_0] - x[i_m1]);
    std::complex<float> v0 = x[i_0];
    return (v2 * mu + v1) * mu + v0;
}

struct SymbolSyncResult {
    std::vector<std::complex<float>> symbols;
    float locked_phase = 0.0f;
    float mean_error = 0.0f;
    bool locked = false;
};

static SymbolSyncResult RunGardnerSymbolSync(const std::vector<std::complex<float>>& rx_filt, int sps) {
    SymbolSyncResult res;
    int n = (int)rx_filt.size();
    if (n < sps * 4) return res;

    float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::abs(rx_filt[i]));
    float power_gate = (peak * 0.25f) * (peak * 0.25f);

    const float Kp = 0.40f;
    const float Ki = 0.02f;
    float integrator = 0.0f;
    float pos = (float)sps;

    double err_acc = 0.0;
    int active_syms = 0;

    while (pos < (float)(n - 3)) {
        std::complex<float> y_curr = InterpolateFarrow(rx_filt, pos);
        std::complex<float> y_mid  = InterpolateFarrow(rx_filt, pos - 0.5f * (float)sps);
        std::complex<float> y_prev = InterpolateFarrow(rx_filt, pos - 1.0f * (float)sps);

        res.symbols.push_back(y_curr);

        float p_avg = 0.5f * (std::norm(y_curr) + std::norm(y_prev));
        float delta = 0.0f;

        if (peak >= 15.0f && p_avg > power_gate) {
            float ted_raw = std::real(std::conj(y_curr - y_prev) * y_mid);
            float err_norm = std::clamp(ted_raw / (p_avg + 1e-6f), -1.0f, 1.0f);

            integrator = std::clamp(integrator + Ki * err_norm, -0.5f, 0.5f);
            delta = Kp * err_norm + integrator;

            err_acc += std::abs(err_norm);
            active_syms++;
        }
        pos += (float)sps - delta;
    }

    float rem = std::fmod(pos, (float)sps);
    if (rem < 0.0f) rem += (float)sps;
    res.locked_phase = rem;
    res.mean_error = (active_syms > 0) ? (float)(err_acc / active_syms) : 0.0f;
    res.locked = (active_syms > 15 && res.mean_error < 0.18f);
    return res;
}

int main(int argc, char* argv[]) {
    std::string uri = (argc > 1) ? argv[1] : "usb:";
    if (uri.find(':') == std::string::npos) uri = "ip:" + uri;

    SoapySDRKwargs args = {};
    SoapySDRKwargs_set(&args, "driver", "plutosdr");
    SoapySDRKwargs_set(&args, "uri", uri.c_str());
    SoapySDRKwargs_set(&args, "direct", "1");
    SoapySDRKwargs_set(&args, "timestamp_every", "1920");
    SoapySDRKwargs_set(&args, "loopback", "0");

    SoapySDRDevice* sdr = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);
    if (!sdr) {
        std::cerr << "[ОШИБКА] PlutoSDR не найден: " << SoapySDRDevice_lastError() << "\n";
        return EXIT_FAILURE;
    }

    SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, SAMPLE_RATE);
    SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, CENTER_FREQ, nullptr);
    SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, 30.0);

    SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_TX, 0, SAMPLE_RATE);
    SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_TX, 0, CENTER_FREQ, nullptr);
    SoapySDRDevice_setGain(sdr, SOAPY_SDR_TX, 0, -40.0);

    size_t channels[] = {0};
    SoapySDRStream* rxStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, SOAPY_SDR_CS16, channels, 1, nullptr);
    SoapySDRStream* txStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_TX, SOAPY_SDR_CS16, channels, 1, nullptr);
    SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0);
    SoapySDRDevice_activateStream(sdr, txStream, 0, 0, 0);

    size_t rx_mtu = SoapySDRDevice_getStreamMTU(sdr, rxStream);
    size_t tx_mtu = SoapySDRDevice_getStreamMTU(sdr, txStream);

    std::vector<int16_t> rx_buffer(2 * rx_mtu, 0);
    std::vector<int16_t> tx_buff = BuildTxBuffer("Hello Pluto!", tx_mtu);
    std::vector<int16_t> full_rx_capture;
    full_rx_capture.reserve(300 * 2 * rx_mtu);

    long long last_time = 0;
    for (size_t iter = 0; iter < 300; ++iter) {
        void* buffs[] = {rx_buffer.data()};
        int flags = 0;
        long long timeNs = 0;
        int sr = SoapySDRDevice_readStream(sdr, rxStream, buffs, rx_mtu, &flags, &timeNs, 200000);
        if (sr > 0) {
            last_time = timeNs;
            full_rx_capture.insert(full_rx_capture.end(), rx_buffer.begin(), rx_buffer.begin() + 2 * sr);
        }

        if (iter >= 20 && iter % 10 == 0) {
            long long tx_time = last_time + 4000000LL;
            for (size_t i = 0; i < 8; ++i) {
                uint8_t b = (tx_time >> (i * 8)) & 0xff;
                tx_buff[2 + i] = (int16_t)(b << 4);
            }
            void* tx_buffs[] = {tx_buff.data()};
            int flags_tx = SOAPY_SDR_HAS_TIME;
            SoapySDRDevice_writeStream(sdr, txStream, (const void* const*)tx_buffs, tx_mtu, &flags_tx, tx_time, 200000);
        }
    }

    SoapySDRDevice_deactivateStream(sdr, rxStream, 0, 0);
    SoapySDRDevice_deactivateStream(sdr, txStream, 0, 0);
    SoapySDRDevice_closeStream(sdr, rxStream);
    SoapySDRDevice_closeStream(sdr, txStream);
    SoapySDRDevice_unmake(sdr);

    FILE* f_raw = std::fopen("txdata.pcm", "wb");
    if (f_raw) {
        std::fwrite(full_rx_capture.data(), sizeof(int16_t), full_rx_capture.size(), f_raw);
        std::fclose(f_raw);
    }

    auto rx_filtered = ApplyRxMatchedFilter(full_rx_capture, SPS);
    auto sync_res    = RunGardnerSymbolSync(rx_filtered, SPS);

    std::cout << std::fixed << std::setprecision(2)
              << "[SDR] Принято сэмплов: " << full_rx_capture.size() / 2
              << " | Символов после CDR: " << sync_res.symbols.size()
              << " | Захват: " << (sync_res.locked ? "ДА" : "НЕТ")
              << " (фаза = " << sync_res.locked_phase << ")\n";

    return EXIT_SUCCESS;
}
