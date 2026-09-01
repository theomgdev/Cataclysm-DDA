#include <cstdio>
#include <string>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "calendar.h"
#include "cata_catch.h"
#include "character.h"
#include "character_attire.h"
#include "clzones.h"
#include "coordinates.h"
#include "faction.h"
#include "game.h"
#include "item.h"
#include "item_category.h"
#include "item_factory.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "map.h"
#include "map_helpers.h"
#include "material.h"
#include "npc.h"
#include "npc_gear_up.h"
#include "npctalk.h"
#include "options_helpers.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "point.h"
#include "rng.h"
#include "string_formatter.h"
#include "type_id.h"
#include "units.h"
#include "weather.h"

static const faction_id faction_free_merchants( "free_merchants" );

static const item_category_id item_category_weapons( "weapons" );

static const itype_id itype_2x4( "2x4" );
static const itype_id itype_9mm( "9mm" );
static const itype_id itype_backpack( "backpack" );
static const itype_id itype_bandages( "bandages" );
static const itype_id itype_box_small( "box_small" );
static const itype_id itype_canteen( "canteen" );
static const itype_id itype_duffelbag( "duffelbag" );
static const itype_id itype_glock_19( "glock_19" );
static const itype_id itype_glockmag( "glockmag" );
static const itype_id itype_kevlar( "kevlar" );
static const itype_id itype_knife_combat( "knife_combat" );
static const itype_id itype_machete( "machete" );
static const itype_id itype_mutagen( "mutagen" );
static const itype_id itype_pointy_stick( "pointy_stick" );
static const itype_id itype_sandwich_cheese( "sandwich_cheese" );
static const itype_id itype_wrench( "wrench" );
static const itype_id itype_shot_00( "shot_00" );
static const itype_id itype_towel( "towel" );
static const itype_id itype_tshirt( "tshirt" );
static const itype_id itype_umbrella( "umbrella" );
static const itype_id itype_water_clean( "water_clean" );

static const zone_type_id zone_type_CAMP_STORAGE( "CAMP_STORAGE" );
static const zone_type_id zone_type_LOOT_ARMOR( "LOOT_ARMOR" );
static const zone_type_id zone_type_LOOT_DRUGS( "LOOT_DRUGS" );
static const zone_type_id zone_type_NO_NPC_PICKUP( "NO_NPC_PICKUP" );

namespace
{

// Gear up is a job, not an instant effect: the character walks their camp's
// zones and works through them.  Every test drives it the way the game does,
// which means every test also doubles as a termination test -- exceeding the
// turn budget fails, because "walks back to the same crate forever" is the
// failure mode this whole feature has to avoid, and the engine's own loop
// detector cannot catch it since every lap of that circle spends moves.
void drive_gear_up( Character &who, int max_turns )
{
    map &here = get_map();
    int turns = 0;
    while( !who.activity.is_null() || who.is_auto_moving() ) {
        if( turns >= max_turns ) {
            FAIL( "turn count exceeded, infinite loop possible" );
            return;
        }
        who.set_moves( who.get_speed() );
        if( who.is_auto_moving() ) {
            who.setpos( here, here.get_bub( *who.destination_point ) );
            here.build_map_cache( who.posz() );
            who.start_destination_activity();
        }
        who.activity.do_turn( who );
        turns++;
    }
}

void run_gear_up( npc &guy, int max_turns = 400 )
{
    talk_function::gear_up_from_stores( guy );
    drive_gear_up( guy, max_turns );
}

// The same order the avatar gives itself from the zone-activities menu.
void run_gear_up( avatar &you, int max_turns = 400 )
{
    REQUIRE( gear_up_stores_available( you ) );
    start_gear_up_from_stores( you );
    drive_gear_up( you, max_turns );
}

npc &spawn_bare_npc( const point_bub_ms &pos )
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

// clear_character strips them bare, and someone with no pockets at all cannot
// pick anything up.  Most cases want somewhere to put things; the ones that
// deliberately do not use spawn_bare_npc.
npc &spawn_gear_up_npc( const point_bub_ms &pos )
{
    npc &guy = spawn_bare_npc( pos );
    guy.worn.wear_item( guy, item( itype_backpack ), false, false );
    return guy;
}

avatar &prepare_avatar( const point_bub_ms &pos )
{
    avatar &you = get_avatar();
    map &here = get_map();
    clear_character( you, true );
    you.set_all_parts_temp_conv( BODYTEMP_NORM );
    you.set_all_parts_temp_cur( BODYTEMP_NORM );
    you.set_hunger( 0 );
    you.set_thirst( 0 );
    you.set_stored_kcal( you.get_healthy_kcal() );
    you.setpos( here, tripoint_bub_ms( pos.x(), pos.y(), 0 ) );
    here.build_map_cache( you.posz() );
    you.worn.wear_item( you, item( itype_backpack ), false, false );
    return you;
}

tripoint_bub_ms make_zone( Character &who, const zone_type_id &type, const tripoint &offset )
{
    const tripoint_bub_ms tile = who.pos_bub() + offset;
    zone_manager &mgr = zone_manager::get_manager();
    mgr.add( "test_" + type.str(), type, who.get_faction_id(), false, true,
             get_map().get_abs( tile ), get_map().get_abs( tile ) );
    mgr.cache_data();
    return tile;
}

tripoint_bub_ms make_storage_zone( Character &who )
{
    return make_zone( who, zone_type_CAMP_STORAGE, tripoint::east );
}

int count_of( const Character &guy, const itype_id &id )
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

bool wearing( const Character &guy, const itype_id &id )
{
    return guy.amount_worn( id ) > 0;
}

int items_on( const tripoint_bub_ms &tile )
{
    return get_map().i_at( tile ).size();
}

void reset_world()
{
    // Mid-morning.  Rummaging through crates needs light, and the activity
    // prunes dark work locations like every other one does, so a test world
    // stuck at midnight has nothing to offer anybody.
    calendar::turn = calendar::turn_zero + 9_hours + 30_minutes;
    clear_map_without_vision();
    clear_avatar();
    zone_manager::get_manager().clear();
    // Every test resets calendar::turn to the same constant and the avatar
    // cases all reuse the same singleton character, so the order's per-turn
    // scan caches need an explicit kick or a later test can be answered by a
    // scan a completely different earlier test cached.
    reset_gear_up_caches();
}

} // namespace

