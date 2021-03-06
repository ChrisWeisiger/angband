/**
 * \file mon-make.c
 * \brief Monster creation / placement code.
 *
 * Copyright (c) 1997-2007 Ben Harrison, James E. Wilson, Robert A. Koeneke
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "alloc.h"
#include "game-world.h"
#include "init.h"
#include "mon-desc.h"
#include "mon-lore.h"
#include "mon-make.h"
#include "mon-timed.h"
#include "mon-util.h"
#include "obj-identify.h"
#include "obj-make.h"
#include "obj-pile.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "player-calcs.h"
#include "player-history.h"
#include "player-quest.h"
#include "target.h"

s16b num_repro;

static s16b alloc_race_size;
static struct alloc_entry *alloc_race_table;

static void init_race_allocs(void) {
	int i;
	struct monster_race *race;
	alloc_entry *table;
	s16b *num = mem_zalloc(z_info->max_depth * sizeof(s16b));
	s16b *aux = mem_zalloc(z_info->max_depth * sizeof(s16b));

	/* Size of "alloc_race_table" */
	alloc_race_size = 0;

	/* Scan the monsters (not the ghost) */
	for (i = 1; i < z_info->r_max - 1; i++) {
		/* Get the i'th race */
		race = &r_info[i];

		/* Legal monsters */
		if (race->rarity) {
			/* Count the entries */
			alloc_race_size++;

			/* Group by level */
			num[race->level]++;
		}
	}

	/* Collect the level indexes */
	for (i = 1; i < z_info->max_depth; i++) {
		/* Group by level */
		num[i] += num[i - 1];
	}

	/* Paranoia */
	if (!num[0]) quit("No town monsters!");


	/*** Initialize monster allocation info ***/

	/* Allocate the alloc_race_table */
	alloc_race_table = mem_zalloc(alloc_race_size * sizeof(alloc_entry));

	/* Get the table entry */
	table = alloc_race_table;

	/* Scan the monsters (not the ghost) */
	for (i = 1; i < z_info->r_max - 1; i++) {
		/* Get the i'th race */
		race = &r_info[i];

		/* Count valid pairs */
		if (race->rarity) {
			int p, x, y, z;

			/* Extract the base level */
			x = race->level;

			/* Extract the base probability */
			p = (100 / race->rarity);

			/* Skip entries preceding our locale */
			y = (x > 0) ? num[x-1] : 0;

			/* Skip previous entries at this locale */
			z = y + aux[x];

			/* Load the entry */
			table[z].index = i;
			table[z].level = x;
			table[z].prob1 = p;
			table[z].prob2 = p;
			table[z].prob3 = p;

			/* Another entry complete for this locale */
			aux[x]++;
		}
	}
	mem_free(aux);
	mem_free(num);
}

static void cleanup_race_allocs(void) {
	mem_free(alloc_race_table);
}

/**
 * Deletes a monster by index.
 *
 * When a monster is deleted, all of its objects are deleted.
 */
void delete_monster_idx(int m_idx)
{
	int x, y;
	struct object *obj;
	struct monster *mon;

	assert(m_idx > 0);

	mon = cave_monster(cave, m_idx);

	/* Monster location */
	y = mon->fy;
	x = mon->fx;
	assert(square_in_bounds(cave, y, x));

	/* Hack -- Reduce the racial counter */
	mon->race->cur_num--;

	/* Hack -- count the number of "reproducers" */
	if (rf_has(mon->race->flags, RF_MULTIPLY)) num_repro--;

	/* Hack -- remove target monster */
	if (target_get_monster() == mon) target_set_monster(NULL);

	/* Hack -- remove tracked monster */
	if (player->upkeep->health_who == mon) health_track(player->upkeep, NULL);

	/* Monster is gone */
	cave->squares[y][x].mon = 0;

	/* Delete objects */
	obj = mon->held_obj;
	while (obj) {
		struct object *next = obj->next;

		/* Preserve unseen artifacts (we assume they were created as this
		 * monster's drop) - this will cause unintended behaviour in preserve
		 * off mode if monsters can pick up artifacts */
		if (obj->artifact && !object_was_sensed(obj))
			obj->artifact->created = FALSE;

		/* Delete the object */
		object_delete(&obj);
		obj = next;
	}

	/* Delete mimicked objects */
	if (mon->mimicked_obj) {
		square_excise_object(cave, y, x, mon->mimicked_obj);
		object_delete(&mon->mimicked_obj);
	}

	/* Wipe the Monster */
	memset(mon, 0, sizeof(struct monster));

	/* Count monsters */
	cave->mon_cnt--;

	/* Visual update */
	square_light_spot(cave, y, x);
}


/**
 * Deletes the monster, if any, at the given location.
 */
void delete_monster(int y, int x)
{
	assert(square_in_bounds(cave, y, x));

	/* Delete the monster (if any) */
	if (cave->squares[y][x].mon > 0)
		delete_monster_idx(cave->squares[y][x].mon);
}


/**
 * Move a monster from index i1 to index i2 in the monster list.
 */
static void compact_monsters_aux(int i1, int i2)
{
	int y, x;
	struct monster *mon;
	struct object *obj;

	/* Do nothing */
	if (i1 == i2) return;

	/* Old monster */
	mon = cave_monster(cave, i1);
	y = mon->fy;
	x = mon->fx;

	/* Update the cave */
	cave->squares[y][x].mon = i2;
	
	/* Update midx */
	mon->midx = i2;

	/* Repair objects being carried by monster */
	for (obj = mon->held_obj; obj; obj = obj->next)
		obj->held_m_idx = i2;

	/* Move mimicked objects (heh) */
	if (mon->mimicked_obj)
		mon->mimicked_obj->mimicking_m_idx = i2;

	/* Hack -- Update the target */
	if (target_get_monster() == mon)
		target_set_monster(cave_monster(cave, i2));

	/* Hack -- Update the health bar */
	if (player->upkeep->health_who == mon)
		player->upkeep->health_who = cave_monster(cave, i2);

	/* Hack -- move monster */
	memcpy(cave_monster(cave, i2), cave_monster(cave, i1),
		   sizeof(struct monster));

	/* Hack -- wipe hole */
	memset(cave_monster(cave, i1), 0, sizeof(struct monster));
}


