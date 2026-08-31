#include "npctalk.h" // IWYU pragma: associated

#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "character_attire.h"
#include "clzones.h"
#include "coordinates.h"
#include "creature.h"
#include "damage.h"
#include "debug.h"
#include "enums.h"
#include "faction.h"
#include "game.h"
#include "game_constants.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "line.h"
#include "map.h"
#include "map_selector.h"
#include "messages.h"
#include "npc.h"
#include "output.h"
#include "ret_val.h"
#include "translations.h"
#include "units.h"
#include "value_ptr.h"
#include "vehicle.h"
#include "vehicle_selector.h"
#include "visitable.h"
#include "vpart_position.h"
#include "weather.h"
#include "weather_type.h"

/*
 * Manually triggered "gear up from the camp stores" order.
 *
 * This is deliberately a ONE-SHOT resolution rather than an activity: it has a
 * single trigger, runs to completion inside that one call, and prints a summary
 * of everything it decided.  That gives the order a clear beginning and a clear
 * end, which is the entire point -- when something misbehaves in play it is
 * obvious which step produced it.  There is no persistent state, no backlog and
 * no multi-turn planning, so there is also no way for it to spin in a loop.
 *
 * Because it resolves synchronously and physically removes items from the map,
 * several NPCs handled by the same menu selection never contend for the same
 * item: the second one simply no longer sees what the first one took.
 *
 * The ordering of the steps is not arbitrary:
 *   1. shed ballast   - free up storage before trying to fill it
 *   2. weapon         - the weapon decides which ammunition is coherent
 *   2b. backup blade  - never leave with a gun and nothing else
 *   3. worn gear      - worn gear is where the carrying capacity comes from
 *   4. ammunition     - matched to the weapon chosen in step 2, then reload
 *   5. consumables    - medical first, then food and water
 */

static const damage_type_id damage_bash( "bash" );
static const damage_type_id damage_bullet( "bullet" );
static const damage_type_id damage_cut( "cut" );
static const damage_type_id damage_stab( "stab" );

static const itype_id itype_acetaminophen( "acetaminophen" );
static const itype_id itype_aspirin( "aspirin" );
static const itype_id itype_codeine( "codeine" );
static const itype_id itype_heroin( "heroin" );
static const itype_id itype_ibuprofen( "ibuprofen" );
static const itype_id itype_oxycodone( "oxycodone" );
static const itype_id itype_tramadol( "tramadol" );

static const zone_type_id zone_type_CAMP_STORAGE( "CAMP_STORAGE" );
static const zone_type_id zone_type_LOOT_UNSORTED( "LOOT_UNSORTED" );
static const zone_type_id zone_type_NO_NPC_PICKUP( "NO_NPC_PICKUP" );

namespace
{

// How far the NPC will reach for the camp stores.  This order does not walk the
// NPC anywhere, so the radius is deliberately the same one the other NPC local
// acquisition helpers use (find_nearby_food, find_nearby_warm_clothing).
constexpr int gear_up_radius = 6;

// Nothing is swapped for a marginal gain.  An experienced player does not strip
// and re-dress for a three percent improvement; the time and the risk are not
// worth it.
constexpr double weapon_swap_margin = 1.15;
constexpr double outfit_swap_margin = 1.15;
// A swap that costs mobility has to clear a higher bar than one that does not.
constexpr double outfit_mobility_margin = 1.25;

// Hard iteration caps.  A player-triggered order may be a little expensive, but
// it must never be unbounded.
constexpr int max_worn_swaps = 12;
constexpr int max_wear_trials = 60;

// Supply targets.  Concrete numbers rather than "a little", so that falling
// short of them is reportable and testable.
constexpr int want_healing_items = 3;
constexpr int want_painkillers = 4;
constexpr int want_kcal = 2400;
constexpr int want_quench = 400;
constexpr int want_ammo_loads = 3;
constexpr int want_spare_magazines = 2;

// Scoring weights.  Coverage-weighted protection rather than raw resistance,
// head and torso worth four times a limb, mobility loss punished harder than
// anything except freezing, and storage treated as genuinely valuable rather
// than as an afterthought.
constexpr double weight_protection = 1.0;
constexpr double weight_encumbrance = 0.25;
// Legs carry double weight because being unable to run is how survivors die.
constexpr double weight_leg_encumbrance = 2.0;
// Encumbered hands cost reload speed and melee accuracy.
constexpr double weight_hand_encumbrance = 1.0;
// Eyes and mouth: a gas mask that blinds you is not a free upgrade.
constexpr double weight_sense_encumbrance = 1.5;
constexpr double weight_storage_per_liter = 0.30;
constexpr double weight_warmth = 0.10;
// Being badly dressed for the weather grows quadratically, so that "you will
// freeze" and "you will cook" both beat any amount of extra plating, while a
// small mismatch stays cheap enough not to cause constant re-dressing.
constexpr double warmth_curve = 25.0;
// Gas masks and sealed suits earn a small standing credit, enough that the NPC
// does not trade one away for a marginally better hat.
constexpr double weight_environment = 0.10;
// Filthy gear risks infection through any wound.  Better than nothing, worse
// than the clean equivalent.
constexpr double weight_filth = 0.5;

// Dressing for the reading on the thermometer right now is how an NPC ends up
// freezing after dark.  Plan for a colder moment than the current one.
constexpr int night_margin_c = 8;
// At or above this ambient temperature no extra warmth is wanted at all.
constexpr int comfort_temperature_c = 21;
// Roughly how much clothing warmth one degree of missing ambient heat costs.
constexpr double warmth_per_degree = 4.0;

struct gear_up_report {
    std::vector<std::string> changes;
    std::vector<std::string> notes;
    std::vector<std::string> problems;

