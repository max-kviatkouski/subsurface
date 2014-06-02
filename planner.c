/* planner.c
 *
 * code that allows us to plan future dives
 *
 * (c) Dirk Hohndel 2013
 */
#include <assert.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include "dive.h"
#include "divelist.h"
#include "planner.h"
#include "gettext.h"
#include "libdivecomputer/parser.h"

#define TIMESTEP 1 /* second */
#define DECOTIMESTEP 60 /* seconds. Unit of deco stop times */

int decostoplevels[] = { 0, 3000, 6000, 9000, 12000, 15000, 18000, 21000, 24000, 27000,
				  30000, 33000, 36000, 39000, 42000, 45000, 48000, 51000, 54000, 57000,
				  60000, 63000, 66000, 69000, 72000, 75000, 78000, 81000, 84000, 87000,
				  90000, 100000, 110000, 120000, 130000, 140000, 150000, 160000, 170000,
				  180000, 190000, 200000, 220000, 240000, 260000, 280000, 300000,
				  320000, 340000, 360000, 380000 };
double plangflow, plangfhigh;
bool plan_verbatim = false, plan_display_runtime = true, plan_display_duration = false, plan_display_transitions = false;

#if DEBUG_PLAN
void dump_plan(struct diveplan *diveplan)
{
	struct divedatapoint *dp;
	struct tm tm;

	if (!diveplan) {
		printf("Diveplan NULL\n");
		return;
	}
	utc_mkdate(diveplan->when, &tm);

	printf("\nDiveplan @ %04d-%02d-%02d %02d:%02d:%02d (surfpres %dmbar):\n",
	       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	       tm.tm_hour, tm.tm_min, tm.tm_sec,
	       diveplan->surface_pressure);
	dp = diveplan->dp;
	while (dp) {
		printf("\t%3u:%02u: %dmm gas: %d o2 %d h2\n", FRACTION(dp->time, 60), dp->depth, dp->o2, dp->he);
		dp = dp->next;
	}
}
#endif

bool diveplan_empty(struct diveplan *diveplan)
{
	struct divedatapoint *dp;
	if (!diveplan || !diveplan->dp)
		return true;
	dp = diveplan->dp;
	while(dp) {
		if (dp->time)
			return false;
		dp = dp->next;
	}
	return true;
}

void set_last_stop(bool last_stop_6m)
{
	if (last_stop_6m == true)
		decostoplevels[1] = 6000;
	else
		decostoplevels[1] = 3000;
}

void set_verbatim(bool verbatim)
{
	plan_verbatim = verbatim;
}

void set_display_runtime(bool display)
{
	plan_display_runtime = display;
}

void set_display_duration(bool display)
{
	plan_display_duration = display;
}

void set_display_transitions(bool display)
{
	plan_display_transitions = display;
}

void get_gas_from_events(struct divecomputer *dc, int time, struct gasmix *gas)
{
	// we don't modify the values passed in if nothing is found
	// so don't call with uninitialized gasmix !
	struct event *event = dc->events;
	while (event && event->time.seconds <= time) {
		if (!strcmp(event->name, "gaschange"))
			*gas = *get_gasmix_from_event(event);
		event = event->next;
	}
}

int get_gasidx(struct dive *dive, struct gasmix *mix)
{
	int gasidx = -1;

	while (++gasidx < MAX_CYLINDERS)
		if (gasmix_distance(&dive->cylinder[gasidx].gasmix, mix) < 200)
			return gasidx;
	return -1;
}

double interpolate_transition(struct dive *dive, int t0, int t1, int d0, int d1, const struct gasmix *gasmix, int ppo2)
{
	int j;
	double tissue_tolerance;

	for (j = t0; j < t1; j++) {
		int depth = interpolate(d0, d1, j - t0, t1 - t0);
		tissue_tolerance = add_segment(depth_to_mbar(depth, dive) / 1000.0, gasmix, 1, ppo2, dive);
	}
	return tissue_tolerance;
}

