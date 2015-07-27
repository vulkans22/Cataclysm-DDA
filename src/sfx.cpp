#include "sfx.h"
#include "json.h"
#include "weather.h"
#include "path_info.h"
#include "debug.h"
#include "messages.h"
#include "rng.h"
#include "options.h"
#include "game.h"
#include "map.h"
#include "mapdata.h"
#include "npc.h"
#include "monster.h"
#include "vehicle.h"
#include "veh_type.h"
#include "audiere.h"
#if (defined _WIN32 || defined WINDOWS)
#   include "mingw.thread.h"
#endif

#include <string>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <pthread.h>
#include <semaphore.h>

using namespace audiere;

#define dbg(x) DebugLog((DebugLevel)(x),DC_ALL) << __FILE__ << ":" << __LINE__ << ": "

weather_type previous_weather;

float g_sfx_volume_multiplier = 1;
bool audio_muted = false;

int previous_speed = 0;
int previous_gear = 0;
int prev_hostiles = 0;
int danger_level = 0;
int prev_danger_level = 0;
int fatigue_level = 0;
int prev_fatigue_level = 0;
int deafness_turns = 0;
int current_deafness_turns = 0;

auto start_sfx_timestamp = std::chrono::high_resolution_clock::now();
auto start_danger_sfx_timestamp = std::chrono::high_resolution_clock::now();

bool ambient_sfx_init = false;
bool danger_sfx_init = false;
bool fatigue_sfx_init = false;
bool deafness_sfx_init = false;
bool engine_sfx_init = false;

bool playing_daytime = false;
bool playing_nighttime = false;
bool playing_underground = false;
bool playing_indoors = false;
bool playing_indoors_rain = false;
bool playing_drizzle = false;
bool playing_rain = false;
bool playing_thunder = false;
bool playing_flurries = false;
bool playing_snowstorm = false;
bool playing_deafness = false;

std::vector<sound_effect> sound_effects_vec;
std::vector<channel_data> audio_channels( 300 );

sem_t mutex1;

AudioDevicePtr device;

struct sound_thread {
    SampleSourcePtr sound;
    float sample_volume;
    bool loop;
    float angle;
    float pitch_min;
    float pitch_max;
    int delay;
};

struct fade_thread {
    int id;
    int duration;
};

struct slide_thread {
    int id;
    float pitch;
    float old_pitch;
};

struct sound_effect {
    std::vector<std::string> files;
    std::string id;
    std::vector<std::string> variants;
    int volume;
    SampleSourcePtr sound;
    sound_effect() {
        id = "";
        volume = 0;
    }
};

struct channel_data	{
    OutputStreamPtr sound;
    float volume;
    unsigned int locks;
    channel_data(): volume( 0 ), locks( 0 ) {}
    bool playing() const {
        return sound && sound->isPlaying();
    }
    bool is_fading = false;
    void stop() {
        if( sound ) {
            sound->stop();
        }
    }
    void play() {
        if( sound ) {
            sound->play();
        }
    }
};

void sfx::load_sound_effects( JsonObject &jsobj ) {
    int index = 0;
    std::string file;
    sound_effect new_sound_effect;
    if (!device) {
        device = OpenDevice();
    }
    new_sound_effect.id = jsobj.get_string( "id" );
    new_sound_effect.volume = jsobj.get_int( "volume" );
    JsonArray jsarr1 = jsobj.get_array( "variants" );
    while( jsarr1.has_more() ) {
        new_sound_effect.variants.push_back( jsarr1.next_string().c_str() );
    }
    JsonArray jsarr2 = jsobj.get_array( "files" );
    while( jsarr2.has_more() ) {
        new_sound_effect.files.push_back( jsarr2.next_string().c_str() );
        file = new_sound_effect.files[index];
        index++;
        std::string path = ( FILENAMES[ "datadir" ] + "sound/" + file );
        SampleSourcePtr sound_file( OpenSampleSource( path.c_str() ) );
        if( !sound_file ) {
            std::stringstream err;
            err << "Could not load audio file:" << path;
            dbg( D_ERROR ) << "Could not load audio file:" << path;
        }
        new_sound_effect.sound = sound_file;
        sound_effects_vec.push_back( new_sound_effect );
    }
}

channel_data &sfx::slot_at( int channel ) {
    if( channel == -1 ) {
        return slot_at( get_open_channel() );
    }
    return audio_channels.at( channel );
}

void sfx::play_variant_sound( std::string id, std::string variant, float volume, bool loop,
                              float angle, float pitch_min, float pitch_max, int delay ) {
    if( volume <= 0 || audio_muted ) {
        return;
    }
    std::vector<sound_effect> valid_sound_effects;
    sound_effect selected_sound_effect;
    for( auto &i : sound_effects_vec ) {
        if( i.id == id ) {
            if( std::find( i.variants.begin(), i.variants.end(), variant ) != i.variants.end() ) {
                valid_sound_effects.push_back( i );
            }
        }
    }
    float sample_volume = 0;
    if (!device) {
        device = OpenDevice();
    }
    SampleSourcePtr sound;
    if( valid_sound_effects.empty() ) {
        sound = OpenSampleSource( "data/sound/misc/silence.ogg" );
        if( !sound ) return;
    } else {
        int index = rng( 0, valid_sound_effects.size() - 1 );
        selected_sound_effect = valid_sound_effects[index];
        sound = selected_sound_effect.sound;
        if( !sound ) return;
        sample_volume = ( selected_sound_effect.volume * 0.01 ) * ( OPTIONS["SOUND_EFFECT_VOLUME"] *
                        0.01 ) * volume;
    }
    sound_thread * out = new sound_thread();
    out->sound = sound;
    out->angle = angle;
    out->loop = loop;
    out->pitch_max = pitch_max;
    out->pitch_min = pitch_min;
    out->sample_volume = sample_volume;
    out->delay = delay;
    pthread_t thread1;
    pthread_create( &thread1, NULL, play_variant_sound_thread, out );
}

void sfx::play_variant_sound_channel( std::string id, std::string variant, int channel,
                                      float volume, float angle ) {
    if (!device) {
        device = OpenDevice();
    }
    SampleSourcePtr sound;
    if( volume <= 0 || audio_muted ) {
        sound = OpenSampleSource( "data/sound/misc/silence.ogg" );
        if( !sound ) return;
        OutputStreamPtr effect_to_play = OpenSound( device, sound, true );
        if( !effect_to_play ) return;
        channel_data &s = slot_at( channel );
        s.sound = effect_to_play;
        return;
    }
    std::vector<sound_effect> valid_sound_effects;
    sound_effect selected_sound_effect;
    for( auto &i : sound_effects_vec ) {
        if( i.id == id ) {
            if( std::find( i.variants.begin(), i.variants.end(), variant ) != i.variants.end() ) {
                valid_sound_effects.push_back( i );
                break;
            }
        }
    }
    float sample_volume = 0;
    if( valid_sound_effects.empty() ) {
        sound = OpenSampleSource( "data/sound/misc/silence.ogg" );
        if( !sound ) return;
    } else {
        int index = rng( 0, valid_sound_effects.size() - 1 );
        selected_sound_effect = valid_sound_effects[index];
        sound = selected_sound_effect.sound;
        if( !sound ) return;
        sample_volume = ( selected_sound_effect.volume * 0.01 ) * ( OPTIONS["SOUND_EFFECT_VOLUME"] *
                        0.01 ) * volume;
    }
    OutputStreamPtr effect_to_play = OpenSound( device, sound, true );
    if( !effect_to_play ) return;
    effect_to_play->setVolume( sample_volume );
    effect_to_play->setPan( angle );
    effect_to_play->setRepeat( 1 );
    channel_data &s = slot_at( channel );
    s.sound = effect_to_play;
    s.volume = sample_volume;
}

