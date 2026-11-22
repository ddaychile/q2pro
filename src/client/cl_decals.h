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

#ifndef CL_DECALS_H
#define CL_DECALS_H

#define MAX_DECALS  512

typedef enum {
    DECAL_BULLET,
    DECAL_BLOOD,
    DECAL_BURN
} decaltype_t;

typedef struct {
    vec3_t      origin;
    vec3_t      normal;
    float       size;
    float       die_time;       // cl.time in ms; 0 means unused slot
    float       fade_duration;  // ms
    float       rotation;       // degrees
    int         type;
    qhandle_t   image;
} decal_t;

void CL_DecalVectors(const vec3_t normal, float angle, vec3_t right, vec3_t up);
void CL_SpawnDecal(const vec3_t origin, const vec3_t dir, decaltype_t type, float radius);
void CL_AddDecals(void);

#endif // CL_DECALS_H
