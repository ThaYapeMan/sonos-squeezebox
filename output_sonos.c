// SONOS::Squeezebox -- deploy Sonos in a Logitech Media Server (LMS) streaming environment
//
// Copyright (c) 2023 Martin van der Werff <github (at) newinnovations.nl>
//
// This file is part of SONOS::Squeezebox.
//
// SONOS::Squeezebox is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "squeezelite.h"
#include "output_sonos.h"
#include "sonos-position.h"

#if BYTES_PER_FRAME != 8
#error BYTES_PER_FRAME not 8 bytes
#endif

#define FRAME_BLOCK MAX_SILENCE_FRAMES

static log_level loglevel;

static bool running = true;

extern struct outputstate output;
extern struct buffer* outputbuf;

#define LOCK mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

void encode_squeezebox_audio(const char* data, int len);
void close_squeezebox_audio();

static u8_t* buf;
static unsigned buffill;
static int bytes_per_frame;
static unsigned squeezebox_stream_id = 0;

static bool silent = true;

// Temporary diagnostic switch — remove once root cause of stutter is confirmed.
// Set DISABLE_SONOS_POSITION_FIX=1 in the systemd unit to revert to pre-fix behaviour
// (device_frames stays 0) without rebuilding. Restart the service; no make needed.
static bool disable_position_fix = false;

void new_squeezebox_stream_id(void)
{
    ++squeezebox_stream_id;
    printf("Creating new stream (%u) for Sonos\n", squeezebox_stream_id);
}

unsigned get_squeezebox_stream_id(void)
{
    return squeezebox_stream_id;
}

static int _sonos_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
    s32_t cross_gain_in, s32_t cross_gain_out, s32_t** cross_ptr)
{

    u8_t* obuf;

    if (!silence) {

        if (silent) {
            printf("From silent to non-silent\n");
            new_squeezebox_stream_id();
            silent = false;
        }

        if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
            _apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
        }

        obuf = outputbuf->readp;

    } else {

        if (!silent) {
            printf("From non-silent to silent\n");
            close_squeezebox_audio();
            silent = true;
        }

        return 0; // no silence output
    }

    // _scale_and_pack_frames(buf + buffill * bytes_per_frame, (s32_t *)(void *)obuf, out_frames, gainL, gainR, flags, output.format);
    _scale_and_pack_frames(buf + buffill * bytes_per_frame, (s32_t*)(void*)obuf, out_frames, FIXED_ONE, FIXED_ONE, 0, output.format);

    buffill += out_frames;

    return (int)out_frames;
}

uint32_t get_sb_time_ms(void)
{
    uint32_t res = gettime_ms();
    if (res) {
        return res;
    }
    return 0;
}

static void* output_thread()
{
    while (running) {

        LOCK;
        output.updated = gettime_ms();
        output.frames_played_dmp = output.frames_played;
        _output_frames(FRAME_BLOCK);

        // Compute device_frames from the actual Sonos playback position so that
        // slimproto's ms_played formula yields the Sonos position rather than the
        // internal decode position (which runs 1800 ms+ ahead due to Sonos buffering).
        //
        // slimproto formula:
        //   ms_played = (frames_played_dmp - device_frames) * 1000 / sample_rate
        //               + (now - updated)
        //
        // Setting device_frames = frames_played_dmp - sonos_frames gives:
        //   ms_played ≈ sonos_ms   (+ sub-10 ms interpolation from now-updated)
        if (!disable_position_fix) {
            u32_t sr = output.current_sample_rate;
            u32_t sonos_ms = get_sonos_position_ms();
            if (sonos_ms > 0 && sr > 0) {
                u64_t sonos_frames = (u64_t)sonos_ms * sr / 1000;
                u64_t fp = (u64_t)output.frames_played_dmp;
                output.device_frames = (fp > sonos_frames) ? (u32_t)(fp - sonos_frames) : 0;
            } else {
                output.device_frames = 0;
            }
        }
        UNLOCK;

        if (buffill) {
            encode_squeezebox_audio((const char*)buf, buffill * bytes_per_frame);
            buffill = 0;
        }

        usleep(10);
    }

    return 0;
}

static thread_type thread;

void output_init_sonos(log_level level, unsigned output_buf_size, char* params, unsigned rates[], unsigned rate_delay)
{
    loglevel = level;

    disable_position_fix = (getenv("DISABLE_SONOS_POSITION_FIX") != NULL);
    if (disable_position_fix)
        printf("DISABLE_SONOS_POSITION_FIX set: UPnP position fix disabled\n");

    LOG_INFO("init output sonos");

    buf = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
    if (!buf) {
        LOG_ERROR("unable to malloc buf");
        return;
    }
    buffill = 0;

    memset(&output, 0, sizeof(output));

    output.format = S16_LE;
    output.start_frames = FRAME_BLOCK * 2;
    output.write_cb = &_sonos_write_frames;
    output.rate_delay = rate_delay;

    switch (output.format) {
    case S24_3LE:
        bytes_per_frame = 3 * 2;
        break;
    case S16_LE:
        bytes_per_frame = 2 * 2;
        break;
    case S32_LE:
    default:
        bytes_per_frame = 4 * 2;
        break;
    }

    // ensure output rate is specified to avoid test open
    if (!rates[0]) {
        rates[0] = 44100;
    }

    output_init_common(level, "-", output_buf_size, rates, 0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
    pthread_create(&thread, &attr, output_thread, NULL);
    pthread_attr_destroy(&attr);
}

void output_close_sonos(void)
{
    LOG_INFO("close output");

    LOCK;
    running = false;
    UNLOCK;

    free(buf);

    output_close_common();
}

bool test_open(const char* device, unsigned rates[], bool userdef_rates)
{
    rates[0] = 44100;
    return true;
}

void set_volume(unsigned left, unsigned right)
{
    // printf("SONOS: set_volume(%d, %d)\n", left, right);
}
