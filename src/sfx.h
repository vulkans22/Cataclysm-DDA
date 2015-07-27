#ifndef SFX_H
#define SFX_H

#include <string>
#include <cstring>

#include "json.h"
#include "enums.h"
#include "creature.h"
#include "audiere.h"
using namespace audiere;

struct sound_effect;
struct channel_data;

typedef std::string mat_type;
typedef std::string ter_type;
typedef std::string ignore_ter;

namespace sfx {
    channel_data& slot_at( int channel );
    void load_sound_effects( JsonObject &jsobj );
    void play_variant_sound( std::string id, std::string variant, float volume, bool loop = 0,
                             float angle = 0, float pitch_mix = 1.0, float pitch_max = 1.0, int delay = 0 );
    void play_variant_sound_channel( std::string id, std::string variant, int channel, float volume,
                                     float angle = 0 );
    void *play_variant_sound_thread( void * argument );
    void set_channel_volume( int channel, float volume );
    void generate_gun_soundfx( const tripoint source );
    void generate_melee_soundfx( const tripoint source, const tripoint target, bool hit,
                                 bool targ_mon = 0, std::string material = "flesh" );
    void do_hearing_loss_sfx( int turns );
    void remove_hearing_loss_sfx();
    void do_projectile_hit_sfx( const Creature *target = nullptr );
    void do_vehicle_engine_sfx();
    float get_heard_volume( const tripoint source, int loudness = 1 );
    void do_footstep_sfx();
    void do_danger_music();
    void do_ambient_sfx();
    void do_player_death_hurt_sfx( bool gender, bool death );
    void do_fatigue_sfx();
    float get_heard_angle( const tripoint source );
    void do_obstacle_sfx();
    int get_open_channel();
    void lock_channel( int id, bool lock );
    void stop_all_channels();
    void fade_out_all_channels();
    void play_channel( int id );
    void stop_channel( int id );
    bool channel_fadein( int id, int duration = 0 );
    void *channel_fadein_thread( void * argument );
    bool channel_fadeout( int id, int duration = 0 );
    void *channel_fadeout_thread( void * argument );
    bool channel_pitch_slide( int id, float pitch, float old_pitch );
    void *channel_pitch_slide_thread( void * out );
    void do_vehicle_stereo_sfx( tripoint pos3, int id );
    void do_door_alarm_sfx( tripoint pos3, int id );
    void do_vehicle_exterior_engine_sfx( tripoint pos3, int id );
    bool playing_weather();
    bool get_sleep_state();
}

#endif