/**
 * Compacts and reorders the monster list.
 *
 * This function can be very dangerous, use with caution!
 *
 * When `num_to_compact` is 0, we just reorder the monsters into a more compact
 * order, eliminating any "holes" left by dead monsters. If `num_to_compact` is
 * positive, then we delete at least that many monsters and then reorder.
 * We try not to delete monsters that are high level or close to the player.
 * Each time we make a full pass through the monster list, if we haven't
 * deleted enough monsters, we relax our bounds a little to accept
 * monsters of a slightly higher level, and monsters slightly closer to
 * the player.
 */
void compact_monsters(int num_to_compact)
{
	int m_idx, num_compacted, iter;

	int max_lev, min_dis, chance;


	/* Message (only if compacting) */
	if (num_to_compact)
		msg("Compacting monsters...");


	/* Compact at least 'num_to_compact' objects */
	for (num_compacted = 0, iter = 1; num_compacted < num_to_compact; iter++) {
		/* Get more vicious each iteration */
		max_lev = 5 * iter;

		/* Get closer each iteration */
		min_dis = 5 * (20 - iter);

		/* Check all the monsters */
		for (m_idx = 1; m_idx < cave_monster_max(cave); m_idx++) {
			struct monster *mon = cave_monster(cave, m_idx);

			/* Skip "dead" monsters */
			if (!mon->race) continue;

			/* High level monsters start out "immune" */
			if (mon->race->level > max_lev) continue;

			/* Ignore nearby monsters */
			if ((min_dis > 0) && (mon->cdis < min_dis)) continue;

			/* Saving throw chance */
			chance = 90;

			/* Only compact "Quest" Monsters in emergencies */
			if (rf_has(mon->race->flags, RF_QUESTOR) && (iter < 1000))
				chance = 100;

			/* Try not to compact Unique Monsters */
			if (rf_has(mon->race->flags, RF_UNIQUE)) chance = 99;

			/* All monsters get a saving throw */
			if (randint0(100) < chance) continue;

			/* Delete the monster */
			delete_monster(mon->fy, mon->fx);

			/* Count the monster */
			num_compacted++;
		}
	}


	/* Excise dead monsters (backwards!) */
	for (m_idx = cave_monster_max(cave) - 1; m_idx >= 1; m_idx--) {
		struct monster *mon = cave_monster(cave, m_idx);

		/* Skip real monsters */
		if (mon->race) continue;

		/* Move last monster into open hole */
		compact_monsters_aux(cave_monster_max(cave) - 1, m_idx);

		/* Compress "cave->mon_max" */
		cave->mon_max--;
	}
}


/**
 * Deletes all the monsters when the player leaves the level.
 *
 * This is an efficient method of simulating multiple calls to the
 * "delete_monster()" function, with no visual effects.
 *
 * Note that we must delete the objects the monsters are carrying, but we
 * do nothing with mimicked objects.
 */
void wipe_mon_list(struct chunk *c, struct player *p)
{
	int m_idx;

	/* Delete all the monsters */
	for (m_idx = cave_monster_max(c) - 1; m_idx >= 1; m_idx--) {
		struct monster *mon = cave_monster(c, m_idx);
		struct object *held_obj = mon ? mon->held_obj : NULL;

		/* Skip dead monsters */
		if (!mon->race) continue;

		/* Delete all the objects */
		if (held_obj) {
			/* Go through all held objects and check for artifacts */
			struct object *obj = held_obj;
			while (obj) {
				if (obj->artifact && !object_was_sensed(obj))
					obj->artifact->created = FALSE;
				obj = obj->next;
			}
			object_pile_free(held_obj);
		}

		/* Reduce the racial counter */
		mon->race->cur_num--;

		/* Monster is gone */
		c->squares[mon->fy][mon->fx].mon = 0;

		/* Wipe the Monster */
		memset(mon, 0, sizeof(struct monster));
	}

	/* Reset "cave->mon_max" */
	c->mon_max = 1;

	/* Reset "mon_cnt" */
	c->mon_cnt = 0;

	/* Hack -- reset "reproducer" count */
	num_repro = 0;

	/* Hack -- no more target */
	target_set_monster(0);

	/* Hack -- no more tracking */
	health_track(p->upkeep, 0);
}

/**
 * Returns the index of a "free" monster, or 0 if no slot is available.
 *
 * This routine should almost never fail, but it *can* happen.
 * The calling code must check for and handle a 0 return.
 */
s16b mon_pop(struct chunk *c)
{
	int m_idx;

	/* Normal allocation */
	if (cave_monster_max(c) < z_info->level_monster_max) {
		/* Get the next hole */
		m_idx = cave_monster_max(c);

		/* Expand the array */
		c->mon_max++;

		/* Count monsters */
		c->mon_cnt++;

		return m_idx;
	}

	/* Recycle dead monsters if we've run out of room */
	for (m_idx = 1; m_idx < cave_monster_max(c); m_idx++) {
		struct monster *mon = cave_monster(c, m_idx);

		/* Skip live monsters */
		if (mon->race) continue;

		/* Count monsters */
		c->mon_cnt++;

		/* Use this monster */
		return m_idx;
	}

	/* Warn the player if no index is available 
	 * (except during dungeon creation)
	 */
	if (character_dungeon)
		msg("Too many monsters!");

	/* Try not to crash */
	return 0;
}


/**
 * Apply a "monster restriction function" to the "monster allocation table".
 * This way, we can use get_mon_num() to get a level-appropriate monster that
 * satisfies certain conditions (such as belonging to a particular monster
 * family).
 */
