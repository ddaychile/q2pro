/*
Copyright (C) 2003-2006 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

//
// cl_decals.c -- surface decals (bullet holes, blood, burn marks)
//

#include "client.h"
#include "cl_decals.h"

#define DECAL_TRACE_DIST    64.0f
#define DECAL_TRACE_BACKOFF 4.0f
#define DECAL_FADE_MS       1000.0f

static decal_t      decal_buf[MAX_DECALS];
static qhandle_t    decal_image;
static bool         decal_image_tried;

// Blood sprays (D-Day, AQ2-style mods) are invisible EF_GIB projectiles that
// the server removes on contact, usually within a frame or two, so their
// particle trail rarely reaches a wall. Track them and stain where they hit.
#define BLOOD_TRACK_MAX     64
#define BLOOD_TRACE_MIN     64.0f
#define BLOOD_TRACE_MAX     256.0f
#define BLOOD_SPLATS        5       // stains per blood hit
#define BLOOD_SPREAD        28.0f   // max offset of a splat from the hit point
#define BLOOD_SPLAT_MIN     10.0f   // splat radius range
#define BLOOD_SPLAT_MAX     22.0f

typedef struct {
    int     number;     // entity number, 0 = unused slot
    int     frame;      // last server frame the entity was seen in
    vec3_t  origin;     // last server origin
    vec3_t  dir;        // unit flight direction
    float   step;       // distance moved per server frame, 0 if unknown
} bloodtrack_t;

static bloodtrack_t blood_tracks[BLOOD_TRACK_MAX];
static const vec3_t blood_stain_color = { 0.8f, 0.45f, 0.45f };

/*
=================
CL_BloodSplat

Scatters a few small, light blood stains around a hit point on a surface
with the given normal. Overlapping splats darken the middle on their own.
Returns the number of faces stained.
=================
*/
int CL_BloodSplat(const vec3_t org, const vec3_t normal)
{
    vec3_t right, up, pos;
    float r, a, size;
    int i, n = 0;

    for (i = 0; i < BLOOD_SPLATS; i++) {
        CL_DecalVectors(normal, Q_rand() % 360, right, up);
        // first splat at the hit point, the rest spread out around it
        r = i ? frand() * BLOOD_SPREAD : 0;
        a = frand() * 2.0f * M_PI;
        VectorMA(org, r * cosf(a), right, pos);
        VectorMA(pos, r * sinf(a), up, pos);
        size = BLOOD_SPLAT_MIN + frand() * (BLOOD_SPLAT_MAX - BLOOD_SPLAT_MIN);
        if (!i)
            size = BLOOD_SPLAT_MAX;
        n += R_AddStain(pos, blood_stain_color, size);
    }
    return n;
}

/*
=================
CL_DecalVectors

Builds an orthonormal basis (right, up) on the plane of the given normal,
rotated by angle degrees around the normal.
=================
*/
void CL_DecalVectors(const vec3_t normal, float angle, vec3_t right, vec3_t up)
{
    vec3_t ref, r, u;
    float s, c;

    if (fabsf(normal[2]) < 0.99f)
        VectorSet(ref, 0, 0, 1);
    else
        VectorSet(ref, 1, 0, 0);

    CrossProduct(ref, normal, r);
    VectorNormalize(r);
    CrossProduct(normal, r, u);
    VectorNormalize(u);

    s = sinf(DEG2RAD(angle));
    c = cosf(DEG2RAD(angle));

    VectorScale(r, c, right);
    VectorMA(right, s, u, right);
    VectorScale(u, c, up);
    VectorMA(up, -s, r, up);
}

/*
=================
CL_ClearDecals

Drops all decals and the cached image handle (map change, disconnect).
=================
*/
void CL_ClearDecals(void)
{
    memset(decal_buf, 0, sizeof(decal_buf));
    memset(blood_tracks, 0, sizeof(blood_tracks));
    decal_image = 0;
    decal_image_tried = false;
}

/*
=================
CL_TrackBlood

Records the server position of a blood spray entity seen in the current frame.
=================
*/
void CL_TrackBlood(int number, const vec3_t old_origin, const vec3_t origin, const vec3_t angles)
{
    bloodtrack_t *t, *free_slot = NULL;
    vec3_t delta;
    int i;

    for (i = 0, t = blood_tracks; i < BLOOD_TRACK_MAX; i++, t++) {
        if (t->number == number)
            break;
        if (!t->number && !free_slot)
            free_slot = t;
    }

    if (i == BLOOD_TRACK_MAX) {
        if (!free_slot)
            return;
        t = free_slot;
        t->number = number;
        // a new entity carries its spawn point in old_origin
        VectorSubtract(origin, old_origin, delta);
        t->step = VectorNormalize2(delta, t->dir);
        if (t->step < 1.0f) {
            t->step = 0;
            AngleVectors(angles, t->dir, NULL, NULL);
        }
        if (cl_decals_debug && cl_decals_debug->integer)
            Com_Printf("[blood] tracking spray %d, step %.0f\n", number, t->step);
    } else if (!VectorCompare(origin, t->origin)) {
        VectorSubtract(origin, t->origin, delta);
        t->step = VectorNormalize2(delta, t->dir);
    }

    VectorCopy(origin, t->origin);
    t->frame = cl.frame.number;
}

