#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/dma.h"

#include "sound/sound_sample.h"

// MAX98357A
#define I2S_DATA_PIN        9
#define I2S_CLOCK_PIN_BASE 10   // GP10=BCLK、GP11=LRC

#define OUTPUT_SAMPLE_RATE 44100
#define BUFFER_COUNT           3
#define SAMPLES_PER_BUFFER   256

typedef struct {
    const uint8_t *pcm_data;
    size_t pcm_size;

    uint16_t audio_format;
    uint16_t channel_count;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t block_align;
} wav_info_t;

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0]
         | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0]
         | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[3] << 24);
}

/*
 * WAV内のRIFFチャンクを検索する。
 *
 * 固定44バイトヘッダーとは仮定せず、
 * fmt、data以外のチャンクが含まれていても処理する。
 */
static bool parse_wav(
    const uint8_t *wav,
    size_t wav_size,
    wav_info_t *info
) {
    if (wav == NULL || info == NULL || wav_size < 12) {
        return false;
    }

    if (memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    memset(info, 0, sizeof(*info));

    bool found_fmt = false;
    bool found_data = false;

    size_t position = 12;

    while (position + 8 <= wav_size) {
        const uint8_t *chunk_header = wav + position;
        uint32_t chunk_size = read_le32(chunk_header + 4);

        position += 8;

        if ((size_t)chunk_size > wav_size - position) {
            return false;
        }

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                return false;
            }

            const uint8_t *fmt = wav + position;

            info->audio_format    = read_le16(fmt + 0);
            info->channel_count   = read_le16(fmt + 2);
            info->sample_rate     = read_le32(fmt + 4);
            info->block_align     = read_le16(fmt + 12);
            info->bits_per_sample = read_le16(fmt + 14);

            found_fmt = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            info->pcm_data = wav + position;
            info->pcm_size = chunk_size;

            found_data = true;
        }

        /*
         * RIFFチャンクは偶数バイト境界にパディングされる。
         */
        size_t padded_size =
            (size_t)chunk_size + ((size_t)chunk_size & 1u);

        if (padded_size > wav_size - position) {
            return false;
        }

        position += padded_size;
    }

    return found_fmt && found_data;
}

static audio_buffer_pool_t *init_audio(void) {
    static const audio_format_t audio_format = {
        .sample_freq = OUTPUT_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2
    };

    static const audio_buffer_format_t producer_format = {
        .format = &audio_format,
        .sample_stride = 4
    };

    audio_buffer_pool_t *producer_pool =
        audio_new_producer_pool(
            &producer_format,
            BUFFER_COUNT,
            SAMPLES_PER_BUFFER
        );

    if (producer_pool == NULL) {
        return NULL;
    }

    /*
     * audio_i2s_setup()がDMAチャンネルをclaimするため、
     * 使用可能な番号を取得してから一度解放する。
     */
    int dma_channel = dma_claim_unused_channel(false);

    if (dma_channel < 0) {
        return NULL;
    }

    dma_channel_unclaim((uint)dma_channel);

    const audio_i2s_config_t i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_CLOCK_PIN_BASE,
        .dma_channel = (uint8_t)dma_channel,
        .pio_sm = 0
    };

    const audio_format_t *output_format =
        audio_i2s_setup(&audio_format, &i2s_config);

    if (output_format == NULL) {
        return NULL;
    }

    if (!audio_i2s_connect(producer_pool)) {
        return NULL;
    }

    audio_i2s_set_enabled(true);

    return producer_pool;
}

/*
 * 1つの出力バッファをWAVデータで埋める。
 *
 * 戻り値:
 * true  = まだデータがある、または今回データを出力した
 * false = 再生完了
 */