/* returns the tissue tolerance at the end of this (partial) dive */
double tissue_at_end(struct dive *dive, char **cached_datap)
{
	struct divecomputer *dc;
	struct sample *sample, *psample;
	int i, t0, t1, gasidx, lastdepth;
	double tissue_tolerance;
	struct gasmix gas;

	if (!dive)
		return 0.0;
	if (*cached_datap) {
		tissue_tolerance = restore_deco_state(*cached_datap);
	} else {
		tissue_tolerance = init_decompression(dive);
		cache_deco_state(tissue_tolerance, cached_datap);
	}
	dc = &dive->dc;
	if (!dc->samples)
		return tissue_tolerance;
	psample = sample = dc->sample;
	lastdepth = t0 = 0;
	/* we always start with gas 0 (unless an event tells us otherwise) */
	gas = dive->cylinder[0].gasmix;
	for (i = 0; i < dc->samples; i++, sample++) {
		t1 = sample->time.seconds;
		get_gas_from_events(&dive->dc, t0, &gas);
		if ((gasidx = get_gasidx(dive, &gas)) == -1) {
			report_error(translate("gettextFromC", "Can't find gas %s"), gasname(&gas));
			gasidx = 0;
		}
		if (i > 0)
			lastdepth = psample->depth.mm;
		tissue_tolerance = interpolate_transition(dive, t0, t1, lastdepth, sample->depth.mm, &dive->cylinder[gasidx].gasmix, sample->po2);
		psample = sample;
		t0 = t1;
	}
	return tissue_tolerance;
}


/* if a default cylinder is set, use that */
void fill_default_cylinder(cylinder_t *cyl)
{
	const char *cyl_name = prefs.default_cylinder;
	struct tank_info_t *ti = tank_info;

	if (!cyl_name)
		return;
	while (ti->name != NULL) {
		if (strcmp(ti->name, cyl_name) == 0)
			break;
		ti++;
	}
	if (ti->name == NULL)
		/* didn't find it */
		return;
	cyl->type.description = strdup(ti->name);
	if (ti->ml) {
		cyl->type.size.mliter = ti->ml;
		cyl->type.workingpressure.mbar = ti->bar * 1000;
	} else {
		cyl->type.workingpressure.mbar = psi_to_mbar(ti->psi);
		if (ti->psi)
			cyl->type.size.mliter = cuft_to_l(ti->cuft) * 1000 / bar_to_atm(psi_to_bar(ti->psi));
	}
	cyl->depth.mm = 1600 * 1000 / O2_IN_AIR * 10 - 10000; // MOD of air
}

/* make sure that the gas we are switching to is represented in our
 * list of cylinders */
static int verify_gas_exists(struct dive *dive, struct gasmix mix_in)
{
	int i;
	cylinder_t *cyl;

	for (i = 0; i < MAX_CYLINDERS; i++) {
		cyl = dive->cylinder + i;
		if (cylinder_nodata(cyl))
			continue;
		if (gasmix_distance(&cyl->gasmix, &mix_in) < 200)
			return i;
	}
	fprintf(stderr, "this gas %s should have been on the cylinder list\nThings will fail now\n", gasname(&mix_in));
	return -1;
}

/* calculate the new end pressure of the cylinder, based on its current end pressure and the
 * latest segment. */
static void update_cylinder_pressure(struct dive *d, int old_depth, int new_depth, int duration, int sac, cylinder_t *cyl)
{
	volume_t gas_used;
	pressure_t delta_p;
	depth_t mean_depth;

	if (!cyl)
		return;
	mean_depth.mm = (old_depth + new_depth) / 2;
	gas_used.mliter = depth_to_atm(mean_depth.mm, d) * sac / 60 * duration;
	cyl->gas_used.mliter += gas_used.mliter;
	if (cyl->type.size.mliter) {
		delta_p.mbar = gas_used.mliter * 1000.0 / cyl->type.size.mliter;
		cyl->end.mbar -= delta_p.mbar;
	}
}

static struct dive *create_dive_from_plan(struct diveplan *diveplan, struct dive *master_dive)
{
	struct dive *dive;
	struct divedatapoint *dp;
	struct divecomputer *dc;
	struct sample *sample;
	struct gasmix oldgasmix;
	cylinder_t *cyl;
	int oldpo2 = 0;
	int lasttime = 0;
	int lastdepth = 0;

	if (!diveplan || !diveplan->dp)
		return NULL;
#if DEBUG_PLAN & 4
	printf("in create_dive_from_plan\n");
	dump_plan(diveplan);
#endif
	dive = alloc_dive();
	dive->when = diveplan->when;
	dive->dc.surface_pressure.mbar = diveplan->surface_pressure;
	dc = &dive->dc;
	dc->model = "planned dive"; /* do not translate here ! */
	dp = diveplan->dp;
	copy_cylinders(master_dive, dive);