void *sfx::play_variant_sound_thread( void * out ) {
    sound_thread *in = ( sound_thread* ) out;
    SampleSourcePtr sound = in->sound;
    float angle = in->angle;
    bool loop = in->loop;
    float pitch_max = in->pitch_max;
    float pitch_min = in->pitch_min;
    float sample_volume = in->sample_volume;
    int delay = in->delay;
    if (!device) {
        device = OpenDevice();
    }
    OutputStreamPtr effect_to_play = OpenSound( device, sound, true );
    if( !effect_to_play ) return 0;
    double pitch = rng_float( pitch_min, pitch_max );
    effect_to_play->setVolume( sample_volume );
    effect_to_play->setPan( angle );
    effect_to_play->setRepeat( loop );
    effect_to_play->setPitchShift( pitch );
    if (delay != 0 ) {
        Sleep( delay );
    }
    channel_data &s = slot_at( -1 );
    s.sound = effect_to_play;
    s.sound->play();
    while( effect_to_play->isPlaying() ) {
        Sleep( 1000 );
    }
    return 0;
}

void sfx::set_channel_volume( int channel, float volume ) {
    channel_data &s = slot_at( channel );
    if( !s.sound ) return;
    if( !s.is_fading ) {
        s.volume = volume;
    }
}

int sfx::get_open_channel() {
    for( int i = 30; i >= 30 && ( size_t )i < audio_channels.size(); ++i ) {
        const channel_data &s = audio_channels.at( i );
        if( s.locks == 0 && !s.playing() ) {
            add_msg("Got channel %i", i );
            return i;
        }
    }
    audio_channels.push_back( channel_data() );
    return audio_channels.size() - 1;
}

void sfx::lock_channel( int id, bool dolock ) {
    channel_data &s = slot_at( id );
    if( dolock ) {
        s.locks = 1;
    } else {
        s.locks = 0;
    }
}

void sfx::stop_channel( int id ) {
    channel_data &s = slot_at( id );
    if( !s.sound ) return;
    if( !s.is_fading ) {
        s.stop();
    }
}

void sfx::play_channel( int id ) {
    channel_data &s = slot_at( id );
    if( !s.sound ) return;
    if( !s.is_fading ) {
        s.play();
    }
}

void sfx::stop_all_channels() {
    for( int i = 0; i >= 0 && ( size_t )i < audio_channels.size(); ++i ) {
        if( !audio_channels.at( i ).is_fading ) {
            audio_channels.at( i ).stop();
        }
    }
}

void sfx::fade_out_all_channels() {
    for( int i = 0; i >= 0 && ( size_t )i < audio_channels.size(); ++i ) {
        if( !audio_channels.at( i ).is_fading ) {
            channel_fadeout( i, 100 );
        }
    }
}

bool sfx::channel_fadein( int id, int duration ) {
    channel_data &s = slot_at( id );
    if( !s.sound ) return false;
    if (!device) {
        device = OpenDevice();
    }
    if( s.sound->isPlaying() || s.is_fading ) {
        return false;
    }
    if( duration == 0 ) {
        channel_data &s = slot_at( id );
        s.play();
        return true;
    }
    s.is_fading = true;
    fade_thread * out = new fade_thread();
    out->id = id;
    out->duration = duration;
    pthread_t thread1;
    pthread_create( &thread1, NULL, channel_fadein_thread, out );
    return true;
}

void *sfx::channel_fadein_thread( void * out ) {
    fade_thread *in = ( fade_thread* ) out;
    int id = in->id;
    int duration = in->duration;
    channel_data &s = slot_at( id );
    if (!device) {
        device = OpenDevice();
    }
    s.sound->setVolume( 0 );
    s.play();
    auto initial_timestamp = std::chrono::high_resolution_clock::now();
    auto elapsed_timestamp = std::chrono::high_resolution_clock::now();
    auto elapsed_time = elapsed_timestamp - initial_timestamp;
    double elapsed_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>
                             ( elapsed_time ).count();
    while( s.sound->getVolume() < 1.0 ) {
        elapsed_timestamp = std::chrono::high_resolution_clock::now();
        elapsed_time = elapsed_timestamp - initial_timestamp;
        elapsed_time_ms = std::chrono::duration_cast<std::chrono::milliseconds> ( elapsed_time ).count();
        s.sound->setVolume( elapsed_time_ms / duration );
        if( elapsed_time_ms > duration + 100 ) {
            break;
        }
    }
    s.is_fading = false;
    return 0;
}

bool sfx::channel_fadeout( int id, int duration ) {
    channel_data &s = slot_at( id );
    if( !s.sound ) return false;
    if (!device) {
        device = OpenDevice();
    }
    if( !s.sound->isPlaying() || s.is_fading ) {
        return false;
    }
    if( duration == 0 ) {
        channel_data &s = slot_at( id );
        s.stop();
        return true;
    }
    s.is_fading = true;
    fade_thread * out = new fade_thread();
    out->id = id;
    out->duration = duration;
    pthread_t thread1;
    pthread_create( &thread1, NULL, channel_fadeout_thread, out );
    return true;
}

void *sfx::channel_fadeout_thread( void * out ) {
    fade_thread *in = ( fade_thread* ) out;
    int id = in->id;
    int duration = in->duration;
    channel_data &s = slot_at( id );
    if (!device) {
        device = OpenDevice();
    }
    auto initial_timestamp = std::chrono::high_resolution_clock::now();
    auto elapsed_timestamp = std::chrono::high_resolution_clock::now();
    auto elapsed_time = elapsed_timestamp - initial_timestamp;
    double elapsed_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>
                             ( elapsed_time ).count();
    while( s.sound->getVolume() > 0 ) {
        elapsed_timestamp = std::chrono::high_resolution_clock::now();
        elapsed_time = elapsed_timestamp - initial_timestamp;
        elapsed_time_ms = std::chrono::duration_cast<std::chrono::milliseconds> ( elapsed_time ).count();
        s.sound->setVolume( elapsed_time_ms * -1 / duration + 1 );
        if( elapsed_time_ms > duration + 100 ) {
            break;
        }
    }
    s.is_fading = false;
    s.stop();
    return 0;
}

bool sfx::channel_pitch_slide( int id, float pitch, float old_pitch ) {
    if (!device) {
        device = OpenDevice();
    }
    if( pitch == old_pitch ) {
        channel_data &s = slot_at( id );
        if( !s.sound ) return false;
        s.sound->setPitchShift( pitch );
        return true;
    }
    sem_init( &mutex1, 0, 1 );
    slide_thread * out = new slide_thread();
    out->id = id;
    out->pitch = pitch;
    out->old_pitch = old_pitch;
    pthread_t thread3;
    pthread_create( &thread3, NULL, channel_pitch_slide_thread, out );
    return true;
}

void *sfx::channel_pitch_slide_thread( void * out ) {
    if (!device) {
        device = OpenDevice();
    }
    slide_thread *in = ( slide_thread* ) out;
    int id = in->id;
    float pitch = in->pitch;
    float old_pitch = in->old_pitch;
    channel_data &s = slot_at( id );
    if( !s.sound ) return 0;
    float pitch_difference = pitch - old_pitch;
    double pitch_delta = pitch_difference / 100;
    sem_wait( &mutex1 );
    if( pitch_difference >= 0 ) {
        for( ; old_pitch <= pitch; old_pitch += pitch_delta ) {
            s.sound->setPitchShift( old_pitch );
            Sleep( 2 );
        }
        sem_post( &mutex1 );
        sem_destroy( &mutex1 );
        return 0;
    }
    if( pitch_difference <= 0 ) {
        for( ; old_pitch >= pitch; old_pitch += pitch_delta ) {
            s.sound->setPitchShift( old_pitch );
            Sleep( 2 );
        }
        sem_post( &mutex1 );
        sem_destroy( &mutex1 );
        return 0;
    }
    sem_post( &mutex1 );
    sem_destroy( &mutex1 );
    return 0;
}

