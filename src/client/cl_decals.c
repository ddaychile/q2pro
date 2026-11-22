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
#define DECAL_FADE_MS       1000.0f

// registered by the client cvar setup; NULL means decals are disabled
cvar_t  *cl_decals;
cvar_t  *cl_decals_time;
cvar_t  *cl_decals_max;

static decal_t      decal_buf[MAX_DECALS];
static int          decal_next;
static qhandle_t    decal_image;

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

static decal_t *alloc_decal(void)
{
    decal_t *d;
    int i, oldest;

    // prefer the slot at the write index when it is free, otherwise
    // overwrite the decal that expires first
    d = &decal_buf[decal_next];
    if (d->die_time == 0) {
        decal_next = (decal_next + 1) % MAX_DECALS;
        return d;
    }

    oldest = 0;
    for (i = 1; i < MAX_DECALS; i++) {
        if (decal_buf[i].die_time == 0) {
            oldest = i;
            break;
        }
        if (decal_buf[i].die_time < decal_buf[oldest].die_time)
            oldest = i;
    }
    decal_next = (oldest + 1) % MAX_DECALS;
    return &decal_buf[oldest];
}

/*
=================
CL_SpawnDecal

Traces from origin along dir and stamps a decal on the surface hit.
=================
*/
void CL_SpawnDecal(const vec3_t origin, const vec3_t dir, decaltype_t type, float radius)
{
    trace_t trace;
    vec3_t end;
    decal_t *d;
    float life;

    if (!cl_decals || !cl_decals->integer || !cl.bsp)
        return;

    if (!decal_image)
        decal_image = R_RegisterPic("pics/bullethole.png");

    VectorMA(origin, DECAL_TRACE_DIST, dir, end);
    CM_BoxTrace(&trace, origin, end, vec3_origin, vec3_origin,
                cl.bsp->nodes, MASK_SOLID, cl.csr.extended);
    if (trace.fraction == 1.0f || trace.startsolid)
        return;

    life = cl_decals_time ? cl_decals_time->value : 30.0f;

    d = alloc_decal();
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