	/* reset the end pressure values and start with the gas on the first cylinder */
	reset_cylinders(master_dive);
	cyl = &master_dive->cylinder[0];
	oldgasmix = cyl->gasmix;
	sample = prepare_sample(dc);
	sample->po2 = dp->po2;
	finish_sample(dc);
	while (dp) {
		struct gasmix gasmix = dp->gasmix;
		int po2 = dp->po2;
		int time = dp->time;
		int depth = dp->depth;

		if (time == 0) {
			/* special entries that just inform the algorithm about
			 * additional gases that are available */
			if (verify_gas_exists(dive, gasmix) < 0)
				goto gas_error_exit;
			dp = dp->next;
			continue;
		}
		if (gasmix_is_null(&gasmix))
			gasmix = oldgasmix;

		/* Check for SetPoint change */
		if (oldpo2 != po2) {
			if (lasttime)
				/* this is a bad idea - we should get a different SAMPLE_EVENT type
				 * reserved for this in libdivecomputer... overloading SMAPLE_EVENT_PO2
				 * with a different meaning will only cause confusion elsewhere in the code */
				add_event(dc, lasttime, SAMPLE_EVENT_PO2, 0, po2, "SP change");
			oldpo2 = po2;
		}

		/* Make sure we have the new gas, and create a gas change event */
		if (gasmix_distance(&gasmix, &oldgasmix) > 0) {
			int idx;
			if ((idx = verify_gas_exists(dive, gasmix)) < 0)
				goto gas_error_exit;
			/* need to insert a first sample for the new gas */
			add_gas_switch_event(dive, dc, lasttime + 1, idx);
			sample = prepare_sample(dc);
			sample[-1].po2 = po2;
			sample->time.seconds = lasttime + 1;
			sample->depth.mm = lastdepth;
			finish_sample(dc);
			cyl = &dive->cylinder[idx];
			oldgasmix = gasmix;
		}
		/* Create sample */
		sample = prepare_sample(dc);
		/* set po2 at beginning of this segment */
		/* and keep it valid for last sample - where it likely doesn't matter */
		sample[-1].po2 = po2;
		sample->po2 = po2;
		sample->time.seconds = lasttime = time;
		sample->depth.mm = lastdepth = depth;
		update_cylinder_pressure(dive, sample[-1].depth.mm, depth, time - sample[-1].time.seconds,
				dp->entered ? diveplan->bottomsac : diveplan->decosac, cyl);
		sample->cylinderpressure.mbar = cyl->end.mbar;
		finish_sample(dc);
		dp = dp->next;
	}
	if (dc->samples <= 1) {
		/* not enough there yet to create a dive - most likely the first time is missing */
		free(dive);
		dive = NULL;
	}
#if DEBUG_PLAN & 32
	if (dive)
		save_dive(stdout, dive);
#endif
	return dive;

gas_error_exit:
	free(dive);
	report_error(translate("gettextFromC", "Too many gas mixes"));
	return NULL;
}

void free_dps(struct divedatapoint *dp)
{
	while (dp) {
		struct divedatapoint *ndp = dp->next;
		free(dp);
		dp = ndp;
	}
}

struct divedatapoint *create_dp(int time_incr, int depth, struct gasmix gasmix, int po2)
{
	struct divedatapoint *dp;

	dp = malloc(sizeof(struct divedatapoint));
	dp->time = time_incr;
	dp->depth = depth;
	dp->gasmix = gasmix;
	dp->po2 = po2;
	dp->entered = false;
	dp->next = NULL;
	return dp;
}

struct divedatapoint *get_nth_dp(struct diveplan *diveplan, int idx)
{
	struct divedatapoint **ldpp, *dp = diveplan->dp;
	int i = 0;
	struct gasmix air = { 0 };
	ldpp = &diveplan->dp;

	while (dp && i++ < idx) {
		ldpp = &dp->next;
		dp = dp->next;
	}
	while (i++ <= idx) {
		*ldpp = dp = create_dp(0, 0, air, 0);
		ldpp = &((*ldpp)->next);
	}
	return dp;
}