TEST_CASE( "npc_gear_up_needs_a_zone_to_draw_from", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    map &here = get_map();
    const tripoint_bub_ms tile = guy.pos_bub() + tripoint::east;
    here.add_item_or_charges( tile, item( itype_knife_combat ) );

    // No zone at all: the order changes nothing rather than helping itself to
    // whatever happens to be lying around.
    run_gear_up( guy );

    CHECK( items_on( tile ) == 1 );
    CHECK( count_of( guy, itype_knife_combat ) == 0 );
}

TEST_CASE( "npc_gear_up_terminates_when_there_is_nothing_better", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // A crate of things the character tries on once and turns down.  Without
    // remembering rejections by item type this paces forever.
    for( int i = 0; i < 20; i++ ) {
        here.add_item_or_charges( tile, item( itype_tshirt ) );
    }

    run_gear_up( guy );

    SUCCEED( "gear up finished instead of pacing between crates" );
}

TEST_CASE( "npc_gear_up_reads_inside_containers", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_zone( guy, zone_type_LOOT_DRUGS, tripoint::east );
    map &here = get_map();

    // A sorted camp keeps its bandages inside a box, not loose on the floor.
    // Anything that only reads the top of the pile calls a full store room empty.
    item box( itype_box_small );
    item bandages( itype_bandages );
    bandages.charges = 10;
    REQUIRE( box.put_in( bandages, pocket_type::CONTAINER ).success() );
    here.add_item_or_charges( tile, box );
    REQUIRE( count_of( guy, itype_bandages ) == 0 );

    run_gear_up( guy );

    CHECK( count_of( guy, itype_bandages ) > 0 );
    // A working supply, not the camp's whole stock.
    CHECK( count_of( guy, itype_bandages ) < 10 );
}

TEST_CASE( "npc_gear_up_uses_specific_loot_zones", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    // A camp that has been sorted keeps armour in LOOT_ARMOR.  Looking only at
    // CAMP_STORAGE would report that this camp has nothing to offer.
    const tripoint_bub_ms tile = make_zone( guy, zone_type_LOOT_ARMOR, tripoint::east );
    map &here = get_map();

    guy.worn.wear_item( guy, item( itype_tshirt ), false, false );
    here.add_item_or_charges( tile, item( itype_kevlar ) );

    run_gear_up( guy );

    CHECK( wearing( guy, itype_kevlar ) );
}

TEST_CASE( "npc_gear_up_walks_to_the_stores", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    map &here = get_map();

    // Well beyond arm's reach: the character has to go there.
    const tripoint_bub_ms tile = guy.pos_bub() + tripoint( 12, 0, 0 );
    zone_manager &mgr = zone_manager::get_manager();
    mgr.add( "test_far_storage", zone_type_CAMP_STORAGE, guy.get_fac_id(), false, true,
             here.get_abs( tile ), here.get_abs( tile ) );
    mgr.cache_data();

    guy.worn.wear_item( guy, item( itype_tshirt ), false, false );
    here.add_item_or_charges( tile, item( itype_kevlar ) );

    run_gear_up( guy );

    CHECK( wearing( guy, itype_kevlar ) );
}

TEST_CASE( "npc_gear_up_empties_a_pack_before_wearing_it", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_bare_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // Putting on a bag and inheriting somebody else's laundry is not gearing up.
    item pack( itype_backpack );
    for( int i = 0; i < 6; i++ ) {
        pack.put_in( item( itype_tshirt ), pocket_type::CONTAINER );
    }
    REQUIRE( !pack.all_items_top( pocket_type::CONTAINER ).empty() );
    here.add_item_or_charges( tile, pack );

    run_gear_up( guy );

    CHECK( wearing( guy, itype_backpack ) );
    // Wearing a shirt or two is fine -- they started with nothing on.  What
    // must not happen is the whole load coming along inside the bag.
    int stowed = 0;
    guy.visit_items( [&guy, &stowed]( const item * node, item * ) {
        if( node->typeId() == itype_tshirt && !guy.is_worn( *node ) ) {
            stowed++;
        }
        return VisitResponse::NEXT;
    } );
    CHECK( stowed == 0 );
    // The laundry stays in the camp's zones: not carried, not destroyed.
    CHECK( items_on( tile ) > 0 );
}

