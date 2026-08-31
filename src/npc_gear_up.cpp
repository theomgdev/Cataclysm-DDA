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
#include "item_category.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "line.h"
#include "map.h"
#include "map_selector.h"
#include "messages.h"
#include "npc.h"
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
 * "Gear up from the camp stores": the character walks the faction's loot and
 * camp zones tile by tile, like sorting or building, and equips from what is
 * actually stored there -- including inside containers, which is where a
 * sorted camp keeps things.  Displaced gear goes back to the zone the zone
 * manager says it belongs in, so the job leaves the camp sorted.
 *
 * Two stages: equipment first (weapon, backup blade, clothing), supplies
 * second (magazines, ammunition, medical, rations).  The stage only advances
 * once no tile offers an equipment improvement, so the weapon is final before
 * ammunition for it is picked, or an archer ends up carrying pistol rounds.
 *
 * Termination hinges on gear_up_rejected: item types tried and turned down --
 * or that could not be carried -- are remembered on the character, because the
 * engine's loop detector cannot see a character walking back to the same
 * crate forever when every lap spends moves.
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

// Supply targets.  Concrete numbers rather than "a little", so that falling
// short of them is reportable and testable.
constexpr int want_healing_items = 3;
constexpr int want_painkillers = 4;
constexpr int want_kcal = 2400;
constexpr int want_quench = 400;
constexpr int want_ammo_loads = 3;
constexpr int want_spare_magazines = 2;

// Carrying capacity worth having: ammunition, a medical kit, a day of food
// and water, some salvage.  Past that a liter of pocket keeps a tenth of its
// credit, or storage outscores real armour once a character has a bag or two.
constexpr double want_storage_liters = 25.0;
constexpr double weight_storage_per_liter = 0.30;
constexpr double weight_storage_marginal = weight_storage_per_liter * 0.1;

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

// Same patch of skin?  Sub-part granularity, the same the engine's own
// conflict rule uses (outfit::check_rigid_conflicts): kneepads and shin
// guards share a leg but not a sub-part.
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
        for( const sub_bodypart_id &sbp : cand_parts ) {
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

double local_encumbrance_weight( const item &it )
{
    for( const char *const bp_id : { "leg_l", "leg_r" } ) {
        if( it.covers( bodypart_id( bp_id ) ) ) {
            return weight_leg_encumbrance;
        }
    }
    for( const char *const bp_id : { "eyes", "mouth" } ) {
        if( it.covers( bodypart_id( bp_id ) ) ) {
            return weight_sense_encumbrance;
        }
    }
    return 1.0;
}

// The score clothing decisions are made on: local to the candidate's own
// coverage, never a whole-body average -- averaging is what let leg armour
// vanish next to an unrelated backpack's storage term.  Fit and sizing need
// no term of their own: get_avg_encumber() already charges wrong-size and
// unfitted garments extra, so they lose here automatically.
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

    const double warmth_gap = target_warmth - mean_warmth_of( who );
    const double warmth = warmth_gap > 0
                          ? std::min<double>( it.get_warmth(), warmth_gap ) * weight_warmth_needed
                          : -it.get_warmth() * weight_warmth_excess;

    double proxy = resist + storage - enc + warmth + it.get_env_resist() * 0.2;
    if( it.is_filthy() ) {
        proxy -= filth_penalty;
    }
    return proxy;
}

