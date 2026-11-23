# Decals de impacto, quemadura y sangre: plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dibujar marcas persistentes de bala, quemadura y sangre sobre el BSP en cada cliente, disparadas desde los eventos `TE_*` existentes.

**Architecture:** El cliente (`src/client/cl_decals.c`) guarda los decals en un buffer circular, calcula el alfa con `cl.time` y los entrega al refresher a través de `refdef_t`, igual que hoy se entregan las partículas (`V_AddParticle` en `src/client/view.c`). El refresher (`src/refresh/decal.c`) los dibuja en la pasada translúcida, justo antes de `GL_DrawParticles()`. **Desviación del diseño:** el spec proponía una función nueva en `refexport_t`; se usa `refdef_t` porque es el mecanismo que ya usan las partículas y no requiere ampliar la tabla de funciones.

**Tech Stack:** C99, Meson, OpenGL legacy-compatible (`qgl*`), la capa `Cvar_Get` del cliente.

**Spec:** `docs/superpowers/specs/2026-11-22-blood-decals-design.md`

## Global Constraints

- C99 limpio, sin variables no inicializadas, sin leaks.
- Los decals son solo visuales en el cliente. Sin cambios de protocolo ni de servidor.
- Cvars con defaults exactos: `cl_decals` = `1`, `cl_decals_time` = `20`, `cl_decals_max` = `256`.
- `MAX_DECALS` = `512` (capacidad del buffer). `cl_decals_max` nunca supera `MAX_DECALS`.
- Offset de polígono: `glPolygonOffset(-1.0f, -2.0f)`. Escritura de profundidad desactivada mientras se dibujan.
- Radios: bala 4 a 6, sangre 8 a 16, quemadura 24 a 48.
- Traza de sangre: `dir * 64`.
- No incluir `dday_logo.png` ni `docs/` en los commits del feature.
- Textura: `pics/bullethole.png` (PNG 512×512 con alfa, ya convertido).

## Review Focus

- **Traza sin impacto** (la dirección apunta al vacío o el punto cae dentro de sólido): no se crea decal y no hay crash. Test en Task 3.
- **Evento sin BSP cargado** (cambio de mapa o `TE_*` en la pantalla de carga): `cl.bsp` es NULL, así que no se traza y el evento se ignora. Test en Task 3.
- **`cl_decals_max` bajado en caliente** (de 256 a 10 con 200 decals activos): el buffer se recorta sin acceso fuera de rango. Test en Task 3.
- **Reproducción de demo o salto de `cl.time` hacia atrás**: los decals con `die_time` futuro no deben desaparecer ni volverse opacos de golpe. El alfa se limita a [0, 1]. Test en Task 3.
- **Refresher sin decals** (`num_decals` = 0 o `decals` = NULL): `GL_DrawDecals` retorna sin tocar el estado de GL. Test en Task 2.

## Task 1: Transporte de decals en refdef_t

**Files:**
- Modify: `inc/refresh/refresh.h` (struct `refdecal_t` junto a `particle_t`; campos en `refdef_t`)
- Modify: `src/client/view.c` (`V_AddDecal`, y asignación en el bloque de `cl.refdef` cerca de la línea 449)
- Test: compilación (no hay tests unitarios para la capa de refdef)

**Interfaces:**
- Produces: `refdecal_t` con campos `vec3_t origin; vec3_t right; vec3_t up; float alpha; image handle qhandle_t image;` y `refdef_t` con `int num_decals; const refdecal_t *decals;`.
- Consumes: nada.

- [ ] **Step 1: Añadir el struct y los campos**

En `inc/refresh/refresh.h`, después de `particle_t`:

```c
typedef struct {
    vec3_t      origin[4];      // cuatro esquinas del quad, en orden de tira
    qhandle_t   image;          // handle de R_RegisterPic
    float       alpha;          // ya calculado por el cliente, en [0, 1]
} refdecal_t;
```

En `refdef_t`, después de `particle_t *particles;`:

```c
    int         num_decals;
    const refdecal_t *decals;
```

- [ ] **Step 2: Compilar para verificar**

Run: `meson compile -C builddir`
Expected: compila sin errores (los campos nuevos aún no se usan).

- [ ] **Step 3: Añadir `V_AddDecal` y conectarlo**