TEST_CASE( "npc_gear_up_leaves_a_pack_holding_a_favourite_on_the_shelf", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_bare_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // Wearing a bag empties it into the camp's zones first, so taking this one
    // would move the player's favourite out of it.  A favourite is not to be
    // taken and not to be shuffled around either, so the bag stays put.
    item pack( itype_backpack );
    item marked( itype_knife_combat );
    marked.set_favorite( true );
    REQUIRE( pack.put_in( marked, pocket_type::CONTAINER ).success() );
    here.add_item_or_charges( tile, pack );

    run_gear_up( guy );

    CHECK_FALSE( wearing( guy, itype_backpack ) );
    CHECK( count_of( guy, itype_knife_combat ) == 0 );
    CHECK( items_on( tile ) == 1 );
}

TEST_CASE( "npc_gear_up_moves_contents_into_the_replacement", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_bare_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // A worn pack with something in it, and a bigger empty one in the stores.
    // Wearing the bigger one over the top and leaving the old one full is the
    // bug this exists to prevent.
    guy.worn.wear_item( guy, item( itype_backpack ), false, false );
    guy.i_add( item( itype_2x4 ) );
    REQUIRE( count_of( guy, itype_2x4 ) == 1 );

    here.add_item_or_charges( tile, item( itype_duffelbag ) );

    run_gear_up( guy );

    // Whatever they ended up wearing, what was in the old pack came with them.
    CHECK( count_of( guy, itype_2x4 ) == 1 );
}

TEST_CASE( "npc_gear_up_respects_explicit_player_intent", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    SECTION( "a favourited item in the stores is left alone" ) {
        item knife( itype_knife_combat );
        knife.set_favorite( true );
        here.add_item_or_charges( tile, knife );

        run_gear_up( guy );

        CHECK( items_on( tile ) == 1 );
        CHECK( count_of( guy, itype_knife_combat ) == 0 );
    }

    SECTION( "a favourite inside a container is left alone too" ) {
        item box( itype_box_small );
        item bandages( itype_bandages );
        bandages.charges = 10;
        bandages.set_favorite( true );
        REQUIRE( box.put_in( bandages, pocket_type::CONTAINER ).success() );
        here.add_item_or_charges( tile, box );

        run_gear_up( guy );

        CHECK( count_of( guy, itype_bandages ) == 0 );
    }
}

TEST_CASE( "npc_gear_up_leaves_someone_elses_property_alone", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // Wielding asks before taking what belongs to someone else.  A sweep
    // through a whole camp cannot stop and ask about every crate, so it leaves
    // other people's property where it is instead.
    item knife( itype_knife_combat );
    knife.set_owner( faction_free_merchants );
    REQUIRE_FALSE( knife.is_owned_by( guy, true ) );
    here.add_item_or_charges( tile, knife );

    run_gear_up( guy );

    CHECK_FALSE( guy.get_wielded_item() );
    CHECK( items_on( tile ) == 1 );
}

TEST_CASE( "npc_gear_up_never_touches_a_no_pickup_zone", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    map &here = get_map();
    const tripoint_bub_ms storage = make_storage_zone( guy );

    // A second tile, also inside a storage zone, but marked hands-off.
    const tripoint_bub_ms forbidden = guy.pos_bub() + tripoint::west;
    zone_manager &mgr = zone_manager::get_manager();
    mgr.add( "test_storage_west", zone_type_CAMP_STORAGE, guy.get_fac_id(), false, true,
             here.get_abs( forbidden ), here.get_abs( forbidden ) );
    mgr.add( "test_no_pickup", zone_type_NO_NPC_PICKUP, guy.get_fac_id(), false, true,
             here.get_abs( forbidden ), here.get_abs( forbidden ) );
    mgr.cache_data();

    here.add_item_or_charges( storage, item( itype_bandages ) );
    here.add_item_or_charges( forbidden, item( itype_kevlar ) );

    run_gear_up( guy );

    CHECK( items_on( forbidden ) == 1 );
    CHECK_FALSE( wearing( guy, itype_kevlar ) );
}

TEST_CASE( "npc_gear_up_takes_a_better_weapon", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    item stick( itype_pointy_stick );
    REQUIRE( guy.wield( stick ) );
    const double before = guy.evaluate_weapon( *guy.get_wielded_item() );

    here.add_item_or_charges( tile, item( itype_knife_combat ) );
    REQUIRE( guy.evaluate_weapon( item( itype_knife_combat ) ) > before );

    run_gear_up( guy );

    REQUIRE( guy.get_wielded_item() );
    CHECK( guy.get_wielded_item()->typeId() == itype_knife_combat );
    // The displaced weapon is kept, never dropped where it happened to stand.
    CHECK( ( count_of( guy, itype_pointy_stick ) == 1 || items_on( tile ) > 0 ) );
}

