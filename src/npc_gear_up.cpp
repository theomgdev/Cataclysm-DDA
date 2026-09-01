#include "activity_actor_definitions.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "activity_handlers.h"
#include "activity_item_handling.h"
#include "avatar.h"
#include "body_part_set.h"
#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "character_attire.h"
#include "character_id.h"
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
#include "item_category.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "line.h"
#include "map.h"
#include "map_selector.h"
#include "messages.h"
#include "npc.h"
#include "npc_gear_up.h"
#include "npctalk.h"
#include "output.h"
#include "player_activity.h"
#include "ret_val.h"
#include "translations.h"
#include "units.h"
#include "value_ptr.h"
#include "visitable.h"
#include "weather.h"
#include "weather_type.h"

/*
 * "Gear up from the stores": the character walks the faction's loot and camp
 * zones tile by tile, the way loot sorting does, and equips from what is
 * stored there, containers included.  Displaced gear goes back to the zone the
 * zone manager picks for it.  NPCs are given the order in conversation, the
 * avatar takes it from the zone-activities menu.
 *
 * Equipment first (weapon, backup blade, clothing), supplies second
 * (magazines, ammunition, medical, rations, water).  The stage advances only
 * once no tile offers an equipment improvement, so the weapon is settled
 * before ammunition is chosen for it.  Within clothing, pockets come first
 * while there are none: everything the supply stage does needs somewhere to
 * put what it picks up.
 *
 * Termination hinges on gear_up_rejected: types tried and turned down, or that
 * could not be carried, are remembered.  The engine's loop detector cannot see
 * a character circling back to the same crate, because every lap spends moves.
 */

static const damage_type_id damage_bash( "bash" );
static const damage_type_id damage_bullet( "bullet" );
static const damage_type_id damage_cut( "cut" );
static const damage_type_id damage_stab( "stab" );

static const item_category_id item_category_weapons( "weapons" );

static const itype_id itype_acetaminophen( "acetaminophen" );
static const itype_id itype_aspirin( "aspirin" );
static const itype_id itype_codeine( "codeine" );
static const itype_id itype_heroin( "heroin" );
static const itype_id itype_ibuprofen( "ibuprofen" );
static const itype_id itype_oxycodone( "oxycodone" );
static const itype_id itype_tramadol( "tramadol" );

static const zone_type_id zone_type_CAMP_FOOD( "CAMP_FOOD" );
static const zone_type_id zone_type_CAMP_STORAGE( "CAMP_STORAGE" );
static const zone_type_id zone_type_LOOT_IGNORE( "LOOT_IGNORE" );
static const zone_type_id zone_type_LOOT_UNSORTED( "LOOT_UNSORTED" );
static const zone_type_id zone_type_NO_NPC_PICKUP( "NO_NPC_PICKUP" );