bool sfx::playing_weather() {
    if( playing_drizzle || playing_rain || playing_thunder || playing_flurries || playing_snowstorm ) {
        return true;
    } else {
        return false;
    }
}

void sfx::do_ambient_sfx() {
    if( !ambient_sfx_init ) {
        play_variant_sound_channel( "environment", "daytime", 0, get_heard_volume( g->u.pos() ) );
        lock_channel( 0, 1 );
        play_variant_sound_channel( "environment", "nighttime", 1, get_heard_volume( g->u.pos() ) );
        lock_channel( 1, 1 );
        play_variant_sound_channel( "environment", "underground", 2, get_heard_volume( g->u.pos() ) );
        lock_channel( 2, 1 );
        play_variant_sound_channel( "environment", "indoors", 3, get_heard_volume( g->u.pos() ) );
        lock_channel( 3, 1 );
        play_variant_sound_channel( "environment", "indoors_rain", 4, get_heard_volume( g->u.pos() ) );
        lock_channel( 4, 1 );
        play_variant_sound_channel( "environment", "WEATHER_DRIZZLE", 5, get_heard_volume( g->u.pos() ) );
        lock_channel( 5, 1 );
        play_variant_sound_channel( "environment", "WEATHER_RAINY", 6, get_heard_volume( g->u.pos() ) );
        lock_channel( 6, 1 );
        play_variant_sound_channel( "environment", "WEATHER_THUNDER", 7, get_heard_volume( g->u.pos() ) );
        lock_channel( 7, 1 );
        play_variant_sound_channel( "environment", "WEATHER_FLURRIES", 8, get_heard_volume( g->u.pos() ) );
        lock_channel( 8, 1 );
        play_variant_sound_channel( "environment", "WEATHER_SNOW", 9, get_heard_volume( g->u.pos() ) );
        lock_channel( 9, 1 );
        ambient_sfx_init = true;
    }
    /**
    Channel assignments:
    0: Daytime
    1: Nighttime
    2: Underground
    3: Indoors
    4: Indoors - Rain
    5: Weather - Drizzle
    6: Weather - Rainy
    7: Weather - Thunder
    8: Weather - Flurries
    9: Weather - Snowstorm
    **/
    if( get_sleep_state() ) {
        return;
    }
    // Check weather at player position
    w_point weather_at_player = g->weatherGen.get_weather( g->u.global_square_location(),
                                calendar::turn );
    g->weather = g->weatherGen.get_weather_conditions( weather_at_player );
    // Step in at night time / we are not indoors
    channel_data &s0 = slot_at( 0 );
    playing_daytime = s0.sound->isPlaying();
    channel_data &s1 = slot_at( 1 );
    playing_nighttime = s1.sound->isPlaying();
    channel_data &s2 = slot_at( 2 );
    playing_underground = s2.sound->isPlaying();
    channel_data &s3 = slot_at( 3 );
    playing_indoors = s3.sound->isPlaying();
    channel_data &s4 = slot_at( 4 );
    playing_indoors_rain = s4.sound->isPlaying();
    channel_data &s5 = slot_at( 5 );
    playing_rain = s5.sound->isPlaying();
    channel_data &s6 = slot_at( 6 );
    playing_drizzle = s6.sound->isPlaying();
    channel_data &s7 = slot_at( 7 );
    playing_thunder = s7.sound->isPlaying();
    channel_data &s8 = slot_at( 8 );
    playing_flurries = s8.sound->isPlaying();
    channel_data &s9 = slot_at( 9 );
    playing_snowstorm = s9.sound->isPlaying();
    if( calendar::turn.is_night() && !g->is_sheltered( g->u.pos() ) &&
            !g->u.get_effect_int( "deaf" ) > 0 && !playing_nighttime ) {
        if( channel_fadeout( 0, 500 ) ) {
            playing_daytime = false;
        }
        if( channel_fadein( 1, 500 ) ) {
            playing_nighttime = true;
        }
        if( channel_fadeout( 2, 500 ) ) {
            playing_underground = false;
        }
        if( channel_fadeout( 3, 500 ) ) {
            playing_indoors = false;
        }
        if( channel_fadeout( 4, 500 ) ) {
            playing_indoors_rain = false;
        }
        // Step in at day time / we are not indoors
    } else if( !calendar::turn.is_night() && !g->is_sheltered( g->u.pos() )
               && !g->u.get_effect_int( "deaf" ) > 0  && !playing_daytime ) {
        if( channel_fadein( 0, 500 ) ) {
            playing_daytime = true;
        }
        if( channel_fadeout( 1, 500 ) ) {
            playing_nighttime = false;
        }
        if( channel_fadeout( 2, 500 ) ) {
            playing_underground = false;
        }
        if( channel_fadeout( 3, 500 ) ) {
            playing_indoors = false;
        }
        if( channel_fadeout( 4, 500 ) ) {
            playing_indoors_rain = false;
        }
    }
    // We are underground
    if( ( g->is_underground( g->u.pos() ) && !playing_underground &&
            !g->u.get_effect_int( "deaf" ) > 0 ) || ( g->is_underground( g->u.pos() ) &&
                    g->weather != previous_weather && !g->u.get_effect_int( "deaf" ) > 0 ) ) {
        if( channel_fadeout( 0, 500 ) ) {
            playing_daytime = false;
        }
        if( channel_fadeout( 1, 500 ) ) {
            playing_nighttime = false;
        }
        if( channel_fadein( 2, 500 ) ) {
            playing_underground = true;
        }
        if( channel_fadeout( 3, 500 ) ) {
            playing_indoors = false;
        }
        if( channel_fadeout( 4, 500 ) ) {
            playing_indoors_rain = false;
        }
        if( channel_fadeout( 5, 500 ) ) {
            playing_drizzle = false;
        }
        if( channel_fadeout( 6, 500 ) ) {
            playing_rain = false;
        }
        if( channel_fadeout( 7, 500 ) ) {
            playing_thunder = false;
        }
        if( channel_fadeout( 8, 500 ) ) {
            playing_flurries = false;
        }
        if( channel_fadeout( 9, 500 ) ) {
            playing_snowstorm = false;
        }
        // We are indoors
    } else if( ( g->is_sheltered( g->u.pos() ) && !g->is_underground( g->u.pos() )
                 && !g->u.get_effect_int( "deaf" ) > 0  && !playing_indoors ) ||
               ( g->is_sheltered( g->u.pos() ) && !g->is_underground( g->u.pos() ) &&
                 g->weather != previous_weather && !g->u.get_effect_int( "deaf" ) > 0 ) ) {
        if( channel_fadeout( 0, 500 ) ) {
            playing_daytime = false;
        }
        if( channel_fadeout( 1, 500 ) ) {
            playing_nighttime = false;
        }
        if( channel_fadeout( 2, 500 ) ) {
            playing_underground = false;
        }
        if( channel_fadein( 3, 500 ) ) {
            playing_indoors = true;
        }
        if( channel_fadeout( 4, 500 ) ) {
            playing_indoors_rain = false;
        }
        if( channel_fadeout( 5, 500 ) ) {
            playing_drizzle = false;
        }
        if( channel_fadeout( 6, 500 ) ) {
            playing_rain = false;
        }
        if( channel_fadeout( 7, 500 ) ) {
            playing_thunder = false;
        }
        if( channel_fadeout( 8, 500 ) ) {
            playing_flurries = false;
        }
        if( channel_fadeout( 9, 500 ) ) {
            playing_snowstorm = false;
        }
    }
    // We are indoors and it is also raining
    if( g->is_sheltered( g->u.pos() ) && g->weather >= WEATHER_DRIZZLE
            && g->weather <= WEATHER_ACID_RAIN && !g->is_underground( g->u.pos() ) && !playing_indoors_rain ) {
        if( channel_fadeout( 0, 500 ) ) {
            playing_daytime = false;
        }
        if( channel_fadeout( 1, 500 ) ) {
            playing_nighttime = false;
        }
        if( channel_fadeout( 2, 500 ) ) {
            playing_underground = false;
        }
        if( channel_fadein( 3, 500 ) ) {
            playing_indoors = true;
        }
        if( channel_fadein( 4, 500 ) ) {
            playing_indoors_rain = true;
        }
        if( channel_fadeout( 5, 500 ) ) {
            playing_drizzle = false;
        }
        if( channel_fadeout( 6, 500 ) ) {
            playing_rain = false;
        }
        if( channel_fadeout( 7, 500 ) ) {
            playing_thunder = false;
        }
        if( channel_fadeout( 8, 500 ) ) {
            playing_flurries = false;
        }
        if( channel_fadeout( 9, 500 ) ) {
            playing_snowstorm = false;
        }
    }
    if( !g->is_sheltered( g->u.pos() ) && g->weather <= WEATHER_CLOUDY
            && !g->u.get_effect_int( "deaf" ) > 0  && g->weather != previous_weather ) {
        // We are outside and there is no precipitation
        if( channel_fadeout( 2, 500 ) ) {
            playing_underground = false;
        }
        if( channel_fadeout( 3, 500 ) ) {
            playing_indoors = false;
        }
        if( channel_fadeout( 4, 500 ) ) {
            playing_indoors_rain = false;
        }
        if( channel_fadeout( 5, 500 ) ) {
            playing_drizzle = false;
        }
        if( channel_fadeout( 6, 500 ) ) {
            playing_rain = false;
        }
        if( channel_fadeout( 7, 500 ) ) {
            playing_thunder = false;
        }
        if( channel_fadeout( 8, 500 ) ) {
            playing_flurries = false;
        }
        if( channel_fadeout( 9, 500 ) ) {
            playing_snowstorm = false;
        }
    }
    if( ( !playing_weather() && !g->is_sheltered( g->u.pos() ) && g->weather >= WEATHER_DRIZZLE
            && !g->u.get_effect_int( "deaf" ) > 0 )
            || ( !g->is_sheltered( g->u.pos() ) &&
                 g->weather != previous_weather  && !g->u.get_effect_int( "deaf" ) > 0
                 && g->weather >= WEATHER_DRIZZLE ) ) {
        // We are outside and there is precipitation
        if( channel_fadeout( 2, 500 ) ) {
            playing_underground = false;
        }
        if( channel_fadeout( 3, 500 ) ) {
            playing_indoors = false;
        }
        if( channel_fadeout( 4, 500 ) ) {
            playing_indoors_rain = false;
        }
        if( channel_fadeout( 5, 500 ) ) {
            playing_drizzle = false;
        }
        if( channel_fadeout( 6, 500 ) ) {
            playing_rain = false;
        }
        if( channel_fadeout( 7, 500 ) ) {
            playing_thunder = false;
        }
        if( channel_fadeout( 8, 500 ) ) {
            playing_flurries = false;
        }
        if( channel_fadeout( 9, 500 ) ) {
            playing_snowstorm = false;
        }
        switch( g->weather ) {
        case WEATHER_DRIZZLE:
            if( channel_fadein( 5, 500 ) ) {
                playing_drizzle = true;
            }
            break;
        case WEATHER_RAINY:
            if( channel_fadein( 6, 500 ) ) {
                playing_rain = true;
            }
            break;
        case WEATHER_THUNDER:
        case WEATHER_LIGHTNING:
            if( channel_fadein( 7, 500 ) ) {
                playing_thunder = true;
            }
            break;
        case WEATHER_FLURRIES:
            if( channel_fadein( 8, 500 ) ) {
                playing_flurries = true;
            }
            break;
        case WEATHER_SNOWSTORM:
        case WEATHER_SNOW:
            if( channel_fadein( 9, 500 ) ) {
                playing_snowstorm = true;
            }
            break;
        }
    }
    // Keep track of weather to compare for next iteration
    previous_weather = g->weather;
}