void get_mon_num_prep(bool (*get_mon_num_hook)(struct monster_race *race))
{
	int i;

	/* Scan the allocation table */
	for (i = 0; i < alloc_race_size; i++) {
		alloc_entry *entry = &alloc_race_table[i];

		/* Accept monsters which pass the restriction, if any */
		if (!get_mon_num_hook || (*get_mon_num_hook)(&r_info[entry->index]))
			entry->prob2 = entry->prob1;

		/* Do not use this monster */
		else
			entry->prob2 = 0;
	}

	return;
}

/**
 * Helper function for get_mon_num(). Scans the prepared monster allocation
 * table and picks a random monster. Returns the index of a monster in
 * `table`.
 */
static struct monster_race *get_mon_race_aux(long total,
											 const alloc_entry *table)
{
	int i;

	/* Pick a monster */
	long value = randint0(total);

	/* Find the monster */
	for (i = 0; i < alloc_race_size; i++) {
		/* Found the entry */
		if (value < table[i].prob3) break;

		/* Decrement */
		value -= table[i].prob3;
	}
	
	return &r_info[table[i].index];
}

/**
 * Chooses a monster race that seems "appropriate" to the given level
 *
 * This function uses the "prob2" field of the "monster allocation table",
 * and various local information, to calculate the "prob3" field of the
 * same table, which is then used to choose an "appropriate" monster, in
 * a relatively efficient manner.
 *
 * Note that "town" monsters will *only* be created in the town, and
 * "normal" monsters will *never* be created in the town, unless the
 * "level" is "modified", for example, by polymorph or summoning.
 *
 * There is a small chance (1/50) of "boosting" the given depth by
 * a small amount (up to four levels), except in the town.
 *
 * It is (slightly) more likely to acquire a monster of the given level
 * than one of a lower level.  This is done by choosing several monsters
 * appropriate to the given level and keeping the "hardest" one.
 *
 * Note that if no monsters are "appropriate", then this function will
 * fail, and return zero, but this should *almost* never happen.
 */
struct monster_race *get_mon_num(int level)
{
	int i, p;

	long total;

	struct monster_race *race;

	alloc_entry *table = alloc_race_table;

	/* Occasionally produce a nastier monster in the dungeon */
	if (level > 0 && one_in_(z_info->ood_monster_chance))
		level += MIN(level / 4 + 2, z_info->ood_monster_amount);

	total = 0L;

	/* Process probabilities */
	for (i = 0; i < alloc_race_size; i++) {
		time_t cur_time = time(NULL);
		struct tm *date = localtime(&cur_time);

		/* Monsters are sorted by depth */
		if (table[i].level > level) break;

		/* Default */
		table[i].prob3 = 0;

		/* No town monsters in dungeon */
		if ((level > 0) && (table[i].level <= 0)) continue;

		/* Get the chosen monster */
		race = &r_info[table[i].index];

		/* No seasonal monsters outside of Christmas */
		if (rf_has(race->flags, RF_SEASONAL) && 
			!(date->tm_mon == 11 && date->tm_mday >= 24 && date->tm_mday <= 26))
			continue;

		/* Only one copy of a a unique must be around at the same time */
		if (rf_has(race->flags, RF_UNIQUE) && 
				race->cur_num >= race->max_num)
			continue;

		/* Some monsters never appear out of depth */
		if (rf_has(race->flags, RF_FORCE_DEPTH) && race->level > player->depth)
			continue;

		/* Accept */
		table[i].prob3 = table[i].prob2;

		/* Total */
		total += table[i].prob3;
	}

	/* No legal monsters */
	if (total <= 0) return NULL;

	/* Pick a monster */
	race = get_mon_race_aux(total, table);

	/* Try for a "harder" monster once (50%) or twice (10%) */
	p = randint0(100);
	
	if (p < 60) {
		struct monster_race *old = race;

		/* Pick a new monster */
		race = get_mon_race_aux(total, table);

		/* Keep the deepest one */
		if (race->level < old->level) race = old;
	}

	/* Try for a "harder" monster twice (10%) */
	if (p < 10) {
		struct monster_race *old = race;

		/* Pick a monster */
		race = get_mon_race_aux(total, table);

		/* Keep the deepest one */
		if (race->level < old->level) race = old;
	}

	/* Result */
	return race;
}


/**
 * Return the number of things dropped by a monster.
 *
 * \param race is the monster race.
 * \param maximize should be set to FALSE for a random number, TRUE to find
 * out the maximum count.
 */
int mon_create_drop_count(const struct monster_race *race, bool maximize)
{
	int number = 0;
	static const int drop_4_max = 6;
	static const int drop_3_max = 4;
	static const int drop_2_max = 3;

	if (maximize) {
		if (rf_has(race->flags, RF_DROP_20)) number++;
		if (rf_has(race->flags, RF_DROP_40)) number++;
		if (rf_has(race->flags, RF_DROP_60)) number++;
		if (rf_has(race->flags, RF_DROP_4)) number += drop_4_max;
		if (rf_has(race->flags, RF_DROP_3)) number += drop_3_max;
		if (rf_has(race->flags, RF_DROP_2)) number += drop_2_max;
		if (rf_has(race->flags, RF_DROP_1)) number++;
	} else {
		if (rf_has(race->flags, RF_DROP_20) && randint0(100) < 20) number++;
		if (rf_has(race->flags, RF_DROP_40) && randint0(100) < 40) number++;
		if (rf_has(race->flags, RF_DROP_60) && randint0(100) < 60) number++;
		if (rf_has(race->flags, RF_DROP_4)) number += rand_range(2, drop_4_max);
		if (rf_has(race->flags, RF_DROP_3)) number += rand_range(2, drop_3_max);
		if (rf_has(race->flags, RF_DROP_2)) number += rand_range(1, drop_2_max);
		if (rf_has(race->flags, RF_DROP_1)) number++;
	}

	return number;
}

/**
 * Creates a specific monster's drop, including any drops specified
 * in the monster.txt file.
 *
 * Returns TRUE if anything is created, FALSE if nothing is.
 */
