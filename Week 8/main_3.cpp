#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <stdio.h> //printf
#include <stdlib.h> //free
#include <stdint.h>
#include <string.h> //strcmp
#include <complex.h>
#include <math.h>

int main(int argc, char *argv[])
{
    // --- Чтение флагов командной строки ---
    int do_tx = 1;
    int do_rx = 1;

    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "tx") == 0) {
            do_tx = 1;
            do_rx = 0;
        } else if (strcmp(argv[a], "rx") == 0) {
            do_tx = 0;
            do_rx = 1;
        }
    }
    printf("Режим: TX=%d, RX=%d | Модуляция: BPSK\n", do_tx, do_rx);

    // Создание сущности устройства
    SoapySDRKwargs args = {};
    SoapySDRKwargs_set(&args, "driver", "plutosdr");
    if (1) {
        SoapySDRKwargs_set(&args, "uri", "usb:");
    } else {
        SoapySDRKwargs_set(&args, "uri", "ip:192.168.2.1");
    }
    SoapySDRKwargs_set(&args, "direct", "1");
    SoapySDRKwargs_set(&args, "timestamp_every", "1920");
    SoapySDRKwargs_set(&args, "loopback", "0");
    SoapySDRDevice *sdr = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);

    if (sdr == NULL)
    {
        printf("SoapySDRDevice_make fail: %s\n", SoapySDRDevice_lastError());
        return EXIT_FAILURE;
    }

    // Настраиваем устройства RX\TX
    if (SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, 1e6) != 0)
    {
        printf("setSampleRate rx fail: %s\n", SoapySDRDevice_lastError());
    }
    if (SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, 800e6, NULL) != 0)
    {
        printf("setFrequency rx fail: %s\n", SoapySDRDevice_lastError());
    }
    if (SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_TX, 0, 1e6) != 0)
    {
        printf("setSampleRate tx fail: %s\n", SoapySDRDevice_lastError());
    }
    if (SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_TX, 0, 800e6, NULL) != 0)
    {
        printf("setFrequency tx fail: %s\n", SoapySDRDevice_lastError());
    }
    printf("SoapySDRDevice_getFrequency tx: %lf\n", SoapySDRDevice_getFrequency(sdr, SOAPY_SDR_TX, 0));
    
    // Настройка каналов и стримов
    size_t channels[] = {0} ; 

    size_t channel_count = sizeof(channels) / sizeof(channels[0]);
    SoapySDRStream *rxStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, SOAPY_SDR_CS16, channels, channel_count, NULL);
    if (rxStream == NULL)
    {
        printf("setupStream rx fail: %s\n", SoapySDRDevice_lastError());
        SoapySDRDevice_unmake(sdr);
        return EXIT_FAILURE;
    }
    if(SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, channels[0], 30.0) !=0 ){
        printf("setGain rx fail: %s\n", SoapySDRDevice_lastError());
    }
    if(SoapySDRDevice_setGain(sdr, SOAPY_SDR_TX, channels[0], 40.0) !=0 ){
        printf("setGain tx fail: %s\n", SoapySDRDevice_lastError());
    }
    SoapySDRStream *txStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_TX, SOAPY_SDR_CS16, channels, channel_count, NULL);
    if (txStream == NULL)
    {
        printf("setupStream tx fail: %s\n", SoapySDRDevice_lastError());
        SoapySDRDevice_unmake(sdr);
        return EXIT_FAILURE;
    }

    size_t rx_mtu = SoapySDRDevice_getStreamMTU(sdr, rxStream);
    size_t tx_mtu = SoapySDRDevice_getStreamMTU(sdr, txStream);
    printf("MTU - TX: %lu, RX: %lu\n", tx_mtu, rx_mtu);
    
    int16_t tx_buff[2*tx_mtu];
    int16_t rx_buffer[2*rx_mtu];

    // =========================================================================
    // ДЗ: 1. Генератор битов -> 2. Модуляция BPSK -> 3. Upsampling -> 4. Свёртка
    // =========================================================================
    size_t N = tx_mtu;           
    int16_t max_amp = 2000;      
    size_t sps = 16; // Samples per symbol (сколько отсчетов длится 1 прямоугольный символ)

    size_t active_len = N / 2;
    size_t num_bits = active_len / sps; // В BPSK 1 символ = 1 бит

    // 1. Генератор битов (случайные 0 и 1)
    int bits[num_bits];
    srand(42); 
    for (size_t i = 0; i < num_bits; i++) {
        bits[i] = rand() % 2;
    }

    // 2. Модуляция BPSK (только канал I: 0 -> +max_amp, 1 -> -max_amp)
    float sym_I[num_bits];
    for (size_t i = 0; i < num_bits; i++) {
        sym_I[i] = (bits[i] == 0) ? (float)max_amp : (float)(-max_amp);
    }

    // 3. Upsampling (добавляем sps-1 нулей между символами)
    float up_I[active_len];
    for (size_t i = 0; i < active_len; i++) {
        up_I[i] = 0.0f;
    }
    for (size_t i = 0; i < num_bits; i++) {
        up_I[i * sps] = sym_I[i];
    }

    // 4. Свёртка (Convolution) с прямоугольным импульсом h[k] = [1, 1, ..., 1] длины sps
    float h[sps];
    for (size_t k = 0; k < sps; k++) {
        h[k] = 1.0f;
    }

    // Заполнение буфера tx_buff результатом свёртки
    for (size_t i = 0; i < N; i++)
    {
        if (i < active_len) {
            float sum_I = 0.0f;
            for (size_t k = 0; k < sps; k++) {
                if (i >= k) {
                    sum_I += up_I[i - k] * h[k];
                }
            }
            tx_buff[2 * i]     = (int16_t)sum_I; // I-компонента
            tx_buff[2 * i + 1] = 0;              // Q-компонента в идеальном BPSK всегда 0
        } else {
            tx_buff[2 * i]     = 0;       
            tx_buff[2 * i + 1] = 0;   
        }
    }

    // Сохраняем локальный дамп для проверки работы математики передатчика
    if (do_tx && !do_rx) {
        FILE *f_tx = fopen("txdata.pcm", "wb");
        if (f_tx) {
            fwrite(tx_buff, 2 * tx_mtu * sizeof(int16_t), 1, f_tx);
            fclose(f_tx);
        }
    }

    for(size_t i = 0; i < 2; i++)
    {
        tx_buff[0 + i] = 0xffff;
        tx_buff[10 + i] = 0xffff;
    }

    //activate streams
    if (do_rx) SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0); 
    if (do_tx) SoapySDRDevice_activateStream(sdr, txStream, 0, 0, 0); 

    printf("Start test...\n");
    const long  timeoutUs = 400000; 

    long long last_time = 0;

    FILE *file = NULL;
    if (do_rx) {
        file = fopen("txdata.pcm", "wb");
    }
    
    // RX слушает дольше (20000), чтобы гарантированно поймать эфир
    size_t iteration_count = (do_rx && !do_tx) ? 20000 : 2000;

    for (size_t buffers_read = 0; buffers_read < iteration_count; buffers_read++)
    {
        if (do_rx)
        {
            void *rx_buffs[] = {rx_buffer};
            int flags;        
            long long timeNs; 

            int sr = SoapySDRDevice_readStream(sdr, rxStream, rx_buffs, rx_mtu, &flags, &timeNs, timeoutUs);
            if (sr < 0)
            {
                continue;
            }
      
            printf("Buffer: %lu - Samples: %i, Flags: %i, Time: %lli, TimeDiff: %lli\n", buffers_read, sr, flags, timeNs, timeNs - last_time);
            fwrite(rx_buffer, 2 * rx_mtu * sizeof(int16_t), 1, file);
            last_time = timeNs;
        }

        if (do_tx)
        {
            long long tx_time = last_time + (4 * 1000 * 1000); 

            for(size_t i = 0; i < 8; i++)
            {
                uint8_t tx_time_byte = (tx_time >> (i * 8)) & 0xff;
                tx_buff[2 + i] = tx_time_byte << 4;
            }

            void *tx_buffs[] = {tx_buff};
            
            printf("Transmitting buffer: %zu\n", buffers_read);
            
            int flags_tx = 0; 
            int st = SoapySDRDevice_writeStream(sdr, txStream, (const void * const*)tx_buffs, tx_mtu, &flags_tx, 0, timeoutUs);
            
            if ((size_t)st != tx_mtu)
            {
                printf("TX Failed: %i\n", st);
            }
        }
    }
    if (file) fclose(file);

    //stop streaming
    if (do_rx) SoapySDRDevice_deactivateStream(sdr, rxStream, 0, 0);
    if (do_tx) SoapySDRDevice_deactivateStream(sdr, txStream, 0, 0);

    //shutdown the stream
    if (do_rx) SoapySDRDevice_closeStream(sdr, rxStream);
    if (do_tx) SoapySDRDevice_closeStream(sdr, txStream);

    SoapySDRDevice_unmake(sdr);

    for (size_t j = 0; j < channel_count; j++)
    {
        printf("Checking channel %zu\n", j);
    }

    printf("test complete!\n");

    return EXIT_SUCCESS;
}