TEST_CASE( "npc_gear_up_keeps_a_backup_blade", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // A pistol and nothing else.  Guns run dry; something to swing is not
    // optional, and an experienced player never leaves camp without one.
    item pistol( itype_glock_19 );
    REQUIRE( guy.wield( pistol ) );
    REQUIRE( guy.get_wielded_item()->is_gun() );

    here.add_item_or_charges( tile, item( itype_knife_combat ) );

    run_gear_up( guy );

    CHECK( count_of( guy, itype_knife_combat ) == 1 );
}

TEST_CASE( "npc_gear_up_picks_the_best_backup_blade_in_reach", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    map &here = get_map();

    // A pistol in hand settles the weapon stage immediately, so only backup
    // selection is under test.
    item pistol( itype_glock_19 );
    REQUIRE( guy.wield( pistol ) );
    REQUIRE( guy.get_wielded_item()->is_gun() );

    // A poor blade sits on the near tile, a genuinely good one two tiles
    // further out.  Wanting whichever the walk happens to reach first, rather
    // than the best one anywhere in reach, is the bug this guards: it once
    // took the umbrella and left the machete on the shelf.
    const tripoint_bub_ms near_tile = make_zone( guy, zone_type_CAMP_STORAGE, tripoint::east );
    const tripoint_bub_ms far_tile = make_zone( guy, zone_type_CAMP_STORAGE, tripoint( 6, 0, 0 ) );
    REQUIRE( guy.evaluate_weapon( item( itype_machete ), false ) >
             guy.evaluate_weapon( item( itype_umbrella ), false ) );
    here.add_item_or_charges( near_tile, item( itype_umbrella ) );
    here.add_item_or_charges( far_tile, item( itype_machete ) );

    run_gear_up( guy );

    CHECK( count_of( guy, itype_machete ) > 0 );
    CHECK( count_of( guy, itype_umbrella ) == 0 );
}

TEST_CASE( "npc_gear_up_leaves_no_unworn_clothing_debris_from_repeated_swaps",
           "[npc][gear_up]" )
{
    reset_world();

    // A bare follower and several identical, individually-adequate shirts on
    // one shelf.  Comparing each only against whatever the last one happened
    // to be, the way a per-tile-local comparison once did, would wear the
    // first, then displace it for the second, then the third -- and every
    // displaced piece lands in the follower's own pack rather than the shelf,
    // because put_away() tries the follower's own pockets first. Only the
    // single best one anywhere in reach should ever be worn at all; the rest
    // should still be sitting untouched where they were put down, not
    // carried, not worn twice over.
    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    for( int i = 0; i < 4; i++ ) {
        here.add_item_or_charges( tile, item( itype_tshirt ) );
    }

    run_gear_up( guy );

    CHECK( guy.amount_worn( itype_tshirt ) == 1 );
    CHECK( count_of( guy, itype_tshirt ) == 1 );
    CHECK( items_on( tile ) == 3 );
}

TEST_CASE( "npc_gear_up_never_doubles_up_on_a_garment_with_no_sub_body_part_data",
           "[npc][gear_up]" )
{
    reset_world();

    // A towel carries only whole-bodypart armor data, no sub-bodypart entries
    // at all -- get_covered_sub_body_parts() comes back empty for it.  The
    // conflict check has to fall back to whole-bodypart granularity, or a
    // second one never reads as redundant and this paces the shelf forever.
    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    for( int i = 0; i < 3; i++ ) {
        here.add_item_or_charges( tile, item( itype_towel ) );
    }

    run_gear_up( guy );

    CHECK( guy.amount_worn( itype_towel ) <= 1 );
}

TEST_CASE( "npc_gear_up_matches_ammunition_to_the_weapon", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // A melee fighter has no business stuffing shotgun shells in their pockets.
    item knife( itype_knife_combat );
    REQUIRE( guy.wield( knife ) );

    item shells( itype_shot_00 );
    shells.charges = 20;
    here.add_item_or_charges( tile, shells );

    run_gear_up( guy );

    CHECK( count_of( guy, itype_shot_00 ) == 0 );
}

TEST_CASE( "npc_gear_up_loads_the_gun_it_chose", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    item pistol( itype_glock_19 );
    REQUIRE( guy.wield( pistol ) );
    REQUIRE( guy.get_wielded_item()->ammo_remaining() == 0 );

    here.add_item_or_charges( tile, item( itype_glockmag ) );
    here.add_item_or_charges( tile, item( itype_glockmag ) );
    item rounds( itype_9mm );
    rounds.charges = 100;
    here.add_item_or_charges( tile, rounds );

    run_gear_up( guy );

    // Ammunition matching the weapon it is actually holding, a magazine to feed
    // it with, and the weapon loaded rather than merely accompanied by bullets.
    CHECK( count_of( guy, itype_glockmag ) > 0 );
    CHECK( count_of( guy, itype_9mm ) > 0 );
    CHECK( guy.get_wielded_item()->ammo_remaining() > 0 );
}

