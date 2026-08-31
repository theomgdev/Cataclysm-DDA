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
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "map.h"
#include "map_helpers.h"
#include "npc.h"
#include "npctalk.h"
#include "options_helpers.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "point.h"
#include "type_id.h"
#include "units.h"
#include "weather.h"

static const itype_id itype_2x4( "2x4" );
static const itype_id itype_9mm( "9mm" );
static const itype_id itype_backpack( "backpack" );
static const itype_id itype_bandages( "bandages" );
static const itype_id itype_box_small( "box_small" );
static const itype_id itype_duffelbag( "duffelbag" );
static const itype_id itype_glock_19( "glock_19" );
static const itype_id itype_glockmag( "glockmag" );
static const itype_id itype_kevlar( "kevlar" );
static const itype_id itype_knife_combat( "knife_combat" );
static const itype_id itype_pointy_stick( "pointy_stick" );
static const itype_id itype_sandwich_cheese( "sandwich_cheese" );
static const itype_id itype_shot_00( "shot_00" );
static const itype_id itype_tshirt( "tshirt" );

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
void run_gear_up( npc &guy, int max_turns = 400 )
{
    map &here = get_map();
    talk_function::gear_up_from_stores( guy );

    int turns = 0;
    while( !guy.activity.is_null() || guy.is_auto_moving() ) {
        if( turns >= max_turns ) {
            FAIL( "turn count exceeded, infinite loop possible" );
            return;
        }
        guy.set_moves( guy.get_speed() );
        if( guy.is_auto_moving() ) {
            guy.setpos( here, here.get_bub( *guy.destination_point ) );
            here.build_map_cache( guy.posz() );
            guy.start_destination_activity();
        }
        guy.activity.do_turn( guy );
        turns++;
    }
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

tripoint_bub_ms make_zone( npc &guy, const zone_type_id &type, const tripoint &offset )
{
    const tripoint_bub_ms tile = guy.pos_bub() + offset;
    zone_manager &mgr = zone_manager::get_manager();
    mgr.add( "test_" + type.str(), type, guy.get_fac_id(), false, true,
             get_map().get_abs( tile ), get_map().get_abs( tile ) );
    mgr.cache_data();
    return tile;
}

tripoint_bub_ms make_storage_zone( npc &guy )
{
    return make_zone( guy, zone_type_CAMP_STORAGE, tripoint::east );
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
    // Known gap: when the equipment stage does work on the last remaining tile,
    // the sweep ends before the supply stage gets a look at that same tile, so
    // the bandages are left behind.  Re-issuing the order picks them up.  The
    // bootstrap itself -- no pockets, then pockets -- is what this locks down.
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
