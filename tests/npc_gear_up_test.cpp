#include <string>
#include <vector>

#include "calendar.h"
#include "cata_catch.h"
#include "character.h"
#include "character_attire.h"
#include "clzones.h"
#include "coordinates.h"
#include "faction.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "map.h"
#include "map_helpers.h"
#include "npc.h"
#include "npctalk.h"
#include "options_helpers.h"
#include "player_helpers.h"
#include "point.h"
#include "type_id.h"
#include "units.h"
#include "weather.h"

static const itype_id itype_2x4( "2x4" );
static const itype_id itype_backpack( "backpack" );
static const itype_id itype_9mm( "9mm" );
static const itype_id itype_glock_19( "glock_19" );
static const itype_id itype_glockmag( "glockmag" );
static const itype_id itype_bandages( "bandages" );
static const itype_id itype_kevlar( "kevlar" );
static const itype_id itype_knife_combat( "knife_combat" );
static const itype_id itype_pointy_stick( "pointy_stick" );
static const itype_id itype_shot_00( "shot_00" );
static const itype_id itype_sandwich_cheese( "sandwich_cheese" );
static const itype_id itype_tshirt( "tshirt" );

static const zone_type_id zone_type_CAMP_STORAGE( "CAMP_STORAGE" );
static const zone_type_id zone_type_NO_NPC_PICKUP( "NO_NPC_PICKUP" );

namespace
{

// A follower standing next to a camp storage zone, with the world quiet enough
// that the order is not refused for danger.
npc &spawn_gear_up_npc( const point_bub_ms &pos )
{
    npc &guy = spawn_npc( pos, "test_talker" );
    clear_character( guy, true );
    guy.set_all_parts_temp_conv( BODYTEMP_NORM );
    guy.set_all_parts_temp_cur( BODYTEMP_NORM );
    guy.set_hunger( 0 );
    guy.set_thirst( 0 );
    guy.set_stored_kcal( guy.get_healthy_kcal() );
    // clear_character strips the NPC bare, and someone with no pockets at all
    // cannot pick anything up.  Give them somewhere to put things.
    guy.worn.wear_item( guy, item( itype_backpack ), false, false );
    talk_function::follow( guy );
    return guy;
}

// The same, but left with no pockets at all -- the state clear_character leaves
// an NPC in, and the one that proves gear up can bootstrap its own storage.
npc &spawn_pocketless_npc( const point_bub_ms &pos )
{
    npc &guy = spawn_npc( pos, "test_talker" );
    clear_character( guy, true );
    guy.set_all_parts_temp_conv( BODYTEMP_NORM );
    guy.set_all_parts_temp_cur( BODYTEMP_NORM );
    guy.set_hunger( 0 );
    guy.set_thirst( 0 );
    guy.set_stored_kcal( guy.get_healthy_kcal() );
    talk_function::follow( guy );
    return guy;
}

// Storage tile sits directly next to the NPC so it is inside the reach radius.
tripoint_bub_ms make_storage_zone( npc &guy, const zone_type_id &type )
{
    const tripoint_bub_ms tile = guy.pos_bub() + tripoint::east;
    zone_manager &mgr = zone_manager::get_manager();
    mgr.add( "test_gear_up_zone", type, guy.get_fac_id(), false, true,
             get_map().get_abs( tile ), get_map().get_abs( tile ) );
    mgr.cache_data();
    return tile;
}

int count_of( const npc &guy, const itype_id &id )
{
    int found = 0;
    guy.visit_items( [&found, &id]( const item * node, item * ) {
        if( node->typeId() == id ) {
            found += node->count();
        }
        return VisitResponse::NEXT;
    } );
    return found;
}

bool wearing( const npc &guy, const itype_id &id )
{
    return guy.amount_worn( id ) > 0;
}

} // namespace

