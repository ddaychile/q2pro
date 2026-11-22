# Decals de impacto, quemadura y sangre para Q2PRO

Estado: borrador pendiente de aprobación.
Rama de trabajo: `blood-decals`, creada desde `anticheat-cl` (aún no creada).

## Intención

Mostrar marcas persistentes de impacto de bala, quemaduras de explosión y manchas de sangre sobre las superficies del mapa. Hoy Q2PRO solo muestra partículas y humo efímeros para estos eventos (`src/client/tent.c`, `CL_ParseTEnt`). Los decals dan lectura visual de los combates sin cambiar la jugabilidad.

## Metas

- Decals en `TE_GUNSHOT`, `TE_SHOTGUN`, `TE_BLASTER`, `TE_BLOOD`, `TE_MOREBLOOD`, `TE_ROCKET_EXPLOSION` y `TE_EXPLOSION1`.
- Cliente y refresher separados: el cliente decide *qué* dibujar; el refresher decide *cómo* dibujarlo.
- Dibujo en la pasada translúcida del refresher, junto a `GL_DrawParticles()`.
- Cvars `cl_decals`, `cl_decals_time` y `cl_decals_max`.

## No-metas

- Sin cambios de protocolo ni de servidor. Los decals son solo visuales en cada cliente.
- Sin sincronización entre jugadores.
- Sin cambios en el código del anticheat.

## Anti-metas

- No llamar a OpenGL desde `src/client/`.
- No hacer trazas excesivas por frame que degraden el rendimiento.
- No dejar el estado de OpenGL modificado después del dibujo.

## Restricciones

- C99 limpio. Compilación con meson.
- No incluir el archivo sin seguimiento `dday_logo.png` en los commits.
- Los decals no se generan sobre entidades, solo sobre el BSP.

## Enfoques

1. **Buffer en el cliente, dibujo en el refresher (elegido).** `cl_decals.c` guarda los decals, calcula el alfa según `cl.time` y entrega al refresher una lista de quads ya resueltos mediante una función nueva en `refexport_t`. El refresher solo dibuja. Mantiene la separación.
2. **Llamadas a `qgl*` desde el cliente.** Descartado: rompe la separación y se salta el batching del refresher.
3. **Decals como entidades temporales (`R_AddEntity`).** Viable como respaldo, pero el polygon offset y el fade por alfa son menos precisos y consumen entidades.

## Diseño

**Cliente (`src/client/cl_decals.h`, `src/client/cl_decals.c`)**

- `decal_t`: `origin`, `normal`, `size`, `die_time`, `fade_duration`, `rotation`, `type`, y un identificador de imagen.
- Buffer circular de tamaño fijo `MAX_DECALS = 512`. Cuando se llena, se recicla el más antiguo. `cl_decals_max` limita la cantidad activa y nunca supera 512.
- `CL_DecalVectors(normal, angle, right, up)`: orienta el quad perpendicular a la superficie con rotación aleatoria.
- `CL_SpawnDecal(origin, dir, type)`: traza con `CM_BoxTrace` contra `cl.bsp` para obtener el punto de impacto y la normal.
- `CL_DecalsDraw()`: recorre los decals activos, calcula el alfa `(die_time - cl.time) / fade_duration` cuando corresponda, y llama a la función del refresher.

**Disparadores (`src/client/tent.c`)**

| Evento | Traza | Radio | Tipo |
|---|---|---|---|
| `TE_GUNSHOT`, `TE_SHOTGUN`, `TE_BLASTER` | en la dirección del impacto | 4 a 6 | bala |
| `TE_BLOOD`, `TE_MOREBLOOD` | `dir * 64` | 8 a 16 | sangre |
| `TE_ROCKET_EXPLOSION`, `TE_EXPLOSION1` | hacia las superficies cercanas | 24 a 48 | quemadura |

**Refresher (`src/refresh/`)**

- Función nueva en `refexport_t`, por ejemplo `R_DrawDecals(const decal_draw_t *list, int count)`, donde cada entrada trae cuatro vértices, la imagen y el alfa.
- Dentro de `GL_DrawParticles()` o junto a ella, el refresher configura blending `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`, `glPolygonOffset(-1, -2)`, `glDepthMask(GL_FALSE)`, dibuja y restaura el estado anterior.

## Decisiones tomadas por mí

- El alfa se calcula en el cliente, porque conoce `cl.time`. El refresher solo recibe el valor final.
- La rotación aleatoria usa `Q_rand()`.
- Los decals se traman con la misma semántica que las partículas existentes: si `cl_disable_particles` no incluye el bit de sangre, se dibuja la mancha de sangre.
- Todo queda detrás de `cl_decals`. Si está en 0, no se trazan ni se dibujan.

## Dejado al constructor

- Ajuste exacto de tamaños dentro del rango de cada evento.
- Estrategia de traza para explosiones (cuántas direcciones probar).
- Integración con el sistema de batching del refresher.

## Abierto

- **Texturas:** ¿se toman de AprQ2 (con revisión de licencia antes de copiar) o se usan placeholders? Pendiente de respuesta.