static bool mon_create_drop(struct chunk *c, struct monster *mon, byte origin)
{
	struct monster_drop *drop;

	bool great, good, gold_ok, item_ok;
    bool extra_roll = FALSE;
	bool any = FALSE;

	int number = 0, level, j, monlevel;

	struct object *obj;
	
	assert(mon);

	great = (rf_has(mon->race->flags, RF_DROP_GREAT));
	good = great || (rf_has(mon->race->flags, RF_DROP_GOOD));
	gold_ok = (!rf_has(mon->race->flags, RF_ONLY_ITEM));
	item_ok = (!rf_has(mon->race->flags, RF_ONLY_GOLD));

	/* Determine how much we can drop */
	number = mon_create_drop_count(mon->race, FALSE);

    /* Give added bonus for unique monters */
    monlevel = mon->race->level;
    if (rf_has(mon->race->flags, RF_UNIQUE)) {
        monlevel = MIN(monlevel + 15, monlevel * 2);
        extra_roll = TRUE;
    }
    
	/* Take the best of (average of monster level and current depth)
	   and (monster level) - to reward fighting OOD monsters */
	level = MAX((monlevel + player->depth) / 2, monlevel);
    level = MIN(level, 100);

	/* Specified drops */
	for (drop = mon->race->drops; drop; drop = drop->next) {
		if ((unsigned int)randint0(100) >= drop->percent_chance)
			continue;

		/* Allocate by hand, prep, apply magic */
		obj = mem_zalloc(sizeof(*obj));
		if (drop->artifact) {
			object_prep(obj, lookup_kind(drop->artifact->tval,
				drop->artifact->sval), level, RANDOMISE);
			obj->artifact = drop->artifact;
			copy_artifact_data(obj, obj->artifact);
			obj->artifact->created = TRUE;
		} else {
			object_prep(obj, drop->kind, level, RANDOMISE);
			apply_magic(obj, level, TRUE, good, great, extra_roll);
		}

		/* Set origin details */
		obj->origin = origin;
		obj->origin_depth = player->depth;
		obj->origin_xtra = mon->race->ridx;
		obj->number = randint0(drop->max - drop->min) + drop->min;

		/* Try to carry */
		if (monster_carry(c, mon, obj))
			any = TRUE;
		else {
			obj->artifact->created = FALSE;
			object_wipe(obj);
			mem_free(obj);
		}
	}

	/* Make some objects */
	for (j = 0; j < number; j++) {
		if (gold_ok && (!item_ok || (randint0(100) < 50))) {
			obj = make_gold(level, "any");
		} else {
			obj = make_object(c, level, good, great, extra_roll, NULL, 0);
			if (!obj) continue;
		}

		/* Set origin details */
		obj->origin = origin;
		obj->origin_depth = player->depth;
		obj->origin_xtra = mon->race->ridx;

		/* Try to carry */
		if (monster_carry(c, mon, obj))
			any = TRUE;
		else {
			obj->artifact->created = FALSE;
			object_wipe(obj);
			mem_free(obj);
		}
	}

	return any;
}


/**
 * Attempts to place a copy of the given monster at the given position in
 * the dungeon.
 *
 * All of the monster placement routines eventually call this function. This
 * is what actually puts the monster in the dungeon (i.e., it notifies the cave
 * and sets the monsters position). The dungeon loading code also calls this
 * function directly.
 *
 * `origin` is the item origin to use for any monster drops (e.g. ORIGIN_DROP,
 * ORIGIN_DROP_PIT, etc.) The dungeon loading code calls this with origin = 0,
 * which prevents the monster's drops from being generated again.
 *
 * Returns the m_idx of the newly copied monster, or 0 if the placement fails.
 */
s16b place_monster(struct chunk *c, int y, int x, struct monster *mon,
				   byte origin)
{
	s16b m_idx;
	struct monster *new_mon;

	assert(square_in_bounds(c, y, x));
	assert(!square_monster(c, y, x));

	/* Get a new record */
	m_idx = mon_pop(c);
	if (!m_idx) return 0;

	/* Copy the monster */
	new_mon = cave_monster(c, m_idx);
	memcpy(new_mon, mon, sizeof(struct monster));

	/* Set the ID */
	new_mon->midx = m_idx;

	/* Set the location */
	c->squares[y][x].mon = new_mon->midx;
	new_mon->fy = y;
	new_mon->fx = x;
	assert(square_monster(c, y, x) == new_mon);

	update_mon(new_mon, c, TRUE);

	/* Hack -- Count the number of "reproducers" */
	if (rf_has(new_mon->race->flags, RF_MULTIPLY)) num_repro++;

	/* Count racial occurrences */
	new_mon->race->cur_num++;

	/* Create the monster's drop, if any */
	if (origin)
		(void)mon_create_drop(c, new_mon, origin);

	/* Make mimics start mimicking */
	if (origin && new_mon->race->mimic_kinds) {
		struct object *obj;
		struct object_kind *kind = new_mon->race->mimic_kinds->kind;
		struct monster_mimic *mimic_kind;
		int i = 1;
		
		/* Pick a random object kind to mimic */
		for (mimic_kind = new_mon->race->mimic_kinds; mimic_kind; 
				mimic_kind = mimic_kind->next, i++) {
			if (one_in_(i)) kind = mimic_kind->kind;
		}

		if (tval_is_money_k(kind)) {
			obj = make_gold(player->depth, kind->name);
		} else {
			obj = object_new();
			object_prep(obj, kind, new_mon->race->level, RANDOMISE);
			apply_magic(obj, new_mon->race->level, TRUE, FALSE, FALSE, FALSE);
			obj->number = 1;
			obj->origin = ORIGIN_DROP_MIMIC;
			obj->origin_depth = player->depth;
		}

		obj->mimicking_m_idx = m_idx;
		new_mon->mimicked_obj = obj;

		/* Put the object on the floor if it goes, otherwise no mimicry */
		if (!floor_carry(c, y, x, obj, FALSE)) {
			/* Clear the mimicry */
			obj->mimicking_m_idx = 0;
			new_mon->mimicked_obj = NULL;

			/* Give the object to the monster if appropriate */
			if (rf_has(new_mon->race->flags, RF_MIMIC_INV)) {
				monster_carry(c, new_mon, obj);
			} else {
				/* Otherwise delete the mimicked object */
				object_delete(&obj);
			}
		}
	}

	/* Result */
	return m_idx;
}

