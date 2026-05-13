/*
cl_dlight_colors.h -- named per-event dynamic-light colours.

These are vec3_t multipliers applied to each channel in
R_AddDynamicLights_RGB. {1,1,1} matches the historical mono brightness;
values may (and intentionally do) exceed 1.0 to give dlights real
visible punch on top of static lighting. The dominant channel
saturating at the existing fullbright cap is intentional and matches
how mono dlights white-out at point-blank; secondary channels stay
properly tinted so the hue still reads. Tune values here without
touching call sites.
*/

#ifndef CL_DLIGHT_COLORS_H
#define CL_DLIGHT_COLORS_H

static const vec3_t DLIGHT_COLOR_WHITE      = {2.00f, 2.00f, 2.00f};
static const vec3_t DLIGHT_COLOR_MUZZLE     = {2.00f, 1.70f, 0.90f};  // warm yellow
static const vec3_t DLIGHT_COLOR_ROCKET     = {2.00f, 1.20f, 0.40f};  // orange
static const vec3_t DLIGHT_COLOR_EXPLOSION  = {2.00f, 1.00f, 0.40f};  // orange-red
static const vec3_t DLIGHT_COLOR_LIGHTNING  = {1.20f, 1.40f, 2.00f};  // pale blue
static const vec3_t DLIGHT_COLOR_BRIGHTLIGHT = {2.00f, 1.80f, 1.40f}; // warm
static const vec3_t DLIGHT_COLOR_DIMLIGHT   = {2.00f, 1.70f, 1.20f};  // warm

#endif