TEST_CASE( "npc_gear_up_finds_its_own_storage_first", "[npc][gear_up]" )
{
    reset_world();

    // Someone with no pockets cannot pick anything up, so the order is worth
    // nothing to them unless it puts a bag on their back before reaching for
    // the bandages.  This case has caught a real bug once already.
    npc &guy = spawn_bare_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    REQUIRE( guy.volume_capacity() == 0_ml );

    here.add_item_or_charges( tile, item( itype_backpack ) );
    item bandages( itype_bandages );
    bandages.charges = 10;
    here.add_item_or_charges( tile, bandages );

    run_gear_up( guy );

    CHECK( guy.volume_capacity() > 0_ml );
    // And having found pockets on the last tile of the equipment sweep, the
    // supply stage still gets its look at that same tile: the handover happens
    // when the equipment stage runs dry, not only when a later tile is left to
    // walk to.  One order, not two.
    CHECK( count_of( guy, itype_bandages ) > 0 );
}

TEST_CASE( "npc_gear_up_ends_when_it_cannot_carry_what_it_wants", "[npc][gear_up]" )
{
    reset_world();

    // Naked, armed, no pockets at all: wanted ammunition that can never be
    // stashed must end the order, not send the character back to the crate
    // every turn forever.
    npc &guy = spawn_bare_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    item pistol( itype_glock_19 );
    REQUIRE( guy.wield( pistol ) );
    REQUIRE( guy.volume_capacity() == 0_ml );

    item rounds( itype_9mm );
    rounds.charges = 100;
    here.add_item_or_charges( tile, rounds );

    run_gear_up( guy );

    CHECK( count_of( guy, itype_9mm ) == 0 );
    CHECK( items_on( tile ) == 1 );
}

TEST_CASE( "npc_gear_up_will_not_pile_on_layers_while_overheating", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // The finest plate in the county is worthless to someone about to collapse
    // from heatstroke wearing it.
    guy.set_all_parts_temp_conv( BODYTEMP_VERY_HOT );
    guy.set_all_parts_temp_cur( BODYTEMP_VERY_HOT );

    here.add_item_or_charges( tile, item( itype_kevlar ) );

    run_gear_up( guy );

    CHECK_FALSE( wearing( guy, itype_kevlar ) );
}

TEST_CASE( "npc_gear_up_skips_rations_with_no_npc_food", "[npc][gear_up]" )
{
    reset_world();
    override_option no_food( "NO_NPC_FOOD", "true" );

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    REQUIRE_FALSE( guy.needs_food() );

    here.add_item_or_charges( tile, item( itype_sandwich_cheese ) );

    run_gear_up( guy );

    CHECK( count_of( guy, itype_sandwich_cheese ) == 0 );
}

// Two cases checked directly rather than left to a random draw to happen to
// include -- a random sample not finding a bug is not the same as the bug
// not existing, and these are exactly the two the feature was accused of
// getting wrong from a whole-body score with nothing to catch them.

TEST_CASE( "npc_gear_up_never_wields_a_camp_tool", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // A wrench has real bash stats -- is_melee() reads true for it -- but it
    // is the camp's own construction tool, not a weapon, and taking it locks
    // a later building job out of the wrench it needs.
    here.add_item_or_charges( tile, item( itype_wrench ) );

    run_gear_up( guy );

    CHECK_FALSE( guy.get_wielded_item() );
    CHECK( count_of( guy, itype_wrench ) == 0 );
}

TEST_CASE( "npc_gear_up_never_carries_a_loose_liquid", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // A mutagen sitting loose on a shelf, not in a bottle.  The off-limits
    // filter has to reject it by phase, not by item type, or a liquid found
    // outside its usual container slips through.
    here.add_item_or_charges( tile, item( itype_mutagen ) );

    run_gear_up( guy );

    CHECK( count_of( guy, itype_mutagen ) == 0 );
}

TEST_CASE( "npc_gear_up_leaves_a_gun_it_has_no_ammunition_for", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // A knife in hand and a pistol on the shelf with not one round for it
    // anywhere in the camp.  Scored on what it could do if it were loaded the
    // pistol wins, but an empty pistol is a poor club and the knife was the
    // better weapon all along.
    item knife( itype_knife_combat );
    REQUIRE( guy.wield( knife ) );
    here.add_item_or_charges( tile, item( itype_glock_19 ) );

    run_gear_up( guy );

    REQUIRE( guy.get_wielded_item() );
    CHECK( guy.get_wielded_item()->typeId() == itype_knife_combat );
    CHECK( count_of( guy, itype_glock_19 ) == 0 );
}