/**
 * Calculates hp for a monster. This function assumes that the Rand_normal
 * function has limits of +/- 4x std_dev. If that changes, this function
 * will become inaccurate.
 *
 * \param race is the race of the monster in question.
 * \param hp_aspect is the hp calc we want (min, max, avg, random).
 */
int mon_hp(const struct monster_race *race, aspect hp_aspect)
{
	int std_dev = (((race->avg_hp * 10) / 8) + 5) / 10;

	if (race->avg_hp > 1) std_dev++;

	switch (hp_aspect) {
		case MINIMISE:
			return (race->avg_hp - (4 * std_dev));
		case MAXIMISE:
		case EXTREMIFY:
			return (race->avg_hp + (4 * std_dev));
		case AVERAGE:
			return race->avg_hp;
		case RANDOMISE:
			return Rand_normal(race->avg_hp, std_dev);
	}

	assert(0 && "Should never reach here");
	return 0;
}


/**
 * Attempts to place a monster of the given race at the given location.
 *
 * If `sleep` is true, the monster is placed with its default sleep value,
 * which is given in monster.txt.
 *
 * `origin` is the item origin to use for any monster drops (e.g. ORIGIN_DROP,
 * ORIGIN_DROP_PIT, etc.)
 *
 * To give the player a sporting chance, some especially dangerous
 * monsters are marked as "FORCE_SLEEP" in monster.txt, which will
 * cause them to be placed with low energy. This helps ensure that
 * if such a monster suddenly appears in line-of-sight (due to a
 * summon, for instance), the player gets a chance to move before
 * they do.
 *
 * This routine refuses to place out-of-depth "FORCE_DEPTH" monsters.
 *
 * This is the only function which may place a monster in the dungeon,
 * except for the savefile loading code, which calls place_monster()
 * directly.
 */
static bool place_new_monster_one(struct chunk *c, int y, int x,
								  struct monster_race *race, bool sleep,
								  byte origin)
{
	int i;

	struct monster *mon;
	struct monster monster_body;

	assert(square_in_bounds(c, y, x));
	assert(race && race->name);

	/* Not where monsters already are */
	if (square_monster(c, y, x))
		return FALSE;

	/* Not where the player already is */
	if ((player->py == y) && (player->px == x))
		return FALSE;

	/* Prevent monsters from being placed where they cannot walk, but allow other feature types */
	if (!square_is_monster_walkable(c, y, x))
		return FALSE;

	/* No creation on glyph of warding */
	if (square_iswarded(c, y, x)) return FALSE;

	/* "unique" monsters must be "unique" */
	if (rf_has(race->flags, RF_UNIQUE) && race->cur_num >= race->max_num)
		return (FALSE);

	/* Depth monsters may NOT be created out of depth */
	if (rf_has(race->flags, RF_FORCE_DEPTH) && player->depth < race->level)
		return (FALSE);

	/* Add to level feeling, note uniques for cheaters */
	c->mon_rating += race->power / 20;

	/* Check out-of-depth-ness */
	if (race->level > player->depth) {
		if (rf_has(race->flags, RF_UNIQUE)) { /* OOD unique */
			if (OPT(cheat_hear))
				msg("Deep unique (%s).", race->name);
		} else { /* Normal monsters but OOD */
			if (OPT(cheat_hear))
				msg("Deep monster (%s).", race->name);
		}
		/* Boost rating by power per 10 levels OOD */
		c->mon_rating += (race->level - player->depth) * race->power / 200;
	} else if (rf_has(race->flags, RF_UNIQUE) && OPT(cheat_hear))
		msg("Unique (%s).", race->name);

	/* Get local monster */
	mon = &monster_body;

	/* Clean out the monster */
	memset(mon, 0, sizeof(struct monster));

	/* Save the race */
	mon->race = race;

	/* Enforce sleeping if needed */
	if (sleep && race->sleep) {
		int val = race->sleep;
		mon->m_timed[MON_TMD_SLEEP] = ((val * 2) + randint1(val * 10));
	}

	/* Uniques get a fixed amount of HP */
	if (rf_has(race->flags, RF_UNIQUE))
		mon->maxhp = race->avg_hp;
	else {
		mon->maxhp = mon_hp(race, RANDOMISE);
		mon->maxhp = MAX(mon->maxhp, 1);
	}

	/* And start out fully healthy */
	mon->hp = mon->maxhp;

	/* Extract the monster base speed */
	mon->mspeed = race->speed;

	/* Hack -- small racial variety */
	if (!rf_has(race->flags, RF_UNIQUE)) {
		/* Allow some small variation per monster */
		i = turn_energy(race->speed) / 10;
		if (i) mon->mspeed += rand_spread(0, i);
	}

	/* Give a random starting energy */
	mon->energy = (byte)randint0(50);

	/* Force monster to wait for player */
	if (rf_has(race->flags, RF_FORCE_SLEEP))
		mflag_on(mon->mflag, MFLAG_NICE);

	/* Radiate light? */
	if (rf_has(race->flags, RF_HAS_LIGHT))
		player->upkeep->update |= PU_UPDATE_VIEW;
	
	/* Is this obviously a monster? (Mimics etc. aren't) */
	if (rf_has(race->flags, RF_UNAWARE)) 
		mflag_on(mon->mflag, MFLAG_UNAWARE);
	else
		mflag_off(mon->mflag, MFLAG_UNAWARE);

	/* Set the color if necessary */
	if (rf_has(race->flags, RF_ATTR_RAND))
		mon->attr = randint1(BASIC_COLORS - 1);

	/* Place the monster in the dungeon */
	if (!place_monster(c, y, x, mon, origin))
		return (FALSE);

	/* Success */
	return (TRUE);
}


/*
 * Maximum size of a group of monsters
 */
