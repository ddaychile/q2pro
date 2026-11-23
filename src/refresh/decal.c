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

#include "gl.h"

// Corner order is that of a triangle strip: 0, 1, 2, 3.
static const float decal_st[4][2] = {
    { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 }
};

// two triangles per decal, drawn from a strip-ordered quad
static const int decal_corners[6] = { 0, 1, 2, 2, 1, 3 };

static void GL_DrawDecalQuad(const refdecal_t *decal, vec_t *dst_vert)
{
    color_t color;
    int i, j;

    color.u8[0] = 255;
    color.u8[1] = 255;
    color.u8[2] = 255;
    color.u8[3] = (uint8_t)(Q_clipf(decal->alpha, 0, 1) * 255);

    for (i = 0; i < 6; i++) {
        j = decal_corners[i];
        VectorCopy(decal->origin[j], dst_vert);
        dst_vert[3] = decal_st[j][0];
        dst_vert[4] = decal_st[j][1];
        WN32(dst_vert + 5, color.u32);
        dst_vert += 6;
    }
}

void GL_DrawDecals(void)
{
    const refdecal_t *decal;
    int total, count, numverts;
    GLuint texnum, cur_texnum = 0;
    vec_t *dst_vert;
    glStateBits_t bits;
    glStateBits_t saved_bits;

    if (!glr.fd.num_decals || !glr.fd.decals)
        return;

    saved_bits = gls.state_bits;

    GL_LoadMatrix(glr.viewmatrix);
    GL_LoadUniforms();
    GL_BindArrays(VA_EFFECT);

    // quads are single-sided in practice: ignore winding
    bits = GLS_BLEND_BLEND | GLS_DEPTHMASK_FALSE | GLS_CULL_DISABLE | glr.fog_bits;

    qglEnable(GL_POLYGON_OFFSET_FILL);
    qglPolygonOffset(-1.0f, -2.0f);

    decal = glr.fd.decals;
    total = glr.fd.num_decals;
    while (total) {
        // batch consecutive decals sharing a texture
        texnum = IMG_ForHandle(decal->image)->texnum;
        count = 0;
        dst_vert = tess.vertices;
        do {
            GL_DrawDecalQuad(&decal[count], dst_vert);
            dst_vert += 6 * 6;
            count++;
            if (count >= total || count >= TESS_MAX_VERTICES / 6)
                break;
            cur_texnum = IMG_ForHandle(decal[count].image)->texnum;
        } while (cur_texnum == texnum);

        numverts = count * 6;

        GL_BindTexture(TMU_TEXTURE, texnum);
        GL_StateBits(bits);
        GL_ArrayBits(GLA_VERTEX | GLA_TC | GLA_COLOR);

        GL_LockArrays(numverts);
        qglDrawArrays(GL_TRIANGLES, 0, numverts);
        GL_UnlockArrays();

        decal += count;
        total -= count;
    }

    qglDisable(GL_POLYGON_OFFSET_FILL);
    GL_StateBits(saved_bits);
}