// Body temperature beats every other consideration, in both directions.  The
// cold side is the predicate the behaviour tree already uses; the hot side
// matters just as much, because the finest set of plate in the county is
// worthless to someone who collapses from heatstroke wearing it.
bool temperature_forbids_dressing( const Character &who )
{
    for( const bodypart_id &bp : who.get_all_body_parts() ) {
        const units::temperature part_temp = who.get_part_temp_conv( bp );
        if( part_temp <= BODYTEMP_VERY_COLD || part_temp >= BODYTEMP_VERY_HOT ) {
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
    if( g->check_zone( zone_type_NO_NPC_PICKUP, tile ) ||
        g->check_zone( zone_type_LOOT_IGNORE, tile ) ) {
        return true;
    }
    const npc *guy = who.as_npc();
    return guy && guy->is_no_go_position( get_map().get_abs( tile ) );
}

// Every loot zone the faction owns, plus the basecamp's own storage and food
// zones, which do not carry the LOOT prefix.  Specific zones and the big
// general one laid over them overlap heavily; the set deduplicates.
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

// Put a displaced item somewhere sensible: inventory first, then the zone it
// belongs in.  Nothing this job does ever leaves gear lying in the mud, and a
// swap that cannot find a home for the old piece simply does not happen.
bool put_away( Character &who, const item &it, const tripoint_bub_ms &fallback )
{
    if( who.can_stash( it ) &&
        who.weight_carried() + it.weight() <= who.weight_capacity() &&
        who.i_add( it ) ) {
        return true;
    }
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

// takeoff() without the "<npcname> takes off their X" line.  Trial fittings
// would otherwise bury the summary under dozens of messages, and the summary is
// what the player actually needs to read.
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

// Reload something, but only once we are certain there is something to reload
// it with: npc::do_reload throws a debugmsg if asked to load a thing it cannot.
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

bool off_limits_item( const item &it )
{
    // Favourites are the player's word on the subject, at any depth.
    return it.is_favorite || it.made_of( phase_id::LIQUID );
}

// Everything on this tile, including what is nested inside crates, boxes,
// duffel bags and first aid kits -- which is where a sorted camp actually keeps
// things.  A bandage does not stop being a bandage for being inside a box, and
// a coat is still a candidate for hanging in a locker.
void collect_from( item_location parent, std::vector<item_location> &out, int depth )
{
    if( depth > max_nesting_depth ) {
        return;
    }
    for( item *inner : parent->all_items_top( pocket_type::CONTAINER ) ) {
        if( off_limits_item( *inner ) ) {
            continue;
        }
        item_location child( parent, inner );
        out.push_back( child );
        collect_from( child, out, depth + 1 );
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
        if( off_limits_item( it ) ) {
            continue;
        }
        item_location loc( map_cursor( tile ), &it );
        out.push_back( loc );
        collect_from( loc, out, 1 );
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

// A camp tool (wrench, waffle iron) reads as is_melee() but must not be
// taken as a weapon; a combat knife carries a tool slot but must.  The item's
// own category tells them apart, and guns (a plasma torch is a tool too) are
// exempt from the exclusion entirely.
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

bool wants_as_weapon( npc &p, const item &it )
{
    // A weapon this order already gave up in favour of something else does
    // not get a second look -- otherwise the two can trade places forever,
    // each one briefly ahead of the other.
    if( !is_weapon_candidate( it ) || p.gear_up_rejected.count( it.typeId() ) > 0 ||
        !p.can_wield( it ).success() ) {
        return false;
    }
    item_location wielded = p.get_wielded_item();
    // Ammunition is chosen after the weapon, so an unloaded gun still has to be
    // judged on what it can do once loaded -- otherwise it can never win this
    // comparison against a knife, and a knife is never in a magazine.
    const double current =
        p.evaluate_weapon( wielded ? *wielded : null_item_reference(), true );
    return p.evaluate_weapon( it, true ) > current;
}

// Nobody who knows what they are doing walks out with a rifle and nothing else.
// Guns jam, run dry, and are useless when something is already on top of you.
bool needs_backup_blade( npc &p )
{
    const double fists = p.evaluate_weapon( null_item_reference() );
    item_location wielded = p.get_wielded_item();
    if( wielded && !wielded->is_gun() && p.evaluate_weapon( *wielded ) > fists ) {
        return false;
    }
    bool found = false;
    p.visit_items( [&p, &found, fists]( const item * node, item * ) {
        if( is_weapon_candidate( *node ) && !node->is_gun() && p.evaluate_weapon( *node ) > fists &&
            p.can_wield( *node ).success() ) {
            found = true;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    return !found;
}

bool wants_as_backup( npc &p, const item &it )
{
    if( !is_weapon_candidate( it ) || it.is_gun() ||
        p.gear_up_rejected.count( it.typeId() ) > 0 || !p.can_wield( it ).success() ) {
        return false;
    }
    return p.evaluate_weapon( it ) > p.evaluate_weapon( null_item_reference() );
}

// Worth taking the coat off the rack and trying it on?  Deliberately cheap and
// optimistic: the measured outfit score makes the real decision, and every
// rejection is remembered by item type so the same coat is never carried back
// to the same crate twice.
bool worth_trying_on( npc &p, const item &it )
{
    if( !it.is_armor() || p.gear_up_rejected.count( it.typeId() ) > 0 ) {
        return false;
    }
    if( temperature_forbids_dressing( p ) ) {
        return false;
    }
    const int target_warmth = target_warmth_for( planning_temperature( p ) );
    const double candidate_proxy = wear_proxy( p, it, target_warmth );
    if( candidate_proxy <= 0.0 ) {
        return false;
    }
    // Something that goes straight on without displacing anything is always
    // worth measuring.
    if( p.can_wear( it ).success() ) {
        return true;
    }
    // Otherwise it has to beat something already worn on the same patch of skin.
    bool better_than_something = false;
    p.visit_items( [&]( const item * node, item * ) {
        if( !p.is_worn( *node ) || node->is_favorite || !shares_sub_part( *node, it ) ) {
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

bool wants_magazine( npc &p, const item &it )
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

int ammo_shortfall( npc &p )
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

bool wants_ammo( npc &p, const item &it )
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

bool wants_medical( npc &p, const item &it )
{
    if( is_healing_item( it ) ) {
        return count_carried( p, is_healing_item ) < want_healing_items;
    }
    if( is_painkiller( it ) ) {
        return count_carried( p, is_painkiller ) < want_painkillers;
    }
    return false;
}

// With the NPC needs mod active NPCs neither eat nor drink, so handing them
// rations would be pure clutter.  needs_food() is the same gate the behaviour
// tree uses, so this stays aligned with the mod rather than fighting it.
bool wants_rations( npc &p, const item &it )
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
    p.visit_items( [&kcal, &quench]( const item * node, item * ) {
        if( node->is_food() ) {
            const auto &carried = node->get_comestible();
            if( carried ) {
                kcal += carried->default_nutrition_read_only().kcal() * node->count();
                quench += carried->quench * node->count();
            }
        }
        return VisitResponse::NEXT;
    } );
    if( kcal < want_kcal && com->default_nutrition_read_only().kcal() > 0 ) {
        return true;
    }
    return quench < want_quench && com->quench > 0;
}

bool wanted_for_stage( npc &p, const item &it, gear_stage stage )
{
    if( stage == gear_stage::equipment ) {
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
           wants_medical( p, it ) || wants_rations( p, it );
}

// Cheap tile test for "is this worth walking to".  Bails on the first hit
// rather than building and sorting a pool: with nesting, one storage tile can
// expose hundreds of items, and this runs for every location every turn.
bool tile_has_anything_wanted( npc &p, const tripoint_bub_ms &tile, gear_stage stage )
{
    map &here = get_map();
    if( !here.inbounds( tile ) || !here.sees_some_items( tile, p ) ) {
        return false;
    }
    for( item &it : here.i_at( tile ) ) {
        if( off_limits_item( it ) ) {
            continue;
        }
        bool found = false;
        it.visit_items( [&]( const item * node, item * ) {
            if( off_limits_item( *node ) ) {
                return VisitResponse::SKIP;
            }
            if( wanted_for_stage( p, *node, stage ) ) {
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

// ---------------------------------------------------------------------------
// Acting on one tile
// ---------------------------------------------------------------------------

void say( const Character &who, const std::string &line )
{
    add_msg_if_player_sees( who, m_good, _( "%1$s: %2$s" ), who.disp_name(), line );
}

void report_problem( const Character &who, const std::string &line )
{
    add_msg_if_player_sees( who, m_warning, _( "%1$s: %2$s" ), who.disp_name(), line );
}

// Empty a container out where it stands before wearing it.  Picking up a sports
// bag and inheriting seventy-six shirts is not gearing up, and the contents
// belong in the camp's zones rather than on someone's back.
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

// Move whatever was in the old garment into the new one, then find a home for
// anything that will not fit.  Doing this after the old one came off would
// leave its contents with nowhere to go.
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

// Try one garment on; keep it only if it beats an empty slot or the specific
// piece it displaces, never the whole outfit.  What physically fits (pocket
// length, body size, rigid conflicts, power armour) is the engine's call and
// is not re-derived here.  Returns true if something changed.
bool try_one_garment( npc &p, item_location &loc, const tripoint_bub_ms &tile )
{
    const int target_warmth = target_warmth_for( planning_temperature( p ) );
    const itype_id candidate_type = loc->typeId();

    // A bag full of someone else's laundry is emptied where it stands first.
    empty_where_it_stands( p, loc, tile );
    if( !loc ) {
        return false;
    }
    const item candidate = *loc;
    const double candidate_proxy = wear_proxy( p, candidate, target_warmth );
    p.mod_moves( -p.item_wear_cost( candidate ) );

    // Straight on, if it goes on at all and nothing already worn covers the
    // same skin -- otherwise this falls through to the displacement logic
    // below, which is the only path that ever takes the old piece off.  An
    // empty slot has nothing to clear a margin against: the pre-filter that
    // sent this candidate here already confirmed a positive local score, so
    // physically fitting is the only remaining question.
    if( !conflicts_with_worn( p, candidate ) && p.can_wear( candidate ).success() &&
        p.weight_carried() + candidate.weight() <= p.weight_capacity() ) {
        std::optional<std::list<item>::iterator> worn_it =
            p.wear_item( candidate, false, true, true, true );
        if( worn_it ) {
            loc.remove_item();
            say( p, string_format( _( "puts on %s" ), candidate.tname() ) );
            return true;
        }
    }

    // Otherwise displace whatever already sits on that patch of skin, if this
    // is better.  An old pack is never discarded with its contents inside.
    item_location replace_target;
    double replace_proxy = 0.0;
    for( item_location &worn_loc : p.all_items_loc() ) {
        if( !worn_loc || !p.is_worn( *worn_loc ) ) {
            continue;
        }
        const item &worn = *worn_loc;
        if( worn.is_favorite || !shares_sub_part( worn, candidate ) ||
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

    const item displaced = *replace_target;
    // Wear the new one first, so the old one's contents have somewhere to go.
    std::optional<std::list<item>::iterator> worn_it =
        p.wear_item( candidate, false, true, true, true );
    if( !worn_it ) {
        p.gear_up_rejected.insert( candidate_type );
        return false;
    }
    item &new_worn = **worn_it;
    transfer_contents( p, *replace_target, new_worn, tile );

    std::list<item> removed;
    if( !quiet_takeoff( p, replace_target, removed ) || removed.empty() ) {
        // Could not get the old one off after all; undo and remember.
        std::list<item> undo;
        item_location worn_loc( p, &new_worn );
        quiet_takeoff( p, worn_loc, undo );
        p.gear_up_rejected.insert( candidate_type );
        return false;
    }

    loc.remove_item();
    // The swap changed the totals wear_proxy() scores against, so without
    // this memory the displaced piece can look attractive again later and
    // trade places with its replacement forever.
    p.gear_up_rejected.insert( displaced.typeId() );
    if( !put_away( p, removed.front(), tile ) ) {
        // Nowhere for the old piece, so wear it again over the top rather
        // than lose it.
        p.wear_item( removed.front(), false, true, true, true );
    }
    say( p, string_format( _( "swaps %1$s for %2$s" ), displaced.tname(),
                           candidate.tname() ) );
    return true;
}

bool do_equipment_stage( npc &p, const tripoint_bub_ms &tile )
{
    std::vector<item_location> pool = candidates_at( p, tile );

    // Weapon first: the weapon decides which ammunition is coherent later.
    item_location best;
    double best_value = 0.0;
    for( item_location &loc : pool ) {
        if( loc && wants_as_weapon( p, *loc ) ) {
            const double value = p.evaluate_weapon( *loc, true );
            if( !best || value > best_value ) {
                best = loc;
                best_value = value;
            }
        }
    }
    if( best ) {
        const std::string taken = best->tname();
        item_location wielded = p.get_wielded_item();
        if( wielded ) {
            const item old = *wielded;
            if( !p.can_unwield( old ).success() ) {
                // The candidate can never be taken while the hands are stuck,
                // so remember it or this repeats every turn.
                p.gear_up_rejected.insert( best->typeId() );
                report_problem( p, string_format( _( "can't let go of %s" ), old.tname() ) );
                return true;
            }
            // Empty the hands here rather than letting the wield path stow the
            // old weapon, because that path is allowed to drop it on the ground.
            item removed = p.remove_weapon();
            if( !put_away( p, removed, tile ) ) {
                p.wield( removed );
                p.gear_up_rejected.insert( best->typeId() );
                report_problem( p, string_format( _( "no room to set down %s" ), old.tname() ) );
                return true;
            }
            // Or the displaced weapon can out-score its replacement from the
            // shelf on a later pass and the two trade places forever.
            p.gear_up_rejected.insert( old.typeId() );
        }
        if( p.wield( best ) ) {
            say( p, string_format( _( "takes up %s" ), taken ) );
        }
        return true;
    }

    if( needs_backup_blade( p ) ) {
        item_location blade;
        double blade_value = 0.0;
        for( item_location &loc : pool ) {
            if( loc && wants_as_backup( p, *loc ) ) {
                const double value = p.evaluate_weapon( *loc );
                if( !blade || value > blade_value ) {
                    blade = loc;
                    blade_value = value;
                }
            }
        }
        if( blade ) {
            const std::string taken = blade->tname();
            if( take_into_inventory( p, blade ) ) {
                say( p, string_format( _( "takes %s as a backup" ), taken ) );
                return true;
            }
            p.gear_up_rejected.insert( blade->typeId() );
        }
    }

    // Then clothing, biggest internal volume first -- a pack worn early is
    // where everything displaced later can go.  Ties fall back to the score,
    // so among plain garments the good coat is still measured before the
    // pile of shirts underneath it.
    std::vector<item_location> wearables;
    for( item_location &loc : pool ) {
        if( loc && worth_trying_on( p, *loc ) ) {
            wearables.push_back( loc );
        }
    }
    const int target_warmth = target_warmth_for( planning_temperature( p ) );
    std::sort( wearables.begin(), wearables.end(),
    [&p, target_warmth]( const item_location & a, const item_location & b ) {
        const units::volume va = a->get_volume_capacity();
        const units::volume vb = b->get_volume_capacity();
        if( va != vb ) {
            return va > vb;
        }
        return wear_proxy( p, *a, target_warmth ) > wear_proxy( p, *b, target_warmth );
    } );
    for( item_location &loc : wearables ) {
        if( !loc ) {
            continue;
        }
        if( try_one_garment( p, loc, tile ) || p.get_moves() <= 0 ) {
            return true;
        }
    }
    return false;
}

bool do_supply_stage( npc &p, const tripoint_bub_ms &tile )
{
    std::vector<item_location> pool = candidates_at( p, tile );
    bool did_something = false;

    for( item_location &loc : pool ) {
        if( !loc || !wants_magazine( p, *loc ) ) {
            continue;
        }
        const std::string name = loc->tname();
        if( take_into_inventory( p, loc ) ) {
            say( p, string_format( _( "picks up %s" ), name ) );
            did_something = true;
        } else {
            p.gear_up_rejected.insert( loc->typeId() );
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
        const int got = take_charges( p, loc, want );
        if( got > 0 ) {
            taken += got;
        } else {
            p.gear_up_rejected.insert( loc->typeId() );
        }
    }
    if( taken > 0 ) {
        say( p, string_format( _( "takes %d rounds of ammunition" ), taken ) );
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
        if( take_charges( p, loc, want ) > 0 ) {
            say( p, string_format( _( "takes %s" ), name ) );
            did_something = true;
        } else {
            p.gear_up_rejected.insert( loc->typeId() );
        }
    }

    for( item_location &loc : pool ) {
        if( !loc || !wants_rations( p, *loc ) ) {
            continue;
        }
        const std::string name = loc->tname();
        if( take_into_inventory( p, loc ) ) {
            say( p, string_format( _( "packs %s" ), name ) );
            did_something = true;
        } else {
            p.gear_up_rejected.insert( loc->typeId() );
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

std::unordered_set<tripoint_abs_ms> multi_gear_up_activity_actor::multi_activity_locations(
    Character &you )
{
    npc *p = you.as_npc();
    if( !p ) {
        return {};
    }
    map &here = get_map();
    const std::unordered_set<tripoint_abs_ms> stores = stores_within_reach( you );

    // Ammunition is only coherent once the weapon is final, so the equipment
    // stage has to be exhausted everywhere before the supply stage can begin.
    for( int attempt = 0; attempt < 2; attempt++ ) {
        const gear_stage stage = static_cast<gear_stage>( p->gear_up_stage );
        std::unordered_set<tripoint_abs_ms> wanted;
        for( const tripoint_abs_ms &tile : stores ) {
            const tripoint_bub_ms bub = here.get_bub( tile );
            if( !here.inbounds( bub ) ) {
                // Outside the reality bubble; let the framework route there and
                // decide once it can actually see the tile.
                wanted.emplace( tile );
                continue;
            }
            if( tile_has_anything_wanted( *p, bub, stage ) ) {
                wanted.emplace( tile );
            }
        }
        if( !wanted.empty() || stage == gear_stage::supplies ) {
            // Nothing wanted before pruning means the order is genuinely
            // finished, not merely blocked by darkness or a dangerous field
            // this turn -- that distinction has to be made before pruning
            // touches the set.
            const bool order_complete = wanted.empty() && stage == gear_stage::supplies;
            multi_activity_actor::prune_dangerous_field_locations( wanted );
            // Guarded exactly the way generic_locations guards it: a camp's
            // store room is usually windowless, and sorting loot is allowed
            // there, so gearing up out of it is too.
            if( !multi_activity_actor::can_do_in_dark( get_type() ) ) {
                multi_activity_actor::prune_dark_locations( you, wanted, get_type() );
            }
            if( order_complete && !p->gear_up_done_reported ) {
                p->gear_up_done_reported = true;
                say( *p, _( "finishes gearing up from the stores." ) );
            }
            return wanted;
        }
        p->gear_up_stage = static_cast<int>( gear_stage::supplies );
    }
    return {};
}

activity_reason_info multi_gear_up_activity_actor::multi_activity_can_do( Character &you,
        const tripoint_bub_ms &src_loc )
{
    npc *p = you.as_npc();
    if( !p ) {
        return activity_reason_info::fail( do_activity_reason::NO_ZONE );
    }
    const gear_stage stage = static_cast<gear_stage>( p->gear_up_stage );
    if( !tile_has_anything_wanted( *p, src_loc, stage ) ) {
        return activity_reason_info::fail( do_activity_reason::ALREADY_DONE );
    }
    return activity_reason_info::ok( do_activity_reason::NEEDS_GEAR_UP );
}

bool multi_gear_up_activity_actor::multi_activity_do( Character &you,
        const activity_reason_info &, const tripoint_abs_ms &, const tripoint_bub_ms &src_loc )
{
    npc *p = you.as_npc();
    if( !p ) {
        return true;
    }
    // Going through a crate costs time whether or not anything comes of it.
    you.mod_moves( -search_cost_moves );

    const bool did_something =
        static_cast<gear_stage>( p->gear_up_stage ) == gear_stage::equipment
        ? do_equipment_stage( *p, src_loc )
        : do_supply_stage( *p, src_loc );

    // Let the NPC's own AI re-examine what it is now carrying; it scores
    // weapons with the same function used here, so it will agree.
    p->has_new_items = true;
    p->invalidate_range_cache();

    // false keeps the activity alive so a crate with several useful things is
    // not abandoned after one; true when nothing happened is the only thing
    // that lets the sweep run out and end.
    return !did_something;
}

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

    if( stores_within_reach( p ).empty() ) {
        add_msg( m_info, _( "%s has no loot or camp storage zone in range to draw from." ),
                 p.get_name() );
        return;
    }

    // Reset the sweep here rather than in the actor's start(): multi-zone
    // actors are cloned through the backlog and restarted every turn, so state
    // reset there would drop back to the first stage forever.  A fresh order
    // reconsiders everything turned down last time -- the weather has moved on,
    // and so has what is in the crates.
    p.gear_up_rejected.clear();
    p.gear_up_stage = 0;
    p.gear_up_done_reported = false;
    p.assign_activity( multi_gear_up_activity_actor() );
}