#define GROUP_MAX	25

		
/**
 * Attempts to place a group of monsters of race `r_idx` around
 * the given location. The number of monsters to place is `total`.
 *
 * If `sleep` is true, the monster is placed with its default sleep value,
 * which is given in monster.txt.
 *
 * `origin` is the item origin to use for any monster drops (e.g. ORIGIN_DROP,
 * ORIGIN_DROP_PIT, etc.) 
 */
static bool place_new_monster_group(struct chunk *c, int y, int x, 
									struct monster_race *race, bool sleep,
									int total, byte origin)
{
	int n, i;

	int hack_n;

	/* x and y coordinates of the placed monsters */
	byte hack_y[GROUP_MAX];
	byte hack_x[GROUP_MAX];

	assert(race);
	
	/* Start on the monster */
	hack_n = 1;
	hack_x[0] = x;
	hack_y[0] = y;

	/* Puddle monsters, breadth first, up to total */
	for (n = 0; (n < hack_n) && (hack_n < total); n++) {
		/* Grab the location */
		int hx = hack_x[n];
		int hy = hack_y[n];

		/* Check each direction, up to total */
		for (i = 0; (i < 8) && (hack_n < total); i++) {
			int mx = hx + ddx_ddd[i];
			int my = hy + ddy_ddd[i];

			/* Walls and Monsters block flow */
			if (!square_isempty(c, my, mx)) continue;

			/* Attempt to place another monster */
			if (place_new_monster_one(c, my, mx, race, sleep, origin)) {
				/* Add it to the "hack" set */
				hack_y[hack_n] = my;
				hack_x[hack_n] = mx;
				hack_n++;
			}
		}
	}

	/* Success */
	return (TRUE);
}

/* Maximum distance from center for a group of monsters */
#define GROUP_DISTANCE 5 

static struct monster_base *place_monster_base = NULL;

/**
 * Predicate function for get_mon_num_prep)
 * Check to see if the monster race has the same base as
 * place_monter_base.
 */
static bool place_monster_base_okay(struct monster_race *race)
{
	assert(place_monster_base);
	assert(race);
	
	/* Check if it matches */
	if (race->base != place_monster_base) return FALSE;
	
	/* No uniques */
	if (rf_has(race->flags, RF_UNIQUE)) return FALSE;
	
	return TRUE;
}

/**
 * Helper function to place monsters that appear as friends or escorts
 */
 bool place_friends(struct chunk *c, int y, int x, struct monster_race *race, 
					struct monster_race *friends_race, int total, bool sleep,
					byte origin)
 {
	int level_difference, extra_chance, nx, ny;
	int j;
	bool is_unique, success = TRUE;
	
	/* Find the difference between current dungeon depth and monster level */
	level_difference = player->depth - friends_race->level + 5;
	
	/* Handle unique monsters */
	is_unique = rf_has(friends_race->flags, RF_UNIQUE);
	
	/* Make sure the unique hasn't been killed already */
	if (is_unique){
		total = friends_race->cur_num < friends_race->max_num ? 1 : 0;
	}
	
	/* More than 4 levels OoD, no groups allowed */
	if (level_difference <= 0 && !is_unique){
		return FALSE;
	}
	
	/* Reduce group size within 5 levels of natural depth*/
	if (level_difference < 10 && !is_unique){
		extra_chance = (total * level_difference) % 10;
		total = total * level_difference / 10;
		
		/* Instead of flooring the group value, we use the decimal place
		   as a chance of an extra monster */
		if (randint0(10) > extra_chance){
			total += 1;
		}
	}
	
	/* No monsters in this group */
	if (total <= 0){
		return FALSE;
	}
	
	/* Handle friends same as original monster */
	if (race->ridx == friends_race->ridx){
		success = place_new_monster_group(c, y, x, race, sleep, total, origin);
		return success;
	}
	
	/* Find a nearby place to put the other groups */
	for (j = 0; j < 50; j++){
		scatter(c, &ny, &nx, y, x, GROUP_DISTANCE, FALSE);
		if (square_isopen(c, ny, nx)) break;
	}
		
	/* Place the monsters */
	success = place_new_monster_one(c, ny, nx, friends_race, sleep, origin);
	if (total > 1)
		success = place_new_monster_group(c, ny, nx, friends_race, sleep, total,
										  origin);
	
	return success;
	
}
	
/**
 * Attempts to place a monster of the given race at the given location.
 *
 * Note that certain monsters are placed with a large group of
 * identical or similar monsters. However, if `group_okay` is false,
 * then such monsters are placed by themselves.
 *
 * If `sleep` is true, the monster is placed with its default sleep value,
 * which is given in monster.txt.
 *
 * `origin` is the item origin to use for any monster drops (e.g. ORIGIN_DROP,
 * ORIGIN_DROP_PIT, etc.) 
 */
bool place_new_monster(struct chunk *c, int y, int x, struct monster_race *race,
					   bool sleep, bool group_okay, byte origin)
{
	struct monster_friends *friends;
	struct monster_friends_base *friends_base;
	int total;
	
	assert(c);
	assert(race);
	
	/* Place one monster, or fail */
	if (!place_new_monster_one(c, y, x, race, sleep, origin)) return (FALSE);

	/* We're done unless the group flag is set */
	if (!group_okay) return (TRUE);

	/* Go through friends flags */
	for (friends = race->friends; friends; friends = friends->next) {
		if ((unsigned int)randint0(100) >= friends->percent_chance)
			continue;
		
		/* Calculate the base number of monsters to place */
		total = damroll(friends->number_dice, friends->number_side);
		
		place_friends(c, y, x, race, friends->race, total, sleep, origin);
		
	}

	/* Go through the friends_base flags */
	for (friends_base = race->friends_base; friends_base; 
			friends_base = friends_base->next){
		struct monster_race *friends_race;

		/* Check if we pass chance for the monster appearing */
		if ((unsigned int)randint0(100) >= friends_base->percent_chance)
			continue;
			
		total = damroll(friends_base->number_dice, friends_base->number_side);

		/* Set the escort index base*/
		place_monster_base = friends_base->base;

		/* Prepare allocation table */
		get_mon_num_prep(place_monster_base_okay);

		/* Pick a random race */
		friends_race = get_mon_num(race->level);

		/* Reset allocation table */
		get_mon_num_prep(NULL);

		/* Handle failure */
		if (!friends_race) break;

		place_friends(c, y, x, race, friends_race, total, sleep, origin);
	}

	/* Success */
	return (TRUE);
}