TEST_CASE( "npc_gear_up_takes_a_gun_the_camp_can_feed", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // The same pistol, with the camp's ammunition behind it.  Now it is worth
    // more than the knife, and the knife stays as the backup blade.
    item knife( itype_knife_combat );
    REQUIRE( guy.wield( knife ) );
    here.add_item_or_charges( tile, item( itype_glock_19 ) );
    here.add_item_or_charges( tile, item( itype_glockmag ) );
    item rounds( itype_9mm );
    rounds.charges = 100;
    here.add_item_or_charges( tile, rounds );

    run_gear_up( guy );

    REQUIRE( guy.get_wielded_item() );
    CHECK( guy.get_wielded_item()->typeId() == itype_glock_19 );
    CHECK( guy.get_wielded_item()->ammo_remaining() > 0 );
    CHECK( count_of( guy, itype_knife_combat ) == 1 );
}

TEST_CASE( "npc_gear_up_takes_a_day_of_food_not_the_larder", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    REQUIRE( guy.needs_food() );

    // Forty sandwiches is the camp's week, not one fighter's day.
    for( int i = 0; i < 40; i++ ) {
        here.add_item_or_charges( tile, item( itype_sandwich_cheese ) );
    }

    run_gear_up( guy );

    const int packed = count_of( guy, itype_sandwich_cheese );
    CHECK( packed > 0 );
    CHECK( packed < 20 );
    CHECK( items_on( tile ) > 0 );
}

TEST_CASE( "npc_gear_up_takes_water_by_the_canteen", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    REQUIRE( guy.needs_food() );

    // Loose liquid cannot be carried, and a camp does not keep puddles: the
    // water has to come by the vessel holding it or it never comes at all.
    for( int i = 0; i < 4; i++ ) {
        item canteen( itype_canteen );
        item water( itype_water_clean );
        water.charges = 2;
        REQUIRE( canteen.put_in( water, pocket_type::CONTAINER ).success() );
        here.add_item_or_charges( tile, canteen );
    }

    run_gear_up( guy );

    CHECK( count_of( guy, itype_water_clean ) > 0 );
    // A canteen or two, not the camp's whole water supply.
    CHECK( count_of( guy, itype_canteen ) <= 2 );
    CHECK( items_on( tile ) > 0 );
}

