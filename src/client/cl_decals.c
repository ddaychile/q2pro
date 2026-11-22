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
    decal_image = 0;
    decal_image_tried = false;
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