/*
=================
CL_SweepBlood

Blood sprays missing from the current frame hit something: trace their last
flight segment and stain the world surface they reached.
=================
*/
void CL_SweepBlood(void)
{
    bloodtrack_t *t;
    trace_t trace;
    vec3_t end;
    float dist;
    int i, n;

    for (i = 0, t = blood_tracks; i < BLOOD_TRACK_MAX; i++, t++) {
        if (!t->number || t->frame == cl.frame.number)
            continue;

        t->number = 0;
        if (!cl.bsp)
            continue;

        dist = t->step > 0 ? t->step * 2 : BLOOD_TRACE_MAX;
        dist = Q_clipf(dist, BLOOD_TRACE_MIN, BLOOD_TRACE_MAX);
        VectorMA(t->origin, dist, t->dir, end);
        CM_BoxTrace(&trace, t->origin, end, vec3_origin, vec3_origin,
                    cl.bsp->nodes, MASK_SOLID, cl.csr.extended);
        if (trace.fraction == 1.0f || trace.startsolid) {
            if (cl_decals_debug && cl_decals_debug->integer)
                Com_Printf("[blood] spray gone, no wall within %.0f\n", dist);
            continue;
        }

        n = CL_BloodSplat(trace.endpos, trace.plane.normal);
        if (cl_decals_debug && cl_decals_debug->integer)
            Com_Printf("[blood] spray hit (%.1f %.1f %.1f): %d faces\n",
                       trace.endpos[0], trace.endpos[1], trace.endpos[2], n);
    }
}

static decal_t *alloc_decal(void)
{
    decal_t *free_slot = NULL, *oldest = NULL;
    int i, live = 0, limit;

    limit = cl_decals_max ? Q_clip(cl_decals_max->integer, 0, MAX_DECALS) : MAX_DECALS;
    if (!limit)
        return NULL;

    for (i = 0; i < MAX_DECALS; i++) {
        decal_t *d = &decal_buf[i];
        if (d->die_time <= cl.time) {
            if (!free_slot)
                free_slot = d;
            continue;
        }
        live++;
        if (!oldest || d->die_time < oldest->die_time)
            oldest = d;
    }

    // cl_decals_max caps live decals: recycle the one expiring first
    if (live >= limit || !free_slot)
        return oldest;
    return free_slot;
}

/*
=================
CL_SpawnDecal

Traces from origin against dir (the outward normal) and stamps a decal on the surface hit.
=================
*/
void CL_SpawnDecal(const vec3_t origin, const vec3_t dir, decaltype_t type, float radius)
{
    trace_t trace;
    vec3_t start, end;
    decal_t *d;
    float life;

    if (!cl_decals || !cl_decals->integer || !cl.bsp) {
        if (cl_decals_debug && cl_decals_debug->integer)
            Com_Printf("[decals]   type %d skipped: disabled or no bsp\n", type);
        return;
    }

    // R_RegisterPic prepends "pics/"; a missing file yields notexture, so
    // the handle is cached after the first attempt
    if (!decal_image_tried) {
        decal_image = R_RegisterPic("bullethole.png");
        decal_image_tried = true;
    }

    // dir is the outward surface normal: trace back into the surface.
    // The server often reports the impact exactly on the plane, which a
    // trace starting there treats as inside the brush (startsolid), so
    // start slightly in front of the surface.
    VectorMA(origin, DECAL_TRACE_BACKOFF, dir, start);
    VectorMA(origin, -DECAL_TRACE_DIST, dir, end);
    CM_BoxTrace(&trace, start, end, vec3_origin, vec3_origin,
                cl.bsp->nodes, MASK_SOLID, cl.csr.extended);
    if (cl_decals_debug && cl_decals_debug->integer)
        Com_Printf("[decals]   type %d trace: fraction %.3f startsolid %d allsolid %d -> %s\n",
                   type, trace.fraction, trace.startsolid, trace.allsolid,
                   (trace.fraction == 1.0f || trace.startsolid) ? "REJECTED" : "spawned");
    if (trace.fraction == 1.0f || trace.startsolid)
        return;

    life = cl_decals_time ? cl_decals_time->value : 30.0f;

    d = alloc_decal();
    if (!d)
        return;
    VectorCopy(trace.endpos, d->origin);
    VectorCopy(trace.plane.normal, d->normal);
    d->size = radius;
    d->die_time = cl.time + life * 1000.0f;
    d->fade_duration = DECAL_FADE_MS;
    d->rotation = Q_rand() % 360;
    d->type = type;
    d->image = decal_image;
}

/*
=================
CL_AddDecals

Submits every live decal to the refresh via V_AddDecal.
=================
*/
void CL_AddDecals(void)
{
    int i, count, limit;

    if (!cl_decals || !cl_decals->integer)
        return;

    limit = cl_decals_max ? cl_decals_max->integer : MAX_DECALS;
    limit = Q_clip(limit, 0, MAX_DECALS);

    for (i = 0, count = 0; i < MAX_DECALS && count < limit; i++) {
        const decal_t *d = &decal_buf[i];
        refdecal_t rd;
        vec3_t right, up, r, u;
        float alpha;

        if (d->die_time <= cl.time)
            continue;

        alpha = (d->die_time - cl.time) / d->fade_duration;
        alpha = Q_clipf(alpha, 0, 1);

        CL_DecalVectors(d->normal, d->rotation, right, up);
        VectorScale(right, d->size, r);
        VectorScale(up, d->size, u);

        // triangle strip order: (0,0) (1,0) (0,1) (1,1)
        VectorSubtract(d->origin, r, rd.origin[0]);
        VectorSubtract(rd.origin[0], u, rd.origin[0]);
        VectorAdd(d->origin, r, rd.origin[1]);
        VectorSubtract(rd.origin[1], u, rd.origin[1]);
        VectorSubtract(d->origin, r, rd.origin[2]);
        VectorAdd(rd.origin[2], u, rd.origin[2]);
        VectorAdd(d->origin, r, rd.origin[3]);
        VectorAdd(rd.origin[3], u, rd.origin[3]);

        rd.image = d->image;
        rd.alpha = alpha;
        V_AddDecal(&rd);
        count++;
    }
}