bool sfx::get_sleep_state() {
    if( ( g->u.in_sleep_state() ||
            g->u.has_activity( ACT_WAIT_WEATHER ) ||
            g->u.has_activity( ACT_READ ) ||
            g->u.has_activity( ACT_CRAFT ) ||
            g->u.has_activity( ACT_LONGCRAFT ) ||
            g->u.has_activity( ACT_DISASSEMBLE ) ||
            g->u.has_activity( ACT_LONGSALVAGE ) ||
            g->u.has_activity( ACT_VEHICLE ) ||
            g->u.has_activity( ACT_BUILD ) ||
            g->u.has_activity( ACT_TRAIN ) ||
            g->u.has_activity( ACT_FIRSTAID ) ||
            g->u.has_activity( ACT_FISH ) ||
            g->u.has_activity( ACT_PICKAXE ) ||
            g->u.has_activity( ACT_BURROW ) ||
            g->u.has_activity( ACT_WAIT ) ) && !audio_muted ) {
        audio_muted = true;
        return true;
    } else if( ( g->u.in_sleep_state() ||
                 g->u.has_activity( ACT_WAIT_WEATHER ) ||
                 g->u.has_activity( ACT_READ ) ||
                 g->u.has_activity( ACT_CRAFT ) ||
                 g->u.has_activity( ACT_LONGCRAFT ) ||
                 g->u.has_activity( ACT_DISASSEMBLE ) ||
                 g->u.has_activity( ACT_LONGSALVAGE ) ||
                 g->u.has_activity( ACT_VEHICLE ) ||
                 g->u.has_activity( ACT_BUILD ) ||
                 g->u.has_activity( ACT_TRAIN ) ||
                 g->u.has_activity( ACT_FIRSTAID ) ||
                 g->u.has_activity( ACT_FISH ) ||
                 g->u.has_activity( ACT_PICKAXE ) ||
                 g->u.has_activity( ACT_BURROW ) ||
                 g->u.has_activity( ACT_WAIT ) ) && audio_muted ) {
        return true;
    }
    audio_muted = false;
    return false;
}

float sfx::get_heard_volume( const tripoint source, int loudness ) {
    int distance = rl_dist( g->u.pos3(), source );
    // fract_quiet = -100 / 10
    // fract_normal = -100 / 20
    // fract_loud = -100 / 40
    const float fract_quiet = -10;
    const float fract_normal = -5;
    const float fract_loud = -2.5;
    int heard_volume = 0;
    switch( loudness ) {
    case 0:
        heard_volume = fract_quiet * distance - 1 + 100;
        break;
    case 1:
        heard_volume = fract_normal * distance - 1 + 100;
        break;
    case 2:
        heard_volume = fract_loud * distance - 1 + 100;
        break;
    }
    if( heard_volume <= 0 ) {
        heard_volume = 0;
    }
    heard_volume *= g_sfx_volume_multiplier;
    float volume_as_float = heard_volume * 0.01;
    if( g->u.get_effect_int( "deaf" ) > 0 ) {
        volume_as_float = volume_as_float * 0.1;
    }
    return ( volume_as_float );
}

float sfx::get_heard_angle( const tripoint source ) {
    float angle = g->m.coord_to_angle( g->u.posx(), g->u.posy(), source.x, source.y ) + 90;
    if( angle >= 360 ) {
        angle = angle - 360;
    }
    if( angle >= 0 && angle <= 89 ) {
        angle = angle / 90;
        return ( angle );
    }
    if( angle >= 90 && angle <= 179 ) {
        angle = ( angle - 90 ) * -1 / 90 + 1;
        return ( angle );
    }
    if( angle >= 180 && angle <= 269 ) {
        angle = ( angle - 180 ) * -1 / 90;
        return ( angle );
    }
    if( angle >= 270 && angle <= 359 ) {
        angle = ( angle - 270 ) * 1 / 90 - 1;
        return ( angle );
    }
    return ( angle );
}