void add_to_end_of_diveplan(struct diveplan *diveplan, struct divedatapoint *dp)
{
	struct divedatapoint **lastdp = &diveplan->dp;
	struct divedatapoint *ldp = *lastdp;
	int lasttime = 0;
	while (*lastdp) {
		ldp = *lastdp;
		if (ldp->time > lasttime)
			lasttime = ldp->time;
		lastdp = &(*lastdp)->next;
	}
	*lastdp = dp;
	if (ldp && dp->time != 0)
		dp->time += lasttime;
}

struct divedatapoint *plan_add_segment(struct diveplan *diveplan, int duration, int depth, struct gasmix gasmix, int po2, bool entered)
{
	struct divedatapoint *dp = create_dp(duration, depth, gasmix, po2);
	dp->entered = entered;
	add_to_end_of_diveplan(diveplan, dp);
	return (dp);
}

struct gaschanges {
	int depth;
	int gasidx;
};



static struct gaschanges *analyze_gaslist(struct diveplan *diveplan, struct dive *dive, int *gaschangenr, int depth, int *asc_cylinder)
{
	struct gasmix gas;
	int nr = 0;
	struct gaschanges *gaschanges = NULL;
	struct divedatapoint *dp = diveplan->dp;
	int best_depth = dive->cylinder[*asc_cylinder].depth.mm;
	while (dp) {
		if (dp->time == 0) {
			gas = dp->gasmix;
			if (dp->depth <= depth) {
				int i = 0;
				nr++;
				gaschanges = realloc(gaschanges, nr * sizeof(struct gaschanges));
				while (i < nr - 1) {
					if (dp->depth < gaschanges[i].depth) {
						memmove(gaschanges + i + 1, gaschanges + i, (nr - i - 1) * sizeof(struct gaschanges));
						break;
					}
					i++;
				}
				gaschanges[i].depth = dp->depth;
				gaschanges[i].gasidx = get_gasidx(dive, &gas);
				assert(gaschanges[i].gasidx != -1);
			} else {
				/* is there a better mix to start deco? */
				if (dp->depth < best_depth) {
					best_depth = dp->depth;
					*asc_cylinder = get_gasidx(dive, &gas);
				}
			}
		}
		dp = dp->next;
	}
	*gaschangenr = nr;
#if DEBUG_PLAN & 16
	for (nr = 0; nr < *gaschangenr; nr++) {
		int idx = gaschanges[nr].gasidx;
		printf("gaschange nr %d: @ %5.2lfm gasidx %d (%s)\n", nr, gaschanges[nr].depth / 1000.0,
		       idx, gasname(&dive->cylinder[idx].gasmix));
	}
#endif
	return gaschanges;
}

/* sort all the stops into one ordered list */
static unsigned int *sort_stops(int *dstops, int dnr, struct gaschanges *gstops, int gnr)
{
	int i, gi, di;
	int total = dnr + gnr;
	int *stoplevels = malloc(total * sizeof(int));

	/* no gaschanges */
	if (gnr == 0) {
		memcpy(stoplevels, dstops, dnr * sizeof(int));
		return stoplevels;
	}
	i = total - 1;
	gi = gnr - 1;
	di = dnr - 1;
	while (i >= 0) {
		if (dstops[di] > gstops[gi].depth) {
			stoplevels[i] = dstops[di];
			di--;
		} else if (dstops[di] == gstops[gi].depth) {
			stoplevels[i] = dstops[di];
			di--;
			gi--;
		} else {
			stoplevels[i] = gstops[gi].depth;
			gi--;
		}
		i--;
		if (di < 0) {
			while (gi >= 0)
				stoplevels[i--] = gstops[gi--].depth;
			break;
		}
		if (gi < 0) {
			while (di >= 0)
				stoplevels[i--] = dstops[di--];
			break;
		}
	}
	while (i >= 0)
		stoplevels[i--] = 0;

#if DEBUG_PLAN & 16
	int k;
	for (k = gnr + dnr - 1; k >= 0; k--) {
		printf("stoplevel[%d]: %5.2lfm\n", k, stoplevels[k] / 1000.0);
		if (stoplevels[k] == 0)
			break;
	}
#endif
	return stoplevels;
}