/**
 * Picks a monster race, makes a new monster of that race, then attempts to 
 * place it in the dungeon. The monster race chosen will be appropriate for
 * dungeon level equal to `depth`.
 *
 * If `sleep` is true, the monster is placed with its default sleep value,
 * which is given in monster.txt.
 *
 * If `group_okay` is true, we allow the placing of a group, if the chosen
 * monster appears with friends or an escort.
 *
 * `origin` is the item origin to use for any monster drops (e.g. ORIGIN_DROP,
 * ORIGIN_DROP_PIT, etc.) 
 *
 * Returns TRUE if we successfully place a monster.
 */
bool pick_and_place_monster(struct chunk *c, int y, int x, int depth,
							bool sleep, bool group_okay, byte origin)
{
	/* Pick a monster race */
	struct monster_race *race = get_mon_num(depth);
	if (!race) return (FALSE);

	/* Attempt to place the monster */
	return (place_new_monster(c, y, x, race, sleep, group_okay, origin));
}


/**
 * Picks a monster race, makes a new monster of that race, then attempts to 
 * place it in the dungeon at least `dis` away from the player. The monster 
 * race chosen will be appropriate for dungeon level equal to `depth`.
 *
 * If `sleep` is true, the monster is placed with its default sleep value,
 * which is given in monster.txt.
 *
 * Returns TRUE if we successfully place a monster.
 */
bool pick_and_place_distant_monster(struct chunk *c, struct loc loc, int dis,
		bool sleep, int depth)
{
	int py = loc.y;
	int px = loc.x;

	int y = 0, x = 0;
	int	attempts_left = 10000;

	assert(c);

	/* Find a legal, distant, unoccupied, space */
	while (--attempts_left) {
		/* Pick a location */
		y = randint0(c->height);
		x = randint0(c->width);

		/* Require "naked" floor grid */
		if (!square_isempty(c, y, x)) continue;

		/* Do not put random monsters in marked rooms. */
		if ((!character_dungeon) && square_ismon_restrict(c, y, x))
			continue;

		/* Accept far away grids */
		if (distance(y, x, py, px) > dis) break;
	}

	if (!attempts_left) {
		if (OPT(cheat_xtra) || OPT(cheat_hear))
			msg("Warning! Could not allocate a new monster.");

		return FALSE;
	}

	/* Attempt to place the monster, allow groups */
	if (pick_and_place_monster(c, y, x, depth, sleep, TRUE, ORIGIN_DROP))
		return (TRUE);

	/* Nope */
	return (FALSE);
}


/**
 * Handles the "death" of a monster.
 *
 * Disperses treasures carried by the monster centered at the monster location.
 * Note that objects dropped may disappear in crowded rooms.
 *
 * Checks for "Quest" completion when a quest monster is killed.
 *
 * Note that only the player can induce "monster_death()" on Uniques.
 * Thus (for now) all Quest monsters should be Uniques.
 *
 * If `stats` is true, then we skip updating the monster memory. This is
 * used by stats-generation code, for efficiency.
 */
void monster_death(struct monster *mon, bool stats)
{
	int dump_item = 0;
	int dump_gold = 0;
	struct object *obj = mon->held_obj;

	bool visible = (mflag_has(mon->mflag, MFLAG_VISIBLE) ||
					rf_has(mon->race->flags, RF_UNIQUE));

	/* Delete any mimicked objects */
	if (mon->mimicked_obj)
		object_delete(&mon->mimicked_obj);

	/* Drop objects being carried */
	while (obj) {
		struct object *next = obj->next;

		/* Object no longer held */
		obj->held_m_idx = 0;
		pile_excise(&mon->held_obj, obj);

		/* Count it and drop it - refactor once origin is a bitflag */
		if (!stats) {
			if (tval_is_money(obj) && (obj->origin != ORIGIN_STOLEN))
				dump_gold++;
			else if (!tval_is_money(obj) && ((obj->origin == ORIGIN_DROP)
					|| (obj->origin == ORIGIN_DROP_PIT)
					|| (obj->origin == ORIGIN_DROP_VAULT)
					|| (obj->origin == ORIGIN_DROP_SUMMON)
					|| (obj->origin == ORIGIN_DROP_SPECIAL)
					|| (obj->origin == ORIGIN_DROP_BREED)
					|| (obj->origin == ORIGIN_DROP_POLY)
					|| (obj->origin == ORIGIN_DROP_WIZARD)))
				dump_item++;
		}

		/* Change origin if monster is invisible, unless we're in stats mode */
		if (!visible && !stats)
			obj->origin = ORIGIN_DROP_UNKNOWN;

		drop_near(cave, obj, 0, mon->fy, mon->fx, TRUE);
		obj = next;
	}

	/* Forget objects */
	mon->held_obj = NULL;

	/* Take note of any dropped treasure */
	if (visible && (dump_item || dump_gold))
		lore_treasure(mon, dump_item, dump_gold);

	/* Update monster list window */
	player->upkeep->redraw |= PR_MONLIST;

	/* Check if we finished a quest */
	quest_check(mon);
}


/**
 * Decreases a monster's hit points by `dam` and handle monster death.
 *
 * Hack -- we "delay" fear messages by passing around a "fear" flag.
 *
 * We announce monster death (using an optional "death message" (`note`)
 * if given, and a otherwise a generic killed/destroyed message).
 * 
 * Returns TRUE if the monster has been killed (and deleted).
 *
 * TODO: Consider decreasing monster experience over time, say, by using
 * "(m_exp * m_lev * (m_lev)) / (p_lev * (m_lev + n_killed))" instead
 * of simply "(m_exp * m_lev) / (p_lev)", to make the first monster
 * worth more than subsequent monsters.  This would also need to
 * induce changes in the monster recall code.  XXX XXX XXX
 **/