void sfx::generate_gun_soundfx( const tripoint source ) {
    if( get_sleep_state() ) {
        return;
    }
    auto end_sfx_timestamp = std::chrono::high_resolution_clock::now();
    auto sfx_time = end_sfx_timestamp - start_sfx_timestamp;
    if( std::chrono::duration_cast<std::chrono::milliseconds> ( sfx_time ).count() < 30 ) {
        return;
    }
    float heard_volume = get_heard_volume( source, 2 );
    if( heard_volume <= 0.3 ) {
        heard_volume = 0.3;
    }
    int angle = get_heard_angle( source );
    int distance = rl_dist( g->u.pos3(), source );
    if( source == g->u.pos3() ) {
        itype_id weapon_id = g->u.weapon.typeId();
        std::string weapon_type = g->u.weapon.gun_skill();
        std::string selected_sound = "fire_gun";
        if( g->u.weapon.has_gunmod( "suppressor" ) == 1
                || g->u.weapon.has_gunmod( "homemade suppressor" ) == 1 ) {
            selected_sound = "fire_gun";
            weapon_id = "weapon_fire_suppressed";
        }
        play_variant_sound( selected_sound, weapon_id, heard_volume, 0, 0, 0.8, 1.2 );
        start_sfx_timestamp = std::chrono::high_resolution_clock::now();
        return;
    }
    if( g->npc_at( source ) != -1 ) {
        npc *npc_source = g->active_npc[ g->npc_at( source ) ];
        if( distance <= 17 ) {
            itype_id weapon_id = npc_source->weapon.typeId();
            play_variant_sound( "fire_gun", weapon_id, heard_volume, 0, angle, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        } else {
            std::string weapon_type = npc_source->weapon.gun_skill();
            play_variant_sound( "fire_gun_distant", weapon_type, heard_volume, 0, angle, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        }
    }
    int mon_pos = g->mon_at( source );
    if( mon_pos != -1 ) {
        monster *monster = g->monster_at( source );
        std::string monster_id = monster->type->id;
        if( distance <= 18 ) {
            if( monster_id == "mon_turret" || monster_id == "mon_secubot" ) {
                play_variant_sound( "fire_gun", "hk_mp5", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            } else if( monster_id == "mon_turret_rifle" || monster_id == "mon_chickenbot"
                       || monster_id == "mon_tankbot" ) {
                play_variant_sound( "fire_gun", "m4a1", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            } else if( monster_id == "mon_turret_bmg" ) {
                play_variant_sound( "fire_gun", "m2browning", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            } else if( monster_id == "mon_laserturret" ) {
                play_variant_sound( "fire_gun", "laser_rifle", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            }
        } else {
            if( monster_id == "mon_turret" || monster_id == "mon_secubot" ) {
                play_variant_sound( "fire_gun_distant", "smg", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            } else if( monster_id == "mon_turret_rifle" || monster_id == "mon_chickenbot"
                       || monster_id == "mon_tankbot" ) {
                play_variant_sound( "fire_gun_distant", "rifle", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            } else if( monster_id == "mon_turret_bmg" ) {
                play_variant_sound( "fire_gun_distant", "rifle", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            } else if( monster_id == "mon_laserturret" ) {
                play_variant_sound( "fire_gun_distant", "laser", heard_volume, 0, angle, 0.8, 1.2 );
                start_sfx_timestamp = std::chrono::high_resolution_clock::now();
                return;
            }
        }
    }
}

void sfx::generate_melee_soundfx( tripoint source, tripoint target, bool hit, bool targ_mon,
                                  std::string material ) {
    if( get_sleep_state() ) {
        return;
    }
    std::string variant_used;
    int npc_index = g->npc_at( source );
    if( npc_index == -1 ) {
        std::string weapon_skill = g->u.weapon.weap_skill();
        int weapon_volume = g->u.weapon.volume();
        if( weapon_skill == "bashing" && weapon_volume <= 8 ) {
            variant_used = "small_bash";
            play_variant_sound( "melee_swing", "small_bash", get_heard_volume( source ), 0, 0, 0.8, 1.2 );
        } else if( weapon_skill == "bashing" && weapon_volume >= 9 ) {
            variant_used = "big_bash";
            play_variant_sound( "melee_swing", "big_bash", get_heard_volume( source ), 0, 0, 0.8, 1.2 );
        } else if( ( weapon_skill == "cutting" || weapon_skill == "stabbing" ) && weapon_volume <= 6 ) {
            variant_used = "small_cutting";
            play_variant_sound( "melee_swing", "small_cutting", get_heard_volume( source ), 0, 0, 0.8,
                                1.2 );
        } else if( ( weapon_skill == "cutting" || weapon_skill == "stabbing" ) && weapon_volume >= 7 ) {
            variant_used = "big_cutting";
            play_variant_sound( "melee_swing", "big_cutting", get_heard_volume( source ), 0, 0, 0.8, 1.2 );
        } else {
            variant_used = "default";
            play_variant_sound( "melee_swing", "default", get_heard_volume( source ), 0, 0, 0.8, 1.2 );
        }
        if( hit ) {
            if( targ_mon ) {
                if( material == "steel" ) {
                    play_variant_sound( "melee_hit_metal", variant_used, get_heard_volume( source ), 0,
                                        get_heard_angle( target ), 0.8, 1.2, weapon_volume * rng( 7, 11 ) );
                } else {
                    play_variant_sound( "melee_hit_flesh", variant_used, get_heard_volume( source ), 0,
                                        get_heard_angle( target ), 0.8, 1.2, weapon_volume * rng( 7, 11 ) );
                }
            } else {
                play_variant_sound( "melee_hit_flesh", variant_used, get_heard_volume( source ), 0,
                                    get_heard_angle( target ), 0.8, 1.2, weapon_volume * rng( 7, 11 ) );
            }
        }
    } else {
        npc *p = g->active_npc[npc_index];
        std::string weapon_skill = p->weapon.weap_skill();
        int weapon_volume = p->weapon.volume();
        if( weapon_skill == "bashing" && weapon_volume <= 8 ) {
            variant_used = "small_bash";
            play_variant_sound( "melee_swing", "small_bash", get_heard_volume( target ), 0,
                                get_heard_angle( source ), 0.8, 1.2 );
        } else if( weapon_skill == "bashing" && weapon_volume >= 9 ) {
            variant_used = "big_bash";
            play_variant_sound( "melee_swing", "big_bash", get_heard_volume( target ), 0,
                                get_heard_angle( source ), 0.8, 1.2 );
        } else if( ( weapon_skill == "cutting" || weapon_skill == "stabbing" ) && weapon_volume <= 6 ) {
            variant_used = "small_cutting";
            play_variant_sound( "melee_swing", "small_cutting", get_heard_volume( target ), 0,
                                get_heard_angle( source ), 0.8, 1.2 );
        } else if( ( weapon_skill == "cutting" || weapon_skill == "stabbing" ) && weapon_volume >= 7 ) {
            variant_used = "big_cutting";
            play_variant_sound( "melee_swing", "big_cutting", get_heard_volume( target ), 0,
                                get_heard_angle( source ), 0.8, 1.2 );
        } else {
            variant_used = "default";
            play_variant_sound( "melee_swing", "default", get_heard_volume( target ), 0,
                                get_heard_angle( source ), 0.8, 1.2 );
        }
        if( hit ) {
            if( targ_mon ) {
                if( material == "steel" ) {
                    play_variant_sound( "melee_hit_metal", variant_used, get_heard_volume( source ), 0,
                                        get_heard_angle( target ), 0.8, 1.2, weapon_volume * rng( 7, 11 ) );
                } else {
                    play_variant_sound( "melee_hit_flesh", variant_used, get_heard_volume( source ), 0,
                                        get_heard_angle( target ), 0.8, 1.2, weapon_volume * rng( 7, 11 ) );
                }
            } else {
                play_variant_sound( "melee_hit_flesh", variant_used, get_heard_volume( source ), 0,
                                    get_heard_angle( target ), 0.8, 1.2, weapon_volume * rng( 7, 11 ) );
            }
        }
    }
}

void sfx::do_projectile_hit_sfx( const Creature *target ) {
    if( get_sleep_state() ) {
        return;
    }
    std::string selected_sound;
    float heard_volume;
    if( !target->is_npc() && !target->is_player() ) {
        const monster *mon = dynamic_cast<const monster *>( target );
        heard_volume = get_heard_volume( target->pos3() );
        int angle = get_heard_angle( mon->pos3() );
        const auto material = mon->get_material();
        static std::set<mat_type> const fleshy = {
            mat_type( "flesh" ),
            mat_type( "hflesh" ),
            mat_type( "iflesh" ),
            mat_type( "veggy" ),
            mat_type( "bone" ),
            mat_type( "protoplasmic" ),
        };
        if( fleshy.count( material ) > 0 || mon->has_flag( MF_VERMIN ) ) {
            play_variant_sound( "bullet_hit", "hit_flesh", heard_volume, 0, angle, 0.8, 1.2 );
            return;
        } else if( mon->get_material() == "stone" ) {
            play_variant_sound( "bullet_hit", "hit_wall", heard_volume, 0, angle, 0.8, 1.2 );
            return;
        } else if( mon->get_material() == "steel" ) {
            play_variant_sound( "bullet_hit", "hit_metal", heard_volume, 0, angle, 0.8, 1.2 );
            return;
        } else {
            play_variant_sound( "bullet_hit", "hit_flesh", heard_volume, 0, angle, 0.8, 1.2 );
            return;
        }
    }
    heard_volume = sfx::get_heard_volume( target->pos() );
    int angle = get_heard_angle( target->pos3() );
    play_variant_sound( "bullet_hit", "hit_flesh", heard_volume, 0, angle, 0.8, 1.2 );
}

void sfx::do_vehicle_engine_sfx() {
    /** Channel Assignments:
        23: engine_speed_loop
    **/
    if( get_sleep_state() ) {
        return;
    }
    if( !engine_sfx_init ) {
        play_variant_sound_channel( "vehicle", "engine_speed_loop", 23,
                                    sfx::get_heard_volume( g->u.pos3() ) );
        lock_channel( 23, 1 );
        engine_sfx_init = true;
    }
    vehicle *veh = g->m.veh_at( g->u.pos() );
    if( !veh ) {
        stop_channel( 23 );
        return;
    }
    if( !veh->engine_on ) {
        stop_channel( 23 );
        return;
    }
    channel_data &s = slot_at( 23 );
    s.sound->setVolume( get_heard_volume( veh->global_pos3(), 1 ) );
    channel_fadein( 23, 300 );
    int current_speed = veh->velocity;
    bool in_reverse = false;
    if( current_speed <= -1 ) {
        current_speed = current_speed * -1;
        in_reverse = true;
    }
    float pitch = 0;
    int safe_speed = veh->safe_velocity();
    float quarter_safe_speed = safe_speed / 4;
    int current_gear;
    if( in_reverse == true ) {
        current_gear = 5;
    } else if( current_speed >= 0 && current_speed <= safe_speed / 4 ) {
        current_gear = 1;
    } else if( current_speed >= safe_speed / 4 && current_speed <= safe_speed / 2 ) {
        current_gear = 2;
    } else if( current_speed >= safe_speed / 2 && current_speed <= safe_speed / 1.3334 ) {
        current_gear = 3;
    } else {
        current_gear = 4;
    }
    if( current_gear != previous_gear ) {
        play_variant_sound( "vehicle", "gear_shift", get_heard_volume( g->u.pos3() ), 0, 0, 0.8, 1.2 );
    }
    if( ( safe_speed != 0 && current_speed != 0 ) || quarter_safe_speed + 0.5 != 0 ) {
        if( current_gear == 1 ) {
            pitch = ( current_speed * 1.4 ) / quarter_safe_speed + 0.5;
        } else if( current_gear == 2 ) {
            pitch = ( ( current_speed - safe_speed / 4 ) * 1.4 ) / quarter_safe_speed + 0.5;
        } else if( current_gear == 3 ) {
            pitch = ( ( current_speed - safe_speed / 2 ) * 1.4 ) / quarter_safe_speed + 0.5;
        } else if( current_gear == 4 ) {
            pitch = ( ( current_speed - safe_speed / 1.3334 ) * 1.4 ) / quarter_safe_speed + 0.5;
        } else if( current_gear == 5 ) {
            pitch = ( current_speed * 1.4 ) / quarter_safe_speed + 0.5;
        }
    }
    if( pitch <= 0.5 ) {
        pitch = 0.5;
    }
    if( pitch >= 1.9 ) {
        pitch = 1.9;
    }
    if( current_speed != previous_speed ) {
        float old_pitch = s.sound->getPitchShift();
        channel_pitch_slide( 23, pitch, old_pitch );
    }
    previous_speed = current_speed;
    previous_gear = current_gear;
}

void sfx::do_vehicle_exterior_engine_sfx( tripoint pos3, int id ) {
    if( get_sleep_state() ) {
        return;
    }
    if( id == -1 ) {
        return;
    }
    channel_data &s = slot_at( id );
    if( !s.playing() || s.locks == 0 ) {
        play_variant_sound_channel( "vehicle", "engine_idle_loop_1", id, 1.0 );
    }
    lock_channel( id, 1 );
    vehicle *veh = g->m.veh_at( pos3 );
    if( !veh ) {
        stop_channel( id );
        lock_channel( id, 0 );
        return;
    }
    if( g->u.in_vehicle ) {
        stop_channel( id );
        return;
    }
    int distance = rl_dist( g->u.pos3(), veh->global_pos3() );
    if( distance >= 10 || !veh->engine_on ) {
        stop_channel( id );
        lock_channel( id, 0 );
        veh->exterior_engine_sfx_channel = -1;
        return;
    }
    s.sound->setVolume( get_heard_volume( veh->global_pos3(), 1 ) );
    s.sound->setPan( get_heard_angle( veh->global_pos3() ) );
    play_channel( id );
    int current_speed = veh->velocity;
    if( current_speed <= -1 ) {
        current_speed = current_speed * -1;
    }
    float pitch;
    int safe_speed = veh->safe_velocity();
    if( ( safe_speed != 0 && current_speed != 0 ) || safe_speed / 2 + 1 != 0 ) {
        pitch = ( current_speed * 0.5 ) / ( safe_speed / 2 ) + 1.0;
    } else {
        pitch = 1.0;
    }
    if( pitch >= 1.5 ) {
        pitch = 1.5;
    }
    if( current_speed != previous_speed ) {
        s.sound->setPitchShift( pitch );
    }
    previous_speed = current_speed;
}

void sfx::do_door_alarm_sfx( tripoint pos3, int id ) {
    if( get_sleep_state() ) {
        return;
    }
    if( id == -1 ) {
        return;
    }
    channel_data &s = slot_at( id );
    if( !s.playing() ) {
        play_variant_sound_channel( "vehicle", "door_alarm", id, 1.0 );
    }
    lock_channel( id, 1 );
    vehicle *veh = g->m.veh_at( pos3 );
    if( !veh ) {
        stop_channel( id );
        lock_channel( id, 0 );
        return;
    }
    int distance = rl_dist( g->u.pos3(), veh->global_pos3() );
    if( distance >= 10 ) {
        stop_channel( id );
        lock_channel( id, 0 );
        veh->door_sfx_channel = -1;
        return;
    }
    s.sound->setVolume( get_heard_volume( veh->global_pos3(), 0 ) );
    s.sound->setPan( get_heard_angle( veh->global_pos3() ) );
    auto doors = veh->all_parts_with_feature( VPFLAG_OPENABLE, true );
    for( const int p : doors ) {
        if( veh->part_info( p ).name == "door" && veh->parts[p].open ) {
            play_channel( id );
            return;
        }
    }
    stop_channel( id );
    return;
}

void sfx::do_vehicle_stereo_sfx( tripoint pos3, int id ) {
    if( get_sleep_state() ) {
        return;
    }
    if( id == -1 ) {
        return;
    }
    channel_data &s = slot_at( id );
    if( !s.playing() ) {
        play_variant_sound_channel( "vehicle", "stereo", id, 1.0 );
    }
    lock_channel( id, 1 );
    vehicle *veh = g->m.veh_at( pos3 );
    if( !veh ) {
        stop_channel( id );
        lock_channel( id, 0 );
        return;
    }
    int distance = rl_dist( g->u.pos3(), veh->global_pos3() );
    if( distance >= 50 || !veh->stereo_on ) {
        stop_channel( id );
        lock_channel( id, 0 );
        veh->stereo_sfx_channel = -1;
        return;
    }
    s.sound->setVolume( get_heard_volume( veh->global_pos3(), 0 ) );
    s.sound->setPan( get_heard_angle( veh->global_pos3() ) );
    play_channel( id );
}

void sfx::do_player_death_hurt_sfx( bool gender, bool death ) {
    if( get_sleep_state() ) {
        return;
    }
    float heard_volume = get_heard_volume( g->u.pos() );
    if( !gender && !death ) {
        play_variant_sound( "deal_damage", "hurt_f", heard_volume );
    } else if( gender && !death ) {
        play_variant_sound( "deal_damage", "hurt_m", heard_volume );
    } else if( !gender && death ) {
        play_variant_sound( "clean_up_at_end", "death_f", heard_volume );
    } else if( gender && death ) {
        play_variant_sound( "clean_up_at_end", "death_m", heard_volume );
    }
}

void sfx::do_danger_music() {
    /** Channel Assignments:
        10: Danger_low
        11: Danger_medium
        12: Danger_high
        13: Danger_Extreme
    **/
    if( !danger_sfx_init ) {
        play_variant_sound_channel( "danger_low", "default", 10, get_heard_volume( g->u.pos3() ) );
        lock_channel( 10, 1 );
        play_variant_sound_channel( "danger_medium", "default", 11, get_heard_volume( g->u.pos3() ) );
        lock_channel( 11, 1 );
        play_variant_sound_channel( "danger_high", "default", 12, get_heard_volume( g->u.pos3() ) );
        lock_channel( 12, 1 );
        play_variant_sound_channel( "danger_extreme", "default", 13, get_heard_volume( g->u.pos3() ) );
        lock_channel( 13, 1 );
        danger_sfx_init = true;
    }
    if( get_sleep_state() ) {
        return;
    }
    auto end_danger_sfx_timestamp = std::chrono::high_resolution_clock::now();
    auto danger_sfx_time = end_danger_sfx_timestamp - start_danger_sfx_timestamp;
    if( std::chrono::duration_cast<std::chrono::milliseconds> ( danger_sfx_time ).count() < 3000 ) {
        return;
    }
    start_danger_sfx_timestamp = std::chrono::high_resolution_clock::now();
    int hostiles = 0;
    for( auto &critter : g->u.get_visible_creatures( 40 ) ) {
        if( g->u.attitude_to( *critter ) == Creature::A_HOSTILE ) {
            hostiles++;
        }
    }
    if( hostiles <= 4 ) {
        danger_level = 0;
    } else if( hostiles >= 5 && hostiles <= 9 ) {
        danger_level = 1;
    } else if( hostiles >= 10 && hostiles <= 14 ) {
        danger_level = 2;
    } else if( hostiles >= 15 && hostiles <= 19 ) {
        danger_level = 3;
    } else if( hostiles >= 20 ) {
        danger_level = 4;
    }
    if( danger_level == prev_danger_level ) {
        return;
    }
    if( g->u.in_vehicle ) {
        danger_level = 0;
        channel_fadeout( 10, 500 );
        channel_fadeout( 11, 500 );
        channel_fadeout( 12, 500 );
        channel_fadeout( 13, 500 );
        prev_hostiles = hostiles;
        prev_danger_level = danger_level;
        return;
    }
    if( danger_level == 0 ) {
        channel_fadeout( 10, 500 );
        channel_fadeout( 11, 500 );
        channel_fadeout( 12, 500 );
        channel_fadeout( 13, 500 );
        prev_hostiles = hostiles;
        prev_danger_level = danger_level;
        return;
    }
    if( danger_level == 1 ) {
        channel_fadein( 10, 500 );
        channel_fadeout( 11, 500 );
        channel_fadeout( 12, 500 );
        channel_fadeout( 13, 500 );
        prev_hostiles = hostiles;
        prev_danger_level = danger_level;
        return;
    } else if( danger_level == 2 ) {
        channel_fadeout( 10, 500 );
        channel_fadein( 11, 500 );
        channel_fadeout( 12, 500 );
        channel_fadeout( 13, 500 );
        prev_hostiles = hostiles;
        prev_danger_level = danger_level;
        return;
    } else if( danger_level == 3 ) {
        channel_fadeout( 10, 500 );
        channel_fadeout( 11, 500 );
        channel_fadein( 12, 500 );
        channel_fadeout( 13, 500 );
        prev_hostiles = hostiles;
        prev_danger_level = danger_level;
        return;
    } else if( danger_level == 4 ) {
        channel_fadeout( 10, 500 );
        channel_fadeout( 11, 500 );
        channel_fadeout( 12, 500 );
        channel_fadein( 13, 500 );
        prev_hostiles = hostiles;
        prev_danger_level = danger_level;
        return;
    }
}

void sfx::do_fatigue_sfx() {
    /** Channel Assignments:
        14: fatigue_m_low
        15: fatigue_m_med
        16: fatigue_m_high
        17: fatigue_f_low
        18: fatigue_f_med
        19: fatigue_f_high
    **/
    if( !fatigue_sfx_init ) {
        play_variant_sound_channel( "plmove", "fatigue_m_low", 14, get_heard_volume( g->u.pos3() ) );
        lock_channel( 14, 1 );
        play_variant_sound_channel( "plmove", "fatigue_m_med", 15, get_heard_volume( g->u.pos3() ) );
        lock_channel( 15, 1 );
        play_variant_sound_channel( "plmove", "fatigue_m_high", 16, get_heard_volume( g->u.pos3() ) );
        lock_channel( 16, 1 );
        play_variant_sound_channel( "plmove", "fatigue_f_low", 17, get_heard_volume( g->u.pos3() ) );
        lock_channel( 17, 1 );
        play_variant_sound_channel( "plmove", "fatigue_f_med", 18, get_heard_volume( g->u.pos3() ) );
        lock_channel( 18, 1 );
        play_variant_sound_channel( "plmove", "fatigue_f_high", 19, get_heard_volume( g->u.pos3() ) );
        lock_channel( 19, 1 );
        fatigue_sfx_init = true;
    }
    if( get_sleep_state() ) {
        return;
    }
    if( g->u.stamina >=  g->u.get_stamina_max() * .75 ) {
        stop_channel( 14 );
        stop_channel( 15 );
        stop_channel( 16 );
        stop_channel( 17 );
        stop_channel( 18 );
        stop_channel( 19 );
        return;
    }
    if( g->u.stamina <=  g->u.get_stamina_max() * .74
            && g->u.stamina >=  g->u.get_stamina_max() * .5 ) {
        fatigue_level = 1;
    } else if( g->u.stamina <=  g->u.get_stamina_max() * .49
               && g->u.stamina >=  g->u.get_stamina_max() * .25 ) {
        fatigue_level = 2;
    } else if( g->u.stamina <=  g->u.get_stamina_max() * .24 && g->u.stamina >=  0 ) {
        fatigue_level = 3;
    } else {
        fatigue_level = 0;
    }
    if( fatigue_level == prev_fatigue_level ) {
        return;
    }
    if( fatigue_level == 1 && g->u.male ) {
        play_channel( 14 );
        stop_channel( 15 );
        stop_channel( 16 );
        stop_channel( 17 );
        stop_channel( 18 );
        stop_channel( 19 );
        prev_fatigue_level = fatigue_level;
        return;
    } else if( fatigue_level == 2 && g->u.male ) {
        stop_channel( 14 );
        play_channel( 15 );
        stop_channel( 16 );
        stop_channel( 17 );
        stop_channel( 18 );
        stop_channel( 19 );
        prev_fatigue_level = fatigue_level;
        return;
    } else if( fatigue_level == 3 && g->u.male ) {
        stop_channel( 14 );
        stop_channel( 15 );
        play_channel( 16 );
        stop_channel( 17 );
        stop_channel( 18 );
        stop_channel( 19 );
        prev_fatigue_level = fatigue_level;
        return;
    } else if( fatigue_level == 1 && !g->u.male ) {
        stop_channel( 14 );
        stop_channel( 15 );
        stop_channel( 16 );
        play_channel( 17 );
        stop_channel( 18 );
        stop_channel( 19 );
        prev_fatigue_level = fatigue_level;
        return;
    } else if( fatigue_level == 2 && !g->u.male ) {
        stop_channel( 14 );
        stop_channel( 15 );
        stop_channel( 16 );
        stop_channel( 17 );
        play_channel( 18 );
        stop_channel( 19 );
        prev_fatigue_level = fatigue_level;
        return;
    } else if( fatigue_level == 3 && !g->u.male ) {
        stop_channel( 14 );
        stop_channel( 15 );
        stop_channel( 16 );
        stop_channel( 17 );
        stop_channel( 18 );
        play_channel( 19 );
        prev_fatigue_level = fatigue_level;
        return;
    }
}

void sfx::do_hearing_loss_sfx( int turns ) {
    /** Channel Assignments:
        20: deafness_tone_light
        21: deafness_tone_medium
        22: deafness_tone_heavy
    **/
    if( !deafness_sfx_init ) {
        play_variant_sound_channel( "environment", "deafness_tone_light", 20, 1 );
        lock_channel( 20, 1 );
        play_variant_sound_channel( "environment", "deafness_tone_medium", 21, 1 );
        lock_channel( 21, 1 );
        play_variant_sound_channel( "environment", "deafness_tone_heavy", 22, 1 );
        lock_channel( 22, 1 );
        deafness_sfx_init = true;
    }
    if( get_sleep_state() ) {
        return;
    }
    if( deafness_turns == 0 ) {
        playing_deafness = true;
        deafness_turns = turns;
        stop_all_channels();
        play_variant_sound( "environment", "deafness_shock", 1.0 );
        play_variant_sound( "environment", "deafness_tone_start", 0.8 );
        if( deafness_turns <= 90 ) {
            play_channel( 20 );
            stop_channel( 21 );
            stop_channel( 22 );
        } else if( deafness_turns <= 120 ) {
            stop_channel( 20 );
            play_channel( 21 );
            stop_channel( 22 );
        } else if( deafness_turns >= 121 ) {
            stop_channel( 20 );
            stop_channel( 21 );
            play_channel( 22 );
        }
    } else {
        deafness_turns += turns;
    }
}

void sfx::remove_hearing_loss_sfx() {
    if( get_sleep_state() ) {
        return;
    }
    if( !playing_deafness ) {
        return;
    }
    if( current_deafness_turns >= deafness_turns ) {
        stop_channel( 20 );
        stop_channel( 21 );
        stop_channel( 22 );
        playing_daytime = false;
        playing_nighttime = false;
        playing_underground = false;
        playing_indoors = false;
        playing_indoors_rain = false;
        playing_drizzle = false;
        playing_rain = false;
        playing_thunder = false;
        playing_flurries = false;
        playing_snowstorm = false;
        deafness_turns = 0;
        current_deafness_turns = 0;
        do_ambient_sfx();
        playing_deafness = false;
    }
    current_deafness_turns++;
}

void sfx::do_footstep_sfx() {
    if( get_sleep_state() ) {
        return;
    }
    auto end_sfx_timestamp = std::chrono::high_resolution_clock::now();
    auto sfx_time = end_sfx_timestamp - start_sfx_timestamp;
    if( std::chrono::duration_cast<std::chrono::milliseconds> ( sfx_time ).count() > 400 ) {
        float heard_volume = sfx::get_heard_volume( g->u.pos() );
        const auto terrain = g->m.ter_at( g->u.pos() ).id;
        static std::set<ter_type> const grass = {
            ter_type( "t_grass" ),
            ter_type( "t_shrub" ),
            ter_type( "t_underbrush" ),
        };
        static std::set<ter_type> const dirt = {
            ter_type( "t_dirt" ),
            ter_type( "t_sand" ),
            ter_type( "t_dirtfloor" ),
            ter_type( "t_palisade_gate_o" ),
            ter_type( "t_sandbox" ),
        };
        static std::set<ter_type> const metal = {
            ter_type( "t_ov_smreb_cage" ),
            ter_type( "t_metal_floor" ),
            ter_type( "t_grate" ),
            ter_type( "t_bridge" ),
            ter_type( "t_elevator" ),
            ter_type( "t_guardrail_bg_dp" ),
        };
        static std::set<ter_type> const water = {
            ter_type( "t_water_sh" ),
            ter_type( "t_water_dp" ),
            ter_type( "t_swater_sh" ),
            ter_type( "t_swater_dp" ),
            ter_type( "t_water_pool" ),
            ter_type( "t_sewage" ),
        };
        static std::set<ter_type> const chain_fence = {
            ter_type( "t_chainfence_h" ),
            ter_type( "t_chainfence_v" ),
        };
        if( !g->u.wearing_something_on( bp_foot_l ) ) {
            play_variant_sound( "plmove", "walk_barefoot", heard_volume, 0, 0, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        } else if( grass.count( terrain ) > 0 ) {
            play_variant_sound( "plmove", "walk_grass", heard_volume, 0, 0, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        } else if( dirt.count( terrain ) > 0 ) {
            play_variant_sound( "plmove", "walk_dirt", heard_volume, 0, 0, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        } else if( metal.count( terrain ) > 0 ) {
            play_variant_sound( "plmove", "walk_metal", heard_volume, 0, 0, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        } else if( water.count( terrain ) > 0 ) {
            play_variant_sound( "plmove", "walk_water", heard_volume, 0, 0, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        } else if( chain_fence.count( terrain ) > 0 ) {
            play_variant_sound( "plmove", "clear_obstacle", heard_volume, 0, 0, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        } else {
            play_variant_sound( "plmove", "walk_tarmac", heard_volume, 0, 0, 0.8, 1.2 );
            start_sfx_timestamp = std::chrono::high_resolution_clock::now();
            return;
        }
    }
}

void sfx::do_obstacle_sfx() {
    if( get_sleep_state() ) {
        return;
    }
    float heard_volume = sfx::get_heard_volume( g->u.pos() );
    const auto terrain = g->m.ter_at( g->u.pos() ).id;
    static std::set<ignore_ter> const water = {
        ignore_ter( "t_water_sh" ),
        ignore_ter( "t_water_dp" ),
        ignore_ter( "t_swater_sh" ),
        ignore_ter( "t_swater_dp" ),
        ignore_ter( "t_water_pool" ),
        ignore_ter( "t_sewage" ),
    };
    if( water.count( terrain ) > 0 ) {
        return;
    } else {
        play_variant_sound( "plmove", "clear_obstacle", heard_volume, 0, 0 );
    }
}