static void add_plan_to_notes(struct diveplan *diveplan, struct dive *dive, bool show_disclaimer)
{
	char buffer[20000];
	int len, gasidx, lastdepth = 0, lasttime = 0;
	struct divedatapoint *dp = diveplan->dp;
	const char *disclaimer = "";
	bool gaschange = true;

	if (!dp)
		return;

	if (show_disclaimer)
		disclaimer = translate("gettextFromC", "DISCLAIMER / WARNING: THIS IS A NEW IMPLEMENTATION OF THE BUHLMANN "
				       "ALGORITHM AND A DIVE PLANNER IMPLEMENTION BASED ON THAT WHICH HAS "
				       "RECEIVED ONLY A LIMITED AMOUNT OF TESTING. WE STRONGLY RECOMMEND NOT TO "
				       "PLAN DIVES SIMPLY BASED ON THE RESULTS GIVEN HERE.");
	len = snprintf(buffer, sizeof(buffer),
		       translate("gettextFromC", "%s\nSubsurface dive plan\nbased on GFlow = %d and GFhigh = %d\n\ndepth"),
		       disclaimer,  diveplan->gflow, diveplan->gfhigh);
	if (plan_display_runtime)
		len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", " runtime"));
	if (plan_display_duration)
		len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", " stop time"));
	len += snprintf(buffer + len, sizeof(buffer) - len, " gas\n");
	do {
		struct gasmix gasmix, newgasmix;
		const char *depth_unit;
		double depthvalue;
		int decimals;
		struct divedatapoint *nextdp;

		if (dp->time == 0)
			continue;
		gasmix = dp->gasmix;
		depthvalue = get_depth_units(dp->depth, &decimals, &depth_unit);
		/* analyze the dive points ahead */
		nextdp = dp->next;
		while (nextdp && nextdp->time == 0)
			nextdp = nextdp->next;
		if (nextdp) {
			newgasmix = nextdp->gasmix;
			if (gasmix_is_null(&newgasmix))
				newgasmix = gasmix;
		}
		/* do we want to skip this leg as it is devoid of anything useful? */
		if (!dp->entered &&
		    gasmix_distance(&gasmix, &newgasmix) == 0 &&
		    nextdp &&
		    dp->depth != lastdepth &&
		    nextdp->depth != dp->depth)
			continue;
		gasidx = get_gasidx(dive, &gasmix);
		len = strlen(buffer);
		if (dp->depth != lastdepth) {
			if (plan_display_transitions)
				snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "Transition to %.*f %s in %d:%02d min - runtime %d:%02u on %s\n"),
					 decimals, depthvalue, depth_unit,
					 FRACTION(dp->time - lasttime, 60),
					 FRACTION(dp->time, 60),
					 gasname(&gasmix));
			else
				if (dp->entered) {
					len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "%3.0f%s"), depthvalue, depth_unit);
					if (plan_display_runtime)
						len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "  %3dmin "), (dp->time + 30) / 60);
					if (plan_display_duration)
						len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "   %3dmin "), (dp->time - lasttime + 30) / 60);
					if (gaschange) {
						len += snprintf(buffer + len, sizeof(buffer) - len, " %s", gasname(&newgasmix));
						gaschange = false;
					}
					len += snprintf(buffer + len, sizeof(buffer) - len, "\n");
				}
		} else {
			if (plan_verbatim) {
				snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "Stay at %.*f %s for %d:%02d min - runtime %d:%02u on %s\n"),
					 decimals, depthvalue, depth_unit,
					 FRACTION(dp->time - lasttime, 60),
					 FRACTION(dp->time, 60),
					 gasname(&gasmix));
			} else {
				len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "%3.0f%s"), depthvalue, depth_unit);
				if (plan_display_runtime)
					len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "  %3dmin "), (dp->time + 30) / 60);
				if (plan_display_duration)
					len += snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "   %3dmin "), (dp->time - lasttime + 30) / 60);
				if (gaschange) {
					len += snprintf(buffer + len, sizeof(buffer) - len, " %s", gasname(&newgasmix));
					gaschange = false;
				}
				len += snprintf(buffer + len, sizeof(buffer) - len, "\n");
			}
		}
		if (nextdp && gasmix_distance(&gasmix, &newgasmix)) {
			// gas switch at this waypoint
			if (plan_verbatim)
				snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "Switch gas to %s\n"), gasname(&newgasmix));
			else
				gaschange = true;
			gasmix = newgasmix;
		}
		lasttime = dp->time;
		lastdepth = dp->depth;
	} while ((dp = dp->next) != NULL);
	len = strlen(buffer);
	snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "\nGas consumption:\n"));
	for (gasidx = 0; gasidx < MAX_CYLINDERS; gasidx++) {
		double volume;
		const char *unit;
		const char *warning = "";
		cylinder_t *cyl = &dive->cylinder[gasidx];
		if (cylinder_none(cyl))
			break;
		len = strlen(buffer);
		volume = get_volume_units(cyl->gas_used.mliter, NULL, &unit);
		if (cyl->type.size.mliter) {
			/* Warn if the plan uses more gas than is available in a cylinder
			 * This only works if we have working pressure for the cylinder
			 * 10bar is a made up number - but it seemed silly to pretend you could breathe cylinder down to 0 */
			if (cyl->end.mbar < 10000)
				warning = translate("gettextFromC", "WARNING: this is more gas than available in the specified cylinder!");
		}
		snprintf(buffer + len, sizeof(buffer) - len, translate("gettextFromC", "%.0f%s of %s%s\n"), volume, unit, gasname(&cyl->gasmix), warning);
	}
	dp = diveplan->dp;
	while (dp) {
		if (dp->time != 0) {
			int pO2 = depth_to_atm(dp->depth, dive) * dp->gasmix.o2.permille;
			if (pO2 > 1600) {
				const char *depth_unit;
				int decimals;
				double depth_value = get_depth_units(dp->depth, &decimals, &depth_unit);
				len = strlen(buffer);
				snprintf(buffer + len, sizeof(buffer) - len,
					 translate("gettextFromC", "Warning: high pO2 value %.2f at %d:%02u with gas %s at depth %.*f %s"),
					 pO2 / 1000.0, FRACTION(dp->time, 60), gasname(&dp->gasmix), depth_value, decimals, depth_unit);
			}
		}
		dp = dp->next;
	}
	dive->notes = strdup(buffer);
}

