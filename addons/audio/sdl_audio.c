#include "allegro5/allegro.h"
#include "allegro5/internal/aintern_audio.h"
#include "allegro5/platform/allegro_internal_sdl.h"

ALLEGRO_DEBUG_CHANNEL("SDL")

_AL_LIST* device_list;

typedef struct SDL_VOICE
{
   SDL_AudioDeviceID device;
   SDL_AudioSpec spec;
   ALLEGRO_VOICE *voice;
   bool is_playing;
} SDL_VOICE;

typedef struct SDL_RECORDER
{
   SDL_AudioDeviceID device;
   SDL_AudioSpec spec;
   unsigned int fragment;
} SDL_RECORDER;

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
   // TODO: Allegro has those mysterious "non-streaming" samples, but I
   // can't figure out what their purpose is and how would I play them...

   SDL_VOICE *sv = userdata;
   ALLEGRO_SAMPLE_INSTANCE *instance = sv->voice->attached_stream;
   ALLEGRO_SAMPLE *sample = &instance->spl_data;

   unsigned int frames = sv->spec.samples;
   const void *data = _al_voice_update(sv->voice, sv->voice->mutex, &frames);
   if (data) {
      // FIXME: What is frames for?
      memcpy(stream, sample->buffer.ptr, len);
   }
}

static int sdl_open(void)
{
   return 0;
}

static void sdl_close(void)
{
}

static SDL_AudioFormat allegro_format_to_sdl(ALLEGRO_AUDIO_DEPTH d)
{
   if (d == ALLEGRO_AUDIO_DEPTH_INT8) return AUDIO_S8;
   if (d == ALLEGRO_AUDIO_DEPTH_UINT8) return AUDIO_U8;
   if (d == ALLEGRO_AUDIO_DEPTH_INT16) return AUDIO_S16;
   if (d == ALLEGRO_AUDIO_DEPTH_UINT16) return AUDIO_U16;
   return AUDIO_F32;
}

static int sdl_allocate_voice(ALLEGRO_VOICE *voice)
{
   SDL_VOICE *sv = al_malloc(sizeof *sv);
   SDL_AudioSpec want;
   memset(&want, 0, sizeof want);

   want.freq = voice->frequency;
   want.format = allegro_format_to_sdl(voice->depth);
   want.channels = al_get_channel_count(voice->chan_conf);
   // TODO: Should make this configurable somehow
   want.samples = 4096;
   want.callback = audio_callback;
   want.userdata = sv;

   sv->device = SDL_OpenAudioDevice(NULL, 0, &want, &sv->spec,
      SDL_AUDIO_ALLOW_FORMAT_CHANGE);

   voice->extra = sv;
   sv->voice = voice;

   return 0;
}

static void sdl_deallocate_voice(ALLEGRO_VOICE *voice)
{
   SDL_VOICE *sv = voice->extra;
   _al_list_destroy(device_list);
   SDL_CloseAudioDevice(sv->device);
   al_free(sv);
}

static int sdl_load_voice(ALLEGRO_VOICE *voice, const void *data)
{
   (void)data;
   voice->attached_stream->pos = 0;
   return 0;
}

static void sdl_unload_voice(ALLEGRO_VOICE *voice)
{
   (void) voice;
}

static int sdl_start_voice(ALLEGRO_VOICE *voice)
{
   SDL_VOICE *sv = voice->extra;
   sv->is_playing = true;
   SDL_PauseAudioDevice(sv->device, 0);
   return 0;
}

static int sdl_stop_voice(ALLEGRO_VOICE *voice)
{
   SDL_VOICE *sv = voice->extra;
   sv->is_playing = false;
   SDL_PauseAudioDevice(sv->device, 1);
   return 0;
}

static bool sdl_voice_is_playing(const ALLEGRO_VOICE *voice)
{
   SDL_VOICE *sv = voice->extra;
   return sv->is_playing;
}

static unsigned int sdl_get_voice_position(const ALLEGRO_VOICE *voice)
{
   return voice->attached_stream->pos;
}

static int sdl_set_voice_position(ALLEGRO_VOICE *voice, unsigned int pos)
{
   voice->attached_stream->pos = pos;
   return 0;
}