TEST_CASE( "npc_gear_up_dresses_someone_who_is_freezing", "[npc][gear_up]" )
{
    reset_world();

    npc &guy = spawn_gear_up_npc( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( guy );
    map &here = get_map();

    // Being too cold is the strongest reason there is to put a coat on, not a
    // reason to refuse one.
    guy.set_all_parts_temp_conv( BODYTEMP_VERY_COLD );
    guy.set_all_parts_temp_cur( BODYTEMP_VERY_COLD );

    here.add_item_or_charges( tile, item( itype_kevlar ) );

    run_gear_up( guy );

    CHECK( wearing( guy, itype_kevlar ) );
}

TEST_CASE( "avatar_gear_up_equips_from_the_stores", "[npc][gear_up][avatar]" )
{
    reset_world();

    // The same order the player gives themselves from the zone-activities
    // menu.  Nothing about the sweep is NPC-only.
    avatar &you = prepare_avatar( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( you );
    map &here = get_map();

    here.add_item_or_charges( tile, item( itype_kevlar ) );
    here.add_item_or_charges( tile, item( itype_knife_combat ) );
    item bandages( itype_bandages );
    bandages.charges = 10;
    here.add_item_or_charges( tile, bandages );

    run_gear_up( you );

    CHECK( wearing( you, itype_kevlar ) );
    REQUIRE( you.get_wielded_item() );
    CHECK( you.get_wielded_item()->typeId() == itype_knife_combat );
    CHECK( count_of( you, itype_bandages ) > 0 );
}

TEST_CASE( "avatar_gear_up_uses_specific_loot_zones", "[npc][gear_up][avatar]" )
{
    reset_world();

    // Unlike an NPC given the order, the avatar gearing itself up is trusted
    // with however the rest of the camp has been sorted: a camp that has been
    // sorted keeps armour in LOOT_ARMOR, not CAMP_STORAGE.
    avatar &you = prepare_avatar( { 50, 50 } );
    const tripoint_bub_ms tile = make_zone( you, zone_type_LOOT_ARMOR, tripoint::east );
    map &here = get_map();

    you.worn.wear_item( you, item( itype_tshirt ), false, false );
    here.add_item_or_charges( tile, item( itype_kevlar ) );

    run_gear_up( you );

    CHECK( wearing( you, itype_kevlar ) );
}

TEST_CASE( "avatar_gear_up_leaves_favourites_alone", "[npc][gear_up][avatar]" )
{
    reset_world();

    avatar &you = prepare_avatar( { 50, 50 } );
    const tripoint_bub_ms tile = make_storage_zone( you );
    map &here = get_map();

    item knife( itype_knife_combat );
    knife.set_favorite( true );
    here.add_item_or_charges( tile, knife );

    run_gear_up( you );

    CHECK_FALSE( you.get_wielded_item() );
    CHECK( items_on( tile ) == 1 );
}

// ---------------------------------------------------------------------------
// Wide-pool diagnostic
//
// Not a correctness test in the usual sense -- a fast way to throw an unbiased
// crate at the gear-up logic and read back what it decided, without a full game
// launch.  A hand-picked list only ever probes the cases already thought of;
// the whole item database can hand it a mutagen, a corpse, a gun with no
// matching ammunition anywhere in the draw, three coats and no pants.  Hidden
// from the default run ("[.]") because its assertions are invariants meant to
// be read, not specific expected items.
//
// Deterministic on purpose.  A check that is green on one run and red on the
// next reports nothing anyone can reproduce, and it teaches a team to re-run
// until it passes.  Coverage comes from the stride walking the whole database
// instead of from a different sample every time.
//
// For wider coverage, raise wide_pool_trials or move draw_offset and rebuild;
// each value is a different slice and every one of them repeats exactly.  The
// original sampling is kept here for the same purpose -- swap it into
// wide_item_draw and pick the sample with `--rng-seed <n>`:
//
//     draw.push_back( pool[rng( 0, static_cast<int>( pool.size() ) - 1 )] );
//
// Whatever that turns up belongs back in this file as a named case with the
// offending item in it, not as a seed nobody will remember.
// ---------------------------------------------------------------------------

namespace
{

// Pinned because the activity rolls its own dice on top of the draw.
constexpr unsigned int diagnostic_rng_seed = 4242424242;
// Where the stride starts; see the note above.
constexpr int draw_offset = 0;

const std::vector<itype_id> &item_pool()
{
    static std::vector<itype_id> pool;
    if( pool.empty() ) {
        for( const itype *type : item_controller->all() ) {
            pool.push_back( type->get_id() );
        }
    }
    return pool;
}

// A stride across the pool rather than a clump out of one corner of it:
// neighbouring ids are near-identical variants, so a run of them would hand the
// sweep sixty flavours of the same thing.  Each trial starts one step further
// along, so the trials within a run do not repeat each other.
std::vector<itype_id> wide_item_draw( int count, int trial )
{
    const std::vector<itype_id> &pool = item_pool();
    const int size = static_cast<int>( pool.size() );
    REQUIRE( size > 0 );
    const int stride = std::max( 1, size / std::max( 1, count ) );
    std::vector<itype_id> draw;
    draw.reserve( count );
    for( int i = 0; i < count; i++ ) {
        draw.push_back( pool[( draw_offset + trial + i * stride ) % size] );
    }
    return draw;
}

std::string describe_item( const item &it )
{
    return string_format( "%s [%s]", it.tname( 1, false ), it.get_base_material().name() );
}

// A sorted camp keeps things in crates and duffel bags, not in a heap on the
// floor, so part of every draw goes inside a container.  Loose items alone
// would never reach the nested-candidate or empty-before-wearing paths.
void scatter_draw_over( const tripoint_bub_ms &tile, const std::vector<itype_id> &draw )
{
    map &here = get_map();
    item crate( itype_duffelbag );
    for( size_t i = 0; i < draw.size(); i++ ) {
        item made( draw[i] );
        if( i % 3 == 0 && crate.can_contain( made ).success() ) {
            crate.put_in( made, pocket_type::CONTAINER );
        } else {
            here.add_item_or_charges( tile, made, true );
        }
    }
    here.add_item_or_charges( tile, crate, true );
}

void report_gear_up_outcome( npc &guy, const tripoint_bub_ms &tile,
                              const std::vector<itype_id> &draw )
{
    printf( "\n=== gear-up random draw (%zu items) ===\n", draw.size() );

    item_location wielded = guy.get_wielded_item();
    printf( "weapon: %s\n", wielded ? describe_item( *wielded ).c_str() : "(none)" );
    // A camp tool wielded as a weapon is the bug; a camp tool *worn*, if it
    // happens to also carry an armor slot, is not -- a LENS helmet or a
    // headwrap can legitimately be both, so only the wielded slot counts.
    // Mirrors is_weapon_candidate()'s own carve-out: a blade kept as a tool
    // slot for its gunmod/butchering use (a combat knife, a machete) is still
    // a weapon if its own category says so.
    const bool tool_taken_as_weapon = wielded && wielded->is_tool() && !wielded->is_gun() &&
                                      wielded->get_category_shallow().get_id() != item_category_weapons;

    std::vector<std::string> worn_desc;
    std::vector<std::string> backup_desc;
    std::vector<std::string> ammo_desc;
    std::vector<std::string> med_desc;
    std::vector<std::string> food_desc;
    std::vector<std::string> loose_liquid_desc;
    int main_pack_count = 0;
    // A liter and a half is roughly a full pair of cargo pockets; anything
    // holding more than that is doing the job of a pack, not a pocket.
    constexpr units::volume main_pack_threshold = 1500_ml;

    guy.visit_items( [&]( const item * node, item * ) {
        if( node == wielded.get_item() ) {
            return VisitResponse::NEXT;
        }
        if( guy.is_worn( *node ) ) {
            worn_desc.push_back( describe_item( *node ) );
            if( node->get_volume_capacity() > main_pack_threshold ) {
                main_pack_count++;
            }
        } else if( node->is_gun() || ( node->is_melee() && !node->is_tool() ) ) {
            backup_desc.push_back( describe_item( *node ) );
        } else if( node->is_ammo() || node->is_magazine() ) {
            ammo_desc.push_back( describe_item( *node ) );
        } else if( node->is_medical_tool() || node->typeId().str().find( "pill" ) != std::string::npos ) {
            med_desc.push_back( describe_item( *node ) );
        } else if( node->is_food() ) {
            food_desc.push_back( describe_item( *node ) );
        }
        if( node->made_of( phase_id::LIQUID ) ) {
            loose_liquid_desc.push_back( describe_item( *node ) );
        }
        return VisitResponse::NEXT;
    } );

    auto print_list = [&]( const char * label, const std::vector<std::string> &items ) {
        printf( "%s (%zu): ", label, items.size() );
        for( const std::string &s : items ) {
            printf( "%s; ", s.c_str() );
        }
        printf( "\n" );
    };
    print_list( "worn", worn_desc );
    print_list( "carried weapon-like", backup_desc );
    print_list( "ammo/magazines", ammo_desc );
    print_list( "medical", med_desc );
    print_list( "food", food_desc );

    printf( "left on the storage tile: %d item(s)\n", items_on( tile ) );

    // Invariants, not specific expected items -- specific expectations rot
    // every time the item JSON changes upstream.
    CHECK_FALSE( tool_taken_as_weapon );
    if( tool_taken_as_weapon ) {
        printf( "!! tool taken as weapon: %s\n", describe_item( *wielded ).c_str() );
    }
    CHECK( loose_liquid_desc.empty() );
    if( !loose_liquid_desc.empty() ) {
        print_list( "!! loose liquid in inventory", loose_liquid_desc );
    }
    // Informational, not a hard invariant: a backpack plus a war belt plus a
    // chest rig is a realistic loadout, not redundant stacking.  What the
    // storage cap in wear_proxy() actually guards against is unbounded
    // accumulation (six bags for the storage alone); read this as a number
    // to sanity-check by eye, not a fixed pass/fail line.
    printf( "main-pack-sized items worn: %d\n", main_pack_count );

    // A container the sweep decided against keeps its contents; one it wore is
    // gone from the tile with its contents emptied into the camp's zones.  So
    // this is a reading, not a target: what it must never show is items the
    // character wanted still sealed away where the sweep could not reach them.
    map &here = get_map();
    int nested_leftover = 0;
    for( item &it : here.i_at( tile ) ) {
        nested_leftover += it.all_items_top( pocket_type::CONTAINER ).size();
    }
    printf( "items still nested inside containers left on the tile: %d\n", nested_leftover );
}

} // namespace

// Game data load dominates this binary's startup cost (order ten seconds) far
// more than any single draw costs to simulate, so several trials are looped
// inside one process launch: that pays the load cost once and still covers as
// much ground per second of wall-clock time as re-launching would.
constexpr int wide_pool_trials = 6;

TEST_CASE( "npc_gear_up_wide_item_pool", "[npc][gear_up][.]" )
{
    rng_set_engine_seed( diagnostic_rng_seed );
    for( int trial = 0; trial < wide_pool_trials; trial++ ) {
        reset_world();

        npc &guy = spawn_bare_npc( { 50, 50 } );
        const tripoint_bub_ms tile = make_storage_zone( guy );

        const std::vector<itype_id> draw = wide_item_draw( 60, trial );
        scatter_draw_over( tile, draw );

        run_gear_up( guy, 4000 );

        report_gear_up_outcome( guy, tile, draw );
    }
}

// A naked character only ever exercises the "empty slot" path.  Whether a
// better find actually displaces something already worn -- the more common
// case in a real, ongoing game -- needs a character with ordinary gear on
// already, or the test suite has the same "always start from nothing" bias
// the feature itself was accused of.
TEST_CASE( "npc_gear_up_wide_item_pool_over_starting_gear", "[npc][gear_up][.]" )
{
    rng_set_engine_seed( diagnostic_rng_seed );
    for( int trial = 0; trial < wide_pool_trials; trial++ ) {
        reset_world();

        npc &guy = spawn_gear_up_npc( { 50, 50 } );
        guy.worn.wear_item( guy, item( itype_tshirt ), false, false );
        item stick( itype_pointy_stick );
        REQUIRE( guy.wield( stick ) );
        const bool wore_tshirt_at_start = wearing( guy, itype_tshirt );

        const tripoint_bub_ms tile = make_storage_zone( guy );

        const std::vector<itype_id> draw = wide_item_draw( 60, trial );
        scatter_draw_over( tile, draw );

        run_gear_up( guy, 4000 );

        report_gear_up_outcome( guy, tile, draw );

        item_location wielded = guy.get_wielded_item();
        const bool weapon_changed = !wielded || wielded->typeId() != itype_pointy_stick;
        printf( "started dressed (backpack, tshirt, pointy stick) -- weapon changed: %s, "
                "starting tshirt still worn: %s\n",
                weapon_changed ? "yes" : "no",
                ( wore_tshirt_at_start && wearing( guy, itype_tshirt ) ) ? "yes" : "unknown/no" );
    }
}