int ascend_velocity(int depth, int avg_depth, int bottom_time)
{
	/* We need to make this configurable */

	/* As an example (and possibly reasonable default) this is the Tech 1 provedure according
	 * to http://www.globalunderwaterexplorers.org/files/Standards_and_Procedures/SOP_Manual_Ver2.0.2.pdf */

	if (depth <= 6000)
		return 1000 / 60;

	if (depth * 4 > avg_depth *3)
		return 9000 / 60;
	else
		return 6000 / 60;
}

void plan(struct diveplan *diveplan, char **cached_datap, struct dive **divep, struct dive *master_dive, bool add_deco, bool show_disclaimer)
{
	struct dive *dive;
	struct sample *sample;
	int po2;
	int transitiontime, gi;
	int current_cylinder;
	unsigned int stopidx;
	int depth;
	double tissue_tolerance;
	struct gaschanges *gaschanges = NULL;
	int gaschangenr;
	int *stoplevels = NULL;
	char *trial_cache = NULL;
	bool stopping = false;
	bool clear_to_ascend;
	int clock, previous_point_time;
	int avg_depth, bottom_time;
	int last_ascend_rate;
	int best_first_ascend_cylinder;
	struct gasmix gas;

	set_gf(diveplan->gflow, diveplan->gfhigh, default_prefs.gf_low_at_maxdepth);
	if (!diveplan->surface_pressure)
		diveplan->surface_pressure = SURFACE_PRESSURE;
	if (*divep)
		delete_single_dive(dive_table.nr - 1);
	*divep = dive = create_dive_from_plan(diveplan, master_dive);
	if (!dive)
		return;
	record_dive(dive);

	/* Let's start at the last 'sample', i.e. the last manually entered waypoint. */
	sample = &dive->dc.sample[dive->dc.samples - 1];
	/* we start with gas 0, then check if that was changed */
	gas = dive->cylinder[0].gasmix;
	get_gas_from_events(&dive->dc, sample->time.seconds, &gas);
	po2 = dive->dc.sample[dive->dc.samples - 1].po2;
	if ((current_cylinder = get_gasidx(dive, &gas)) == -1) {
		report_error(translate("gettextFromC", "Can't find gas %s"), gasname(&gas));
		current_cylinder = 0;
	}
	depth = dive->dc.sample[dive->dc.samples - 1].depth.mm;
	avg_depth = average_depth(diveplan);
	last_ascend_rate = ascend_velocity(depth, avg_depth, bottom_time);

	/* if all we wanted was the dive just get us back to the surface */
	if (!add_deco) {
		transitiontime = depth / 75; /* this still needs to be made configurable */
		plan_add_segment(diveplan, transitiontime, 0, gas, po2, false);
		/* re-create the dive */
		delete_single_dive(dive_table.nr - 1);
		*divep = dive = create_dive_from_plan(diveplan, master_dive);
		if (dive)
			record_dive(dive);
		return;
	}

	tissue_tolerance = tissue_at_end(dive, cached_datap);

#if DEBUG_PLAN & 4
	printf("gas %s\n", gasname(&gas));
	printf("depth %5.2lfm ceiling %5.2lfm\n", depth / 1000.0, ceiling / 1000.0);
#endif

	best_first_ascend_cylinder = current_cylinder;
	/* Find the gases available for deco */
	gaschanges = analyze_gaslist(diveplan, dive, &gaschangenr, depth, &best_first_ascend_cylinder);
	/* Find the first potential decostopdepth above current depth */
	for (stopidx = 0; stopidx < sizeof(decostoplevels) / sizeof(int); stopidx++)
		if (decostoplevels[stopidx] >= depth)
			break;
	if (stopidx > 0)
		stopidx--;
	/* Stoplevels are either depths of gas changes or potential deco stop depths. */
	stoplevels = sort_stops(decostoplevels, stopidx + 1, gaschanges, gaschangenr);
	stopidx += gaschangenr;

	/* Keep time during the ascend */
	bottom_time = clock = previous_point_time = dive->dc.sample[dive->dc.samples - 1].time.seconds;
	gi = gaschangenr - 1;

	if (best_first_ascend_cylinder != current_cylinder) {
		stopping = true;

		current_cylinder = best_first_ascend_cylinder;
		gas = dive->cylinder[current_cylinder].gasmix;
#if DEBUG_PLAN & 16
		printf("switch to gas %d (%d/%d) @ %5.2lfm\n", best_first_ascend_cylinder,
			       (gas.o2.permille + 5) / 10, (gas.he.permille + 5) / 10, gaschanges[best_first_ascend_cylinder].depth / 1000.0);
#endif

	}
	while (1) {
		/* We will break out when we hit the surface */
		do {
			/* Ascend to next stop depth */
			int deltad = ascend_velocity(depth, avg_depth, bottom_time) * TIMESTEP;
			if (ascend_velocity(depth, avg_depth, bottom_time) != last_ascend_rate) {
				plan_add_segment(diveplan, clock - previous_point_time, depth, gas, po2, false);
				previous_point_time = clock;
				stopping = false;
				last_ascend_rate = ascend_velocity(depth, avg_depth, bottom_time);
			}
			if (depth - deltad < stoplevels[stopidx])
				deltad = depth - stoplevels[stopidx];

			tissue_tolerance = add_segment(depth_to_mbar(depth, dive) / 1000.0, &dive->cylinder[current_cylinder].gasmix, TIMESTEP, po2, dive);
			clock += TIMESTEP;
			depth -= deltad;
		} while (depth > stoplevels[stopidx]);

		if (depth <= 0)
			break;	/* We are at the surface */


		if (gi >= 0 && stoplevels[stopidx] == gaschanges[gi].depth) {
			/* We have reached a gas change.
			 * Record this in the dive plan */
			plan_add_segment(diveplan, clock - previous_point_time, depth, gas, po2, false);
			previous_point_time = clock;
			stopping = true;

			current_cylinder = gaschanges[gi].gasidx;
			gas = dive->cylinder[current_cylinder].gasmix;
#if DEBUG_PLAN & 16
			printf("switch to gas %d (%d/%d) @ %5.2lfm\n", gaschanges[gi].gasidx,
				       (gas.o2.permille + 5) / 10, (gas.he.permille + 5) / 10, gaschanges[gi].depth / 1000.0);
#endif
			gi--;
		}

		--stopidx;

		/* Save the current state and try to ascend to the next stopdepth */
		int trial_depth = depth;
		cache_deco_state(tissue_tolerance, &trial_cache);
		while(1) {
			/* Check if ascending to next stop is clear, go back and wait if we hit the ceiling on the way */
			clear_to_ascend = true;
			while (trial_depth > stoplevels[stopidx]) {
				int deltad = ascend_velocity(trial_depth, avg_depth, bottom_time) * TIMESTEP;
				tissue_tolerance = add_segment(depth_to_mbar(trial_depth, dive) / 1000.0, &dive->cylinder[current_cylinder].gasmix, TIMESTEP, po2, dive);
				if (deco_allowed_depth(tissue_tolerance, diveplan->surface_pressure / 1000.0, dive, 1) > trial_depth - deltad){
					/* We should have stopped */
					clear_to_ascend = false;
					break;
				}
				trial_depth -= deltad;
			}
			restore_deco_state(trial_cache);

			if(clear_to_ascend)
				break;	/* We did not hit the ceiling */

			/* Add a minute of deco time and then try again */
			if (!stopping) {
				/* The last segment was an ascend segment.
				 * Add a waypoint for start of this deco stop */
				plan_add_segment(diveplan, clock - previous_point_time, depth, gas, po2, false);
				previous_point_time = clock;
				stopping = true;
			}
			tissue_tolerance = add_segment(depth_to_mbar(depth, dive) / 1000.0, &dive->cylinder[current_cylinder].gasmix, DECOTIMESTEP, po2, dive);
			cache_deco_state(tissue_tolerance, &trial_cache);
			clock += DECOTIMESTEP;
			trial_depth = depth;
		}
		if (stopping) {
			/* Next we will ascend again. Add a waypoint if we have spend deco time */
			plan_add_segment(diveplan, clock - previous_point_time, depth, gas, po2, false);
			previous_point_time = clock;
			stopping = false;
		}

	}

	/* We made it to the surface */
	plan_add_segment(diveplan, clock - previous_point_time, 0, gas, po2, false);
	delete_single_dive(dive_table.nr - 1);
	*divep = dive = create_dive_from_plan(diveplan, master_dive);
	if (!dive)
		goto error_exit;
	add_plan_to_notes(diveplan, dive, show_disclaimer);

error_exit:
	free(stoplevels);
	free(gaschanges);
}