static void recorder_callback(void *userdata, Uint8 *stream, int len)
{
   ALLEGRO_AUDIO_RECORDER *r = (ALLEGRO_AUDIO_RECORDER *) userdata;
   SDL_RECORDER *sdl = (SDL_RECORDER *) r->extra;

   al_lock_mutex(r->mutex);
   if (!r->is_recording) {
      al_unlock_mutex(r->mutex);
      return;
   }

   while (len > 0) {
      int count = SDL_min(len, r->samples * r->sample_size);
      memcpy(r->fragments[sdl->fragment], stream, count);

      ALLEGRO_EVENT user_event;
      ALLEGRO_AUDIO_RECORDER_EVENT *e;
      user_event.user.type = ALLEGRO_EVENT_AUDIO_RECORDER_FRAGMENT;
      e = al_get_audio_recorder_event(&user_event);
      e->buffer = r->fragments[sdl->fragment];
      e->samples = count / r->sample_size;
      al_emit_user_event(&r->source, &user_event, NULL);

      sdl->fragment++;
      if (sdl->fragment == r->fragment_count) {
         sdl->fragment = 0;
      }
      len -= count;
   }

   al_unlock_mutex(r->mutex);
}

static int sdl_allocate_recorder(ALLEGRO_AUDIO_RECORDER *r)
{
   SDL_RECORDER *sdl;

   sdl = al_calloc(1, sizeof(*sdl));
   if (!sdl) {
     ALLEGRO_ERROR("Unable to allocate memory for SDL_RECORDER.\n");
     return 1;
   }

   SDL_AudioSpec want;
   memset(&want, 0, sizeof want);

   want.freq = r->frequency;
   want.format = allegro_format_to_sdl(r->depth);
   want.channels = al_get_channel_count(r->chan_conf);
   want.samples = r->samples;
   want.callback = recorder_callback;
   want.userdata = r;

   sdl->device = SDL_OpenAudioDevice(NULL, 1, &want, &sdl->spec, 0);
   sdl->fragment = 0;
   r->extra = sdl;

   SDL_PauseAudioDevice(sdl->device, 0);

   return 0;
}

static void sdl_deallocate_recorder(ALLEGRO_AUDIO_RECORDER *r)
{
   SDL_RECORDER *sdl = (SDL_RECORDER *) r->extra;
   SDL_CloseAudioDevice(sdl->device);
   al_free(r->extra);
}

void _device_list_dtor(void* value, void* userdata)
{
   ALLEGRO_AUDIO_DEVICE* device = (ALLEGRO_AUDIO_DEVICE*)value;
   al_free(device->name);
   al_free(device->identifier);
}

_AL_LIST* sdl_get_devices()
{
   if (!device_list) {
      device_list = _al_list_create();

      int i, count = SDL_GetNumAudioDevices(0);
      for (i = 0; i < count; ++i) {
         int len = strlen(SDL_GetAudioDeviceName(i, 0)) + 1;

         ALLEGRO_AUDIO_DEVICE* device = (ALLEGRO_AUDIO_DEVICE*)al_malloc(sizeof(ALLEGRO_AUDIO_DEVICE));
         device->identifier = (void*)al_malloc(sizeof(int));
         device->name = (char*)al_malloc(len);

         memset(device->identifier, 0, sizeof(int));
         memset(device->name, 0, len);

         memcpy(device->identifier, &i, sizeof(int));
         strcpy(device->name, SDL_GetAudioDeviceName(i, 0));

         _al_list_push_back_ex(device_list, device, _device_list_dtor);
      }
   }

   return device_list;
}

ALLEGRO_AUDIO_DRIVER _al_kcm_sdl_driver =
{
   "SDL",
   
   sdl_open,
   sdl_close,

   sdl_allocate_voice,
   sdl_deallocate_voice,

   sdl_load_voice,
   sdl_unload_voice,

   sdl_start_voice,
   sdl_stop_voice,

   sdl_voice_is_playing,

   sdl_get_voice_position,
   sdl_set_voice_position,

   sdl_allocate_recorder,
   sdl_deallocate_recorder,

   sdl_get_devices,
};