En `src/client/view.c`, junto a `V_AddParticle`, con un array estático `r_decals[MAX_DECALS]` y contador `r_numdecals` (reset donde se reinicia `r_numparticles`). Mismo patrón: si está lleno, se descarta. En el bloque de `cl.refdef`:

```c
cl.refdef.num_decals = r_numdecals;
cl.refdef.decals = r_decals;
```

- [ ] **Step 4: Compilar de nuevo**

Run: `meson compile -C builddir`
Expected: compila sin errores ni warnings nuevos.

- [ ] **Step 5: Commit**

```bash
git add inc/refresh/refresh.h src/client/view.c
git commit -m "refdef: transport decal list to the refresher" -m "Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>"
```

## Task 2: Dibujo translúcido en el refresher

**Files:**
- Create: `src/refresh/decal.c`
- Modify: `src/refresh/gl.h` (prototipo `void GL_DrawDecals(void);` junto a `GL_DrawParticles`)
- Modify: `src/refresh/main.c` (llamada en la línea 846, justo antes de `GL_DrawParticles();`)
- Modify: `meson.build` (añadir `src/refresh/decal.c` a la lista del refresher)

**Interfaces:**
- Consumes: `glr.fd.num_decals`, `glr.fd.decals` (de Task 1), `IMG_ForHandle(h)->texnum` (`src/refresh/images.h:93`).
- Produces: `void GL_DrawDecals(void)`.

- [ ] **Step 1: Escribir la prueba de no-op**

Como el refresher no tiene tests unitarios de dibujo, la verificación es por código: `GL_DrawDecals` debe salir al inicio con `if (!glr.fd.num_decals || !glr.fd.decals) return;`. Confirmar esa línea al revisar el archivo.

- [ ] **Step 2: Implementar `GL_DrawDecals`**

Pasos del dibujo, en este orden:
1. Salida temprana si no hay decals.
2. `GL_LoadMatrix(glr.viewmatrix)` y `GL_LoadUniforms()`, igual que `GL_DrawParticles`.
3. Estado: `GLS_BLEND_BLEND | GLS_DEPTHMASK_FALSE | glr.fog_bits` vía `GL_StateBits`.
4. Activar offset con `qglEnable(GL_POLYGON_OFFSET_FILL)` y `qglPolygonOffset(-1.0f, -2.0f)`, siguiendo el patrón de `src/refresh/mesh.c:587`.
5. Por cada decal, enlazar `IMG_ForHandle(decal->image)->texnum` en `TMU_TEXTURE` y emitir un quad (cuatro vértices) con color `(255, 255, 255, alpha*255)`.
6. Restaurar: `qglDisable(GL_POLYGON_OFFSET_FILL)` y `GL_StateBits` al estado previo al salir.

Agrupar por textura si el número de decals lo justifica; con 512 máximo basta un bucle directo.

- [ ] **Step 3: Compilar**

Run: `meson compile -C builddir`
Expected: compila; `GL_DrawDecals` existe y se enlaza.

- [ ] **Step 4: Verificación visual**

Run: arrancar el cliente con un decal de prueba forzado (un `decal` de 4 vértices hardcodeado temporalmente en `V_AddDecal`) y mirar una pared.
Expected: el quad aparece sobre la pared sin parpadeo (offset), con la textura visible. Quitar el hardcode antes del commit.

- [ ] **Step 5: Commit**

```bash
git add src/refresh/decal.c src/refresh/gl.h src/refresh/main.c meson.build
git commit -m "refresh: draw decals in the translucent pass" -m "Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>"
```

## Task 3: Módulo de decals en el cliente

**Files:**
- Create: `src/client/cl_decals.h`, `src/client/cl_decals.c`
- Modify: `src/client/client.h` (declaraciones `extern` de las cvars; `CL_AddDecals` y `CL_SpawnDecal` si se llaman desde fuera)
- Modify: `meson.build` (añadir `src/client/cl_decals.c` a `client_src`)

**Interfaces:**
- Produces:
  - `typedef struct { vec3_t origin; vec3_t normal; float size; float die_time; float fade_duration; float rotation; int type; qhandle_t image; } decal_t;`
  - `void CL_DecalVectors(const vec3_t normal, float angle, vec3_t right, vec3_t up);`
  - `void CL_SpawnDecal(const vec3_t origin, const vec3_t dir, int type, float radius);`
  - `void CL_AddDecals(void);` llama a `V_AddDecal` por cada decal activo con alfa calculado.