/*
 * Get a value in tenths (so "10.2" == 102, "9" = 90)
 *
 * Return negative for errors.
 */
static int get_tenths(const char *begin, const char **endp)
{
	char *end;
	int value = strtol(begin, &end, 10);

	if (begin == end)
		return -1;
	value *= 10;

	/* Fraction? We only look at the first digit */
	if (*end == '.') {
		end++;
		if (!isdigit(*end))
			return -1;
		value += *end - '0';
		do {
			end++;
		} while (isdigit(*end));
	}
	*endp = end;
	return value;
}

static int get_permille(const char *begin, const char **end)
{
	int value = get_tenths(begin, end);
	if (value >= 0) {
		/* Allow a percentage sign */
		if (**end == '%')
			++*end;
	}
	return value;
}

int validate_gas(const char *text, struct gasmix *gas)
{
	int o2, he;

	if (!text)
		return 0;

	while (isspace(*text))
		text++;

	if (!*text)
		return 0;

	if (!strcasecmp(text, translate("gettextFromC", "air"))) {
		o2 = O2_IN_AIR;
		he = 0;
		text += strlen(translate("gettextFromC", "air"));
	} else if (!strncasecmp(text, translate("gettextFromC", "ean"), 3)) {
		o2 = get_permille(text + 3, &text);
		he = 0;
	} else {
		o2 = get_permille(text, &text);
		he = 0;
		if (*text == '/')
			he = get_permille(text + 1, &text);
	}

	/* We don't want any extra crud */
	while (isspace(*text))
		text++;
	if (*text)
		return 0;

	/* Validate the gas mix */
	if (*text || o2 < 1 || o2 > 1000 || he < 0 || o2 + he > 1000)
		return 0;

	/* Let it rip */
	gas->o2.permille = o2;
	gas->he.permille = he;
	return 1;
}

int validate_po2(const char *text, int *mbar_po2)
{
	int po2;

	if (!text)
		return 0;

	po2 = get_tenths(text, &text);
	if (po2 < 0)
		return 0;

	while (isspace(*text))
		text++;

	while (isspace(*text))
		text++;
	if (*text)
		return 0;

	*mbar_po2 = po2 * 100;
	return 1;
}