TEST_CASE( "npc_gear_up_needs_a_storage_zone", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    map &here = get_map();
    const tripoint_bub_ms tile = guy.pos_bub() + tripoint::east;
    here.add_item_or_charges( tile, item( itype_knife_combat ) );

    // No zone at all: the order changes nothing rather than helping itself to
    // whatever happens to be lying around.
    talk_function::gear_up_from_stores( guy );

    CHECK( here.i_at( tile ).size() == 1 );
    CHECK_FALSE( guy.is_wielding( item( itype_knife_combat ) ) );
}

TEST_CASE( "npc_gear_up_takes_a_better_weapon", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    item stick( itype_pointy_stick );
    REQUIRE( guy.wield( stick ) );
    const double before = guy.evaluate_weapon( *guy.get_wielded_item() );

    here.add_item_or_charges( tile, item( itype_knife_combat ) );
    REQUIRE( guy.evaluate_weapon( item( itype_knife_combat ) ) > before );

    talk_function::gear_up_from_stores( guy );

    CHECK( guy.get_wielded_item() );
    CHECK( guy.get_wielded_item()->typeId() == itype_knife_combat );
    // The displaced weapon is kept, never dropped on the floor.
    CHECK( ( count_of( guy, itype_pointy_stick ) == 1 ||
             here.i_at( tile ).size() > 0 ) );
}

TEST_CASE( "npc_gear_up_respects_explicit_player_intent", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here( get_map() );

    SECTION( "a favourited item in the stores is left alone" ) {
        item knife( itype_knife_combat );
        knife.set_favorite( true );
        here.add_item_or_charges( tile, knife );

        talk_function::gear_up_from_stores( guy );

        CHECK( here.i_at( tile ).size() == 1 );
        CHECK( count_of( guy, itype_knife_combat ) == 0 );
    }

    SECTION( "deliberately given cargo is not dumped just for being heavy" ) {
        // Well under the carrying limit: the NPC has no reason to shed it, and
        // the leader may have handed it over for a reason it cannot see.
        item planks( itype_2x4 );
        guy.i_add( planks );
        REQUIRE( count_of( guy, itype_2x4 ) >= 1 );

        talk_function::gear_up_from_stores( guy );

        CHECK( count_of( guy, itype_2x4 ) >= 1 );
    }
}

TEST_CASE( "npc_gear_up_never_touches_a_no_pickup_zone", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms storage = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    // A second tile, inside reach, marked hands-off.
    const tripoint_bub_ms forbidden = guy.pos_bub() + tripoint::west;
    zone_manager &mgr = zone_manager::get_manager();
    mgr.add( "test_no_pickup", zone_type_NO_NPC_PICKUP, guy.get_fac_id(), false, true,
             here.get_abs( forbidden ), here.get_abs( forbidden ) );
    mgr.cache_data();

    here.add_item_or_charges( storage, item( itype_bandages ) );
    here.add_item_or_charges( forbidden, item( itype_knife_combat ) );

    talk_function::gear_up_from_stores( guy );

    CHECK( here.i_at( forbidden ).size() == 1 );
    CHECK( count_of( guy, itype_knife_combat ) == 0 );
}

TEST_CASE( "npc_gear_up_matches_ammunition_to_the_weapon", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    // A melee fighter has no business stuffing shotgun shells in their pockets.
    item knife( itype_knife_combat );
    REQUIRE( guy.wield( knife ) );

    item shells( itype_shot_00 );
    shells.charges = 20;
    here.add_item_or_charges( tile, shells );

    talk_function::gear_up_from_stores( guy );

    CHECK( count_of( guy, itype_shot_00 ) == 0 );
}

TEST_CASE( "npc_gear_up_stocks_bandages", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    item bandages( itype_bandages );
    bandages.charges = 10;
    here.add_item_or_charges( tile, bandages );
    REQUIRE( count_of( guy, itype_bandages ) == 0 );

    talk_function::gear_up_from_stores( guy );

    CHECK( count_of( guy, itype_bandages ) > 0 );
    // It takes a working supply, not the whole camp's stock.
    CHECK( count_of( guy, itype_bandages ) < 10 );
}

