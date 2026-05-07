// snd_sdl.c -- SDL3 audio driver replacing snd_win.c
// Uses SDL3 audio stream with a get-callback that reads from Quake's DMA ring buffer.

#include <SDL3/SDL.h>
#include "quakedef.h"
#include "winquake.h"

// DirectSound globals — always NULL/0 in SDL build; guards in snd_dma/mix prevent access.
LPDIRECTSOUNDBUFFER pDSBuf    = NULL;
DWORD               gSndBufSize = 0;

static SDL_AudioDeviceID sdl_audio_dev  = 0;
static SDL_AudioStream  *sdl_audio_stream = NULL;

// DMA ring buffer - owned by us, pointed to by shm->buffer
#define DMA_BUFFER_SAMPLES  (1 << 15)  // 32768 mono samples = ~0.74s at 22050Hz
static byte dma_buf[DMA_BUFFER_SAMPLES * 2]; // *2 for 16-bit

volatile dma_t sn;

// Playback cursor in mono samples, advanced by the audio callback
static volatile int dma_play_pos;

// ---------------------------------------------------------------------------
// SDL3 audio stream get-callback
// Called from SDL's audio thread when the device needs more data.
// ---------------------------------------------------------------------------

static void audio_get_callback(void *userdata, SDL_AudioStream *stream,
                                int additional_amount, int total_amount)
{
    (void)userdata; (void)total_amount;
    if (!shm || !shm->buffer) return;

    int bytes_per_sample = shm->samplebits / 8;       // 2 for 16-bit
    int total_buf_bytes  = shm->samples * bytes_per_sample;
    int needed           = additional_amount;

    // Clamp to available - don't provide more than our buffer holds
    if (needed > total_buf_bytes)
        needed = total_buf_bytes;

    byte scratch[4096];
    int pos_bytes = dma_play_pos * bytes_per_sample;

    while (needed > 0)
    {
        int chunk = needed < (int)sizeof(scratch) ? needed : (int)sizeof(scratch);
        int avail = total_buf_bytes - pos_bytes;
        if (avail <= 0) { pos_bytes = 0; avail = total_buf_bytes; }
        if (chunk > avail) chunk = avail;

        memcpy(scratch, shm->buffer + pos_bytes, chunk);
        SDL_PutAudioStreamData(stream, scratch, chunk);

        pos_bytes = (pos_bytes + chunk) % total_buf_bytes;
        needed -= chunk;
    }

    dma_play_pos = pos_bytes / bytes_per_sample;
    shm->samplepos = dma_play_pos;
}

// ---------------------------------------------------------------------------
// SNDDMA interface
// ---------------------------------------------------------------------------

extern qboolean sys_headless;

qboolean SNDDMA_Init(void)
{
    shm = &sn;
    memset(shm, 0, sizeof(*shm));

    shm->channels       = 2;
    shm->samplebits     = 16;
    shm->speed          = 22050;
    shm->samples        = DMA_BUFFER_SAMPLES;
    shm->submission_chunk = 1;
    shm->buffer         = dma_buf;
    shm->soundalive     = true;
    shm->splitbuffer    = false;

    memset(dma_buf, 0, sizeof(dma_buf));
    dma_play_pos = 0;

    if (sys_headless)
    {
        // Pretend audio is initialized so S_Init doesn't deref a null shm,
        // but never open a real device — main loop won't run anyway.
        return true;
    }

    SDL_AudioSpec spec = {
        .format   = SDL_AUDIO_S16,
        .channels = shm->channels,
        .freq     = shm->speed
    };

    sdl_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
        audio_get_callback, NULL);

    if (!sdl_audio_stream)
    {
        Con_Printf("SNDDMA_Init: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        shm = NULL;
        return false;
    }

    SDL_ResumeAudioStreamDevice(sdl_audio_stream);

    Con_Printf("SDL audio initialized: %d Hz, %d-bit, %s\n",
               shm->speed, shm->samplebits,
               shm->channels == 2 ? "stereo" : "mono");
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    if (!shm) return 0;
    return dma_play_pos;
}

void SNDDMA_Submit(void) {}  // pull-callback model; nothing to submit

void SNDDMA_Shutdown(void)
{
    if (sdl_audio_stream)
    {
        SDL_DestroyAudioStream(sdl_audio_stream);
        sdl_audio_stream = NULL;
        sdl_audio_dev    = 0;
    }
    shm = NULL;
}

// ---------------------------------------------------------------------------
// Block / unblock (called on focus loss / gain)
// ---------------------------------------------------------------------------

void S_BlockSound(void)
{
    snd_blocked++;
    if (snd_blocked == 1 && sdl_audio_stream)
        SDL_PauseAudioStreamDevice(sdl_audio_stream);
}

void S_UnblockSound(void)
{
    snd_blocked--;
    if (snd_blocked == 0 && sdl_audio_stream)
        SDL_ResumeAudioStreamDevice(sdl_audio_stream);
}