namespace
{

// Bumped by reset_gear_up_caches() to invalidate every (turn, character)
// cache below out of band.  In real play calendar::turn only ever advances,
// so a (turn, character) key alone is safe; a test process reuses the same
// avatar object and resets calendar::turn to the same constant for every
// TEST_CASE, which without this would let a scan cached by one test answer
// for a completely different scenario in the next one.
unsigned int gear_up_cache_generation = 0;

// What "enough supplies for a fight" means, in numbers, so that falling short
// of it is something the code can act on.
constexpr int want_healing_items = 3;
constexpr int want_painkillers = 4;
constexpr int want_kcal = 2400;
constexpr int want_quench = 400;
constexpr int want_ammo_loads = 3;
constexpr int want_spare_magazines = 2;
constexpr int want_drink_vessels = 2;

// Carrying capacity worth having: ammunition, a medical kit, a day of food
// and water, some salvage.  Past that a liter of pocket keeps a tenth of its
// credit, or storage outscores real armour once a character has a bag or two.
constexpr double want_storage_liters = 25.0;
constexpr double weight_storage_per_liter = 0.30;
constexpr double weight_storage_marginal = weight_storage_per_liter * 0.1;

// Below this, a bag or a pair of pockets is sought to the exclusion of
// everything else armour has to offer.  Storage is a prerequisite, not one
// candidate competing on its own merits: can_stash() refuses a backup blade,
// spare ammunition and medical supplies alike without room to put them, so
// deferring pockets to a raw score contest against real armour can starve
// them indefinitely whenever the camp has enough of the latter.
constexpr double want_storage_liters_floor = 3.0;

// What exposure costs.  Bare skin is not a free baseline: cloth that stops
// thorns, sun, weather and scrapes registers as almost no armour at all, so
// judged on resistance alone a plain pair of jeans lands a fraction under
// nothing and the character walks into a fight bare-legged.  A piece covering
// skin has to beat this instead of zero; one going over something already
// worn still has to beat the piece it displaces.
constexpr double weight_bare_skin = 6.0;

// Extra encumbrance weight for legs (can't run) and eyes/mouth (a gas mask
// that blinds you is not a free upgrade), scoped to the covered part.
constexpr double weight_leg_encumbrance = 2.0;
constexpr double weight_sense_encumbrance = 1.5;

// Warmth is credited only while short of the planning target; past that it
// is a smaller liability, not a bonus.
constexpr double weight_warmth_needed = 0.10;
constexpr double weight_warmth_excess = 0.05;

// Filthy gear risks infection through any wound.  Better than nothing, worse
// than the clean equivalent.
constexpr double filth_penalty = 2.0;

// Dressing for the reading on the thermometer right now is how someone ends up
// freezing after dark.  Plan for a colder moment than the current one.
constexpr int night_margin_c = 8;
// At or above this ambient temperature no extra warmth is wanted at all.
constexpr int comfort_temperature_c = 21;
// Roughly how much clothing warmth one degree of missing ambient heat costs.
constexpr double warmth_per_degree = 4.0;

// Going through a crate costs time whether or not anything comes of it, and
// moving one item in or out of a container costs a little more.
constexpr int search_cost_moves = 30;
constexpr int handle_cost_moves = 20;

// How deep into nested containers to look.  A crate holding a first aid kit
// holding bandages is two; beyond three is packing material, not storage.
constexpr int max_nesting_depth = 3;

enum class gear_stage : int {
    equipment = 0,
    supplies = 1,
};

// ---------------------------------------------------------------------------
// Small predicates
// ---------------------------------------------------------------------------

bool is_painkiller( const item &it )
{
    // Mirrors inventory::most_appropriate_painkiller, so the character only
    // stocks painkillers the NPC AI already knows how to use.
    const itype_id &id = it.typeId();
    return id == itype_aspirin || id == itype_acetaminophen || id == itype_ibuprofen ||
           id == itype_codeine || id == itype_oxycodone || id == itype_tramadol ||
           id == itype_heroin;
}

bool is_healing_item( const item &it )
{
    return it.is_medical_tool();
}

// Same patch of skin?  Sub-part granularity where both sides have it, the
// same the engine's own conflict rule uses (outfit::check_rigid_conflicts):
// kneepads and shin guards share a leg but not a sub-part.  A towel and most
// old-format garments carry no sub-bodypart armor data at all, so
// get_covered_sub_body_parts() comes back empty for them and the fine check
// would report no overlap no matter what -- whole-bodypart coverage is the
// fallback, since every armor entry has that much.
bool shares_sub_part( const item &a, const item &b )
{
    const std::vector<sub_bodypart_id> a_parts = a.get_covered_sub_body_parts();
    const std::vector<sub_bodypart_id> b_parts = b.get_covered_sub_body_parts();
    if( !a_parts.empty() && !b_parts.empty() ) {
        for( const sub_bodypart_id &sbp : a_parts ) {
            if( std::find( b_parts.begin(), b_parts.end(), sbp ) != b_parts.end() ) {
                return true;
            }
        }
        return false;
    }
    return a.get_covered_body_parts().make_intersection( b.get_covered_body_parts() ).any();
}

// Same sub-part *and* same layer as something worn: that is the redundancy
// (second pair of pants), where an undershirt under a hoodie is not.
// can_wear() only forbids a second rigid piece, so the soft case lands here.
bool conflicts_with_worn( const Character &who, const item &candidate )
{
    const std::vector<sub_bodypart_id> cand_parts = candidate.get_covered_sub_body_parts();
    bool conflict = false;
    who.visit_items( [&]( const item * node, item * ) {
        if( !who.is_worn( *node ) || !shares_sub_part( *node, candidate ) ) {
            return VisitResponse::NEXT;
        }
        const std::vector<sub_bodypart_id> worn_parts = node->get_covered_sub_body_parts();
        if( !cand_parts.empty() && !worn_parts.empty() ) {
            for( const sub_bodypart_id &sbp : cand_parts ) {
                if( std::find( worn_parts.begin(), worn_parts.end(), sbp ) == worn_parts.end() ) {
                    continue;
                }
                const std::vector<layer_level> cand_layers = candidate.get_layer( sbp );
                const std::vector<layer_level> worn_layers = node->get_layer( sbp );
                for( const layer_level &l : cand_layers ) {
                    if( std::find( worn_layers.begin(), worn_layers.end(), l ) != worn_layers.end() ) {
                        conflict = true;
                        return VisitResponse::ABORT;
                    }
                }
            }
            return VisitResponse::NEXT;
        }
        // One side has no sub-bodypart data: compare at whole-bodypart
        // granularity instead, or a second towel never conflicts with the first.
        for( const bodypart_id &bp : who.get_all_body_parts() ) {
            if( !candidate.covers( bp ) || !node->covers( bp ) ) {
                continue;
            }
            const std::vector<layer_level> cand_layers = candidate.get_layer( bp );
            const std::vector<layer_level> worn_layers = node->get_layer( bp );
            for( const layer_level &l : cand_layers ) {
                if( std::find( worn_layers.begin(), worn_layers.end(), l ) != worn_layers.end() ) {
                    conflict = true;
                    return VisitResponse::ABORT;
                }
            }
        }
        return VisitResponse::NEXT;
    } );
    return conflict;
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

units::temperature planning_temperature( const Character &who )
{
    const units::temperature now = get_weather().get_temperature( who.pos_bub() );
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

// How warm the character already is where this piece would sit.  A whole-body
// average would judge a scarf by an already well-covered torso.  A piece
// already worn is measured on what is left without its own contribution, so
// that it scores the same on the shelf as on the back -- otherwise a garment
// and its replacement trade places forever.
double warmth_under( const Character &who, const item &it )
{
    const std::map<bodypart_id, int> warmth = who.worn.warmth( who );
    double total = 0.0;
    int parts = 0;
    for( const std::pair<const bodypart_id, int> &entry : warmth ) {
        if( it.covers( entry.first ) ) {
            total += entry.second;
            parts++;
        }
    }
    if( parts == 0 ) {
        return 0.0;
    }
    const double mean = total / parts;
    return who.is_worn( it ) ? std::max( 0.0, mean - it.get_warmth() ) : mean;
}

// How much of what this piece would cover is bare skin right now, as a
// fraction of its own coverage.  Worn pieces are gathered once rather than
// asked per body part: this runs for every candidate in the camp.
double exposed_share( const Character &who, const item &it )
{
    body_part_set clothed;
    who.visit_items( [&who, &clothed]( const item * node, item * ) {
        if( who.is_worn( *node ) ) {
            clothed.unify_set( node->get_covered_body_parts() );
        }
        return VisitResponse::NEXT;
    } );

    int covered = 0;
    int bare = 0;
    for( const bodypart_id &bp : who.get_all_body_parts() ) {
        if( !it.covers( bp ) ) {
            continue;
        }
        covered++;
        if( !clothed.test( bp.id() ) ) {
            bare++;
        }
    }
    return covered > 0 ? static_cast<double>( bare ) / covered : 0.0;
}

double local_encumbrance_weight( const item &it )
{
    if( it.covers( body_part_leg_l.id() ) || it.covers( body_part_leg_r.id() ) ) {
        return weight_leg_encumbrance;
    }
    if( it.covers( body_part_eyes.id() ) || it.covers( body_part_mouth.id() ) ) {
        return weight_sense_encumbrance;
    }
    return 1.0;
}

// The score clothing decisions are made on, scoped to the candidate's own
// coverage: a whole-body average dilutes leg armour against an unrelated
// backpack's storage.  Fit and sizing need no term of their own, because
// get_avg_encumber() already charges wrong-size and unfitted garments extra.
double wear_proxy( const Character &who, const item &it, int target_warmth )
{
    double resist = it.resist( damage_bash ) + it.resist( damage_cut ) +
                    it.resist( damage_stab ) + it.resist( damage_bullet );
    resist *= it.get_avg_coverage() / 100.0;

    // Storage is judged against what the character carries *without* this
    // piece, or a worn pack scores lower than its unworn twin and the two
    // trade places forever.
    const double item_liters = units::to_liter( it.get_volume_capacity() );
    double current_liters = units::to_liter( who.volume_capacity() );
    if( who.is_worn( it ) ) {
        current_liters = std::max( 0.0, current_liters - item_liters );
    }
    const double room_left = std::max( 0.0, want_storage_liters - current_liters );
    const double credited_liters = std::min( item_liters, room_left );
    const double storage = credited_liters * weight_storage_per_liter +
                           ( item_liters - credited_liters ) * weight_storage_marginal;

    const double enc = it.get_avg_encumber( who ) * 0.5 * local_encumbrance_weight( it );

    const double warmth_gap = target_warmth - warmth_under( who, it );
    const double warmth = warmth_gap > 0
                          ? std::min<double>( it.get_warmth(), warmth_gap ) * weight_warmth_needed
                          : -it.get_warmth() * weight_warmth_excess;

    double proxy = resist + storage - enc + warmth + it.get_env_resist() * 0.2;
    if( it.is_filthy() ) {
        proxy -= filth_penalty;
    }
    return proxy;
}

// The finest set of plate in the county is worthless to someone who collapses
// from heatstroke wearing it, so somebody already overheating puts nothing
// else on.  Only the hot side: someone freezing needs clothes more than
// anybody, and refusing to hand them a coat would be the opposite of help.
bool overheating_forbids_dressing( const Character &who )
{
    for( const bodypart_id &bp : who.get_all_body_parts() ) {
        if( who.get_part_temp_conv( bp ) >= BODYTEMP_VERY_HOT ) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Zone-aware search and placement
// ---------------------------------------------------------------------------

bool tile_is_off_limits( const Character &who, const tripoint_bub_ms &tile )
{
    // Loot: Ignore is the player saying "leave this pile alone", which holds
    // whoever is doing the sorting.
    if( g->check_zone( zone_type_LOOT_IGNORE, tile ) ) {
        return true;
    }
    const npc *guy = who.as_npc();
    if( !guy ) {
        return false;
    }
    // The rest is about NPCs specifically: a zone marked hands-off to them,
    // and anywhere this one has been told not to go.
    return g->check_zone( zone_type_NO_NPC_PICKUP, tile ) ||
           guy->is_no_go_position( get_map().get_abs( tile ) );
}

// Every loot zone the faction owns, plus the basecamp's own storage and food
// zones, which do not carry the LOOT prefix.  Specific zones and the big
// general one laid over them overlap heavily; the set deduplicates.  NPC and
// avatar look at exactly the same set -- relying on which specific LOOT_*
// zone types exist would be its own kind of fragile -- and lean on the
// existing exceptions (NO_NPC_PICKUP, LOOT_IGNORE, favourites, someone else's
// property, a no-go position) to keep an NPC out of what it should not touch.
std::unordered_set<tripoint_abs_ms> stores_within_reach( Character &who )
{
    zone_manager &mgr = zone_manager::get_manager();
    map &here = get_map();
    const faction_id fac = who.get_faction_id();
    std::unordered_set<tripoint_abs_ms> tiles;

    for( const tripoint_bub_ms &tile :
         mgr.get_point_set_loot( who.pos_abs(), MAX_VIEW_DISTANCE, who.is_npc(), fac ) ) {
        tiles.emplace( here.get_abs( tile ) );
    }
    for( const zone_type_id &type : {
             zone_type_CAMP_STORAGE, zone_type_CAMP_FOOD
         } ) {
        for( const tripoint_abs_ms &tile :
             mgr.get_near( type, who.pos_abs(), MAX_VIEW_DISTANCE, nullptr, fac ) ) {
            tiles.emplace( tile );
        }
    }

    for( auto it = tiles.begin(); it != tiles.end(); ) {
        const tripoint_bub_ms bub = here.get_bub( *it );
        if( here.inbounds( bub ) && tile_is_off_limits( who, bub ) ) {
            it = tiles.erase( it );
        } else {
            ++it;
        }
    }
    return tiles;
}

// Where does this belong?  The zone manager already knows -- the same
// question the loot sorter asks -- so putting things down here leaves the
// camp sorted instead of piled wherever we stood.
std::optional<tripoint_bub_ms> home_for( Character &who, const item &it,
        const tripoint_bub_ms &fallback )
{
    zone_manager &mgr = zone_manager::get_manager();
    map &here = get_map();
    const faction_id fac = who.get_faction_id();

    const zone_type_id dest =
        mgr.get_near_zone_type_for_item( it, who.pos_abs(), MAX_VIEW_DISTANCE, fac );

    std::vector<zone_type_id> tries;
    if( !dest.is_empty() ) {
        tries.push_back( dest );
    }
    tries.push_back( zone_type_CAMP_STORAGE );
    tries.push_back( zone_type_LOOT_UNSORTED );

    for( const zone_type_id &type : tries ) {
        std::optional<tripoint_bub_ms> best;
        int best_dist = INT_MAX;
        for( const tripoint_abs_ms &spot :
             mgr.get_near( type, who.pos_abs(), MAX_VIEW_DISTANCE, &it, fac ) ) {
            const tripoint_bub_ms bub = here.get_bub( spot );
            if( !here.inbounds( bub ) || tile_is_off_limits( who, bub ) ) {
                continue;
            }
            const int dist = rl_dist( who.pos_bub(), bub );
            if( dist < best_dist ) {
                best_dist = dist;
                best = bub;
            }
        }
        if( best ) {
            return best;
        }
    }
    if( here.inbounds( fallback ) ) {
        return fallback;
    }
    return std::nullopt;
}

// Somewhere sensible for a displaced piece: the zone the zone manager picks
// for it, never the character's own pockets.  A displaced piece is camp
// property being put away, not a keepsake -- whoever is doing the gearing up
// is standing in the camp already, so there is nowhere better to carry it to,
// and one character's "might need it later" is everyone else's item made
// unavailable and everyone's carry weight made worse for nothing.  False
// means even that failed, and every caller answers that by putting it down on
// the crate rather than losing it -- also in the camp, so no worse a home.
bool put_away( Character &who, const item &it, const tripoint_bub_ms &fallback )
{
    map &here = get_map();
    const std::optional<tripoint_bub_ms> home = home_for( who, it, fallback );
    if( !home ) {
        return false;
    }
    return !here.add_item_or_charges( *home, it ).is_null();
}

// ---------------------------------------------------------------------------
// Moving items around
// ---------------------------------------------------------------------------

bool take_into_inventory( Character &who, item_location &loc )
{
    if( !loc ) {
        return false;
    }
    const item copy = *loc;
    // can_stash only asks about volume.  Ammunition and rations are heavy, and
    // someone weighed down below walking pace has been made worse, not better.
    if( who.weight_carried() + copy.weight() > who.weight_capacity() ) {
        return false;
    }
    if( !who.can_stash( copy ) || !who.i_add( copy ) ) {
        return false;
    }
    who.mod_moves( -handle_cost_moves );
    loc.remove_item();
    return true;
}

// The same, for part of a charge-based stack such as ammunition or water.
int take_charges( Character &who, item_location &loc, int wanted )
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
        // Take what can actually be carried rather than all or nothing.
        const std::int64_t stack_grams = std::max<std::int64_t>( 1, to_gram( copy.weight() ) );
        const std::int64_t headroom_grams =
            to_gram( who.weight_capacity() ) - to_gram( who.weight_carried() );
        if( stack_grams > headroom_grams ) {
            take = static_cast<int>( std::max<std::int64_t>( 0,
                                     headroom_grams * take / stack_grams ) );
            if( take <= 0 ) {
                return 0;
            }
            copy.charges = take;
        }
    } else {
        take = 1;
    }
    if( who.weight_carried() + copy.weight() > who.weight_capacity() ) {
        return 0;
    }
    if( !who.can_stash( copy ) || !who.i_add( copy ) ) {
        return 0;
    }
    who.mod_moves( -handle_cost_moves );
    if( loc->count_by_charges() && loc->charges > take ) {
        loc->charges -= take;
    } else {
        loc.remove_item();
    }
    return take;
}

// takeoff() without the "<npcname> takes off their X" line, so that trial
// fittings do not bury what the character actually decided.
bool quiet_takeoff( Character &who, item_location loc, std::list<item> &into )
{
    if( !loc || !who.can_takeoff( *loc, &into ).success() ) {
        return false;
    }
    if( !who.worn.takeoff( loc, &into, who ) ) {
        return false;
    }
    who.recalc_sight_limits();
    who.calc_encumbrance();
    who.worn.recalc_ablative_blocking( &who );
    who.calc_discomfort();
    return true;
}

// npc::wield and avatar::wield each do bookkeeping of their own on top of
// Character::wield -- martial arts styles, the range cache, the message -- and
// none of the three is virtual, so a Character-typed call silently skips it.
bool wield_loc( Character &who, item_location loc )
{
    if( npc *guy = who.as_npc() ) {
        return guy->wield( loc );
    }
    if( avatar *me = who.as_avatar() ) {
        return me->wield( loc );
    }
    return who.wield( loc );
}

// can_reload() says yes for items that then refuse the load itself -- a folded
// bow keeps its ammo type but loses the pocket -- and npc::do_reload answers a
// refused load with a debugmsg, so ask here first.
bool has_room_for_a_load( const item &target )
{
    if( target.is_gun() && !target.magazine_integral() ) {
        // A magazine well takes a magazine, not loose rounds: it is free when
        // it is empty, and worth topping up when the magazine in it is not.
        const item *mag = target.magazine_current();
        return !mag || !mag->is_magazine_full();
    }
    return target.remaining_ammo_capacity() > 0;
}

bool try_reload( Character &who, item_location target )
{
    if( !target || !has_room_for_a_load( *target ) || !who.can_reload( *target ) ) {
        return false;
    }
    if( npc *guy = who.as_npc() ) {
        if( !guy->find_usable_ammo( target ) ) {
            return false;
        }
        guy->do_reload( target );
        return true;
    }
    // The avatar's own select_ammo() prompts, which an activity must not do, so
    // the load is found by hand: a magazine feeds a magazine-fed gun, loose
    // rounds feed magazines and integral wells.  Whatever is already inside the
    // target is not a load for it.
    const auto usable = [&target]( const item_location & carried ) {
        return carried && carried.get_item() != target.get_item() &&
               !target->has_item( *carried );
    };
    item_location load;
    int qty = 0;
    if( target->is_gun() && !target->magazine_integral() ) {
        const std::set<itype_id> compatible = target->magazine_compatible();
        for( item_location &carried : who.all_items_loc() ) {
            if( usable( carried ) && carried->is_magazine() &&
                compatible.count( carried->typeId() ) > 0 && carried->ammo_remaining() > 0 &&
                ( !load || carried->ammo_remaining() > load->ammo_remaining() ) ) {
                load = carried;
            }
        }
        qty = 1;
    } else {
        const std::set<ammotype> types = target->ammo_types();
        for( item_location &carried : who.all_items_loc() ) {
            if( usable( carried ) && carried->is_ammo() &&
                types.count( carried->ammo_type() ) > 0 &&
                ( !load || carried->charges > load->charges ) ) {
                load = carried;
            }
        }
        if( load ) {
            qty = std::min( load->count(), target->remaining_ammo_capacity() );
        }
    }
    if( !load || qty <= 0 ) {
        return false;
    }
    const std::string target_name = target->tname();
    who.mod_moves( -who.item_reload_cost( *target, *load, qty ) );
    if( !target->reload( who, std::move( load ), qty ) ) {
        return false;
    }
    who.add_msg_if_player( m_good, _( "You reload the %s." ), target_name );
    return true;
}

// Find a carried item of this type again after it has been stowed -- the
// location it had before the move is not the one it has now.
item_location find_carried( Character &who, const itype_id &id )
{
    for( item_location &loc : who.all_items_loc() ) {
        if( loc && loc->typeId() == id && !who.is_worn( *loc ) && !who.is_wielding( *loc ) ) {
            return loc;
        }
    }
    return {};
}

int count_carried( const Character &who, const std::function<bool( const item & )> &pred )
{
    int total = 0;
    who.visit_items( [&total, &pred]( const item * node, item * ) {
        if( pred( *node ) ) {
            total += node->count();
        }
        return VisitResponse::NEXT;
    } );
    return total;
}

// ---------------------------------------------------------------------------
// Candidates on one tile
// ---------------------------------------------------------------------------

// Favourites are the player's word on the subject, at any depth.  Somebody
// else's property is theirs: the wield path would stop and ask about it, and
// an activity working through a whole camp cannot stop and ask about
// everything, so it leaves those alone instead.
bool off_limits_item( const Character &who, const item &it )
{
    return it.is_favorite || it.made_of( phase_id::LIQUID ) ||
           !it.is_owned_by( who, true );
}

// Wearing a bag empties it into the camp's zones first, so one holding
// something the player marked keeps its place on the shelf: a favourite is
// not to be taken, and not to be moved either.
bool holds_a_favorite( const item &it )
{
    bool found = false;
    for( const item *inner : it.all_items_top( pocket_type::CONTAINER ) ) {
        inner->visit_items( [&found]( const item * node, item * ) {
            if( node->is_favorite ) {
                found = true;
                return VisitResponse::ABORT;
            }
            return VisitResponse::NEXT;
        } );
        if( found ) {
            return true;
        }
    }
    return false;
}

// Everything nested inside crates, boxes, duffel bags and first aid kits,
// which is where a sorted camp keeps things.
void collect_from( const Character &who, item_location parent,
                   std::vector<item_location> &out, int depth )
{
    if( depth > max_nesting_depth ) {
        return;
    }
    for( item *inner : parent->all_items_top( pocket_type::CONTAINER ) ) {
        if( off_limits_item( who, *inner ) ) {
            continue;
        }
        item_location child( parent, inner );
        out.push_back( child );
        collect_from( who, child, out, depth + 1 );
    }
}

std::vector<item_location> candidates_at( Character &who, const tripoint_bub_ms &tile )
{
    std::vector<item_location> out;
    map &here = get_map();
    if( !here.inbounds( tile ) || !here.sees_some_items( tile, who ) ) {
        return out;
    }
    for( item &it : here.i_at( tile ) ) {
        if( off_limits_item( who, it ) ) {
            continue;
        }
        item_location loc( map_cursor( tile ), &it );
        out.push_back( loc );
        collect_from( who, loc, out, 1 );
    }
    return out;
}

// ---------------------------------------------------------------------------
// Wants
//
// One predicate answers both "is this tile worth walking to" and "what do I do
// now that I am standing here".  They must agree, or the character walks
// somewhere and then does nothing, over and over.
// ---------------------------------------------------------------------------

// A camp tool (wrench, waffle iron) reads as is_melee() but must not be taken
// as a weapon; a combat knife carries a tool slot but must.  Only the item's
// own category tells them apart.  Guns are exempt -- a plasma torch is a tool.
bool is_weapon_candidate( const item &it )
{
    if( it.is_gun() ) {
        return true;
    }
    if( !it.is_melee() ) {
        return false;
    }
    return !it.is_tool() || it.get_category_shallow().get_id() == item_category_weapons;
}

// Which ammunition types the camp can supply.  Rebuilt once a turn: the answer
// is the same for every candidate weapon on every shelf, and working it out per
// candidate would walk the whole camp for each one.
const std::set<ammotype> &ammo_types_in_reach( Character &who )
{
    static std::set<ammotype> types;
    static time_point cached_turn = calendar::before_time_starts;
    static character_id cached_who;
    static unsigned int cached_generation = 0;
    if( cached_turn == calendar::turn && cached_who == who.getID() &&
        cached_generation == gear_up_cache_generation ) {
        return types;
    }
    cached_turn = calendar::turn;
    cached_who = who.getID();
    cached_generation = gear_up_cache_generation;
    types.clear();

    const auto note = [&]( const item & it ) {
        if( it.is_ammo() ) {
            types.insert( it.ammo_type() );
        } else if( it.is_magazine() ) {
            for( const ammotype &at : it.ammo_types() ) {
                types.insert( at );
            }
        }
    };
    who.visit_items( [&note]( const item * node, item * ) {
        note( *node );
        return VisitResponse::NEXT;
    } );
    map &here = get_map();
    for( const tripoint_abs_ms &tile : stores_within_reach( who ) ) {
        const tripoint_bub_ms bub = here.get_bub( tile );
        if( !here.inbounds( bub ) ) {
            continue;
        }
        for( item &it : here.i_at( bub ) ) {
            it.visit_items( [&note]( const item * node, item * ) {
                note( *node );
                return VisitResponse::NEXT;
            } );
        }
    }
    return types;
}

// Is there anything to feed this gun with, anywhere the character can get at?
// A gun already holding rounds needs no further justification.
bool gun_has_ammo_in_reach( Character &who, const item &gun )
{
    if( gun.ammo_remaining() > 0 ) {
        return true;
    }
    const std::set<ammotype> wanted = gun.ammo_types();
    if( wanted.empty() ) {
        // Needs no ammunition at all: a bionic weapon, a UPS tool.
        return true;
    }
    const std::set<ammotype> &available = ammo_types_in_reach( who );
    for( const ammotype &at : wanted ) {
        if( available.count( at ) > 0 ) {
            return true;
        }
    }
    return false;
}

// Every weapon judged on the same terms.  evaluate_weapon() caches its answer
// for the item in hand and that cached path ignores pretend_have_ammo, so an
// unloaded gun scores differently in hand than on the shelf and the two swap
// places turn after turn; a copy takes the uncached path.  Pretend ammo is the
// default because ammunition is chosen after the weapon, and a gun judged
// empty can never beat a knife.
double weapon_score( const Character &p, const item &it, bool pretend_ammo = true )
{
    if( p.is_wielding( it ) ) {
        const item copy = it;
        return p.evaluate_weapon( copy, pretend_ammo );
    }
    return p.evaluate_weapon( it, pretend_ammo );
}

// What bare hands are worth.  Depends only on body and skills, neither of
// which moves during a sweep, so once a turn rather than once per item.
double unarmed_score( const Character &p )
{
    static double score = 0.0;
    static time_point cached_turn = calendar::before_time_starts;
    static character_id cached_who;
    static unsigned int cached_generation = 0;
    if( cached_turn == calendar::turn && cached_who == p.getID() &&
        cached_generation == gear_up_cache_generation ) {
        return score;
    }
    cached_turn = calendar::turn;
    cached_who = p.getID();
    cached_generation = gear_up_cache_generation;
    score = p.evaluate_weapon( null_item_reference(), false );
    return score;
}

// The bar every candidate is measured against, worked out once a turn: the
// copy that dodges evaluate_weapon()'s cache is not free.
double current_weapon_score( Character &p )
{
    static double score = 0.0;
    static time_point cached_turn = calendar::before_time_starts;
    static character_id cached_who;
    static itype_id cached_weapon;
    static unsigned int cached_generation = 0;

    item_location wielded = p.get_wielded_item();
    const itype_id now = wielded ? wielded->typeId() : itype_id::NULL_ID();
    if( cached_turn == calendar::turn && cached_who == p.getID() && cached_weapon == now &&
        cached_generation == gear_up_cache_generation ) {
        return score;
    }
    cached_turn = calendar::turn;
    cached_who = p.getID();
    cached_weapon = now;
    cached_generation = gear_up_cache_generation;
    score = wielded ? weapon_score( p, *wielded ) : unarmed_score( p );
    return score;
}

// Everything asked of a weapon candidate except whether some other tile in
// reach holds a better one.  best_weapon_in_reach() scans with this, so this
// side must never call back into wants_as_weapon() or the two recurse.
bool is_weapon_upgrade( Character &p, const item &it )
{
    // A weapon this order already gave up gets no second look, or the two trade
    // places forever, each briefly ahead of the other.
    if( !is_weapon_candidate( it ) || p.gear_up_rejected.count( it.typeId() ) > 0 ||
        !p.can_wield( it ).success() ) {
        return false;
    }
    // What cannot be lifted is not an upgrade, and the hands would be emptied
    // for nothing.
    if( p.weight_carried() + it.weight() > p.weight_capacity() ) {
        return false;
    }
    // A gun with no round for it anywhere in the camp is a club with an awkward
    // grip.  Scored on pretend ammo it beats the knife already in hand.
    if( it.is_gun() && !gun_has_ammo_in_reach( p, it ) ) {
        return false;
    }
    return weapon_score( p, it ) > current_weapon_score( p );
}

// Walks every store tile in reach and remembers the single best pick, so a
// weapon or backup blade taken from the first crate visited cannot lose to
// one two tiles further on that this sweep would otherwise never compare it
// against.  Recomputed once a turn like ammo_types_in_reach(): stores get
// emptied and carried gear changes, and this walks the whole reachable area.
item_location best_candidate_in_reach( Character &p,
        const std::function<bool( Character &, const item & )> &wants,
        const std::function<double( Character &, const item & )> &value )
{
    item_location best;
    double best_value = 0.0;
    map &here = get_map();
    for( const tripoint_abs_ms &tile : stores_within_reach( p ) ) {
        const tripoint_bub_ms bub = here.get_bub( tile );
        if( !here.inbounds( bub ) ) {
            continue;
        }
        for( item_location &loc : candidates_at( p, bub ) ) {
            if( !loc || !wants( p, *loc ) ) {
                continue;
            }
            const double v = value( p, *loc );
            if( !best || v > best_value ) {
                best = loc;
                best_value = v;
            }
        }
    }
    return best;
}

item_location best_weapon_in_reach( Character &p )
{
    static item_location best;
    static time_point cached_turn = calendar::before_time_starts;
    static character_id cached_who;
    static unsigned int cached_generation = 0;
    if( cached_turn == calendar::turn && cached_who == p.getID() &&
        cached_generation == gear_up_cache_generation ) {
        return best;
    }
    cached_turn = calendar::turn;
    cached_who = p.getID();
    cached_generation = gear_up_cache_generation;
    best = best_candidate_in_reach( p, is_weapon_upgrade,
    []( Character & who, const item & it ) {
        return weapon_score( who, it );
    } );
    return best;
}

// Only the single best weapon anywhere in reach counts as wanted, or the
// character wields whatever mediocre gun the walk happens to reach first and
// never sees the better one sitting on the next shelf.
bool wants_as_weapon( Character &p, const item &it )
{
    return is_weapon_upgrade( p, it ) && best_weapon_in_reach( p ).get_item() == &it;
}

// Nobody who knows what they are doing walks out with a rifle and nothing else.
// Guns jam, run dry, and are useless when something is already on top of you.
bool needs_backup_blade( Character &p )
{
    const double fists = unarmed_score( p );
    item_location wielded = p.get_wielded_item();
    if( wielded && !wielded->is_gun() && weapon_score( p, *wielded, false ) > fists ) {
        return false;
    }
    bool found = false;
    p.visit_items( [&p, &found, fists]( const item * node, item * ) {
        if( is_weapon_candidate( *node ) && !node->is_gun() &&
            weapon_score( p, *node, false ) > fists && p.can_wield( *node ).success() ) {
            found = true;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    return !found;
}

// Everything asked of a backup-blade candidate except whether some other
// tile in reach holds a better one -- see is_weapon_upgrade() for why this
// side must not call wants_as_backup().
bool is_backup_upgrade( Character &p, const item &it )
{
    if( !is_weapon_candidate( it ) || it.is_gun() ||
        p.gear_up_rejected.count( it.typeId() ) > 0 || !p.can_wield( it ).success() ) {
        return false;
    }
    // It has to be carried to be a backup, and can_stash() only asks about
    // volume, so the weight is asked about here.
    if( p.weight_carried() + it.weight() > p.weight_capacity() || !p.can_stash( it ) ) {
        return false;
    }
    return weapon_score( p, it, false ) > unarmed_score( p );
}

item_location best_backup_in_reach( Character &p )
{
    static item_location best;
    static time_point cached_turn = calendar::before_time_starts;
    static character_id cached_who;
    static unsigned int cached_generation = 0;
    if( cached_turn == calendar::turn && cached_who == p.getID() &&
        cached_generation == gear_up_cache_generation ) {
        return best;
    }
    cached_turn = calendar::turn;
    cached_who = p.getID();
    cached_generation = gear_up_cache_generation;
    best = best_candidate_in_reach( p, is_backup_upgrade,
    []( Character & who, const item & it ) {
        return weapon_score( who, it, false );
    } );
    return best;
}

// Nothing beats a machete in a crate three tiles away like an umbrella in the
// crate at hand: only the single best backup anywhere in reach is wanted.
bool wants_as_backup( Character &p, const item &it )
{
    return is_backup_upgrade( p, it ) && best_backup_in_reach( p ).get_item() == &it;
}

// Everything asked of a garment candidate except whether some other tile in
// reach holds a better one -- see is_weapon_upgrade() for why this side must
// not call worth_trying_on().  Must ask the same questions try_one_garment()
// does, or the character walks to a crate and does nothing there, turn after
// turn.
bool is_garment_upgrade( Character &p, const item &it )
{
    if( !it.is_armor() || p.gear_up_rejected.count( it.typeId() ) > 0 ) {
        return false;
    }
    if( overheating_forbids_dressing( p ) || holds_a_favorite( it ) ) {
        return false;
    }
    // Contents stay in the camp, so only the empty weight is the wearer's.
    if( p.weight_carried() + it.weight( false ) > p.weight_capacity() ) {
        return false;
    }
    const int target_warmth = target_warmth_for( planning_temperature( p ) );
    const double candidate_proxy = wear_proxy( p, it, target_warmth );
    // What it has to beat: bare skin where it would cover any, nothing where
    // the skin under it is already clothed.  A parka in August still loses to
    // the shirt already on, and still wins against a bare back.
    const double bar = -weight_bare_skin * exposed_share( p, it ) *
                       ( it.get_avg_coverage() / 100.0 );
    if( candidate_proxy <= bar ) {
        return false;
    }
    // Straight onto an empty slot: can_wear() only forbids a second rigid
    // piece, so the soft redundancy is asked about separately.
    if( !conflicts_with_worn( p, it ) && p.can_wear( it ).success() ) {
        return true;
    }
    // Otherwise it has to beat something already worn on the same patch of skin.
    bool better_than_something = false;
    p.visit_items( [&]( const item * node, item * ) {
        if( !p.is_worn( *node ) || node->is_favorite || !shares_sub_part( *node, it ) ||
            !p.can_takeoff( *node ).success() ) {
            return VisitResponse::NEXT;
        }
        if( wear_proxy( p, *node, target_warmth ) < candidate_proxy ) {
            better_than_something = true;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    return better_than_something;
}

item_location best_garment_in_reach( Character &p )
{
    static item_location best;
    static time_point cached_turn = calendar::before_time_starts;
    static character_id cached_who;
    static unsigned int cached_generation = 0;
    if( cached_turn == calendar::turn && cached_who == p.getID() &&
        cached_generation == gear_up_cache_generation ) {
        return best;
    }
    cached_turn = calendar::turn;
    cached_who = p.getID();
    cached_generation = gear_up_cache_generation;
    best = best_candidate_in_reach( p, is_garment_upgrade,
    []( Character & who, const item & it ) {
        return wear_proxy( who, it, target_warmth_for( planning_temperature( who ) ) );
    } );
    return best;
}

bool needs_storage( const Character &p )
{
    return units::to_liter( p.volume_capacity() ) < want_storage_liters_floor;
}

item_location best_storage_in_reach( Character &p )
{
    static item_location best;
    static time_point cached_turn = calendar::before_time_starts;
    static character_id cached_who;
    static unsigned int cached_generation = 0;
    if( cached_turn == calendar::turn && cached_who == p.getID() &&
        cached_generation == gear_up_cache_generation ) {
        return best;
    }
    cached_turn = calendar::turn;
    cached_who = p.getID();
    cached_generation = gear_up_cache_generation;
    best = best_candidate_in_reach( p, []( Character & who, const item & it ) {
        return it.get_volume_capacity() > 0_ml && is_garment_upgrade( who, it );
    },
    []( Character & who, const item & it ) {
        return wear_proxy( who, it, target_warmth_for( planning_temperature( who ) ) );
    } );
    return best;
}

// Only the single best garment anywhere in reach counts as wanted: comparing
// only what one tile happens to offer against whatever is already worn is how
// a character ends up trying on and shedding half the camp's wardrobe walking
// past crate after crate, never seeing that a better piece was two tiles back.
// Processed one at a time in strictly improving order this way, several
// non-conflicting pieces still end up worn -- each pass finds whatever is now
// the single best remaining upgrade, worn or not, anywhere in reach.  While
// storage is critically low, the search narrows to pocket-bearing candidates
// only, unless the camp genuinely has none to offer.
bool worth_trying_on( Character &p, const item &it )
{
    if( needs_storage( p ) ) {
        item_location storage_pick = best_storage_in_reach( p );
        if( storage_pick ) {
            return storage_pick.get_item() == &it && is_garment_upgrade( p, it );
        }
    }
    return is_garment_upgrade( p, it ) && best_garment_in_reach( p ).get_item() == &it;
}

bool wants_magazine( Character &p, const item &it )
{
    item_location wielded = p.get_wielded_item();
    if( !wielded || !wielded->is_gun() || wielded->magazine_integral() || !it.is_magazine() ) {
        return false;
    }
    const std::set<itype_id> compatible = wielded->magazine_compatible();
    if( compatible.count( it.typeId() ) == 0 ) {
        return false;
    }
    const int have = count_carried( p, [&compatible]( const item & carried ) {
        return carried.is_magazine() && compatible.count( carried.typeId() ) > 0;
    } );
    return have < want_spare_magazines;
}

int ammo_shortfall( Character &p )
{
    item_location wielded = p.get_wielded_item();
    if( !wielded || !wielded->is_gun() ) {
        return 0;
    }
    const std::set<ammotype> types = wielded->ammo_types();
    if( types.empty() ) {
        return 0;
    }
    // An empty magazine-fed gun reports no capacity of its own, so take the
    // figure from a magazine actually being carried.
    int capacity = wielded->ammo_capacity( *types.begin() );
    if( capacity <= 0 && !wielded->magazine_integral() ) {
        const std::set<itype_id> compatible = wielded->magazine_compatible();
        p.visit_items( [&capacity, &types, &compatible]( const item * node, item * ) {
            if( node->is_magazine() && compatible.count( node->typeId() ) > 0 ) {
                capacity = std::max( capacity, node->ammo_capacity( *types.begin() ) );
            }
            return VisitResponse::NEXT;
        } );
    }
    capacity = std::max( 1, capacity );
    const int have = count_carried( p, [&types]( const item & carried ) {
        return carried.is_ammo() && types.count( carried.ammo_type() ) > 0;
    } );
    return std::max( 0, capacity * want_ammo_loads - have );
}

bool wants_ammo( Character &p, const item &it )
{
    if( !it.is_ammo() ) {
        return false;
    }
    item_location wielded = p.get_wielded_item();
    if( !wielded || !wielded->is_gun() ) {
        return false;
    }
    return wielded->ammo_types().count( it.ammo_type() ) > 0 && ammo_shortfall( p ) > 0;
}

bool wants_medical( Character &p, const item &it )
{
    if( is_healing_item( it ) ) {
        return count_carried( p, is_healing_item ) < want_healing_items;
    }
    if( is_painkiller( it ) ) {
        return count_carried( p, is_painkiller ) < want_painkillers;
    }
    return false;
}

void carried_nutrition( const Character &who, int &kcal, int &quench )
{
    kcal = 0;
    quench = 0;
    who.visit_items( [&kcal, &quench]( const item * node, item * ) {
        if( node->is_food() ) {
            const auto &carried = node->get_comestible();
            if( carried ) {
                kcal += carried->default_nutrition_read_only().kcal() * node->count();
                quench += carried->quench * node->count();
            }
        }
        return VisitResponse::NEXT;
    } );
}

// With the NPC needs mod active NPCs neither eat nor drink, so handing them
// rations would be pure clutter.  needs_food() is the same gate the behaviour
// tree uses, so this stays aligned with the mod rather than fighting it.
bool wants_rations( Character &p, const item &it )
{
    if( !p.needs_food() || !it.is_food() ) {
        return false;
    }
    const auto &com = it.get_comestible();
    if( !com || !p.will_eat( it ).success() ) {
        return false;
    }
    int kcal = 0;
    int quench = 0;
    carried_nutrition( p, kcal, quench );
    if( kcal < want_kcal && com->default_nutrition_read_only().kcal() > 0 ) {
        return true;
    }
    return quench < want_quench && com->quench > 0;
}

// Everything in it is something worth drinking: taking a vessel means taking
// all of it, so a jug of water counts and a toolbox with a bottle rattling
// around in it does not.
bool holds_only_drink( Character &p, const item &it )
{
    if( it.is_food() || it.made_of( phase_id::LIQUID ) ) {
        return false;
    }
    const std::list<const item *> contents = it.all_items_top( pocket_type::CONTAINER );
    if( contents.empty() ) {
        return false;
    }
    for( const item *inner : contents ) {
        if( inner->is_favorite || !inner->is_food() ) {
            return false;
        }
        const auto &com = inner->get_comestible();
        if( !com || com->quench <= 0 || !p.will_eat( *inner ).success() ) {
            return false;
        }
    }
    return true;
}

// Loose liquid cannot be carried, and a camp keeps its water in canteens and
// jugs, so the drink has to be taken by the vessel holding it.  Bounded by the
// count as well as by the target: a camp keeping its water in twenty small
// bottles should not be stripped of all twenty.
bool wants_as_drink( Character &p, const item &it )
{
    if( !p.needs_food() || !holds_only_drink( p, it ) ) {
        return false;
    }
    int kcal = 0;
    int quench = 0;
    carried_nutrition( p, kcal, quench );
    if( quench >= want_quench ) {
        return false;
    }
    const int have = count_carried( p, [&p]( const item & carried ) {
        return holds_only_drink( p, carried );
    } );
    return have < want_drink_vessels;
}

// How many of this ration close the gap to the food and water targets.  The
// whole stack would be the camp's larder, not one fighter's day.
int rations_wanted( const Character &p, const item &it )
{
    const auto &com = it.get_comestible();
    if( !com ) {
        return 0;
    }
    int kcal = 0;
    int quench = 0;
    carried_nutrition( p, kcal, quench );
    int want = 0;
    const int per_kcal = com->default_nutrition_read_only().kcal();
    if( per_kcal > 0 && kcal < want_kcal ) {
        want = ( want_kcal - kcal + per_kcal - 1 ) / per_kcal;
    }
    if( com->quench > 0 && quench < want_quench ) {
        want = std::max( want, ( want_quench - quench + com->quench - 1 ) / com->quench );
    }
    return want;
}

bool wanted_for_stage( Character &p, const item &it, gear_stage stage )
{
    if( stage == gear_stage::equipment ) {
        // needs_backup_blade() first: it only walks carried gear, where
        // wants_as_backup() scans every store tile in reach for the best
        // candidate, and this runs for every item in the camp.
        return wants_as_weapon( p, it ) ||
               ( needs_backup_blade( p ) && wants_as_backup( p, it ) ) ||
               worth_trying_on( p, it );
    }
    // A supply that could not be carried (no room, too heavy) never stops
    // being wanted on its own; only this memory ends the sweep.
    if( p.gear_up_rejected.count( it.typeId() ) > 0 ) {
        return false;
    }
    return wants_magazine( p, it ) || wants_ammo( p, it ) ||
           wants_medical( p, it ) || wants_rations( p, it ) || wants_as_drink( p, it );
}

// Mirrors collect_from()'s descent exactly, including where it stops: a want
// seen deeper than the pool ever reaches would be a tile walked to and nothing
// done there, every turn, forever.
bool pile_has_anything_wanted( Character &p, const item &it, gear_stage stage, int depth )
{
    if( wanted_for_stage( p, it, stage ) ) {
        return true;
    }
    if( depth > max_nesting_depth ) {
        return false;
    }
    for( const item *inner : it.all_items_top( pocket_type::CONTAINER ) ) {
        if( !off_limits_item( p, *inner ) &&
            pile_has_anything_wanted( p, *inner, stage, depth + 1 ) ) {
            return true;
        }
    }
    return false;
}

// Cheap tile test for "is this worth walking to".  Bails on the first hit
// rather than building and sorting a pool: with nesting, one storage tile can
// expose hundreds of items, and this runs for every location every turn.
bool tile_has_anything_wanted( Character &p, const tripoint_bub_ms &tile, gear_stage stage )
{
    map &here = get_map();
    if( !here.inbounds( tile ) || !here.sees_some_items( tile, p ) ||
        tile_is_off_limits( p, tile ) ) {
        return false;
    }
    for( item &it : here.i_at( tile ) ) {
        if( !off_limits_item( p, it ) && pile_has_anything_wanted( p, it, stage, 1 ) ) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Acting on one tile
// ---------------------------------------------------------------------------

// Putting on a sports bag and inheriting seventy-six shirts is not gearing up:
// the contents go back into the camp's zones, not onto someone's back.
void empty_where_it_stands( Character &who, item_location &loc, const tripoint_bub_ms &tile )
{
    map &here = get_map();
    const std::list<item *> contents = loc->all_items_top( pocket_type::CONTAINER );
    int moved = 0;
    for( item *inner : contents ) {
        const item copy = *inner;
        const std::optional<tripoint_bub_ms> home = home_for( who, copy, tile );
        if( !home || here.add_item_or_charges( *home, copy ).is_null() ) {
            continue;
        }
        loc->remove_item( *inner );
        moved++;
    }
    if( moved > 0 ) {
        who.mod_moves( -moved * handle_cost_moves );
    }
}

// What was in the old pack goes into the new one, and what will not fit goes
// into storage.  A swapped-out pack is never discarded with its contents in it.
void transfer_contents( Character &who, item &from, item &to, const tripoint_bub_ms &tile )
{
    map &here = get_map();
    const std::list<item *> contents = from.all_items_top( pocket_type::CONTAINER );
    for( item *inner : contents ) {
        const item copy = *inner;
        if( to.can_contain( copy ).success() ) {
            from.remove_item( *inner );
            to.put_in( copy, pocket_type::CONTAINER );
            who.mod_moves( -handle_cost_moves );
            continue;
        }
        if( who.can_stash( copy ) && who.i_add( copy ) ) {
            from.remove_item( *inner );
            who.mod_moves( -handle_cost_moves );
            continue;
        }
        const std::optional<tripoint_bub_ms> home = home_for( who, copy, tile );
        if( home && !here.add_item_or_charges( *home, copy ).is_null() ) {
            from.remove_item( *inner );
            who.mod_moves( -handle_cost_moves );
        }
    }
}

// Keep a garment only if it beats an empty slot or the specific piece it
// displaces, never the whole outfit.  What physically fits -- pocket length,
// body size, rigid conflicts, power armour -- stays the engine's call.
// Returns true if something changed.
bool try_one_garment( Character &p, item_location &loc, const tripoint_bub_ms &tile )
{
    const int target_warmth = target_warmth_for( planning_temperature( p ) );
    const itype_id candidate_type = loc->typeId();

    // Judged and weighed as it would be worn: empty.  It is only actually
    // emptied once the decision is made, so a coat turned down leaves the
    // camp's crates as it found them.
    const item &on_the_shelf = *loc;
    const double candidate_proxy = wear_proxy( p, on_the_shelf, target_warmth );
    const units::mass candidate_weight = on_the_shelf.weight( false );
    p.mod_moves( -p.item_wear_cost( on_the_shelf ) );

    // Onto an empty slot.  The pre-filter already confirmed a positive score,
    // so fitting is the only question left.
    if( !conflicts_with_worn( p, on_the_shelf ) && p.can_wear( on_the_shelf ).success() &&
        p.weight_carried() + candidate_weight <= p.weight_capacity() ) {
        empty_where_it_stands( p, loc, tile );
        // Weighed again on what is left: a camp with nowhere to put the
        // contents hands over a bag that is still full, and that weight is
        // the wearer's problem, not the estimate's.
        if( !loc || p.weight_carried() + loc->weight() > p.weight_capacity() ) {
            p.gear_up_rejected.insert( candidate_type );
            return false;
        }
        const item candidate = *loc;
        std::optional<std::list<item>::iterator> worn_it =
            p.wear_item( candidate, false, true, true, true );
        if( worn_it ) {
            loc.remove_item();
            p.add_msg_player_or_npc( m_good, _( "You put on the %s." ),
                                     _( "<npcname> puts on a %s." ), candidate.tname() );
            return true;
        }
        p.gear_up_rejected.insert( candidate_type );
        return false;
    }

    // Otherwise displace the worst piece already on that patch of skin, if this
    // beats it.
    item_location replace_target;
    double replace_proxy = 0.0;
    for( item_location &worn_loc : p.all_items_loc() ) {
        if( !worn_loc || !p.is_worn( *worn_loc ) ) {
            continue;
        }
        const item &worn = *worn_loc;
        if( worn.is_favorite || !shares_sub_part( worn, on_the_shelf ) ||
            !p.can_takeoff( worn ).success() ) {
            continue;
        }
        const double proxy = wear_proxy( p, worn, target_warmth );
        if( proxy >= candidate_proxy ) {
            continue;
        }
        if( !replace_target || proxy < replace_proxy ) {
            replace_target = worn_loc;
            replace_proxy = proxy;
        }
    }
    if( !replace_target ) {
        p.gear_up_rejected.insert( candidate_type );
        return false;
    }

    // The old piece comes off first: two rigid pieces cannot share a sub-part
    // even for a moment, so wearing the new one first would quietly veto every
    // helmet-for-helmet upgrade.  It keeps its contents while it waits in the
    // takeoff list, so nothing is homeless in between.
    const item displaced = *replace_target;
    std::list<item> removed;
    if( !quiet_takeoff( p, replace_target, removed ) || removed.empty() ) {
        p.gear_up_rejected.insert( candidate_type );
        return false;
    }
    item &old_worn = removed.front();

    // Only now is the candidate emptied.  Everything that could still turn this
    // swap down has been asked, so a garment left on the shelf leaves the
    // camp's crates as it found them.
    empty_where_it_stands( p, loc, tile );

    std::optional<std::list<item>::iterator> worn_it;
    if( loc && p.weight_carried() + loc->weight() <= p.weight_capacity() ) {
        worn_it = p.wear_item( *loc, false, true, true, true );
    }
    if( !worn_it ) {
        // The new one would not go on after all, so the old one goes straight
        // back on, contents untouched.
        if( !p.wear_item( old_worn, false, true, true, true ) &&
            !put_away( p, old_worn, tile ) ) {
            get_map().add_item_or_charges( tile, old_worn );
        }
        p.gear_up_rejected.insert( candidate_type );
        return false;
    }
    const item candidate = **worn_it;
    transfer_contents( p, old_worn, **worn_it, tile );

    loc.remove_item();
    // The swap moved the totals wear_proxy() scores against, so the displaced
    // piece can look attractive again and trade places with its replacement
    // forever without this.
    p.gear_up_rejected.insert( displaced.typeId() );
    // Back into its zone, and failing that down on the crate it was traded at:
    // it exists only in the takeoff list here, so anything less loses it.
    if( !put_away( p, old_worn, tile ) ) {
        get_map().add_item_or_charges( tile, old_worn );
    }
    p.add_msg_player_or_npc( m_good, _( "You swap your %1$s for the %2$s." ),
                             _( "<npcname> swaps their %1$s for a %2$s." ),
                             displaced.tname(), candidate.tname() );
    return true;
}

bool do_equipment_stage( Character &p, const tripoint_bub_ms &tile )
{
    std::vector<item_location> pool = candidates_at( p, tile );

    // Weapon first: the weapon decides which ammunition is coherent later.
    // At most one entry in this tile's pool can be the camp's single best
    // weapon, wants_as_weapon() having already ruled out every runner-up.
    item_location best;
    for( item_location &loc : pool ) {
        if( loc && wants_as_weapon( p, *loc ) ) {
            best = loc;
            break;
        }
    }
    if( best ) {
        const std::string taken = best->tname();
        const itype_id taken_type = best->typeId();
        item_location wielded = p.get_wielded_item();
        if( wielded ) {
            const item old = *wielded;
            if( !p.can_unwield( old ).success() ) {
                // The candidate can never be taken while the hands are stuck,
                // so remember it or this repeats every turn.
                p.gear_up_rejected.insert( taken_type );
                p.add_msg_player_or_npc( m_warning, _( "You can't let go of your %s." ),
                                         _( "<npcname> can't let go of their %s." ), old.tname() );
                return true;
            }
            // Empty the hands here: the wield path is allowed to stow the old
            // weapon anywhere, up to and including the ground under it.  Into
            // storage, and failing that down on the crate being traded at.
            item removed = p.remove_weapon();
            if( !put_away( p, removed, tile ) ) {
                get_map().add_item_or_charges( tile, removed );
            }
            if( !wield_loc( p, best ) ) {
                // Hands are empty and the new weapon would not come: take the
                // old one back rather than walk away unarmed.
                p.gear_up_rejected.insert( taken_type );
                item_location recovered = find_carried( p, old.typeId() );
                if( recovered ) {
                    wield_loc( p, recovered );
                }
                return true;
            }
            // The old weapon is not blacklisted here: weapon_score() scores a
            // wielded item and a shelved one on identical, uncached terms, so a
            // strictly worse weapon stays strictly worse and cannot trade places
            // with its replacement.  Blacklisting it would only cost the one
            // role gear_up_rejected does not distinguish -- the same item as a
            // candidate backup blade, back on a store tile where it belongs.
        } else if( !wield_loc( p, best ) ) {
            p.gear_up_rejected.insert( taken_type );
            return true;
        }
        // npc::wield announces itself; only the avatar's side needs saying.
        p.add_msg_if_player( m_good, _( "You take up the %s." ), taken );
        return true;
    }

    if( needs_backup_blade( p ) ) {
        // Same reasoning as the weapon above: at most one entry here can be
        // the camp's single best backup blade.
        item_location blade;
        for( item_location &loc : pool ) {
            if( loc && wants_as_backup( p, *loc ) ) {
                blade = loc;
                break;
            }
        }
        if( blade ) {
            const std::string taken = blade->tname();
            const itype_id blade_type = blade->typeId();
            if( take_into_inventory( p, blade ) ) {
                p.add_msg_player_or_npc( m_good, _( "You take the %s as a backup." ),
                                         _( "<npcname> takes a %s as a backup." ), taken );
                return true;
            }
            p.gear_up_rejected.insert( blade_type );
        }
    }

    // Then clothing: at most one entry in this tile's pool can be the single
    // best garment anywhere in reach, worth_trying_on() having already ruled
    // out every runner-up.  Nothing to sort between candidates that compete
    // with each other any more -- only whether the winner happens to be here.
    for( item_location &loc : pool ) {
        if( loc && worth_trying_on( p, *loc ) ) {
            return try_one_garment( p, loc, tile );
        }
    }
    return false;
}

bool do_supply_stage( Character &p, const tripoint_bub_ms &tile )
{
    std::vector<item_location> pool = candidates_at( p, tile );
    bool did_something = false;

    for( item_location &loc : pool ) {
        if( !loc || !wants_magazine( p, *loc ) ) {
            continue;
        }
        const std::string name = loc->tname();
        const itype_id type = loc->typeId();
        if( take_into_inventory( p, loc ) ) {
            p.add_msg_player_or_npc( m_good, _( "You pick up the %s." ),
                                     _( "<npcname> picks up a %s." ), name );
            did_something = true;
        } else {
            p.gear_up_rejected.insert( type );
        }
    }

    int taken = 0;
    for( item_location &loc : pool ) {
        const int want = ammo_shortfall( p );
        if( want <= 0 ) {
            break;
        }
        if( !loc || !wants_ammo( p, *loc ) ) {
            continue;
        }
        const itype_id type = loc->typeId();
        const int got = take_charges( p, loc, want );
        if( got > 0 ) {
            taken += got;
        } else {
            p.gear_up_rejected.insert( type );
        }
    }
    if( taken > 0 ) {
        p.add_msg_player_or_npc( m_good, n_gettext( "You take %d round of ammunition.",
                                 "You take %d rounds of ammunition.", taken ),
                                 n_gettext( "<npcname> takes %d round of ammunition.",
                                            "<npcname> takes %d rounds of ammunition.", taken ), taken );
        did_something = true;
    }

    for( item_location &loc : pool ) {
        if( !loc || !wants_medical( p, *loc ) ) {
            continue;
        }
        const bool healing = is_healing_item( *loc );
        const int want = healing
                         ? want_healing_items - count_carried( p, is_healing_item )
                         : want_painkillers - count_carried( p, is_painkiller );
        const std::string name = loc->tname();
        const itype_id type = loc->typeId();
        if( take_charges( p, loc, want ) > 0 ) {
            p.add_msg_player_or_npc( m_good, _( "You take the %s." ),
                                     _( "<npcname> takes a %s." ), name );
            did_something = true;
        } else {
            p.gear_up_rejected.insert( type );
        }
    }

    for( item_location &loc : pool ) {
        if( !loc || !wants_rations( p, *loc ) ) {
            continue;
        }
        // A day's food, not the camp's larder: take only what closes the gap.
        const int want = rations_wanted( p, *loc );
        const std::string name = loc->tname();
        const itype_id type = loc->typeId();
        if( want > 0 && take_charges( p, loc, want ) > 0 ) {
            p.add_msg_player_or_npc( m_good, _( "You pack the %s." ),
                                     _( "<npcname> packs a %s." ), name );
            did_something = true;
        } else {
            p.gear_up_rejected.insert( type );
        }
    }

    // Water comes by the canteen, so the whole vessel goes along.
    for( item_location &loc : pool ) {
        if( !loc || !wants_as_drink( p, *loc ) ) {
            continue;
        }
        const std::string name = loc->tname();
        const itype_id type = loc->typeId();
        if( take_into_inventory( p, loc ) ) {
            p.add_msg_player_or_npc( m_good, _( "You pack the %s." ),
                                     _( "<npcname> packs a %s." ), name );
            did_something = true;
        } else {
            p.gear_up_rejected.insert( type );
        }
    }

    // Walk into the fight with a full magazine, not just a full pocket.  Two
    // steps, because a magazine-fed weapon needs both: loose rounds go into the
    // magazines first, and only then does a loaded magazine go into the gun.
    item_location wielded = p.get_wielded_item();
    if( wielded && wielded->is_gun() ) {
        if( !wielded->magazine_integral() ) {
            const std::set<itype_id> compatible = wielded->magazine_compatible();
            for( item_location &carried : p.all_items_loc() ) {
                if( !carried || !carried->is_magazine() ||
                    compatible.count( carried->typeId() ) == 0 || carried->is_magazine_full() ) {
                    continue;
                }
                if( try_reload( p, carried ) ) {
                    did_something = true;
                }
            }
        }
        item_location to_load = p.get_wielded_item();
        if( to_load && try_reload( p, to_load ) ) {
            did_something = true;
        }
    }

    return did_something;
}

} // namespace

// ---------------------------------------------------------------------------
// The activity
// ---------------------------------------------------------------------------

namespace
{

// Every store tile that still offers something for the given stage.  No
// pruning: callers that route a character there prune, callers that only ask
// "is this stage finished" must not, or a dark corner would read as done.
std::unordered_set<tripoint_abs_ms> tiles_wanted_for( Character &you, gear_stage stage )
{
    map &here = get_map();
    std::unordered_set<tripoint_abs_ms> wanted;
    for( const tripoint_abs_ms &tile : stores_within_reach( you ) ) {
        const tripoint_bub_ms bub = here.get_bub( tile );
        if( !here.inbounds( bub ) ) {
            // Outside the reality bubble; let the framework route there and
            // decide once it can actually see the tile.
            wanted.emplace( tile );
            continue;
        }
        if( tile_has_anything_wanted( you, bub, stage ) ) {
            wanted.emplace( tile );
        }
    }
    return wanted;
}

void report_gear_up_finished( Character &you )
{
    if( you.gear_up_done_reported ) {
        return;
    }
    you.gear_up_done_reported = true;
    if( npc *p = you.as_npc() ) {
        // Unconditional, unlike add_msg_player_or_npc: whether an order the
        // player gave is finished is worth knowing off-screen too, not only
        // at the moment the player happens to be looking at whoever got it.
        p->add_msg_if_npc( m_good, _( "<npcname> finishes gearing up from the stores." ) );
    } else {
        you.add_msg_if_player( m_good, _( "You finish gearing up from the stores." ) );
    }
}

} // namespace

std::unordered_set<tripoint_abs_ms> multi_gear_up_activity_actor::multi_activity_locations(
    Character &you )
{
    // Ammunition is only coherent once the weapon is final, so the equipment
    // stage has to be exhausted everywhere before the supply stage can begin.
    std::unordered_set<tripoint_abs_ms> wanted =
        tiles_wanted_for( you, static_cast<gear_stage>( you.gear_up_stage ) );
    if( wanted.empty() && you.gear_up_stage == static_cast<int>( gear_stage::equipment ) ) {
        you.gear_up_stage = static_cast<int>( gear_stage::supplies );
        wanted = tiles_wanted_for( you, gear_stage::supplies );
    }

    // Empty before pruning means finished; empty after it could just be a
    // dangerous field this turn.  The distinction has to be taken here.
    const bool order_complete =
        wanted.empty() && you.gear_up_stage == static_cast<int>( gear_stage::supplies );
    multi_activity_actor::prune_dangerous_field_locations( wanted );
    // Guarded the way generic_locations guards it.  A camp's store room is
    // usually windowless and loot sorting is allowed there, so this is too.
    if( !multi_activity_actor::can_do_in_dark( get_type() ) ) {
        multi_activity_actor::prune_dark_locations( you, wanted, get_type() );
    }
    if( order_complete ) {
        report_gear_up_finished( you );
    }
    return wanted;
}

// The base class ends the activity the moment a sweep comes up empty, and its
// per-turn source cache short-circuits multi_activity_locations(), which is
// where the equipment stage would otherwise hand over to supplies.  So the
// handover happens here, on the way out: without it a character whose last
// equipment find was on the last crate leaves with no ammunition, no bandages
// and no rations.
//
// The base class's ending rule is mirrored rather than called, because clearing
// the activity destroys this actor and everything after that has to be decided
// beforehand.
void multi_gear_up_activity_actor::do_turn( player_activity &act, Character &you )
{
    const activity_id prior_act = get_type();
    const bool activity_continues = simulate_turn( act, you, false );
    const bool travelling = you.has_destination();
    // Deliberately not the same question as whether there is an activity left to
    // clear: an NPC whose sweep runs out has already been reverted from under us
    // inside simulate_turn, which is exactly the case the handover exists for.
    const bool swept_out = !activity_continues && !travelling;
    const bool clearing = ( travelling || !activity_continues ) &&
                          !act.is_null() && prior_act == you.activity.id();

    bool resume = false;
    if( swept_out && you.gear_up_stage == static_cast<int>( gear_stage::equipment ) ) {
        you.gear_up_stage = static_cast<int>( gear_stage::supplies );
        resume = !tiles_wanted_for( you, gear_stage::supplies ).empty();
    }
    // Say so only when nothing anywhere is still wanted: ending because of a
    // blocked path is not the same as being finished.
    const bool finished = swept_out && !resume &&
                          tiles_wanted_for( you, static_cast<gear_stage>( you.gear_up_stage ) ).empty();

    if( clearing ) {
        // Destroys this actor.  Nothing below may touch `this` or `act`.
        you.activity = player_activity();
    }
    if( resume ) {
        you.assign_activity( multi_gear_up_activity_actor() );
    } else if( finished ) {
        report_gear_up_finished( you );
    }
}

activity_reason_info multi_gear_up_activity_actor::multi_activity_can_do( Character &you,
        const tripoint_bub_ms &src_loc )
{
    const gear_stage stage = static_cast<gear_stage>( you.gear_up_stage );
    if( !tile_has_anything_wanted( you, src_loc, stage ) ) {
        return activity_reason_info::fail( do_activity_reason::ALREADY_DONE );
    }
    return activity_reason_info::ok( do_activity_reason::NEEDS_GEAR_UP );
}

bool multi_gear_up_activity_actor::multi_activity_do( Character &you,
        const activity_reason_info &, const tripoint_abs_ms &, const tripoint_bub_ms &src_loc )
{
    // Going through a crate costs time whether or not anything comes of it.
    you.mod_moves( -search_cost_moves );

    const bool did_something =
        static_cast<gear_stage>( you.gear_up_stage ) == gear_stage::equipment
        ? do_equipment_stage( you, src_loc )
        : do_supply_stage( you, src_loc );

    // The per-turn scans name one specific item as the thing to go and get,
    // and working a tile invalidates that answer: the item is off the shelf,
    // or its type has just been turned down.  Either way every later question
    // this turn would match nothing and read as "nothing wanted anywhere",
    // which ends the sweep a couple of items in.  The scans still only run
    // once per pass over the locations, which is where the cost is.
    reset_gear_up_caches();

    if( npc *p = you.as_npc() ) {
        // Let the NPC's own AI re-examine what it is now carrying; it scores
        // weapons with the same function used here, so it will agree.
        p->has_new_items = true;
        p->invalidate_range_cache();
    }

    // false keeps the activity alive so a crate with several useful things is
    // not abandoned after one; true when nothing happened is the only thing
    // that lets the sweep run out and end.
    return !did_something;
}

bool gear_up_stores_available( Character &who )
{
    return !stores_within_reach( who ).empty();
}

void reset_gear_up_caches()
{
    gear_up_cache_generation++;
}

void start_gear_up_from_stores( Character &who )
{
    // Reset the sweep here rather than in the actor's start(): multi-zone
    // actors are cloned through the backlog and restarted every turn, so state
    // reset there would drop back to the first stage forever.  A fresh order
    // reconsiders everything turned down last time -- the weather has moved on,
    // and so has what is in the crates.
    who.gear_up_rejected.clear();
    who.gear_up_stage = static_cast<int>( gear_stage::equipment );
    who.gear_up_done_reported = false;
    who.assign_activity( multi_gear_up_activity_actor() );
}

// Rummaging through crates with something hunting you is how people die.  The
// avatar is trusted to make that call themselves; an NPC given the order is
// not, because the player cannot see what the NPC can.
static bool hostiles_in_sight( npc &p )
{
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
        return true;
    }
    return false;
}

void talk_function::gear_up_from_stores( npc &p )
{
    if( p.is_hallucination() || hostiles_in_sight( p ) ) {
        return;
    }

    if( !gear_up_stores_available( p ) ) {
        add_msg( m_info, _( "%s has no loot or camp storage zone in range to draw from." ),
                 p.get_name() );
        return;
    }

    // Preparing for a fight is reason enough to cut sleep short: an order
    // assigned to a sleeping follower otherwise sits there unstarted, since
    // the sleeping AI does not act on an activity at all.
    if( p.in_sleep_state() ) {
        wake_up( p );
    }

    start_gear_up_from_stores( p );
}