    void change( const std::string &s ) {
        changes.push_back( s );
    }
    // Something the player should know that is neither a change nor a fault.
    void note( const std::string &s ) {
        notes.push_back( s );
    }
    void problem( const std::string &s ) {
        problems.push_back( s );
    }
};

bool is_painkiller( const item &it )
{
    // Mirrors inventory::most_appropriate_painkiller, so the NPC only stocks
    // painkillers its own AI already knows how to use.
    const itype_id &id = it.typeId();
    return id == itype_aspirin || id == itype_acetaminophen || id == itype_ibuprofen ||
           id == itype_codeine || id == itype_oxycodone || id == itype_tramadol ||
           id == itype_heroin;
}

bool is_healing_item( const item &it )
{
    return it.is_medical_tool();
}

// ---------------------------------------------------------------------------
// Candidate pool
// ---------------------------------------------------------------------------

struct gear_pool {
    std::vector<item_location> items;
    // Tiles belonging to a camp storage zone, nearest first.  Displaced gear
    // goes back here -- never onto the ground.
    std::vector<tripoint_bub_ms> storage_tiles;
    bool saw_storage_zone_out_of_range = false;
};

bool tile_is_camp_storage( const npc &p, const tripoint_bub_ms &tile )
{
    const zone_manager &mgr = zone_manager::get_manager();
    const tripoint_abs_ms abs = get_map().get_abs( tile );
    const faction_id fac = p.get_fac_id();
    return mgr.has( zone_type_CAMP_STORAGE, abs, fac ) ||
           mgr.has( zone_type_LOOT_UNSORTED, abs, fac );
}

// A camp store the leader has pointed us at is not theft: the order to gear up
// is itself the permission.  Anything outside it still goes through the normal
// ownership check.
bool may_take( npc &p, item &it, const tripoint_bub_ms &tile, bool from_storage )
{
    if( it.is_favorite ) {
        return false;
    }
    if( it.made_of( phase_id::LIQUID ) ) {
        return false;
    }
    return from_storage || p.would_take_that( it, tile );
}

void collect_tile_items( npc &p, gear_pool &pool, const tripoint_bub_ms &tile )
{
    map &here = get_map();
    const bool from_storage = tile_is_camp_storage( p, tile );
    if( from_storage ) {
        pool.storage_tiles.push_back( tile );
    }

    if( here.sees_some_items( tile, p ) ) {
        for( item &it : here.i_at( tile ) ) {
            if( may_take( p, it, tile, from_storage ) ) {
                pool.items.emplace_back( map_cursor( tile ), &it );
            }
        }
    }

    const optional_vpart_position vp = here.veh_at( tile );
    if( !vp || vp->vehicle().is_moving() ) {
        return;
    }
    const std::optional<vpart_reference> cargo = vp.cargo();
    if( !cargo || cargo->has_feature( "LOCKED" ) ) {
        return;
    }
    if( vp.part_with_feature( "CARGO_LOCKING", true ) ) {
        return;
    }
    for( item &it : cargo->items() ) {
        if( may_take( p, it, tile, from_storage ) ) {
            pool.items.emplace_back(
                vehicle_cursor( cargo->vehicle(), static_cast<ptrdiff_t>( cargo->part_index() ) ), &it );
        }
    }
}

gear_pool build_pool( npc &p )
{
    gear_pool pool;
    map &here = get_map();

    for( const tripoint_bub_ms &tile : closest_points_first( p.pos_bub(), gear_up_radius ) ) {
        if( p.is_no_go_position( here.get_abs( tile ) ) ) {
            continue;
        }
        if( p.is_player_ally() && g->check_zone( zone_type_NO_NPC_PICKUP, tile ) ) {
            continue;
        }
        if( !p.sees( here, tile ) ) {
            continue;
        }
        collect_tile_items( p, pool, tile );
    }

    // Tell the difference between "this camp has no storage zone at all" and
    // "the storage zone is over there and the NPC is standing here".
    if( pool.storage_tiles.empty() ) {
        const zone_manager &mgr = zone_manager::get_manager();
        pool.saw_storage_zone_out_of_range =
            mgr.has_near( zone_type_CAMP_STORAGE, p.pos_abs(), MAX_VIEW_DISTANCE, p.get_fac_id() );
    }
    return pool;
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

units::temperature planning_temperature( const npc &p )
{
    const units::temperature now = get_weather().get_temperature( p.pos_bub() );
    return now - units::from_celsius_delta( night_margin_c );
}

int target_warmth_for( units::temperature planning )
{
    const double c = units::to_celsius( planning );
    if( c >= comfort_temperature_c ) {
        return 0;
    }
    return std::min( 100, static_cast<int>( ( comfort_temperature_c - c ) * warmth_per_degree ) );
}

// Coverage-weighted protection, with head and torso weighted the way
// npc::estimate_armour already weights them.  Unlike estimate_armour this is
// only ever used for the wearer's own gear decisions and never for threat
// assessment, so it cannot make combat slower.
//
// The coverage term is the part estimate_armour is missing.  get_armor_type
// sums the raw resistance of everything covering a limb and never asks how much
// of that limb is actually covered, so on its own it rates a shoulder pad the
// same as a jacket.  The game rolls against coverage on every hit, which is why
// a light piece that covers all of a limb beats a heavy one that covers half.
double protection_of( const Character &who )
{
    double total = 0.0;
    int parts = 0;
    for( const bodypart_id &bp : who.get_all_body_parts( get_body_part_flags::only_main ) ) {
        double step = who.get_armor_type( damage_bash, bp ) +
                      who.get_armor_type( damage_cut, bp ) +
                      who.get_armor_type( damage_stab, bp ) +
                      who.get_armor_type( damage_bullet, bp );
        step /= 4.0;
        // Layers add up, but a limb cannot be more than fully covered.
        const int coverage = std::min( 100,
                                       who.worn.get_coverage( bp, item::cover_type::COVER_DEFAULT ) );
        step *= coverage / 100.0;
        parts += 1;
        if( bp == bodypart_id( "head" ) || bp == bodypart_id( "torso" ) ) {
            step *= 4.0;
            parts += 3;
        }
        total += step;
    }
    return parts > 0 ? total / parts : 0.0;
}

double mean_warmth_of( const Character &who )
{
    const std::map<bodypart_id, int> warmth = who.worn.warmth( who );
    if( warmth.empty() ) {
        return 0.0;
    }
    double total = 0.0;
    for( const std::pair<const bodypart_id, int> &entry : warmth ) {
        total += entry.second;
    }
    return total / warmth.size();
}

double environmental_of( const Character &who )
{
    double total = 0.0;
    int parts = 0;
    for( const bodypart_id &bp : who.get_all_body_parts( get_body_part_flags::only_main ) ) {
        total += who.get_env_resist( bp );
        parts++;
    }
    return parts > 0 ? total / parts : 0.0;
}

int filthy_worn( const Character &who )
{
    int count = 0;
    who.visit_items( [&who, &count]( const item * node, item * ) {
        if( node->is_filthy() && who.is_worn( *node ) ) {
            count++;
        }
        return VisitResponse::NEXT;
    } );
    return count;
}

// The single number the wear decisions are made on.  Protection, mobility,
// carrying capacity and warmth converted into one currency, because otherwise
// they cannot be traded off against each other at all.
double outfit_score( const Character &who, int target_warmth )
{
    const double protection = protection_of( who ) * weight_protection;

    const double encumbrance =
        ( who.avg_encumb_of_limb_type( bp_type::torso ) +
          who.avg_encumb_of_limb_type( bp_type::arm ) +
          who.avg_encumb_of_limb_type( bp_type::hand ) * weight_hand_encumbrance +
          who.avg_encumb_of_limb_type( bp_type::sensor ) * weight_sense_encumbrance +
          who.avg_encumb_of_limb_type( bp_type::mouth ) * weight_sense_encumbrance +
          who.avg_encumb_of_limb_type( bp_type::leg ) * weight_leg_encumbrance ) *
        weight_encumbrance;

    const double storage =
        units::to_liter( who.volume_capacity() ) * weight_storage_per_liter;

    const double deviation = std::abs( mean_warmth_of( who ) - target_warmth );
    const double warmth_gap = weight_warmth * deviation * deviation / warmth_curve;

    const double environment = environmental_of( who ) * weight_environment;
    const double filth = filthy_worn( who ) * weight_filth;

    return protection - encumbrance + storage - warmth_gap + environment - filth;
}

// Do these two pieces compete for the same patch of skin?  Whole-limb overlap
// is too coarse -- "leg" covers the knee, the shin and the thigh, and pieces
// that share a limb often coexist perfectly well.
bool shares_sub_part( const item &a, const item &b )
{
    const std::vector<sub_bodypart_id> a_parts = a.get_covered_sub_body_parts();
    const std::vector<sub_bodypart_id> b_parts = b.get_covered_sub_body_parts();
    for( const sub_bodypart_id &sbp : a_parts ) {
        if( std::find( b_parts.begin(), b_parts.end(), sbp ) != b_parts.end() ) {
            return true;
        }
    }
    return false;
}

// Cheap pre-filter so a full re-score is not run for every scrap of cloth in
// the camp.  It only decides the order candidates are tried in and whether a
// trial fitting is worth attempting at all; the actual decision is always the
// measured outfit score.
double wear_proxy( const Character &who, const item &it )
{
    double resist = it.resist( damage_bash ) + it.resist( damage_cut ) +
                    it.resist( damage_stab ) + it.resist( damage_bullet );
    resist *= it.get_avg_coverage() / 100.0;
    const double storage = units::to_liter( it.get_volume_capacity() ) * 3.0;
    const double enc = it.get_avg_encumber( who ) * 0.5;
    double proxy = resist + storage - enc + it.get_warmth() * 0.1 + it.get_env_resist() * 0.2;
    if( it.is_filthy() ) {
        proxy -= 2.0;
    }
    return proxy;
}

// ---------------------------------------------------------------------------
// Moving items around
// ---------------------------------------------------------------------------

// Put a displaced item somewhere safe: inventory first, camp storage second.
// If neither works the caller keeps the item where it was.  Nothing this order
// does ever leaves gear lying in the mud.
bool put_away( npc &p, const item &it, const gear_pool &pool )
{
    if( p.can_stash( it ) && p.i_add( it ) ) {
        return true;
    }
    map &here = get_map();
    for( const tripoint_bub_ms &tile : pool.storage_tiles ) {
        if( !here.add_item_or_charges( tile, it ).is_null() ) {
            return true;
        }
    }
    return false;
}

// Move a whole item from the pool into the NPC's inventory.  Adds first and
// only then removes from the source, so a failure cannot destroy the item.
bool take_into_inventory( npc &p, item_location &loc )
{
    if( !loc ) {
        return false;
    }
    const item copy = *loc;
    // can_stash only asks about volume.  Ammunition and rations are heavy, and
    // an NPC weighed down below walking pace has been made worse, not better.
    if( p.weight_carried() + copy.weight() > p.weight_capacity() ) {
        return false;
    }
    if( !p.can_stash( copy ) ) {
        return false;
    }
    if( !p.i_add( copy ) ) {
        return false;
    }
    loc.remove_item();
    return true;
}

// The same, for part of a charge-based stack such as ammunition or water.
int take_charges( npc &p, item_location &loc, int wanted )
{
    if( !loc || wanted <= 0 ) {
        return 0;
    }
    const int available = loc->count();
    int take = std::min( available, wanted );
    if( take <= 0 ) {
        return 0;
    }
    item copy = *loc;
    if( copy.count_by_charges() ) {
        copy.charges = take;
    } else {
        take = 1;
    }
    if( p.weight_carried() + copy.weight() > p.weight_capacity() ) {
        return 0;
    }
    if( !p.can_stash( copy ) || !p.i_add( copy ) ) {
        return 0;
    }
    if( loc->count_by_charges() && loc->charges > take ) {
        loc->charges -= take;
    } else {
        loc.remove_item();
    }
    return take;
}

// Reload something, but only once we are certain there is something to reload it
// with: npc::do_reload throws a debugmsg if asked to load a thing it cannot.
bool try_reload( npc &p, item_location target )
{
    if( !target || !p.can_reload( *target ) ) {
        return false;
    }
    if( !p.find_usable_ammo( target ) ) {
        return false;
    }
    p.do_reload( target );
    return true;
}

// takeoff() without the "<npcname> takes off their X" line.  Trial fittings
// would otherwise bury the summary under dozens of messages, and the summary is
// what the player actually needs to read.
bool quiet_takeoff( npc &p, item_location loc, std::list<item> &into )
{
    if( !loc || !p.can_takeoff( *loc, &into ).success() ) {
        return false;
    }
    if( !p.worn.takeoff( loc, &into, p ) ) {
        return false;
    }
    p.recalc_sight_limits();
    p.calc_encumbrance();
    p.worn.recalc_ablative_blocking( &p );
    p.calc_discomfort();
    return true;
}

int count_carried( const npc &p, const std::function<bool( const item & )> &pred )
{
    int total = 0;
    p.visit_items( [&]( const item * node, item * ) {
        if( pred( *node ) ) {
            total += node->count();
        }
        return VisitResponse::NEXT;
    } );
    return total;
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

// Step 1: shed ballast.
//
// This only fires when the NPC is genuinely overloaded, which is the literal
// reading of "heavy junk that is slowing them down".  Thirty kilos of sand the
// leader handed over on purpose is not thrown away merely for being thirty
// kilos of sand -- the leader may have a reason the NPC cannot see.  Favourites
// and anything on the player's pickup whitelist are never touched, because an
// explicit order outranks the NPC's own judgement.  What is put down goes into
// camp storage, so even a wrong call costs nothing but a short walk.
void shed_ballast( npc &p, const gear_pool &pool, gear_up_report &report )
{
    const bool overloaded = p.weight_carried() > p.weight_capacity() ||
                            p.volume_carried() > p.volume_capacity();
    if( !overloaded ) {
        return;
    }

    const auto is_ballast = [&p]( const item & it ) {
        if( it.is_favorite || p.is_worn( it ) || p.is_wielding( it ) ) {
            return false;
        }
        if( p.item_whitelisted( it ) ) {
            // The player told this NPC to collect these.  Not ours to overrule.
            return false;
        }
        if( it.is_armor() || it.is_gun() || it.is_magazine() || it.is_ammo() ||
            it.is_medication() || it.is_food() || it.is_food_container() ||
            it.is_ammo_container() || it.is_tool() || it.is_book() ) {
            return false;
        }
        // Never put down a container that still has something in it.
        return it.empty();
    };

    std::vector<item_location> ballast;
    for( item_location &loc : p.all_items_loc() ) {
        if( loc && is_ballast( *loc ) ) {
            ballast.push_back( loc );
        }
    }
    // Heaviest first: the fastest way back under the limit.
    std::sort( ballast.begin(), ballast.end(), []( const item_location & a, const item_location & b ) {
        return a->weight() > b->weight();
    } );

    map &here = get_map();
    int dropped = 0;
    for( item_location &loc : ballast ) {
        if( p.weight_carried() <= p.weight_capacity() &&
            p.volume_carried() <= p.volume_capacity() ) {
            break;
        }
        if( !loc ) {
            continue;
        }
        const item copy = *loc;
        bool placed = false;
        for( const tripoint_bub_ms &tile : pool.storage_tiles ) {
            if( !here.add_item_or_charges( tile, copy ).is_null() ) {
                placed = true;
                break;
            }
        }
        if( placed ) {
            loc.remove_item();
            report.change( string_format( _( "put down %s" ), copy.tname() ) );
            dropped++;
        }
    }
    if( dropped == 0 ) {
        report.problem( _( "overloaded, but carrying nothing worth putting down" ) );
    }
}

// Step 2: weapon.
//
// Scored with npc::evaluate_weapon, the same function the NPC's own combat AI
// uses.  A second opinion here would only mean the NPC quietly undoes this
// choice on its next turn, which would look exactly like a bug.
void choose_weapon( npc &p, gear_pool &pool, gear_up_report &report )
{
    item_location wielded = p.get_wielded_item();
    const double current = p.evaluate_weapon( wielded ? *wielded : null_item_reference() );

    item_location best;
    double best_value = current * weapon_swap_margin;
    for( item_location &loc : pool.items ) {
        if( !loc ) {
            continue;
        }
        item &it = *loc;
        if( !it.is_melee() && !it.is_gun() ) {
            continue;
        }
        if( !p.can_wield( it ).success() ) {
            continue;
        }
        const double value = p.evaluate_weapon( it );
        if( value > best_value ) {
            best_value = value;
            best = loc;
        }
    }
    if( !best ) {
        return;
    }

    // Empty the hands here rather than letting the wield path stow the old
    // weapon, because that path is allowed to drop it on the ground.
    if( wielded ) {
        const item old = *wielded;
        if( !p.can_unwield( old ).success() ) {
            report.problem( string_format( _( "can't let go of %s" ), old.tname() ) );
            return;
        }
        item removed = p.remove_weapon();
        if( !put_away( p, removed, pool ) ) {
            // No safe home for it, so put it straight back and change nothing.
            p.wield( removed );
            report.problem( string_format( _( "no room to set down %s" ), old.tname() ) );
            return;
        }
        report.change( string_format( _( "stowed %s" ), old.tname() ) );
    }

    const std::string name = best->tname();
    if( p.wield( best ) ) {
        report.change( string_format( _( "took up %s" ), name ) );
    } else {
        report.problem( string_format( _( "failed to take up %s" ), name ) );
    }
}

// Step 3: worn gear -- armour, ordinary clothing and carrying equipment alike,
// since the game makes no distinction between them.
//
// Every candidate is tried on and the whole outfit re-scored, then reverted if
// it did not help.  The engine decides what physically fits: pocket length,
// body size, integrated and no-takeoff gear, power armour dependencies, layer
// conflicts.  A metre of sword does not end up in a thirty centimetre pocket
// because can_stash says no, not because this function did the arithmetic.
void fit_out( npc &p, gear_pool &pool, gear_up_report &report )
{
    // Body temperature beats every other consideration, in both directions.
    // The cold side is the predicate the behaviour tree already uses
    // (character_oracle_t::needs_warmth_badly); the hot side matters just as
    // much, because the finest set of plate in the county is worthless to
    // someone who collapses from heatstroke wearing it.
    for( const bodypart_id &bp : p.get_all_body_parts() ) {
        const units::temperature part_temp = p.get_part_temp_conv( bp );
        if( part_temp <= BODYTEMP_VERY_COLD ) {
            report.problem( _( "too cold to think about anything but staying warm" ) );
            return;
        }
        if( part_temp >= BODYTEMP_VERY_HOT ) {
            report.problem( _( "too hot already to be putting more layers on" ) );
            return;
        }
    }

    const int target_warmth = target_warmth_for( planning_temperature( p ) );

    std::vector<item_location> candidates;
    for( item_location &loc : pool.items ) {
        if( loc && loc->is_armor() ) {
            candidates.push_back( loc );
        }
    }
    std::sort( candidates.begin(), candidates.end(),
    [&p]( const item_location & a, const item_location & b ) {
        return wear_proxy( p, *a ) > wear_proxy( p, *b );
    } );

    double score = outfit_score( p, target_warmth );
    int swaps = 0;
    int trials = 0;

    for( item_location &loc : candidates ) {
        if( swaps >= max_worn_swaps || trials >= max_wear_trials ) {
            break;
        }
        if( !loc ) {
            continue;
        }
        const item candidate = *loc;

        // First see whether it can simply be put on as an addition.
        if( p.can_wear( candidate ).success() ) {
            trials++;
            std::optional<std::list<item>::iterator> worn_it =
                p.wear_item( candidate, false, true, true, true );
            if( worn_it ) {
                const double after = outfit_score( p, target_warmth );
                if( after > score * outfit_swap_margin ) {
                    score = after;
                    swaps++;
                    loc.remove_item();
                    report.change( string_format( _( "put on %s" ), candidate.tname() ) );
                    continue;
                }
                std::list<item> removed;
                item_location worn_loc( p, &**worn_it );
                if( !quiet_takeoff( p, worn_loc, removed ) ) {
                    // Should not happen -- it was just put on.  Keep it rather
                    // than risk losing track of it, and stop touching clothing.
                    debugmsg( "gear up: %s could not remove trial item %s", p.get_name(),
                              candidate.tname() );
                    loc.remove_item();
                    return;
                }
                continue;
            }
        }

        // Otherwise consider displacing something already worn.  Only empty
        // worn items qualify: taking off a full backpack means finding a home
        // for everything inside it, which is a separate problem and not one
        // worth solving silently.
        item_location replace_target;
        double replace_proxy = 0.0;
        const double candidate_proxy = wear_proxy( p, candidate );
        for( item_location &worn_loc : p.all_items_loc() ) {
            if( !worn_loc || !p.is_worn( *worn_loc ) ) {
                continue;
            }
            const item &worn = *worn_loc;
            if( worn.is_favorite || !worn.empty() ) {
                continue;
            }
            if( !p.can_takeoff( worn ).success() ) {
                continue;
            }
            // Overlap is measured on sub-body-parts, which is the granularity
            // the engine's own conflict rule works at: outfit::check_rigid_conflicts
            // refuses a second rigid piece on a sublimb that already has one,
            // which is why you cannot wear kneepads over kneepads but can wear
            // kneepads and shin guards together.  Testing whole limbs instead
            // would displace the shin guard to make room for the kneepad.
            if( !shares_sub_part( worn, candidate ) ) {
                continue;
            }
            const double proxy = wear_proxy( p, worn );
            if( proxy >= candidate_proxy ) {
                continue;
            }
            if( !replace_target || proxy < replace_proxy ) {
                replace_target = worn_loc;
                replace_proxy = proxy;
            }
        }
        if( !replace_target ) {
            continue;
        }

        trials++;
        const item displaced = *replace_target;
        std::list<item> removed;
        if( !quiet_takeoff( p, replace_target, removed ) || removed.empty() ) {
            continue;
        }
        bool kept = false;
        if( p.can_wear( candidate ).success() ) {
            std::optional<std::list<item>::iterator> worn_it =
                p.wear_item( candidate, false, true, true, true );
            if( worn_it ) {
                const double after = outfit_score( p, target_warmth );
                // A swap that costs mobility has to earn more than one that
                // does not.  Being unable to run is how survivors die.
                const bool costs_mobility =
                    candidate.get_avg_encumber( p ) > displaced.get_avg_encumber( p );
                const double margin = costs_mobility ? outfit_mobility_margin : outfit_swap_margin;
                if( after > score * margin ) {
                    score = after;
                    kept = true;
                } else {
                    std::list<item> undo;
                    item_location worn_loc( p, &**worn_it );
                    if( !quiet_takeoff( p, worn_loc, undo ) ) {
                        debugmsg( "gear up: %s could not revert trial item %s", p.get_name(),
                                  candidate.tname() );
                        kept = true;
                    }
                }
            }
        }

        if( kept ) {
            swaps++;
            loc.remove_item();
            if( put_away( p, removed.front(), pool ) ) {
                report.change( string_format( _( "swapped %1$s for %2$s" ), displaced.tname(),
                                              candidate.tname() ) );
            } else {
                // Nowhere to put the old piece down, so wear it again over the
                // top and say what happened.  Losing it is not an option.
                p.wear_item( removed.front(), false, true, true, true );
                report.change( string_format( _( "put on %s" ), candidate.tname() ) );
            }
        } else {
            // Put the displaced piece back exactly as it was.
            if( !p.wear_item( removed.front(), false, true, true, true ) ) {
                if( !put_away( p, removed.front(), pool ) ) {
                    get_map().add_item_or_charges( p.pos_bub(), removed.front() );
                }
                report.problem( string_format( _( "could not put %s back on" ), displaced.tname() ) );
            }
        }
    }

    if( trials >= max_wear_trials || swaps >= max_worn_swaps ) {
        report.problem( _( "stopped sorting through the clothing before finishing" ) );
    }
}

// Step 2b: a backup you can swing.
//
// Nobody who knows what they are doing walks out with a rifle and nothing else.
// Guns jam, run dry, and are useless when something is already on top of you.
// If the NPC has no melee option beyond their own fists, take one.
void keep_a_backup_blade( npc &p, gear_pool &pool, gear_up_report &report )
{
    item_location wielded = p.get_wielded_item();
    const double fists = p.evaluate_weapon( null_item_reference() );

    // Already holding something you can hit with?  Then there is nothing to do.
    if( wielded && !wielded->is_gun() && p.evaluate_weapon( *wielded ) > fists ) {
        return;
    }
    bool has_backup = false;
    p.visit_items( [&p, &has_backup, fists]( const item * node, item * ) {
        if( node->is_melee() && p.evaluate_weapon( *node ) > fists &&
            p.can_wield( *node ).success() ) {
            has_backup = true;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    if( has_backup ) {
        return;
    }

    item_location best;
    double best_value = fists;
    for( item_location &loc : pool.items ) {
        if( !loc || !loc->is_melee() || loc->is_gun() ) {
            continue;
        }
        if( !p.can_wield( *loc ).success() ) {
            continue;
        }
        const double value = p.evaluate_weapon( *loc );
        if( value > best_value ) {
            best_value = value;
            best = loc;
        }
    }
    if( !best ) {
        return;
    }
    const std::string name = best->tname();
    if( take_into_inventory( p, best ) ) {
        report.change( string_format( _( "took %s as a backup" ), name ) );
    } else {
        report.problem( string_format( _( "no room to carry %s as a backup" ), name ) );
    }
}

// Step 4: ammunition, matched to whatever weapon step 2 settled on.
//
// This is exactly why the weapon is chosen first.  An archer has no business
// filling their pockets with 9mm, and since the ammunition types are read off
// the weapon itself, that mismatch cannot happen.
void stock_ammunition( npc &p, gear_pool &pool, gear_up_report &report )
{
    item_location wielded = p.get_wielded_item();
    if( !wielded || !wielded->is_gun() ) {
        return;
    }
    item &gun = *wielded;
    const std::set<ammotype> types = gun.ammo_types();
    if( types.empty() ) {
        return;
    }

    // A weapon that feeds from a magazine is useless with loose rounds alone.
    if( !gun.magazine_integral() ) {
        const std::set<itype_id> compatible = gun.magazine_compatible();
        const int have = count_carried( p, [&compatible]( const item & it ) {
            return it.is_magazine() && compatible.count( it.typeId() ) > 0;
        } );
        int need = std::max( 0, want_spare_magazines - have );
        for( item_location &loc : pool.items ) {
            if( need <= 0 ) {
                break;
            }
            if( !loc || !loc->is_magazine() || compatible.count( loc->typeId() ) == 0 ) {
                continue;
            }
            const std::string name = loc->tname();
            if( take_into_inventory( p, loc ) ) {
                report.change( string_format( _( "picked up %s" ), name ) );
                need--;
            }
        }
        if( need > 0 && have == 0 ) {
            report.problem( string_format( _( "no magazine for %s in the stores" ), gun.tname() ) );
        }
    }

    // An empty magazine-fed gun reports no capacity of its own, so take the
    // figure from a magazine the NPC is actually carrying.  Otherwise the
    // archer-with-9mm problem turns into a rifleman with four rounds.
    int capacity = gun.ammo_capacity( *types.begin() );
    if( capacity <= 0 && !gun.magazine_integral() ) {
        const std::set<itype_id> compatible = gun.magazine_compatible();
        p.visit_items( [&capacity, &types, &compatible]( const item * node, item * ) {
            if( node->is_magazine() && compatible.count( node->typeId() ) > 0 ) {
                capacity = std::max( capacity, node->ammo_capacity( *types.begin() ) );
            }
            return VisitResponse::NEXT;
        } );
    }
    capacity = std::max( 1, capacity );
    const int want = capacity * want_ammo_loads;
    const auto matches = [&types]( const item & it ) {
        return it.is_ammo() && types.count( it.ammo_type() ) > 0;
    };
    int have = count_carried( p, matches );
    if( have >= want ) {
        return;
    }

    int taken = 0;
    bool saw_any = false;
    for( item_location &loc : pool.items ) {
        if( have >= want ) {
            break;
        }
        if( !loc || !matches( *loc ) ) {
            continue;
        }
        saw_any = true;
        const int got = take_charges( p, loc, want - have );
        have += got;
        taken += got;
    }
    if( taken > 0 ) {
        report.change( string_format( _( "took %d rounds of ammunition" ), taken ) );
    }
    if( have == 0 ) {
        // "There is none" and "there is nowhere to put it" are very different
        // problems and the player can only act on the one they are told about.
        report.problem( saw_any ?
                        string_format( _( "no room to carry ammunition for %s" ), gun.tname() ) :
                        string_format( _( "no ammunition for %s in the stores" ), gun.tname() ) );
        return;
    }

    // Walk into the fight with a full magazine, not just a full pocket.  A
    // pouch of loose rounds is no use in the first three seconds.
    //
    // Two stages, because a magazine-fed weapon needs both: loose rounds go
    // into the magazines first, and only then does a loaded magazine go into
    // the gun.  Skipping the first stage leaves an NPC standing there with
    // ammunition, empty magazines and an empty weapon.
    if( !gun.magazine_integral() ) {
        const std::set<itype_id> compatible = gun.magazine_compatible();
        for( item_location &carried : p.all_items_loc() ) {
            if( !carried || !carried->is_magazine() ||
                compatible.count( carried->typeId() ) == 0 ) {
                continue;
            }
            if( carried->is_magazine_full() ) {
                continue;
            }
            try_reload( p, carried );
        }
    }

    item_location to_load = p.get_wielded_item();
    if( to_load && try_reload( p, to_load ) ) {
        report.change( string_format( _( "loaded %s" ), to_load->tname() ) );
    }
}

// Step 5: medical supplies, then food and water.
void stock_consumables( npc &p, gear_pool &pool, gear_up_report &report )
{
    int healing = count_carried( p, is_healing_item );
    bool saw_healing = false;
    for( item_location &loc : pool.items ) {
        if( healing >= want_healing_items ) {
            break;
        }
        if( !loc || !is_healing_item( *loc ) ) {
            continue;
        }
        saw_healing = true;
        const int got = take_charges( p, loc, want_healing_items - healing );
        healing += got;
        if( got > 0 ) {
            report.change( string_format( _( "took %d bandage(s) or first aid" ), got ) );
        }
    }
    if( healing == 0 ) {
        report.problem( saw_healing ? _( "no room to carry bandages" ) :
                        _( "no bandages in the stores" ) );
    }

    int pain_meds = count_carried( p, is_painkiller );
    for( item_location &loc : pool.items ) {
        if( pain_meds >= want_painkillers ) {
            break;
        }
        if( !loc || !is_painkiller( *loc ) ) {
            continue;
        }
        const int got = take_charges( p, loc, want_painkillers - pain_meds );
        pain_meds += got;
        if( got > 0 ) {
            report.change( string_format( _( "took %d dose(s) of painkiller" ), got ) );
        }
    }

    // With the NPC needs mod active NPCs neither eat nor drink, so handing them
    // rations would be pure clutter.  Skip it, and say that is what happened.
    if( !p.needs_food() ) {
        report.note( _( "took no rations; they don't need to eat or drink" ) );
        return;
    }

    int kcal = 0;
    int quench = 0;
    p.visit_items( [&kcal, &quench]( const item * node, item * ) {
        if( node->is_food() ) {
            const auto &com = node->get_comestible();
            if( com ) {
                kcal += com->default_nutrition_read_only().kcal() * node->count();
                quench += com->quench * node->count();
            }
        }
        return VisitResponse::NEXT;
    } );

    bool saw_rations = false;
    for( item_location &loc : pool.items ) {
        if( kcal >= want_kcal && quench >= want_quench ) {
            break;
        }
        if( !loc ) {
            continue;
        }
        item *food = nullptr;
        if( loc->is_food() ) {
            food = loc.get_item();
        } else if( loc->is_food_container() ) {
            for( item *inner : loc->all_items_top() ) {
                if( inner->is_food() ) {
                    food = inner;
                    break;
                }
            }
        }
        if( !food || !food->is_food() ) {
            continue;
        }
        const auto &com = food->get_comestible();
        if( !com ) {
            continue;
        }
        const bool useful = ( kcal < want_kcal && com->default_nutrition_read_only().kcal() > 0 ) ||
                            ( quench < want_quench && com->quench > 0 );
        if( !useful || !p.will_eat( *food ).success() ) {
            continue;
        }
        saw_rations = true;
        const std::string name = loc->tname();
        const int count = food->count();
        const int gained_kcal = com->default_nutrition_read_only().kcal() * count;
        const int gained_quench = com->quench * count;
        if( take_into_inventory( p, loc ) ) {
            kcal += gained_kcal;
            quench += gained_quench;
            report.change( string_format( _( "packed %s" ), name ) );
        }
    }
    if( kcal < want_kcal ) {
        report.problem( saw_rations ? _( "no room to carry enough food" ) : _( "short on food" ) );
    }
    if( quench < want_quench ) {
        report.problem( saw_rations ? _( "no room to carry enough water" ) : _( "short on water" ) );
    }
}

} // namespace

void talk_function::gear_up_from_stores( npc &p )
{
    if( p.is_hallucination() ) {
        return;
    }

    // Rummaging through crates with something hunting you is how people die.
    map &here = get_map();
    for( Creature &critter : g->all_creatures() ) {
        if( &critter == static_cast<Creature *>( &p ) ) {
            continue;
        }
        if( p.attitude_to( critter ) != Creature::Attitude::HOSTILE ) {
            continue;
        }
        if( rl_dist( p.pos_abs(), critter.pos_abs() ) > MAX_VIEW_DISTANCE / 3 ) {
            continue;
        }
        if( !p.sees( here, critter ) ) {
            continue;
        }
        add_msg( m_warning, _( "%1$s won't stop to sort gear with %2$s in sight." ), p.get_name(),
                 critter.disp_name() );
        return;
    }

    gear_pool pool = build_pool( p );
    if( pool.storage_tiles.empty() ) {
        if( pool.saw_storage_zone_out_of_range ) {
            add_msg( m_info, _( "%s is too far from the camp stores to reach them." ), p.get_name() );
        } else {
            add_msg( m_info, _( "%s has no camp storage or unsorted loot zone within reach." ),
                     p.get_name() );
        }
        return;
    }

    gear_up_report report;
    shed_ballast( p, pool, report );
    choose_weapon( p, pool, report );
    keep_a_backup_blade( p, pool, report );
    fit_out( p, pool, report );
    stock_ammunition( p, pool, report );
    stock_consumables( p, pool, report );

    p.invalidate_range_cache();
    p.calc_encumbrance();
    // Let the NPC's own AI re-examine what it is now carrying next turn.  It
    // scores weapons with the same function used above, so it will agree.
    p.has_new_items = true;
    // Digging through crates and changing clothes is not free.  Move budget
    // carries over between turns, so this really does keep the NPC busy for a
    // few turns afterwards -- capped, so a big re-kit never strands them.
    const int handled = std::min( 10, static_cast<int>( report.changes.size() ) );
    if( handled > 0 ) {
        p.mod_moves( -handled * to_moves<int>( 1_seconds ) );
    }

    if( report.changes.empty() ) {
        add_msg( m_info, _( "%s is already as well equipped as the stores allow." ), p.get_name() );
    } else {
        add_msg( m_good, _( "%1$s gears up: %2$s." ), p.get_name(),
                 enumerate_as_string( report.changes ) );
    }
    for( const std::string &note : report.notes ) {
        add_msg( m_info, _( "%1$s: %2$s." ), p.get_name(), note );
    }
    for( const std::string &problem : report.problems ) {
        add_msg( m_warning, _( "%1$s: %2$s." ), p.get_name(), problem );
    }
}