TEST_CASE( "npc_gear_up_wears_better_protection", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    guy.worn.wear_item( guy, item( itype_tshirt ), false, false );
    REQUIRE( wearing( guy, itype_tshirt ) );

    here.add_item_or_charges( tile, item( itype_kevlar ) );

    talk_function::gear_up_from_stores( guy );

    CHECK( wearing( guy, itype_kevlar ) );
}

TEST_CASE( "npc_gear_up_skips_rations_with_no_npc_food", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();
    override_option no_food( "NO_NPC_FOOD", "true" );

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    REQUIRE_FALSE( guy.needs_food() );

    here.add_item_or_charges( tile, item( itype_sandwich_cheese ) );

    talk_function::gear_up_from_stores( guy );

    CHECK( count_of( guy, itype_sandwich_cheese ) == 0 );
}

TEST_CASE( "npc_gear_up_finds_its_own_storage_first", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    // Someone with no pockets cannot pick anything up, so the order is worth
    // nothing to them unless it puts a bag on their back before reaching for
    // the ammunition.  This case has caught a real bug once already.
    npc &guy = spawn_pocketless_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    REQUIRE( guy.volume_capacity() == 0_ml );

    here.add_item_or_charges( tile, item( itype_backpack ) );
    item bandages( itype_bandages );
    bandages.charges = 10;
    here.add_item_or_charges( tile, bandages );

    talk_function::gear_up_from_stores( guy );

    CHECK( guy.volume_capacity() > 0_ml );
    CHECK( count_of( guy, itype_bandages ) > 0 );
}

TEST_CASE( "npc_gear_up_keeps_a_backup_blade", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    // A pistol and nothing else.  Guns run dry; something to swing is not
    // optional, and an experienced player never leaves camp without one.
    item pistol( itype_glock_19 );
    REQUIRE( guy.wield( pistol ) );
    REQUIRE( guy.get_wielded_item()->is_gun() );

    here.add_item_or_charges( tile, item( itype_knife_combat ) );

    talk_function::gear_up_from_stores( guy );

    CHECK( count_of( guy, itype_knife_combat ) == 1 );
}

TEST_CASE( "npc_gear_up_will_not_pile_on_layers_while_overheating", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    // The finest plate in the county is worthless to someone about to collapse
    // from heatstroke wearing it.
    guy.set_all_parts_temp_conv( BODYTEMP_VERY_HOT );
    guy.set_all_parts_temp_cur( BODYTEMP_VERY_HOT );

    here.add_item_or_charges( tile, item( itype_kevlar ) );

    talk_function::gear_up_from_stores( guy );

    CHECK_FALSE( wearing( guy, itype_kevlar ) );
}

TEST_CASE( "npc_gear_up_loads_the_gun_it_chose", "[npc][gear_up]" )
{
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy, zone_type_CAMP_STORAGE );
    map &here = get_map();

    item pistol( itype_glock_19 );
    REQUIRE( guy.wield( pistol ) );
    REQUIRE( guy.get_wielded_item()->ammo_remaining() == 0 );

    here.add_item_or_charges( tile, item( itype_glockmag ) );
    here.add_item_or_charges( tile, item( itype_glockmag ) );
    item rounds( itype_9mm );
    rounds.charges = 100;
    here.add_item_or_charges( tile, rounds );

    talk_function::gear_up_from_stores( guy );

    // Ammunition matching the weapon it is actually holding, a magazine to feed
    // it with, and the weapon loaded rather than merely accompanied by bullets.
    CHECK( count_of( guy, itype_glockmag ) > 0 );
    CHECK( count_of( guy, itype_9mm ) > 0 );
    CHECK( guy.get_wielded_item()->ammo_remaining() > 0 );
}