- Consumes: `CM_BoxTrace(trace_t *, start, end, mins, maxs, cl.bsp->nodes, contentmask, cl.csr.extended)` (`inc/common/cmodel.h:68`), `Q_rand()` (`inc/shared/shared.h:378`), `cl.time`, `V_AddDecal` (Task 1), `R_RegisterPic` (`inc/refresh/refresh.h:215`).

- [ ] **Step 1: Escribir las pruebas de matemática y estado**

Crear un archivo de prueba independiente (no se compila en el build principal) que llame a `CL_DecalVectors` con normal `(0,0,1)` y `angle` 0 y compruebe que `right` y `up` son ortogonales a la normal y entre sí, con longitud 1. Repetir con normal `(1,0,0)`. Ejecutar y ver que falla antes de implementar.

Run: compilar el archivo de prueba con `gcc` y los headers de Q2PRO.
Expected: FAIL (la función aún no existe).

- [ ] **Step 2: Implementar `CL_DecalVectors`**

Usar un eje de referencia que no sea paralelo a `normal` (`(0,0,1)` si `|normal.z| < 0.99`, si no `(1,0,0)`), derivar `right` como producto cruzado y `up` como producto cruzado de `normal` y `right`. Aplicar `angle` como rotación alrededor de la normal.

- [ ] **Step 3: Correr la prueba de matemática**

Run: ejecutar el archivo de prueba.
Expected: PASS.

- [ ] **Step 4: Implementar el buffer circular y `CL_SpawnDecal`**

- Buffer estático `decal_t cl_decals_buf[MAX_DECALS]` (`MAX_DECALS` = 512) con índice de escritura. Al llenarse, sobrescribir el más antiguo (menor `die_time`).
- `CL_SpawnDecal`: si `!cl_decals->integer` o `cl.bsp == NULL`, retornar sin hacer nada. Trazar desde `origin` hasta `origin + dir * 64` (o la distancia que pida el evento) con `CM_BoxTrace` con `vec3_origin` como mins y maxs. Si `trace.fraction == 1.0` o `trace.startsolid`, retornar. Si hay impacto, guardar `trace.endpos` y `trace.plane.normal`, `size = radius`, `die_time = cl.time + cl_decals_time->value * 1000`, `fade_duration = 1000` ms, `rotation = Q_rand() % 360`.

- [ ] **Step 5: Implementar `CL_AddDecals`**

Recorrer el buffer, saltar los vencidos y los que están fuera del límite `cl_decals_max` (recortar al límite actual sin salir de rango). Calcular el alfa: `alpha = (die_time - cl.time) / fade_duration`, limitado a [0, 1]. Construir los cuatro vértices con `CL_DecalVectors` y `size`, y llamar `V_AddDecal`.

- [ ] **Step 6: Correr verificación**

Run: `meson compile -C builddir`
Expected: compila sin errores ni warnings nuevos.

- [ ] **Step 7: Commit**

```bash
git add src/client/cl_decals.h src/client/cl_decals.c src/client/client.h meson.build
git commit -m "client: decal buffer, surface trace and per-frame list" -m "Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>"
```

## Task 4: Disparadores desde los eventos TE_*

**Files:**
- Modify: `src/client/tent.c` (`CL_ParseTEnt`, casos `TE_BLOOD` línea 1256, `TE_GUNSHOT` 1261, `TE_SHOTGUN` 1293, `TE_BLASTER` 1334, `TE_MOREBLOOD` 1556, `TE_ROCKET_EXPLOSION` 1403, `TE_EXPLOSION1` 1421)

**Interfaces:**
- Consumes: `CL_SpawnDecal` (Task 3), `te.pos1`, `te.dir`, `cl_disable_particles` y `NOPART_BLOOD` (ya usado en `TE_BLOOD`).

- [ ] **Step 1: Mapear radios y direcciones**

Bala (`TE_GUNSHOT`, `TE_SHOTGUN`, `TE_BLASTER`): `CL_SpawnDecal(te.pos1, te.dir, DECAL_BULLET, 4 + Q_rand() % 3)`. Sangre (`TE_BLOOD`, `TE_MOREBLOOD`): `CL_SpawnDecal(te.pos1, te.dir, DECAL_BLOOD, 8 + Q_rand() % 9)` con la traza `dir * 64`; respetar `cl_disable_particles & NOPART_BLOOD` (no dibujar sangre si el bit está activo). Explosión (`TE_ROCKET_EXPLOSION`, `TE_EXPLOSION1`): trazar en las cuatro direcciones horizontales y hacia abajo, y estampar en la superficie más cercana con radio `24 + Q_rand() % 25`.

