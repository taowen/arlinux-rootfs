#ifndef UTAH_TEAPOT_H
#define UTAH_TEAPOT_H

/*
 * Tessellated Utah teapot for GPU probes.
 * Control points are the classic Newell / SGI GLUT data.
 */

int teapot_ensure(void);
int teapot_nverts(void);
void teapot_update(float seconds, float aspect, float r, float g, float b);
const float *teapot_clip(void);
const float *teapot_rgb(void);

#endif