bool mon_take_hit(struct monster *mon, int dam, bool *fear, const char *note)
{
	s32b div, new_exp, new_exp_frac;
	struct monster_lore *lore = get_lore(mon->race);


	/* Redraw (later) if needed */
	if (player->upkeep->health_who == mon)
		player->upkeep->redraw |= (PR_HEALTH);

	/* Wake it up */
	mon_clear_timed(mon, MON_TMD_SLEEP, MON_TMD_FLG_NOMESSAGE, FALSE);

	/* Become aware of its presence */
	if (mflag_has(mon->mflag, MFLAG_UNAWARE))
		become_aware(mon);

	/* Hurt it */
	mon->hp -= dam;

	/* It is dead now */
	if (mon->hp < 0) {
		char m_name[80];
		char buf[80];

		/* Assume normal death sound */
		int soundfx = MSG_KILL;

		/* Play a special sound if the monster was unique */
		if (rf_has(mon->race->flags, RF_UNIQUE)) {
			if (mon->race->base == lookup_monster_base("Morgoth"))
				soundfx = MSG_KILL_KING;
			else
				soundfx = MSG_KILL_UNIQUE;
		}

		/* Extract monster name */
		monster_desc(m_name, sizeof(m_name), mon, MDESC_DEFAULT);

		/* Death */
		if (note) {
			/* Death by Missile/Spell attack */
			if (strlen(note) <= 1) {
				/* Be silent */
			} else {
				char *str = format("%s%s", m_name, note);
				my_strcap(str);
				msgt(soundfx, "%s", str);
			}
		} else if (!mflag_has(mon->mflag, MFLAG_VISIBLE))
			/* Death by physical attack -- invisible monster */
			msgt(soundfx, "You have killed %s.", m_name);
		else if (monster_is_unusual(mon->race))
			/* Death by Physical attack -- non-living monster */
			msgt(soundfx, "You have destroyed %s.", m_name);
		else
			/* Death by Physical attack -- living monster */
			msgt(soundfx, "You have slain %s.", m_name);

		/* Player level */
		div = player->lev;

		/* Give some experience for the kill */
		new_exp = ((long)mon->race->mexp * mon->race->level) / div;

		/* Handle fractional experience */
		new_exp_frac = ((((long)mon->race->mexp * mon->race->level) % div)
		                * 0x10000L / div) + player->exp_frac;

		/* Keep track of experience */
		if (new_exp_frac >= 0x10000L) {
			new_exp++;
			player->exp_frac = (u16b)(new_exp_frac - 0x10000L);
		} else
			player->exp_frac = (u16b)new_exp_frac;

		/* When the player kills a Unique, it stays dead */
		if (rf_has(mon->race->flags, RF_UNIQUE)) {
			char unique_name[80];
			mon->race->max_num = 0;

			/* 
			 * This gets the correct name if we slay an invisible 
			 * unique and don't have See Invisible.
			 */
			monster_desc(unique_name, sizeof(unique_name), mon,
						 MDESC_DIED_FROM);

			/* Log the slaying of a unique */
			strnfmt(buf, sizeof(buf), "Killed %s", unique_name);
			history_add(buf, HIST_SLAY_UNIQUE, 0);
		}

		/* Gain experience */
		player_exp_gain(player, new_exp);

		/* Generate treasure */
		monster_death(mon, FALSE);

		/* Recall even invisible uniques or winners */
		if (mflag_has(mon->mflag, MFLAG_VISIBLE) ||
			rf_has(mon->race->flags, RF_UNIQUE)) {
			/* Count kills this life */
			if (lore->pkills < MAX_SHORT) lore->pkills++;

			/* Count kills in all lives */
			if (lore->tkills < MAX_SHORT) lore->tkills++;

			/* Update lore and tracking */
			lore_update(mon->race, lore);
			monster_race_track(player->upkeep, mon->race);
		}

		/* Delete the monster */
		delete_monster_idx(mon->midx);

		/* Not afraid */
		(*fear) = FALSE;

		/* Monster is dead */
		return (TRUE);
	}


	/* Mega-Hack -- Pain cancels fear */
	if (!(*fear) && mon->m_timed[MON_TMD_FEAR] && (dam > 0)) {
		int tmp = randint1(dam);

		/* Cure a little or all fear */
		if (tmp < mon->m_timed[MON_TMD_FEAR]) {
			/* Reduce fear */
			mon_dec_timed(mon, MON_TMD_FEAR, tmp, MON_TMD_FLG_NOMESSAGE,
				FALSE);
		} else {
			/* Cure fear */
			mon_clear_timed(mon, MON_TMD_FEAR, MON_TMD_FLG_NOMESSAGE, FALSE);

			/* No more fear */
			(*fear) = FALSE;
		}
	}

	/* Sometimes a monster gets scared by damage */
	if (!mon->m_timed[MON_TMD_FEAR] && 
		!rf_has(mon->race->flags, RF_NO_FEAR) &&	dam > 0) {
		int percentage;

		/* Percentage of fully healthy */
		percentage = (100L * mon->hp) / mon->maxhp;

		/*
		 * Run (sometimes) if at 10% or less of max hit points,
		 * or (usually) when hit for half its current hit points
		 */
		if ((randint1(10) >= percentage) ||
		    ((dam >= mon->hp) && (randint0(100) < 80))) {
			int timer = randint1(10) + (((dam >= mon->hp) && (percentage > 7))
										? 20 : ((11 - percentage) * 5));

			/* Hack -- note fear */
			(*fear) = TRUE;

			mon_inc_timed(mon, MON_TMD_FEAR, timer,
					MON_TMD_FLG_NOMESSAGE | MON_TMD_FLG_NOFAIL, FALSE);
		}
	}


	/* Not dead yet */
	return (FALSE);
}

struct init_module mon_make_module = {
	.name = "monster/mon-make",
	.init = init_race_allocs,
	.cleanup = cleanup_race_allocs
};