- [ ] **Step 2: Añadir las llamadas**

Añadir cada `CL_SpawnDecal` en el `case` correspondiente, después de las partículas existentes. No cambiar el orden de los efectos ya presentes.

- [ ] **Step 3: Compilar**

Run: `meson compile -C builddir`
Expected: compila sin errores.

- [ ] **Step 4: Verificación en juego**

Run: disparar una bala contra una pared, una escopeta, un cohete y recibir daño.
Expected: aparece un decal por evento en la superficie; con `cl_disable_particles 1` la sangre no genera decal.

- [ ] **Step 5: Commit**

```bash
git add src/client/tent.c
git commit -m "client: spawn bullet, blood and explosion decals from TE events" -m "Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>"
```

## Task 5: Cvars y llamada por frame

**Files:**
- Modify: `src/client/main.c` (declaración `cvar_t *cl_decals, *cl_decals_time, *cl_decals_max;` cerca de la línea 61; `Cvar_Get` cerca de la línea 2820)
- Modify: `src/client/client.h` (`extern cvar_t *cl_decals, *cl_decals_time, *cl_decals_max;`)
- Modify: `src/client/view.c` (llamada a `CL_AddDecals()` al armar `cl.refdef`)

**Interfaces:**
- Consumes: `Cvar_Get` (`inc/common/cvar.h`), `CL_AddDecals` (Task 3).

- [ ] **Step 1: Declarar y registrar las cvars**

```c
cl_decals       = Cvar_Get("cl_decals", "1", 0);
cl_decals_time  = Cvar_Get("cl_decals_time", "20", 0);
cl_decals_max   = Cvar_Get("cl_decals_max", "256", 0);
```

- [ ] **Step 2: Llamar `CL_AddDecals` por frame**

En `src/client/view.c`, antes de `cl.refdef.num_decals = r_numdecals;`, llamar `CL_AddDecals();`.

- [ ] **Step 3: Compilar y probar cvars**

Run: `meson compile -C builddir`, luego en consola `cl_decals 0` y disparar.
Expected: no aparecen decals. Restaurar a `1`.

- [ ] **Step 4: Commit**

```bash
git add src/client/main.c src/client/client.h src/client/view.c
git commit -m "client: cl_decals, cl_decals_time and cl_decals_max cvars" -m "Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>"
```

## Task 6: Textura, compilación final y verificación

**Files:**
- Create: `pics/bullethole.png` en el directorio de datos del mod (ubicación exacta a confirmar en `dday-nomandy-installer/`; el repo no tiene un `pics/` propio).

**Interfaces:**
- Consumes: `R_RegisterPic("pics/bullethole.png")` en el cliente al cargar el mapa (cachear el handle en `cl_decals.c`).

- [ ] **Step 1: Ubicar el directorio de datos y copiar la textura**

Copiar `C:\Users\admin\AppData\Local\Temp\claude\788179c6e096df14\4cb47c7d-a0f4-487b-bb56-d379d95fcc79\scratchpad\bullethole.png` a la ruta de `pics/` del mod. Verificar que el instalador la incluye.

- [ ] **Step 2: Compilación limpia**

Run: `meson setup --wipe builddir` y luego `meson compile -C builddir`
Expected: compila sin warnings nuevos en los archivos tocados.

- [ ] **Step 3: Revisión de memoria**

Revisar que no hay arrays sin inicializar en `cl_decals.c` (buffer estático zero-inicializado) y que el handle de textura no se pide en cada frame.

- [ ] **Step 4: Verificación en juego contra la Review Focus**

Probar cada línea de la Review Focus: traza en vacío, cambio de mapa, bajar `cl_decals_max` con decals activos, y demo con salto de tiempo.
Expected: sin crashes y sin decals fuera de rango.

- [ ] **Step 5: Commit final**

```bash
git add pics/bullethole.png
git commit -m "assets: bullet hole decal texture" -m "Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>"
```

---

**Decisiones del plan (no estaban en el spec):**
- Transporte por `refdef_t` en lugar de función nueva en `refexport_t`.
- Radio aleatorio dentro de los rangos del spec.
- `fade_duration` fijo en 1000 ms (el spec lo deja como parámetro).
- Textura cargada con `R_RegisterPic`, permanente.
