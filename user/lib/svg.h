#ifndef MYOS_SVG_H
#define MYOS_SVG_H
#include "img.h"
/* Static SVG: paths, basic shapes, inherited paint and affine transforms.
 * No scripts, external references, filters, text layout or animation. */
int svg_decode(const unsigned char *data, int len, img *out, int max_side);
#endif