static bool fill_audio_buffer(
    audio_buffer_t *buffer,
    const wav_info_t *wav,
    size_t *byte_position
) {
    if (*byte_position >= wav->pcm_size) {
        buffer->sample_count = 0;
        return false;
    }

    int16_t *output = (int16_t *)buffer->buffer->bytes;
    uint32_t output_frames = 0;

    while (output_frames < buffer->max_sample_count &&
           *byte_position + wav->block_align <= wav->pcm_size) {

        const uint8_t *input =
            wav->pcm_data + *byte_position;

        int16_t left;
        int16_t right;

        if (wav->channel_count == 1) {
            /*
             * モノラルWAVを左右両方へ複製する。
             */
            int16_t mono = (int16_t)read_le16(input);

            left = mono;
            right = mono;
        } else {
            /*
             * 16bitステレオ:
             * L下位、L上位、R下位、R上位
             */
            left = (int16_t)read_le16(input);
            right = (int16_t)read_le16(input + 2);
        }

        output[output_frames * 2]     = left;
        output[output_frames * 2 + 1] = right;

        *byte_position += wav->block_align;
        output_frames++;
    }

    buffer->sample_count = output_frames;

    return output_frames != 0;
}

static bool validate_wav(const wav_info_t *wav) {
    if (wav->audio_format != 1) {
        printf("Unsupported WAV encoding: %u\n",
               wav->audio_format);
        printf("Only uncompressed PCM is supported\n");
        return false;
    }

    if (wav->bits_per_sample != 16) {
        printf("Unsupported bit depth: %u\n",
               wav->bits_per_sample);
        return false;
    }

    if (wav->channel_count != 1 &&
        wav->channel_count != 2) {
        printf("Unsupported channel count: %u\n",
               wav->channel_count);
        return false;
    }

    if (wav->sample_rate != OUTPUT_SAMPLE_RATE) {
        printf("Unsupported sample rate: %lu Hz\n",
               (unsigned long)wav->sample_rate);
        printf("Expected: %u Hz\n", OUTPUT_SAMPLE_RATE);
        return false;
    }

    uint16_t expected_block_align =
        wav->channel_count *
        (wav->bits_per_sample / 8);

    if (wav->block_align != expected_block_align) {
        printf("Invalid WAV block alignment\n");
        return false;
    }

    return true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    printf("\nMAX98357A WAV player\n");

    wav_info_t wav;

    if (!parse_wav(sound_sample, sound_sample_size, &wav)) {
        printf("WAV parse failed\n");

        while (true) {
            tight_loop_contents();
        }
    }

    printf("Channels:    %u\n", wav.channel_count);
    printf("Sample rate: %lu Hz\n",
           (unsigned long)wav.sample_rate);
    printf("Bit depth:   %u bit\n", wav.bits_per_sample);
    printf("PCM size:    %lu bytes\n",
           (unsigned long)wav.pcm_size);

    if (!validate_wav(&wav)) {
        while (true) {
            tight_loop_contents();
        }
    }

    audio_buffer_pool_t *producer_pool = init_audio();

    if (producer_pool == NULL) {
        printf("Audio initialization failed\n");

        while (true) {
            tight_loop_contents();
        }
    }

    size_t byte_position = 0;

    while (byte_position < wav.pcm_size) {
        audio_buffer_t *buffer =
            take_audio_buffer(producer_pool, true);

        if (buffer == NULL) {
            continue;
        }

        bool has_audio =
            fill_audio_buffer(
                buffer,
                &wav,
                &byte_position
            );

        if (!has_audio) {
            buffer->sample_count = 0;
        }

        give_audio_buffer(producer_pool, buffer);
    }

    /*
     * 最終バッファがDMAで再生される時間を確保する。
     * 厳密に終了を検出したい場合は、音声キューの状態を
     * 確認する実装へ変更する。
     */
    sleep_ms(
        (SAMPLES_PER_BUFFER * 1000 / OUTPUT_SAMPLE_RATE)
        * (BUFFER_COUNT + 1)
    );

    audio_i2s_set_enabled(false);

    printf("Playback finished\n");

    while (true) {
        tight_loop_contents();
    }
}

